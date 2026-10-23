#!/usr/bin/env python3
"""
Unified Grinder Tool - Cross-platform Python replacement for bash script
Single script for all grinder operations: build, upload, export, analyze, report
"""

import argparse
import asyncio
import os
import sys
import subprocess
import platform
import venv
from pathlib import Path
from typing import Optional, List, Dict, Any
import shutil
import stat

from firmware_variants import FIRMWARE_ENV, DEPRECATED_VARIANT_CHOICES, VARIANT_DEPRECATED_MSG

# Color support for cross-platform output
try:
    from colorama import init, Fore, Style
    init(autoreset=True)
    COLORS = {
        'RED': Fore.RED,
        'GREEN': Fore.GREEN,
        'YELLOW': Fore.YELLOW,
        'BLUE': Fore.BLUE,
        'PURPLE': Fore.MAGENTA,
        'CYAN': Fore.CYAN,
        'RESET': Style.RESET_ALL
    }
except ImportError:
    # Fallback for systems without colorama
    COLORS = {k: '' for k in ['RED', 'GREEN', 'YELLOW', 'BLUE', 'PURPLE', 'CYAN', 'RESET']}

class GrinderTool:
    """Unified grinder tool for cross-platform operations."""
    
    def __init__(self):
        self.script_dir = Path(__file__).parent
        self.project_dir = self.script_dir.parent
        self.venv_dir = self.script_dir / "venv"
        
        # Platform-specific paths
        if platform.system() == "Windows":
            self.venv_python = self.venv_dir / "Scripts" / "python.exe"
            self.venv_pip = self.venv_dir / "Scripts" / "pip.exe"
            self.venv_streamlit = self.venv_dir / "Scripts" / "streamlit.exe"
        else:
            self.venv_python = self.venv_dir / "bin" / "python3"
            self.venv_pip = self.venv_dir / "bin" / "pip"
            self.venv_streamlit = self.venv_dir / "bin" / "streamlit"
        
        self.ble_tool = self.script_dir / "ble" / "grinder-ble.py"
        self.wifi_tool = self.script_dir / "wifi" / "grinder-wifi.py"
        self.streamlit_dir = self.script_dir / "streamlit-reports"
        self.db_path = self.script_dir / "database" / "grinder_data.db"
        self.requirements_txt = self.script_dir / "requirements.txt"
    
    def safe_print(self, text: str):
        """Print text with proper encoding handling for all platforms."""
        try:
            print(text)
        except UnicodeEncodeError:
            # Replace problematic Unicode chars for Windows
            safe_text = text.encode('ascii', 'replace').decode('ascii')
            print(safe_text)
    
    def print_header(self, message: str):
        """Print a formatted header."""
        self.safe_print(f"{COLORS['BLUE']}=== {message} ==={COLORS['RESET']}")
    
    def print_success(self, message: str):
        """Print a success message."""
        self.safe_print(f"{COLORS['GREEN']}[OK] {message}{COLORS['RESET']}")
    
    def print_error(self, message: str):
        """Print an error message."""
        self.safe_print(f"{COLORS['RED']}[ERROR] {message}{COLORS['RESET']}")
    
    def print_warning(self, message: str):
        """Print a warning message."""
        self.safe_print(f"{COLORS['YELLOW']}[WARNING] {message}{COLORS['RESET']}")
    
    def print_info(self, message: str):
        """Print an info message."""
        self.safe_print(f"{COLORS['CYAN']}[INFO] {message}{COLORS['RESET']}")
    
    def check_venv(self) -> bool:
        """Check if virtual environment exists and is properly set up."""
        if not self.venv_python.exists():
            self.print_warning("Virtual environment not found, setting up automatically...")
            return self.install_dependencies()
        return True
    
    def install_dependencies(self) -> bool:
        """Create virtual environment and install dependencies."""
        try:
            if not self.venv_dir.exists():
                self.print_info("Creating virtual environment...")
                
                # Find Python executable
                python_cmd = None
                for cmd in ["python3", "python"]:
                    if shutil.which(cmd):
                        python_cmd = cmd
                        break
                
                if not python_cmd:
                    self.print_error("Python not found. Please install Python 3.9+")
                    return False
                
                # Create virtual environment
                venv.create(self.venv_dir, with_pip=True)
            
            self.print_info("Installing Python packages...")
            if not self.venv_pip.exists():
                self.print_error(f"Virtual environment pip not found at: {self.venv_pip}")
                self.print_info(f"Platform: {platform.system()}, Expected venv structure may be incorrect")
                return False
            
            # Install requirements
            result = subprocess.run([
                str(self.venv_pip), "install", "-q", "-r", str(self.requirements_txt)
            ], capture_output=True, text=True)
            
            if result.returncode != 0:
                self.print_error(f"Failed to install dependencies: {result.stderr}")
                return False
            
            # Also install colorama if not already present
            subprocess.run([
                str(self.venv_pip), "install", "-q", "colorama"
            ], capture_output=True, text=True)
            
            return True
            
        except Exception as e:
            self.print_error(f"Failed to set up environment: {e}")
            return False
    
    def run_command(self, cmd: List[str], cwd: Optional[Path] = None, capture_output: bool = False) -> subprocess.CompletedProcess:
        """Run a command with proper error handling."""
        try:
            return subprocess.run(
                cmd, 
                cwd=cwd or self.project_dir,
                capture_output=capture_output,
                text=True,
                check=False
            )
        except FileNotFoundError as e:
            self.print_error(f"Command not found: {cmd[0]}")
            raise e
    
    async def run_async_command(self, cmd: List[str], cwd: Optional[Path] = None) -> int:
        """Run an async command (for BLE operations)."""
        try:
            process = await asyncio.create_subprocess_exec(
                *cmd,
                cwd=cwd or self.project_dir,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT
            )
            
            # Stream output in real-time
            while True:
                line = await process.stdout.readline()
                if not line:
                    break
                print(line.decode().rstrip())
            
            await process.wait()
            return process.returncode
            
        except Exception as e:
            self.print_error(f"Command failed: {e}")
            return 1
    
    def firmware_path(self) -> Path:
        """Path of the built firmware.bin."""
        return self.project_dir / ".pio" / "build" / FIRMWARE_ENV / "firmware.bin"

    def warn_if_variant_given(self, args: argparse.Namespace):
        """The --variant option is deprecated: one image supports both boards."""
        if getattr(args, 'variant', None):
            self.print_warning(VARIANT_DEPRECATED_MSG)

    def cmd_build(self, args: argparse.Namespace) -> int:
        """Build firmware using PlatformIO."""
        self.print_header("Building Firmware")

        if not self.check_venv():
            return 1

        self.warn_if_variant_given(args)

        # Use PlatformIO from the project venv
        result = self.run_command([
            str(self.venv_python), "-m", "platformio", "run",
            "-e", FIRMWARE_ENV
        ])

        if result.returncode == 0:
            self.print_success("Firmware build completed")
        else:
            self.print_error("Build failed")
        
        return result.returncode
    
    async def cmd_upload(self, args: argparse.Namespace) -> int:
        """Upload firmware via BLE OTA."""
        firmware_path = args.firmware
        self.warn_if_variant_given(args)

        if not firmware_path:
            firmware_path = self.firmware_path()
            if not firmware_path.exists():
                self.print_error(f"No firmware found at: {firmware_path}")
                self.print_info("Run: python3 grinder.py build")
                return 1

        self.print_header("BLE OTA Upload")
        self.print_info(f"Using firmware: {firmware_path}")

        if not self.check_venv():
            return 1

        cmd = [str(self.venv_python), str(self.ble_tool), "upload", str(firmware_path)]

        # Add additional arguments
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])
        if hasattr(args, 'force_full') and args.force_full:
            cmd.append("--force-full")
        
        return await self.run_async_command(cmd)
    
    async def cmd_build_upload(self, args: argparse.Namespace) -> int:
        """Build firmware and upload via BLE."""
        build_result = self.cmd_build(args)
        if build_result != 0:
            return build_result
        
        # Upload the firmware that was just built (deprecation warning already shown by the build)
        args.variant = None
        args.firmware = None
        return await self.cmd_upload(args)
    
    async def cmd_export(self, args: argparse.Namespace) -> int:
        """Export grind data from device."""
        self.print_header("Exporting Grind Data")
        
        if not self.check_venv():
            return 1
        
        cmd = [str(self.venv_python), str(self.ble_tool), "export"]
        
        if hasattr(args, 'db') and args.db:
            cmd.extend(["--db", args.db])
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])
        
        return await self.run_async_command(cmd)
    
    async def cmd_analyze(self, args: argparse.Namespace) -> int:
        """Export data and launch Streamlit report."""
        self.print_header("Data Analysis Workflow")
        
        if not self.check_venv():
            return 1
        
        cmd = [str(self.venv_python), str(self.ble_tool), "analyse"]
        
        if hasattr(args, 'db') and args.db:
            cmd.extend(["--db", args.db])
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])
        
        return await self.run_async_command(cmd)
    
    def cmd_report(self, args: argparse.Namespace) -> int:
        """Launch Streamlit report from existing data."""
        self.print_header("Launching Streamlit Report")
        
        # Determine database file
        db_file = self.db_path
        if hasattr(args, 'db') and args.db:
            if Path(args.db).is_absolute():
                db_file = Path(args.db)
            else:
                db_file = self.script_dir / "database" / args.db
        
        if not db_file.exists():
            self.print_error(f"Database file not found: {db_file}")
            self.print_info("Run: python3 grinder.py export")
            return 1
        
        if not self.check_venv():
            return 1
        
        # Check if streamlit exists in venv
        if not self.venv_streamlit.exists():
            self.print_warning("Streamlit not found, installing...")
            result = subprocess.run([
                str(self.venv_pip), "install", "streamlit>=1.28.0", "plotly>=5.15.0"
            ], capture_output=True, text=True)
            
            if result.returncode != 0:
                self.print_error(f"Failed to install Streamlit: {result.stderr}")
                return 1
        
        self.print_info(f"Using database: {db_file}")
        self.print_info("Opening at: http://localhost:8501")
        self.print_info("Press Ctrl+C to stop the server")
        
        # Set environment variables and launch streamlit
        env = os.environ.copy()
        env["GRIND_DB_PATH"] = str(db_file)
        env["PYTHONPATH"] = str(self.streamlit_dir)
        
        try:
            result = subprocess.run([
                str(self.venv_python), "-m", "streamlit", "run", "grind_report.py"
            ], cwd=self.streamlit_dir, env=env)
            return result.returncode
        except KeyboardInterrupt:
            self.print_info("Streamlit server stopped")
            return 0
    
    async def cmd_scan(self, args: argparse.Namespace) -> int:
        """Scan for BLE devices."""
        self.print_header("Scanning for BLE Devices")
        
        if not self.check_venv():
            return 1
        
        cmd = [str(self.venv_python), str(self.ble_tool), "scan"]
        return await self.run_async_command(cmd)
    
    async def cmd_connect(self, args: argparse.Namespace) -> int:
        """Connect to grinder device."""
        self.print_header("Connecting to Grinder")
        
        if not self.check_venv():
            return 1
        
        cmd = [str(self.venv_python), str(self.ble_tool), "connect"]
        
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])
        
        return await self.run_async_command(cmd)
    
    async def cmd_debug(self, args: argparse.Namespace) -> int:
        """Stream live debug logs from device."""
        self.print_header("Debug Monitor")
        
        if not self.check_venv():
            return 1
        
        cmd = [str(self.venv_python), str(self.ble_tool), "debug"]
        
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])
        
        return await self.run_async_command(cmd)
    
    async def cmd_info(self, args: argparse.Namespace) -> int:
        """Get device system information."""
        self.print_header("Device System Information")

        if not self.check_venv():
            return 1

        cmd = [str(self.venv_python), str(self.ble_tool), "info"]

        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])

        return await self.run_async_command(cmd)

    def run_wifi_tool(self, tool_args: List[str]) -> int:
        """Run the WiFi tool with the current interpreter (stdlib only, no venv needed)."""
        cmd = [sys.executable, str(self.wifi_tool)] + tool_args
        return subprocess.run(cmd, cwd=self.project_dir).returncode

    @staticmethod
    def wifi_connection_args(args: argparse.Namespace) -> List[str]:
        tool_args: List[str] = []
        if getattr(args, 'host', None):
            tool_args.extend(["--host", args.host])
        if getattr(args, 'token', None):
            tool_args.extend(["--token", args.token])
        return tool_args

    def cmd_wifi_info(self, args: argparse.Namespace) -> int:
        """Show device info over WiFi."""
        self.print_header("WiFi Device Info")
        return self.run_wifi_tool(["info"] + self.wifi_connection_args(args))

    def cmd_wifi_upload(self, args: argparse.Namespace) -> int:
        """Push a firmware image over WiFi."""
        self.print_header("WiFi Upload")
        tool_args = ["upload"] + self.wifi_connection_args(args)
        if getattr(args, 'expect_build', None) is not None:
            tool_args.extend(["--expect-build", str(args.expect_build)])
        if getattr(args, 'no_wait', False):
            tool_args.append("--no-wait")
        if getattr(args, 'firmware', None):
            tool_args.append(args.firmware)
        return self.run_wifi_tool(tool_args)

    async def cmd_board(self, args: argparse.Namespace) -> int:
        """Set the hardware board override on the device."""
        self.print_header("Hardware Board Override")

        if not self.check_venv():
            return 1

        cmd = [str(self.venv_python), str(self.ble_tool), "board", args.mode]

        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])

        return await self.run_async_command(cmd)

    async def cmd_wifi_set(self, args: argparse.Namespace) -> int:
        """Store WiFi credentials and a freshly generated update token on the device over BLE."""
        self.print_header("WiFi Setup (Bluetooth)")

        if not self.check_venv():
            return 1

        return await self.run_async_command(self.wifi_set_command(args))

    def wifi_set_command(self, args: argparse.Namespace) -> List[str]:
        """BLE tool command line; options first, SSID after '--' so a name starting with '-' is not an option."""
        cmd = [str(self.venv_python), str(self.ble_tool), "wifi-set"]

        if getattr(args, 'password_env', None):
            cmd.extend(["--password-env", args.password_env])
        if getattr(args, 'open', False):
            cmd.append("--open")
        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])

        cmd.extend(["--", args.ssid])
        return cmd

    async def cmd_wifi_clear(self, args: argparse.Namespace) -> int:
        """Remove the stored WiFi credentials from the device over BLE."""
        self.print_header("WiFi Clear (Bluetooth)")

        if not self.check_venv():
            return 1

        cmd = [str(self.venv_python), str(self.ble_tool), "wifi-clear"]

        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])

        return await self.run_async_command(cmd)

    async def cmd_diagnostics(self, args: argparse.Namespace) -> int:
        """Get comprehensive diagnostic report."""
        self.print_header("Diagnostic Report")

        if not self.check_venv():
            return 1

        cmd = [str(self.venv_python), str(self.ble_tool), "diagnostics"]

        if hasattr(args, 'device') and args.device:
            cmd.extend(["--device", args.device])

        if hasattr(args, 'save') and args.save:
            cmd.extend(["--save", args.save])

        return await self.run_async_command(cmd)

    def cmd_install(self, args: argparse.Namespace) -> int:
        """Manually install Python dependencies."""
        self.print_header("Installing Dependencies")
        
        if self.install_dependencies():
            self.print_success("Dependencies installed")
            return 0
        else:
            return 1
    
    def cmd_clean(self, args: argparse.Namespace) -> int:
        """Clean build artifacts."""
        self.print_header("Cleaning Build Artifacts")
        
        if not self.check_venv():
            return 1
        
        # Use PlatformIO from the project venv
        result = self.run_command([
            str(self.venv_python), "-m", "platformio", "run", "--target", "clean"
        ])
        
        # Also remove .pio/build directory
        build_dir = self.project_dir / ".pio" / "build"
        if build_dir.exists():
            shutil.rmtree(build_dir)
        
        if result.returncode == 0:
            self.print_success("Build artifacts cleaned")
        
        return result.returncode
    
    def cmd_release(self, args: argparse.Namespace) -> int:
        """Create a tagged release using the release helper script."""
        self.print_header("Creating Tagged Release")
        
        release_script = self.script_dir / "release.py"
        
        # Make sure the release script exists
        if not release_script.exists():
            self.print_error("Release script not found!")
            self.print_info("The release.py script should be in the tools/ directory")
            return 1
        
        # Run the release script
        try:
            result = subprocess.run([sys.executable, str(release_script)])
            return result.returncode
        except Exception as e:
            self.print_error(f"Failed to run release script: {e}")
            return 1

def create_parser() -> argparse.ArgumentParser:
    """Create the argument parser with all subcommands."""
    parser = argparse.ArgumentParser(
        description="Unified Grinder Tool - All-in-one grinder operations",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
{COLORS['YELLOW']}Examples:{COLORS['RESET']}
  python3 grinder.py build-upload              # Build and upload firmware
  python3 grinder.py build-upload --force-full # Build and force full firmware update
  python3 grinder.py build                     # Build firmware (one image for V1 and V2 boards)
  python3 grinder.py analyze                   # Export data and show interactive report
  python3 grinder.py report                    # Just show report from existing data
  python3 grinder.py export --db session1.db  # Export to custom database
  python3 grinder.py upload --device MyGrinder # Upload to specific device
  python3 grinder.py connect                   # Connect to grinder device
  python3 grinder.py info                      # Get device system information
  python3 grinder.py board v2                  # Set board override (auto always; v1/v2 only if board not confirmed)
  python3 grinder.py wifi-set "My Network"     # Provision WiFi over BLE (prompts for the password), then wifi-upload works
  python3 grinder.py wifi-clear                # Remove the grinder's WiFi credentials
        """
    )
    
    subparsers = parser.add_subparsers(dest='command', required=True, help='Available commands')
    
    # Build & Upload Commands
    variant_help = 'Deprecated and ignored: one firmware image supports both V1 and V2 boards'

    build_parser = subparsers.add_parser('build', help='Build firmware using PlatformIO')
    build_parser.add_argument('--variant', choices=DEPRECATED_VARIANT_CHOICES, help=variant_help)

    upload_parser = subparsers.add_parser('upload', help='Upload firmware via BLE OTA')
    upload_parser.add_argument('firmware', nargs='?', help='Path to firmware .bin file (uses the latest build if not specified)')
    upload_parser.add_argument('--variant', choices=DEPRECATED_VARIANT_CHOICES, help=variant_help)
    upload_parser.add_argument('--force-full', action='store_true', help='Force full firmware update (skip delta patching)')
    upload_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    build_upload_parser = subparsers.add_parser('build-upload', help='Build firmware and upload via BLE')
    build_upload_parser.add_argument('--variant', choices=DEPRECATED_VARIANT_CHOICES, help=variant_help)
    build_upload_parser.add_argument('--force-full', action='store_true', help='Force full firmware update (skip delta patching)')
    build_upload_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    # Data & Analysis Commands
    export_parser = subparsers.add_parser('export', help='Export grind data from device to database')
    export_parser.add_argument('--db', help='Specify database file (default: grinder_data.db)')
    export_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    analyze_parser = subparsers.add_parser('analyze', help='Export data and launch Streamlit report')
    analyze_parser.add_argument('--db', help='Specify database file (default: grinder_data.db)')
    analyze_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    report_parser = subparsers.add_parser('report', help='Launch Streamlit report (no data export)')
    report_parser.add_argument('--db', help='Specify database file (default: grinder_data.db)')
    
    analyze_offline_parser = subparsers.add_parser('analyze-offline', help='Alias for report - uses existing database')
    analyze_offline_parser.add_argument('--db', help='Specify database file (default: grinder_data.db)')
    
    # BLE Commands
    scan_parser = subparsers.add_parser('scan', help='Scan for BLE devices')
    
    connect_parser = subparsers.add_parser('connect', help='Connect to grinder device')
    connect_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    debug_parser = subparsers.add_parser('debug', help='Stream live debug logs from device')
    debug_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    
    info_parser = subparsers.add_parser('info', help='Get comprehensive device system information')
    info_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')

    wifi_info_parser = subparsers.add_parser('wifi-info', help='Show device info over WiFi (no BLE needed)')
    wifi_info_parser.add_argument('--host', help='IP address or name, e.g. grindbyweight.local (or GRINDER_WIFI_HOST)')
    wifi_info_parser.add_argument('--token', help='API token (or GRINDER_WIFI_TOKEN)')

    wifi_upload_parser = subparsers.add_parser('wifi-upload', help='Push a full firmware image over WiFi')
    wifi_upload_parser.add_argument('firmware', nargs='?', help='Path to firmware .bin file (uses the latest build if not specified)')
    wifi_upload_parser.add_argument('--host', help='IP address or name, e.g. grindbyweight.local (or GRINDER_WIFI_HOST)')
    wifi_upload_parser.add_argument('--token', help='API token (or GRINDER_WIFI_TOKEN)')
    wifi_upload_parser.add_argument('--expect-build', type=int, help='Build number the device must report afterwards')
    wifi_upload_parser.add_argument('--no-wait', action='store_true', help='Do not wait for the device to restart')

    board_parser = subparsers.add_parser('board', help='Set the hardware board override (v1, v2 or auto); auto always accepted; v1/v2 only while the board source is not probe or user')
    board_parser.add_argument('mode', choices=['v1', 'v2', 'auto'], help='Board to use after the next restart')
    board_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')

    wifi_set_parser = subparsers.add_parser('wifi-set', help='Store WiFi credentials + a new update token on the grinder over BLE (first setup, or while Menu -> WiFi is open)')
    wifi_set_parser.add_argument('ssid', help='Network name (1-32 bytes)')
    wifi_password_group = wifi_set_parser.add_mutually_exclusive_group()
    wifi_password_group.add_argument('--password-env', metavar='NAME', help='Read the password from this environment variable instead of prompting')
    wifi_password_group.add_argument('--open', action='store_true', help='Open network (no password)')
    wifi_set_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')

    wifi_clear_parser = subparsers.add_parser('wifi-clear', help='Remove the stored WiFi credentials over BLE (first setup state, or while Menu -> WiFi is open)')
    wifi_clear_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')

    diagnostics_parser = subparsers.add_parser('diagnostics', help='Get comprehensive diagnostic report for GitHub issues')
    diagnostics_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    diagnostics_parser.add_argument('--save', metavar='FILE', help='Save report to file (default: print to console)')

    # Development Commands
    install_parser = subparsers.add_parser('install', help='Manually install Python dependencies (auto-setup when needed)')
    monitor_parser = subparsers.add_parser('monitor', help='Monitor live debug output via BLE (alias for debug)')
    monitor_parser.add_argument('--device', default='GrindByWeight', help='Specify device name')
    clean_parser = subparsers.add_parser('clean', help='Clean build artifacts')
    release_parser = subparsers.add_parser('release', help='Create tagged release (triggers automated GitHub release)')
    
    return parser

async def main():
    """Main entry point."""
    parser = create_parser()
    args = parser.parse_args()
    
    tool = GrinderTool()
    
    try:
        # Map commands to methods
        if args.command == 'build':
            return tool.cmd_build(args)
        elif args.command == 'upload':
            return await tool.cmd_upload(args)
        elif args.command == 'build-upload':
            return await tool.cmd_build_upload(args)
        elif args.command == 'export':
            return await tool.cmd_export(args)
        elif args.command in ['analyze', 'analyse']:
            return await tool.cmd_analyze(args)
        elif args.command == 'report':
            return tool.cmd_report(args)
        elif args.command in ['analyze-offline', 'analyse-offline']:
            return tool.cmd_report(args)  # Same as report
        elif args.command == 'scan':
            return await tool.cmd_scan(args)
        elif args.command == 'connect':
            return await tool.cmd_connect(args)
        elif args.command in ['debug', 'monitor']:
            return await tool.cmd_debug(args)
        elif args.command == 'info':
            return await tool.cmd_info(args)
        elif args.command == 'wifi-info':
            return tool.cmd_wifi_info(args)
        elif args.command == 'wifi-upload':
            return tool.cmd_wifi_upload(args)
        elif args.command == 'board':
            return await tool.cmd_board(args)
        elif args.command == 'wifi-set':
            return await tool.cmd_wifi_set(args)
        elif args.command == 'wifi-clear':
            return await tool.cmd_wifi_clear(args)
        elif args.command == 'diagnostics':
            return await tool.cmd_diagnostics(args)
        elif args.command == 'install':
            return tool.cmd_install(args)
        elif args.command == 'clean':
            return tool.cmd_clean(args)
        elif args.command == 'release':
            return tool.cmd_release(args)
        else:
            tool.print_error(f"Unknown command: {args.command}")
            parser.print_help()
            return 1
            
    except KeyboardInterrupt:
        tool.print_info("Interrupted by user")
        return 1
    except Exception as e:
        tool.print_error(f"Unexpected error: {e}")
        return 1

if __name__ == "__main__":
    if platform.system() == "Windows":
        # On Windows, set the event loop policy to avoid issues
        asyncio.set_event_loop_policy(asyncio.WindowsProactorEventLoopPolicy())
    
    try:
        exit_code = asyncio.run(main())
        sys.exit(exit_code)
    except Exception as e:
        print(f"{COLORS['RED']}An unexpected error occurred: {e}{COLORS['RESET']}")
        sys.exit(1)
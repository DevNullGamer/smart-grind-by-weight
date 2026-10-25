# tools/custom_targets.py

Import("env")
import os

def upload_ble_action(source, target, env):
    """
    Upload the firmware built by this run over BLE using the Python uploader.

    The build itself is a dependency of the custom target (see below), so PlatformIO builds
    once and the pre-build script bumps the build number once. Returns non-zero on failure
    so the `pio run -t upload_ble` exit code reflects it.
    """
    print("--- Executing custom target: Upload over BLE ---")

    project_dir = env["PROJECT_DIR"]

    # Get the firmware path
    build_dir = env.subst("$BUILD_DIR")
    firmware_path = os.path.join(build_dir, "firmware.bin")

    if not os.path.exists(firmware_path):
        print(f"[ERROR] Firmware not found at {firmware_path}")
        return 1

    # Run the unified grinder tool for upload with the Python that runs PlatformIO
    print("--- Starting BLE upload ---")
    grinder_script = os.path.join(project_dir, "tools", "grinder.py")
    python_exe = env.subst("$PYTHONEXE")
    result = env.Execute(f'"{python_exe}" "{grinder_script}" upload "{firmware_path}"')
    if result != 0:
        print("[ERROR] BLE upload failed")
        return 1
    return 0

# Register custom targets
env.AddCustomTarget(
    name="upload_ble",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=upload_ble_action,
    title="Upload over BLE",
    description="Builds firmware and uploads over BLE"
)

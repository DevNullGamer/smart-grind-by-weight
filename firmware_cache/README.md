# Firmware Cache

This directory stores compiled firmware binaries during development.

**Note**: Firmware binaries are automatically excluded from git via `.gitignore` to prevent repository bloat.

## Usage

The `./tools/grinder` script automatically manages this directory:
- Built firmware is cached here for BLE OTA uploads
- Old builds are automatically cleaned up by build scripts
- Maximum cache size is limited to prevent disk space issues

Builds are cached per PlatformIO env as `firmware_cache/<env>/build_NNN.bin`, so V1 and V2
builds never mix (delta OTA patches are only made against a base of the same env).
Legacy flat `firmware_cache/build_NNN.bin` files are still used for V1 only.

## Manual Cleanup

To manually clear the firmware cache (per-env subfolders and legacy flat files):
```bash
rm -rf firmware_cache/*/ firmware_cache/*.bin
```
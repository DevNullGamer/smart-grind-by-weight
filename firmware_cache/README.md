# Firmware Cache

This directory stores compiled firmware binaries during development.

**Note**: Firmware binaries are automatically excluded from git via `.gitignore` to prevent repository bloat.

## Usage

The post-build script (`tools/build-scripts/post_build.py`) writes to this directory:
- Each successful build is copied here as the base for delta BLE OTA uploads
- Nothing is pruned automatically and there is no size limit, so old builds accumulate until you delete them (see Manual Cleanup)

Builds are cached per PlatformIO env as `firmware_cache/<env>/build_NNN.bin` (delta OTA
patches are made against a cached build of the same env). One firmware image serves both
V1 and V2 boards, so the production env is the only one needed for normal use.
For the production env the legacy flat `firmware_cache/build_NNN.bin` files and the old
`firmware_cache/waveshare-esp32s3-touch-amoled-164-v2/` folder are also searched; if they hold
different files for the same build number the base is ambiguous and a full update is sent.

## Manual Cleanup

To manually clear the firmware cache (per-env subfolders and legacy flat files):
```bash
rm -rf firmware_cache/*/ firmware_cache/*.bin
```
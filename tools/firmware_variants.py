"""
Firmware environment constants, shared by grinder.py and ble/grinder-ble.py.

One firmware image runs on both Waveshare 1.64" revisions (V1 and V2): the board is detected at runtime.
"""

FIRMWARE_ENV = "waveshare-esp32s3-touch-amoled-164"

# Env of the former separate V2 image; its firmware_cache dir may still hold delta bases of old V2 builds
LEGACY_V2_ENV = "waveshare-esp32s3-touch-amoled-164-v2"

# --variant is deprecated and ignored (accepted so existing scripts keep working)
DEPRECATED_VARIANT_CHOICES = ("v1", "v2")
VARIANT_DEPRECATED_MSG = (
    "--variant is deprecated and ignored: one firmware image supports both V1 and V2 boards "
    "(detected at runtime). Use 'board v1|v2|auto' to override the detected board."
)

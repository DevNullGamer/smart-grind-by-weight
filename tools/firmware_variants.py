"""
Hardware variant -> PlatformIO environment mapping, shared by grinder.py and ble/grinder-ble.py.

V1 and V2 firmware images are NOT interchangeable (flashing the wrong one gives a black screen).
"""

DEFAULT_VARIANT = "v1"

FIRMWARE_ENVS = {
    "v1": "waveshare-esp32s3-touch-amoled-164",
    "v2": "waveshare-esp32s3-touch-amoled-164-v2",
}

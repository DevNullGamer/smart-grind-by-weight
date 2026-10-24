# Development Guide

This guide is for developers who want to build the Smart Grind-by-Weight firmware from source, contribute to the project, or modify the code for their own use.

**End users:** If you just want to use the device, download pre-built firmware from [Releases](https://github.com/DevNullGamer/smart-grind-by-weight/releases) instead.

---

## 🛠️ Development Setup

### Prerequisites

- **Python 3.9+** with pip
- **Git** for version control
- **USB cable** for initial firmware flashing
- **Hardware** (ESP32-S3 board, HX711, load cell) for testing

### Initial Setup

1. **Clone the repository:**
   ```bash
   git clone https://github.com/DevNullGamer/smart-grind-by-weight.git
   cd smart-grind-by-weight
   ```

2. **Install development dependencies:**
   ```bash
   python3 tools/grinder.py install
   ```

This automatically creates a virtual environment (`tools/venv`) and installs all required dependencies including PlatformIO.

In the commands below, `<venv-python>` stands for the virtual environment's Python: `tools/venv/bin/python` on macOS/Linux, `tools\venv\Scripts\python.exe` on Windows. If you see `No module named platformio`, re-run `python3 tools/grinder.py install`.

---

## 🔧 Build Targets

The project has three build targets. All of them run on both the Waveshare 1.64" V1 and V2 boards: the board revision is detected at runtime (see [Board Detection](#board-detection)), so there is no per-revision target.

### Production Target: `waveshare-esp32s3-touch-amoled-164`
- **Use case:** Real hardware (V1 or V2 board) with load cell and grinder connected
- **Hardware:** Full ESP32-S3 + HX711 + load cell + grinder motor relay
- **Features:** All functionality enabled
- **Optimizations:** `-Ofast` optimization level for performance
- **Display:** SH8601 through Waveshare's native `esp_lcd` QSPI driver; board-specific pins come from the runtime board config

### Debug Target: `waveshare-esp32s3-touch-amoled-164-debug`
- **Use case:** Development and debugging with real hardware
- **Hardware:** Full ESP32-S3 + HX711 + load cell + grinder motor relay
- **Features:**
  - All functionality enabled
  - Debug symbols included
  - 2-second UI serial delay for easier debugging
  - Serial monitor filters to suppress harmless touch driver errors

### Mock/Development Target: `waveshare-esp32s3-touch-amoled-164-mock`
- **Use cases:**
  - Development without connected load cell or grinder
  - Testing with device installed in grinder without wasting beans or taxing the motor
- **Hardware:** Can run on just the ESP32-S3 Waveshare board (without HX711 or grinder) OR with full hardware installed
- **Features:**
  - Simulated load cell readings (green background indicates mock HX711 driver is active)
  - Mock grinder motor (visual indicator instead of relay activation)
  - Debug features enabled

**Mock mode benefits:**
- Develop UI changes without affecting the actual grinder
- Bring your waveshare board with you for coding and testing on the road :)
- Work on new features without hardware setup or bean waste
- Capture USB serial messages for debugging

---

## 🚀 Building & Flashing

### Development Platform

This project uses the **pioarduino ESP32 platform** (a community fork) instead of the standard Espressif ESP32 platform. This ensures proper support for the Waveshare ESP32-S3 AMOLED display.

**Platform Details:**
- **Platform**: [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) (stable release)
- **Framework**: Arduino ESP32 Core 3.x
- **Target**: ESP32-S3 with AMOLED touch display

The platform dependency is automatically handled by PlatformIO via the `platformio.ini` configuration.

### Build Commands

**Build production firmware:**
```bash
python3 tools/grinder.py build
```

**Build debug firmware:**
```bash
<venv-python> -m platformio run -e waveshare-esp32s3-touch-amoled-164-debug
```

**Build mock/development firmware:**
```bash
<venv-python> -m platformio run -e waveshare-esp32s3-touch-amoled-164-mock
```

**Clean build artifacts:**
```bash
python3 tools/grinder.py clean
```

### Initial USB Flashing

For the first-time setup or when BLE isn't working:

```bash
# Build and upload via USB (production)
python3 tools/grinder.py build
<venv-python> -m platformio run --target upload -e waveshare-esp32s3-touch-amoled-164

# Or for debug target
<venv-python> -m platformio run --target upload -e waveshare-esp32s3-touch-amoled-164-debug

# Or for mock target
<venv-python> -m platformio run --target upload -e waveshare-esp32s3-touch-amoled-164-mock
```

### BLE OTA Updates (After Initial Setup)

Once the device is running and connected to Bluetooth:

```bash
# Build and upload wirelessly (production)
python3 tools/grinder.py build-upload

# Upload the existing production build (.pio/build/waveshare-esp32s3-touch-amoled-164/firmware.bin)
python3 tools/grinder.py upload

# Upload specific firmware file
python3 tools/grinder.py upload path/to/smart-grind-by-weight-vX.X.X.bin

# Force full firmware update (skip delta patching)
python3 tools/grinder.py build-upload --force-full

# Scan for BLE devices
python3 tools/grinder.py scan

# Get device system info
python3 tools/grinder.py info
```

One image serves both board revisions. `--variant v1|v2` is deprecated: it is still accepted but ignored (a warning is printed).

A bare `upload` (no firmware path) always uploads the **production** env's build output, so run `build` (or `build-upload`) first; a debug or mock build is not picked up automatically, pass its `firmware.bin` path explicitly.

Delta updates use `firmware_cache/<env>/build_NNN.bin` as the base. For the production env, legacy caches (flat `firmware_cache/build_NNN.bin` and `firmware_cache/waveshare-esp32s3-touch-amoled-164-v2/`) are also searched; if they hold different files for the same build number, the base is ambiguous and a full update is sent instead.

---

## Board Detection

The firmware identifies the Waveshare 1.64" revision (V1 or V2) at boot; see [DESIGN-NOTES.md](DESIGN-NOTES.md) for the reasoning.

- **Detection** (`src/hardware/board_detect.cpp`, pure decision logic in `board_decide.cpp`): GPIO45 read with pull-up (low = V2, high = V1), V2 confirmed by counting panel TE edges on GPIO45 with the panel on CS=GPIO46, GPIO18 pull-down read as a hint (a conflicting level can veto to UNKNOWN/cached board). Conflicting signals give UNKNOWN (or the last known board for display only).
- **Motor safety:** the grinder output is enabled only when the board was established by probe or by the user. UNKNOWN and cached boards run with the motor disabled. GPIO18 is never driven before the board is known and never on V2.
- **Runtime config:** use `board()` from `src/config/board_config.h` instead of compile-time pin macros.
- **Override**, stored in NVS (`board_override`, applied after restart):
  - On the device: **Menu → Hardware board** (Auto / V1 / V2). When the board is UNKNOWN an on-screen picker appears at boot.
  - Over BLE: `python3 tools/grinder.py board auto|v1|v2`. `auto` is always accepted; `v1`/`v2` only while the board is not established by probe or user.
  - Restart the grinder to apply.
- **Warning:** forcing V1 on a real V2 board makes the firmware drive GPIO18 as the motor output, which is the V2 touch interrupt input (4.7k pull-up). Use the override only to correct a wrongly detected board.
- `python3 tools/grinder.py info` shows the board and how it was determined (`probe`, `user`, `cached`).
- Host unit tests for the decision logic live in `test_native`; run `pio test -c platformio-native.ini -e native-test`.

---

## 📦 Release Process

For maintainers creating releases, see **[RELEASES.md](RELEASES.md)** for detailed release workflow documentation.

---

## 🐛 Debugging

### Serial Monitor

```bash
# Monitor serial output via PlatformIO
<venv-python> -m platformio device monitor
```

Note: the `LOG_BLE(...)` macro in `src/config/logging.h` is currently just `Serial.printf`, so those messages appear only on the USB serial monitor, not in the BLE debug stream below. Messages in the BLE stream come from the BLE manager's own log path; `LOG_BLE_DEBUG` is serial output enabled by `ENABLE_BLE_DEBUG_VERBOSE`.

### BLE Debug Monitoring

```bash
# Live debug monitoring via BLE
python3 tools/grinder.py debug
```

**⚠️ BLE Monitoring Limitations:**
- **Boot messages are missed** - BLE connection establishes after device boot
- **Kernel panics not captured** - System-level crashes bypass BLE and go directly to serial
- **Framework messages missing** - Low-level Arduino/ESP-IDF messages don't route through BLE
- **Best for application debug** - Primarily receives debug messages from the smart-grind-by-weight firmware itself

For complete debugging (including boot sequence and system messages), use USB serial monitoring.

### Memory Layout (LVGL in PSRAM)

LVGL allocates widgets, styles and draw buffers from PSRAM so internal RAM stays available for BLE and FreeRTOS task stacks.

- `LV_USE_STDLIB_MALLOC` is set to `LV_STDLIB_CUSTOM` in `include/lv_conf.h`.
- The allocator lives in `src/ui/lvgl_psram_alloc.cpp`. It tries PSRAM first and falls back to internal RAM if PSRAM is full or unavailable.
- `lv_mem_monitor` is not supported by this allocator; use the boot log instead.
- At boot, `[HEAP]` lines report free internal RAM and the largest free block before UI init, after UI init and after BLE enable. If BLE fails to start or the device becomes unstable, check these first.

---

## 📚 Additional Documentation

- **[DOC.md](DOC.md)** - Complete build instructions and parts list
- **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)** - Common issues and solutions
- **[GRINDER_COMPATIBILITY.md](GRINDER_COMPATIBILITY.md)** - Adapting to different grinder models
- **[RELEASES.md](RELEASES.md)** - Release process and versioning

---

## Tests and CI

Run the same checks as CI locally (no hardware, BLE or PlatformIO build needed for the Python part):

```bash
python3 -m pip install -r tools/requirements-dev.txt   # pytest, ruff, colorama
python3 -m ruff check .                                # errors-only lint (ruff.toml)
python3 -m compileall -q tools
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest     # tools/tests (pytest.ini)
pio test -c platformio-native.ini -e native-test       # host tests in test_native/
```

On Windows use `python` and set the variable with `$env:PYTEST_DISABLE_PLUGIN_AUTOLOAD='1'` (PowerShell). Node tests under `tools/tests/*.test.js` (if present) run with `node --test tools/tests/*.test.js`.

`.github/workflows/ci.yml` runs these on pushes to `main`, pull requests and manual dispatch, and builds the production env `waveshare-esp32s3-touch-amoled-164` after the tests pass. `release.yml` runs the same tests (without the firmware build) before building a tag, so a failing test blocks the release.

`firmware_cache/<env>/` keeps only the newest 20 `build_NNN.bin` files (`FIRMWARE_CACHE_KEEP` in `tools/build-scripts/post_build.py`); a delta upload against a pruned build falls back to a full update. `tools/web-flasher/firmware/` is generated by `deploy-web-flasher.yml` and is not tracked.

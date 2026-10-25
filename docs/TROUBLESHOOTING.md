# Troubleshooting Guide

## Table of Contents

- [Motor Does Not Start](#motor-does-not-start)
- [Board Not Recognised / Display Stays Black (Waveshare 1.64)](#board-not-recognised--display-stays-black-waveshare-164)
- [HX711 Not Detected / Wrong Sample Rate](#hx711-not-detected--wrong-sample-rate)
- [Unknown board ID 'esp32-s3-devkitc-1'](#unknown-board-id-esp32-s3-devkitc-1)
- [PlatformIO Project Initialization Issues](#platformio-project-initialization-issues)
- [Grind Timeout Screen](#grind-timeout-screen)
- [Grind Stopped With an Err: Message](#grind-stopped-with-an-err-message)
- [Unreliable Pulse Corrections](#unreliable-pulse-corrections)
- [After Updating: Re-run Auto-Tune](#after-updating-re-run-auto-tune)
- [WiFi Updates and the Update Failed Screen](#wifi-updates-and-the-update-failed-screen)
- [Getting Diagnostic Reports](#getting-diagnostic-reports)

---

## Board Not Recognised / Display Stays Black (Waveshare 1.64)

**Applies to:** ESP32-S3-Touch-AMOLED-1.64 boards (V1 or V2). One firmware image supports both revisions and detects the board at boot, so there is no V1/V2 image to choose.

### Symptoms
- A "SELECT BOARD" dialog appears at boot (tap V1 or V2), or the diagnostics page shows "Hardware board not recognised" / "Grinder output is off".
- The grinder motor does not run even though everything else works (the motor is disabled until the board is confirmed).
- The display stays black after flashing, but the device still boots, connects over BLE or produces a diagnostic report.

### How detection works
At boot the firmware reads GPIO45 (held low on V2, floating on V1), confirms a V2 by watching the panel's TE signal, and checks GPIO18 as a hint (a conflicting level can veto to UNKNOWN/cached board). If the signals disagree it does not guess: the motor stays disabled and the on-screen picker is shown. While the board is unknown the display is driven on both V1 and V2 chip-select pins, so the picker is visible on either board. The last known board is remembered, but a remembered board alone never enables the motor.

A V1 board with GPIO18 pulled high by external wiring can look like a V2 hint and end up unrecognised; pick V1 in the picker.

### Resolution
1. If the picker is shown, tap the board you own (V1 or V2). The choice is saved and the grinder restarts.
2. Otherwise open **Menu → Hardware board** to see the detected board and how it was determined, or to set Auto / V1 / V2 manually. A change applies after a restart.
3. If the screen is black, connect over BLE and set the board from the command line:
   ```bash
   python3 tools/grinder.py board v1      # or v2, or auto
   ```
   `auto` is always accepted. `v1`/`v2` are accepted only while the board has not been confirmed by detection or by you (recovery path); once the board is confirmed, change it on the device under Menu → Hardware board. Restart the grinder afterwards.
4. If it stays black, test the board with Waveshare's [official V2 demo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.64-v2) and report the result together with the boot log.

> **Warning:** Forcing V1 on a real V2 board makes the firmware drive GPIO18 as the motor output. On V2, GPIO18 is the touch interrupt input (`TP_INT`) with a 4.7k pull-up, so the motor wire connected there fights the touch controller and can feed voltage into the grinder control input. Only force a board to correct a wrong detection.

### Wiring
The firmware drives both known wirings, so either works on either board. HX711 SCK is clocked on GPIO 1 and GPIO 2 together (DOUT on GPIO 3). The motor lead is driven on GPIO 16 once the board is confirmed, and additionally on GPIO 18 only on a confirmed V1. On V2 connect the motor lead to GPIO 16 and never to GPIO 18.

### Technical background
V2 uses the same SH8601 controller and `esp_lcd` driver as V1 but with display chip select on GPIO 46 (V1: GPIO 9). Earlier releases needed a separate image per revision. Design details are in [DESIGN-NOTES.md](DESIGN-NOTES.md).

---

## Motor Does Not Start

**Applies to:** Eureka Mignon installations where the grinder motor does not activate when commanded.

### Symptoms
- Motor test function (Menu → Motor Test) does not start the motor
- GRIND button does not start the motor
- All other functionality works normally (screen, weight readings, etc.)

**Note:** If you hear a click and humming sound but no coffee grounds come out, the motor **is** starting correctly - your grinder is clogged, not a wiring issue.

### Root Cause
The motor control wire (Pin 3) and button signal wire (Pin 2) from the Eureka 4-pin plug may be reversed on the Waveshare board connection. The button signal wire does not control the motor.

### Diagnosis

**Option 1: Use Motor Test Function (Recommended)**

Access **Menu → Motor Test** (Tools section) to verify motor connectivity. This button activates the motor for a short test pulse. If the motor does not run during the test, the motor wire is incorrectly connected.

**Option 2: Manual Wire Identification (Advanced - Use with Extreme Caution)**

⚠️ **DANGER:** This test involves live wires and can cause the grinder to start unexpectedly, potentially causing injury or damage. Only attempt if comfortable working with electrical systems.

Using the 4-pin Eureka plug pinout (see `../media/4-pin_Eureka_plug_pinout.png` in documentation):
- **Pin 1**: 5V power supply
- **Pin 2**: Button signal (unused in this project)
- **Pin 3**: Motor control signal *(active-high — motor engages when driven to 3.3-5V)*
- **Pin 4**: Ground

The motor control wire (Pin 3) is the wire that **starts the motor when briefly driven HIGH (touch it to Pin 1 / 5V)**. Test carefully:
1. Ensure grinder is powered and hopper has minimal beans
2. Disconnect the suspect wire from the Waveshare board
3. Briefly touch the wire to 5V (Pin 1) - motor should start
4. **Be prepared for the powerful motor to cause the grinder to twist/jump**
5. **Avoid shorts that could damage the board or cause injury**

### Resolution

**Swap the wire connections:**
- The motor control wire (Pin 3) should connect to Waveshare **GPIO 16** (works on both boards; **GPIO 18** also works on V1 only)
- The button signal wire (Pin 2) can remain disconnected (unused in this project)

If wires were reversed, swap them so Pin 3 connects to the correct GPIO. On V2, never use GPIO 18 for motor control because it is shared with `TP_INT`. If the motor never runs and the diagnostics page says the grinder output is off, the board is not confirmed: see [Board Not Recognised](#board-not-recognised--display-stays-black-waveshare-164). The Waveshare board has reverse polarity protection for power connections, but the motor/button wires should be correctly identified for proper operation.

**Important:** Wire colors vary significantly between Eureka units - always refer to pin positions rather than wire colors when troubleshooting. On the physically verified V2 Specialita, Pin 3 was the grey lead and Pin 2 was white, contrary to the earlier assumed assignment; the white Pin 2 lead was left disconnected and insulated.

---

## HX711 Not Detected / Wrong Sample Rate

**Applies to:** First-boot issues when the load cell board is missing, miswired, or strapped for 80 SPS.

### Symptoms
- Startup log shows `HX711_NOT_CONNECTED` or `HX711_SAMPLE_RATE_INVALID`.
- Weight display stays at `0.00 g`; grinding and calibration remain disabled.

### Quick Fixes
- **NOT_CONNECTED:** Verify VCC/GND/SCK/DOUT wiring and that the HX711 board is powered.
- **SAMPLE_RATE_INVALID:** Ensure the HX711 `RATE` pin is tied to GND for 10 SPS; a floating/high pin forces 80 SPS and will now block startup.
- After correcting hardware, reboot the scale. The diagnostic clears automatically when healthy samples are detected.

### Warning: "HX711 sensor sends no data"
- Raised when the HX711 is detected but delivers no samples at boot, or when a load cell that was working has been silent for 2 seconds.
- Check the load cell cable and connectors (a cable that came loose is the usual cause), VCC/GND/SCK/DOUT wiring and the HX711 board, then restart the scale.
- The warning clears by itself when samples return. It can also show briefly during a firmware update, because sampling is paused while the update runs; it goes away after the update finishes or fails.
- Tapping the grind button with a load cell fault from boot (not connected, no data, rate invalid) or during a firmware update shows a "CAN'T START" notice with the cause and does not start the motor; OK dismisses it. Start on Cup is held back in the same cases. (Decision logic is host-tested; the screen is not hardware-verified.)
- A grind that starts while the sensor is silent stops with `Err: sensor` (see below).
- Limitation: an HX711 that is unplugged but keeps returning a constant value is not recognised as "no data". Such a grind normally ends with `Err: no flow`.

## Unknown board ID 'esp32-s3-devkitc-1'

**Applies to:** Windows development environments using PlatformIO with ESP32-S3 boards.

### Symptoms
- `platformio run` fails with `UnknownBoard: Unknown board ID 'esp32-s3-devkitc-1'`.
- `pio boards espressif32 | findstr "esp32-s3"` shows the board, suggesting it exists in the registry.

### Root Cause
Most likely an old or partially installed platform package in the local PlatformIO cache that predates ESP32-S3 board definitions (this error was originally seen with an old `espressif32` 3.x package). The registry lookup reports current boards, but the outdated local package lacks the corresponding JSON definition. This project does not use the stock `espressif32` platform: `platformio.ini` pins the pioarduino fork by URL.

### Resolution
1. Make sure you build with the project's own `platformio.ini` (do not override `platform` with the stock `platformio/espressif32` package), and use a current PlatformIO Core (the project venv created by `python3 tools/grinder.py install` has one).
2. Remove the cached platform packages so PlatformIO re-downloads the pinned pioarduino platform on the next build: delete `~/.platformio/platforms/` (or `%USERPROFILE%\.platformio\platforms\` on Windows). See [PlatformIO Project Initialization Issues](#platformio-project-initialization-issues).
3. Re-run the build:
   ```bash
   python3 tools/grinder.py build
   ```

### Notes
- The warning `Ignore unknown configuration option 'monitor_options'` also disappears once PlatformIO Core is current.
- If multiple PlatformIO installations are present, ensure you are using the instance (the project venv) that builds the project.

---

## PlatformIO Project Initialization Issues

**Applies to:** General PlatformIO project setup issues, especially when using PlatformIO (as this project uses the pioarduino fork as a platform).

### Symptoms
- `Error: Could not find one of 'package.json' manifest files in the package`
- PlatformIO fails to initialize the project properly
- Vague platform-related errors during project setup

### Root Cause
The use of the pioarduino platform fork can sometimes cause PlatformIO's cache or project state to become inconsistent, leading to initialization failures.

### Resolution
1. **Close VS Code completely**
2. **Delete the PlatformIO cache folder:**
   ```bash
   # On Windows
   rmdir /s "%USERPROFILE%\.platformio"
   
   # On macOS/Linux  
   rm -rf ~/.platformio
   ```
3. **Restart VS Code**
4. **Reopen the project** - PlatformIO will reinitialize and download the correct platform packages
5. **Perform a clean build** - Use PlatformIO's "Clean" then "Build" to ensure a fresh compilation

### Alternative (Less Nuclear)
If you want to try a less aggressive approach first:
1. Close VS Code
2. Delete only the platforms cache: `~/.platformio/platforms/` (or `%USERPROFILE%\.platformio\platforms\` on Windows)
3. Restart VS Code and reopen project
4. Perform a clean build in PlatformIO

**Note:** This issue is specific to the pioarduino platform fork usage and the way PlatformIO handles custom platform URLs.

---

## Grind Timeout Screen

**Applies to:** Grinder timing out during operation, showing timeout screen after 60 seconds (`GRIND_TIMEOUT_SEC`).

### Symptoms
- Grinder reaches timeout screen during grinding cycle
- If the screen shows an `Err: ...` text, see [Grind Stopped With an Err: Message](#grind-stopped-with-an-err-message). In weight mode a scale that does not settle after the tare now continues after 10 seconds instead of waiting for the 60 second timeout (not yet run on hardware).
- Long taring process (>10 seconds)
- Unstable weight readings

### Root Cause
Extended taring due to load cell noise prevents grinding from completing within the 60-second limit.

### Diagnosis
Check **Menu → Diagnostics → "Noise Floor"** section (see [Diagnostics System](DOC.md#diagnostics-system) for details). If "Noise level: Too High" (red text) appears persistently, your load cell has sustained noise issues that will cause slow taring (>2 seconds). A warning icon (⚠) will appear in the top-right corner when sustained noise is detected.

### Resolution
1. **Check load cell wiring:**
   - Verify shielding wire connection per wiring diagram
   - Shorten load cell cables if possible
   - Ensure clean, solid connections

2. **Check calibration factor (reference examples only):**

   Calibration factors vary between individual load cells, but extreme deviations may indicate hardware issues. Check your calibration factor in **Menu → Diagnostics → Load Cell Status**. Example values from tested units:
   - **1KG T70 load cell**: ±4400 (example)
   - **0.3KG Mavin Load Cell**: ±6580 (example)

   **Note:** These are examples only and can differ significantly between load cells. Use them as rough reference points, not expected values.

3. **If wiring issues persist:**
   - Increase `GRIND_SCALE_SETTLING_TOLERANCE_G` parameter for more noise tolerance

---

## Grind Stopped With an Err: Message

**Applies to:** Grinds that the firmware stops on its own and shows on the timeout screen (the motor is always stopped first). `Err: sensor`, `Err: tare` and `Err: no flow` are weight mode only; `Err: motor` also applies in time mode. These checks have not yet been run on hardware with a load cell.

| Message | Meaning | What to do |
|---|---|---|
| `Err: sensor` | The load cell stopped delivering samples (newest sample older than 0.5 s) during a grind. | Check the load cell cable and connectors, then restart the scale. Look for the "HX711 sensor sends no data" warning in Menu -> Diagnostics. |
| `Err: tare` | The scale delivered samples too slowly to finish the tare within 5 s after the tare started (the tare needs 18 samples). Cause inferred from the code. | Check the load cell cable, the HX711 board and its RATE pin, then restart. |
| `Err: no flow` | Almost no coffee arrived: during the purge/prime run less than half of the purge amount, or during the main grind less than 0.3 g in the first 4 seconds. | Check that there are beans in the hopper, the grinder is switched on and powered, the cup stands on the scale and the chute is not blocked. If the load cell cable is loose or disconnected this can show up as `Err: no flow` too. |
| `Err: motor` | The motor output was cut off by the hardware safety timer (typically a run over 62 seconds, or the output could not be stopped normally). A "Motor output was cut off" warning stays until the next restart. | Check the grinder and restart the scale. If it repeats, report it with a diagnostic report. |

Notes:
- Weight mode only: after the tare, the scale continues after 10 seconds even if it has not settled, and the final and pulse settling steps continue after 4 seconds with the current reading. The purge settling step keeps its older 10 second limit. Time mode still has no such fallback. Untested on hardware.
- The reason and measured values are printed as `[SAFETY]` lines on the USB serial monitor (best effort, not in the session log).
- Maintainers: `GRIND_SAFETY_ABORTS_ENABLED` in `src/config/grind_control.h` (default 1) turns the sensor, tare and no-flow aborts into log-only lines if one proves to be a false positive.

---

## Unreliable Pulse Corrections

**Applies to:** Pulse corrections failing to produce grounds consistently, requiring multiple correction cycles.

### Symptoms
- Multiple pulse attempts needed to reach target weight
- Inconsistent grounds production during pulse phase
- Overshooting or undershooting target weight frequently

### Root Cause
Motor response latency mismatch between firmware settings and actual hardware characteristics. Default 50ms may not match your specific grinder's relay type (solid-state vs mechanical), voltage (110V vs 220V), or burr inertia.

### Resolution

**Recommended:** Use the Auto-Tune Motor Response feature to automatically calibrate optimal pulse duration for your hardware:

1. **Access auto-tune**: Menu → Tune Pulses (Tools section)
2. **Prepare system**:
   - Ensure beans are in hopper
   - Place dosing cup on scale
   - System will automatically tare
3. **Run calibration**: Process takes 1-2 minutes
   - Priming phase: 1000ms pulse to position beans (exactly 1000 ms since the exact-pulse fix; it used to be about 1.4 s)
   - Binary search: Finds minimum reliable pulse duration
   - Verification: Confirms 80%+ success rate across 5 test pulses
4. **Check result**: New motor latency value displayed on completion
   - Search range: 30-300ms depending on hardware
   - Value saved automatically to device preferences
   - Displayed in **Menu → Diagnostics**

**Verify calibration**: Check **Menu → Diagnostics → Motor Latency** to see current value. Re-run auto-tune if you change grinders, modify relay hardware, or continue experiencing unreliable pulse corrections.

**Manual fallback:** If auto-tune fails, system reverts to safe 50ms default. Check grinder power connection, hopper bean level, and scale setup.

---

## After Updating: Re-run Auto-Tune

**Applies to:** Grinders that ran an auto-tune with an earlier firmware and now run firmware with exact motor pulses.

Earlier firmware made pulses longer than about 65 ms come out wrong: a requested 100 ms pulse ran about 69 ms and a requested 1000 ms pulse about 1443 ms. Pulses are now exact (the time-mode 100 ms pulse is a true 100 ms, the auto-tune and Motor Test 1000 ms pulse is 1000 ms). The motor latency saved by an earlier auto-tune was measured with the old pulse lengths and is therefore off.

**What to do:** after installing this firmware, run **Menu -> Tune Pulses** once (beans in the hopper, cup on the scale). Weight-mode correction pulses over about 65 ms were also mis-timed before. Until then the saved latency is kept and grinds still work, but pulse corrections may be less accurate. Time mode: expect the 100 ms additional pulse to deliver somewhat more than before, since it now really lasts 100 ms.

---

## WiFi Updates and the Update Failed Screen

The WiFi features have been tested with host tests and on one development board; Bluetooth provisioning and the on-screen pages are **not hardware-verified**. If something here does not match what you see, please report it.

### The grinder does not connect to WiFi

Open **Menu → WiFi** and read **Status**.

- **Not set up**: no credentials stored. Use the web flasher **WiFi Setup** tab or `python3 tools/grinder.py wifi-set "<ssid>"` (Bluetooth must be on).
- **Connecting... / Retry in Ns**: the grinder keeps trying with growing pauses (5 s up to 60 s; at least 30 s after a rejected password). Check the password (8-63 characters, or empty for an open network) and re-send it with the page open.
- **2.4 GHz only.** The ESP32-S3 cannot join 5 GHz networks. If your router uses one name for both bands, create a separate 2.4 GHz name, or turn band steering off. Enterprise (802.1X) networks are not supported.
- **Weak signal**: the page shows `Signal`. Below about -75 dBm (Weak) connections drop and uploads crawl (10-14 KB/s measured at -94 dBm, about 4 minutes for an update). Move the router or add an access point.
- To change the network, open **Menu → WiFi** first, otherwise the grinder refuses new details over Bluetooth.

### `wifi-upload` / `wifi-info` cannot reach the grinder

- `cannot resolve host` / `cannot reach`: use the IP shown in **Menu → WiFi** (`--host 192.168.x.y`). `.local` names need mDNS support on your computer.
- Turn **Receive** on in **Menu → WiFi**; the grinder only listens while it is connected and Receive is on.
- The computer must be on the same network (no guest network or client isolation).
- **Connection reset / closed**: the grinder usually refused the request and closed the connection before the tool could read the reason. Run `python3 tools/grinder.py wifi-info` and look at the grinder screen; the causes in the table below are the usual ones.
- **Windows firewall (developers using the pull test server `tools/wifi/pull_test_server.py`):** in our testing, with the network profile set to **Public**, the Windows firewall blocked incoming connections to Python and the grinder got no answer from the test server. Switching the network to the Private profile or adding an inbound rule for the server port is the likely remedy. This does not affect `wifi-upload` (the grinder is the server there).

### Refusals from the grinder (HTTP codes)

| Code | Meaning | What to do |
|---|---|---|
| 400 | The request was malformed: bad JSON or fields, Content-Length different from the announced size (411 if missing), or the upload ended early ("connection lost") | Use the project tools; retry. A truncated upload also aborts the update. |
| 401 | Wrong token | Use the token from the last `wifi-set` / web flasher setup. Three wrong tokens lock logins for 60 s, doubling up to 15 min. |
| 403 `declined` | You tapped DECLINE ("Declined on the device") | Run the command again. |
| 403 `session` | The upload session is unknown or expired | Start again with a new `wifi-upload`. |
| 408 `no_response` | Nobody tapped INSTALL within 30 s | Run again and be at the grinder. |
| 409 "Update refused: grinder busy" | The grinder is grinding or otherwise busy | Wait until it is idle. |
| 409 "Go to the home screen of the grinder" | The grinder is on any screen other than the home screen or the menu (for example a dialog, calibration, or another update screen) | Go to the home screen and retry. |
| 409 "Disconnect Bluetooth first" | A Bluetooth client is connected | Disconnect it. |
| 409 "Update refused: another update running" | A push, a download from the menu or a Bluetooth update is running | Wait for it to finish. |
| 409 "Update cancelled on the device" | The request was withdrawn on the grinder while the prompt was waiting (Receive switched off, Forget WiFi, or WiFi dropped) | Check the WiFi page and retry. |
| 409 "Firmware is still being verified" | The last update has not run for 30 seconds yet (rollback protection) | Wait one minute after the restart and try again. |
| 410 `session` | The update was aborted ("timed out", no data for 15 s) and the session no longer exists | Start again; check signal and that the computer does not sleep. |
| 413 / 422 | Image or request too big / not a valid image for this grinder, or checksum mismatch | Use a firmware `.bin` built for this project (not a `-web-ota` or merged file). |
| 429 `declined_recently` | You declined a moment ago; new requests wait 30 s | Wait the time shown. |
| 429 (locked) | Too many wrong tokens | Wait; see 401. |

While a prompt waits for your tap the grinder answers nothing else over WiFi (`wifi-info` times out); that is expected.

### The Update Failed screen / rollback

The screen says "The firmware update failed. The device is still running the previous version." It appears when the version running after a restart is not the one that was just installed. Typical reasons:

- The new firmware crashed or reset within its first **30 seconds**, so the bootloader switched back to the old firmware. A power cut in that window does the same.
- The firmware shown after the restart differs from the one that was installed for another reason (version or build mismatch).

Tap OK. Try the update again after one minute. If a freshly built image rolls back every time, it has a start-up bug: connect USB and read the serial log (`[BOOT] image state: PENDING_VERIFY` followed by a crash). Updates started from the menu show `Failed: <reason>` on the progress screen for 3 seconds (for example Checksum mism..., Connection lost, Not found, Low memory). The on-screen reason is cut to 16 characters; the full text appears under the buttons on the WiFi page. An aborted transfer never shows the Update Failed screen; it shows `Failed: <reason>` or a tool error and the old firmware keeps running.

### Menu update: Check failed / Install is missing

- **Check failed**: the grinder cannot reach `devnullgamer.github.io` over HTTPS (no internet, DNS, or low memory). Try again, or check the router.
- **No release published**: the update index has no unified (V1 + V2 auto-detect) release for this channel yet. Switch **Beta** on to include release candidates.
- **Install** appears only after a successful Check in the last 10 minutes, while WiFi is connected. Switching Beta clears the result.
- Install is refused while a Bluetooth client is connected (disconnect it first) and when internal memory is too low. A refusal before the download starts leaves Bluetooth as it was; if the refusal comes after Bluetooth was already switched off, the grinder switches it back on.
- The pull flow does not check certificate expiry (the grinder has no clock).

---

## Getting Diagnostic Reports

**Applies to:** Reporting issues on GitHub or troubleshooting system behavior.

### What is a Diagnostic Report?

The diagnostic report is a comprehensive text dump of your device's current state, including:
- Firmware version, build number, and git commit
- System information (uptime, CPU frequency, RAM, flash storage)
- All compile-time constants (grind settings, thresholds, timeouts)
- Runtime statistics (lifetime grinds, total weight)
- Load cell calibration status and noise diagnostics
- Motor latency settings
- Autotune results (if available)

This information helps identify configuration issues, hardware problems, or firmware bugs.

### How to Get a Diagnostic Report

#### Option 1: Web Flasher (Recommended)

1. Visit the **[Web Flasher Diagnostics Tool](https://devnullgamer.github.io/smart-grind-by-weight)**
2. Click the **"Diagnostics"** tab
3. Click **"Connect & Get Diagnostics"**
4. Select your device from the Bluetooth pairing dialog
5. Wait for the report to complete (~5-10 seconds)
6. Click **"Copy to Clipboard"** or **"Download Report"**
7. Paste the report into your GitHub issue

**Browser Requirements:** Chrome, Edge, or Opera on desktop/Android (Web Bluetooth API required)

#### Option 2: Command Line Tool

If you have the development environment set up:

```bash
# Get diagnostics and display in terminal
python3 tools/grinder.py diagnostics

# Save diagnostics to a file
python3 tools/grinder.py diagnostics --save diagnostics.txt
```

### When to Include a Diagnostic Report

**Always include a diagnostic report when reporting:**
- Grinding accuracy issues (overshooting/undershooting)
- Timeout errors or slow taring
- Load cell calibration problems
- Motor response issues
- Any unexpected behavior or crashes
- Feature requests that depend on your hardware configuration

**The diagnostic report is anonymous** and contains no personal information. It only includes technical device settings and statistics.

### Including Grind Results in Your Report

**For grind-related issues, follow these steps before generating the diagnostic report:**

1. **Enable logging:** Menu → Logs & Data → turn on the **Logging** toggle
2. **Reproduce the issue:** Perform at least one grind where the issue occurs
3. **Generate diagnostics:** Use one of the methods above (Web Flasher or Command Line)

The diagnostic report will automatically include your recent grind session data, which is essential for troubleshooting accuracy issues, pulse corrections, timeout problems, and other grind behavior issues. Without recent grind data, it's much harder to diagnose what's going wrong.

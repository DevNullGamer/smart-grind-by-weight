# Design Notes

Design reasoning and measurements behind larger changes. The binding rules are summarised in the project `CLAUDE.md`; this file explains why they exist. One section per step.

## Single firmware for Waveshare 1.64" V1 and V2 (runtime board detection)

Before this change V1 and V2 boards needed separate firmware images, and the wrong image gave a black screen (so nothing could even tell the user what was wrong). One image now supports both and works out the board at boot.

### Findings that made one image possible

- Waveshare's V1 and V2 ("V1.1") demos use the same `esp_lcd_sh8601` driver, init table, X offset (20 px) and 40 MHz QSPI clock. The only proven display difference is the chip-select pin: GPIO9 on V1, GPIO46 on V2. Hardware spike confirmed: colour bars render correctly on CS9 (V1) and CS46 (V2) through the same driver, so the Arduino_GFX CO5300 path was dropped.
- Both boards have the same MCU, flash, PSRAM and I2C devices (FT3168 touch at 0x38, QMI8658 IMU at 0x6B), so none of those can identify the board.
- HX711 SCK and motor wiring (V1: GPIO2 / GPIO18, V2: GPIO1 / GPIO16) are a project wiring convention, not something the board dictates.

### Board detection

Implemented in `src/hardware/board_detect.cpp` (pin reads, NVS, panel probe) and `src/hardware/board_decide.cpp` (pure `decide_variant()`, unit-tested on the host in `test_native`).

Signals, in order:

1. **GPIO45 with internal pull-up** (primary). V2 holds GPIO45 low (the panel TE line, held low by the board, measured); V1 leaves it floating so it reads high. 0 means V2 candidate, 1 means V1. GPIO45 was chosen over GPIO18 because GPIO45 is not a user wire.
2. **V2 confirmation**: the panel is brought up on CS=GPIO46 and TE edges on GPIO45 are counted for 100 ms (`HW_BOARD_TE_WINDOW_MS`). A V2 panel produces about 60 Hz TE (~12 edges per window); V1 produces none. At least `BOARD_TE_MIN_EDGES` (6) edges are required.
3. **GPIO18 with internal pull-down** (hint; a conflicting level can veto to UNKNOWN/cached board). V2 holds it high (touch interrupt with 4.7k pull-up); a bare V1 reads low. It is never the primary signal because on V1 it is the user's motor wire and may legitimately be pulled high.

Contradictory or missing evidence gives the cached value (`board_last` in NVS) for display only, or UNKNOWN when there is no cache. A user override (`board_override` in NVS: 0 auto, 1 V1, 2 V2) skips probing completely. A successful PROBE result is written to `board_last`.

Why not the obvious approaches:

- A bit-banged CO5300/SH8601 ID read (command 03h, register DA/DB/DC, 16 timing variants, both CS pins) got no answer on either board. Dropped.
- Reading I2C addresses, chip ID, flash or PSRAM: identical on both boards.

Evidence base: one V1 and one V2 board. A second V2 board would raise confidence.

### Safety rules for the motor

GPIO18 is the motor output on a V1 wiring but a touch interrupt input (with pull-up) on V2. Driving it on a V2 fights the touch controller and can leak voltage into the grinder input. Hence:

- At the top of `setup()`, `board_park_pins()` drives GPIO16 low and puts GPIO18 into input with pull-down. GPIO18 is never an output before the board is known.
- The motor is enabled only when the board was established by **PROBE or USER**. UNKNOWN and CACHED boards get `motor_pin = -1`, `Grinder::init(-1)` leaves the grinder uninitialised and the UI shows a motor-disabled notice. An unknown board must never default to the V1 motor pin.
- `board_motor_pin_allowed()` is an allow-list check: it rejects UNKNOWN boards, GPIO18 on V2, and display, touch, IMU, HX711, strapping, USB, UART, flash and PSRAM pins.
- Forcing V1 on a real V2 (user override) would drive GPIO18 against the touch interrupt. This is why the BLE override of V1/V2 is only accepted while the board is not established (see picker/override below) and why the docs warn about it.

### Dual wiring (HX711 and motor)

Both known wirings are active at once so the user does not have to tell the firmware which one they built:

- HX711 clock is pulsed on GPIO1 and GPIO2 together (`loadcell_sck_pin` plus `loadcell_sck_pin_alt`, mask in `hx711_clock_mask.h`); DOUT is GPIO3 on both. The unused pin is simply an unconnected output.
- Motor: GPIO16 always (when the board is established), plus GPIO18 only when the variant is V1 and was established by PROBE or USER. Never on V2 or UNKNOWN.

### Display design

One backend: Waveshare's `esp_lcd` SH8601 driver (`src/hardware/esp_lcd_sh8601.c`) driven by `DisplayManager`. Board-specific values (CS pin, X offset, QSPI clock) come from `board()`.

Render path, as built:

- LVGL renders partially into a **40-row buffer in PSRAM** (`HW_DISPLAY_DRAW_BUF_ROWS_PSRAM`, 22.4 KB, about 12 strips per full redraw).
- Each strip is copied in chunks through **two internal DMA staging buffers** (`HW_DISPLAY_STAGING_ROWS` = 8 rows each, 4.5 KB each) and sent while the next chunk is copied. A semaphore signalled from the `on_color_trans_done` callback paces the transfers.
- Fallback when the PSRAM or staging allocation fails: a small internal DMA draw buffer (8 rows, 57 strips per redraw) sent directly. Staging rows halve down to `HW_DISPLAY_STAGING_MIN_ROWS` (4) on allocation failure.

Why PSRAM-direct failed: the first attempt handed the PSRAM LVGL buffer straight to `esp_lcd_panel_draw_bitmap()`. On hardware this logged `spi transmit (queue) color failed`, because esp_lcd queues the colour data for DMA and a queued transfer from PSRAM does not work with this SPI configuration, so the display stopped updating. Hence the extra copy through internal DMA staging buffers. (Hardware note: the SPI bus `max_transfer_sz` is sized for one chunk of the larger of staging and internal draw rows, 8 rows = 4480 B, not a full frame, to save internal RAM.)

Measurements on V1 (UIRender task, per 10 s): released rc.2 (Arduino_GFX) about 477 cycles, 20 ms average; build #44 with this design about 302-306 cycles, 32-33 ms average; build #70 about 310 cycles, 32 ms. Staging with 4 rows was slower (166-213 cycles, 46 ms), so 8 rows is used. The user judged the feel much better than rc.2 regardless. Possible follow-up: second PSRAM render buffer to overlap rendering and transfer.

Re-init safety: `release_panel()` must run before `init_panel()` again (used by the board probe and degraded mode).

### UI/BLE reporting and override

- Menu → **Hardware board** page: shows `V1/V2 (source)` and an Auto / V1 / V2 radio group. The choice is stored in NVS (`board_override`) and applies after a restart.
- BLE debug command `0x03` + `0|1|2` (`BLE_DEBUG_CMD_SET_BOARD`, wrapped by `python3 tools/grinder.py board v1|v2|auto`). `auto` is always accepted (it cannot enable the motor). `v1`/`v2` are accepted only while the board is **not** established by PROBE or USER, as a recovery path; once established the override can only be changed on the device.
- The system-info JSON reports `board` and `board_src`. Diagnostics codes `BOARD_UNRECOGNIZED` and `MOTOR_DISABLED` raise the warning icon.

### On-screen board picker (degraded display)

When the board is UNKNOWN the panel is driven on **both** chip selects (CS9 and CS46): every strip is sent twice, and only the panel that is really wired answers. This works because the staged flush path sends from one buffer. On top of that the UI shows a V1 / V2 picker. The choice is stored as a USER override and the device restarts after `HW_BOARD_PICKER_RESTART_DELAY_MS` (1.5 s); if saving fails the picker reappears after 2 s. Mirroring needs the staging buffers; without them the mirror panel is disabled.

A wrong pick on the real board is recoverable through BLE (`board auto`) or the Hardware board menu page.

A V1 user with GPIO18 pulled high by external wiring reads as a conflict (GPIO45 high, GPIO18 high), ends up UNKNOWN and gets the picker; this is intentional rather than guessing.

### Single image, build and release

- One PlatformIO env builds the production image (`waveshare-esp32s3-touch-amoled-164`); debug and mock envs derive from it. The `-v2` env and `HW_DISPLAY_VARIANT_V2` style compile-time macros are gone.
- `release.yml` builds and publishes one image under the base artifact names. `deploy-web-flasher.yml` keeps the separate V1/V2 entries for old tags and labels new releases "V1 + V2 (auto-detect)".
- `tools/grinder.py --variant` is accepted but deprecated and ignored (prints a warning). Delta OTA bases are looked up in `firmware_cache/<env>/build_NNN.bin`; for the main env the legacy flat cache and the old `-v2` cache dir are also searched, and if the candidates differ the base is ambiguous and a full update is sent.
- Merged flash images must use `--flash-mode dio` (bootloader is DIO for the octal-PSRAM build).
- Bare `grinder.py upload` uploads `.pio/build/waveshare-esp32s3-touch-amoled-164/firmware.bin`, so it needs a build of the main env; a debug or mock build is not picked up automatically (pass the path explicitly).

### LVGL allocator and heap

LVGL allocates from PSRAM (`src/ui/lvgl_psram_alloc.cpp`, falling back to internal RAM) so internal RAM is left for BLE and task stacks. Internal heap is tight: about 6-7 KB free after boot with the display staging buffers on builds #44-#70. Boot logs `[HEAP]` lines before UI init, after UI init and after BLE enable, plus `[STARTUP] Free internal heap after hardware init`. Re-measure before adding features that need internal RAM, including the UNKNOWN (mirror) path, which holds an extra panel handle.

### Known loose ends

- Hardware validation matrix on both boards (cold-boot visuals, forced UNKNOWN, OTA migration from old V1/V2 images) is tracked outside the repo.
- `LICENSE` still lists Arduino_GFX although the library is no longer used.

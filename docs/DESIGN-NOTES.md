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

### BLE soft disable

- **Why:** repeated `BLEDevice::deinit()`/`init()` leaked ~11 KB of internal heap per cycle and crashed the V2 board after 8-12 cycles (stack canary panic in the 1 KB `ipc0` task, which the BT controller uses for interrupt allocation; heap exhaustion at the same cadence). Moving the caller to another core does not help because the work always runs on ipc0.
- **What:** `BluetoothManager::disable()` no longer deinitialises BLE. It aborts OTA/data export, stops advertising, disconnects the client and marks BLE disabled; the stack stays resident (~79 KB internal RAM) and `handle()` keeps advertising stopped. The first `enable()` after boot does the full init; later calls only restart advertising on the existing server and services (characteristic pointers stay valid). Connections and writes arriving while disabled are refused.
- **Flags and envs:** `DEBUG_BLE_HARD_DEINIT=1` restores the old teardown for comparison. `DEBUG_BLE_ENABLE_STRESS=1` runs 150 disable/enable cycles from a Core 1 task and 150 from a Core 0 task (`[BLESTRESS]` log lines, panic-surviving state in RTC memory). Envs: `waveshare-esp32s3-touch-amoled-164-ble-stress` (soft) and `...-ble-stress-harddeinit` (baseline).
- **Measured (V2):** 300 cycles, 0 crashes, free internal heap flat at 126.5-128.9 KB.
- **WiFi spike:** its "BLE off" phases only free RAM when built with `-DDEBUG_BLE_HARD_DEINIT=1`.
- **Residual:** the first init at boot still goes through ipc0 (has worked in practice); the soft re-enable path is not yet tested with a real client.

### WiFi manager (chg008 pass 1)

- `src/network/wifi_manager.*` connects as a station from NVS namespace `wifi` (`ssid`, `pass`, `token`, `push_enabled`, `name`). `begin()` runs last in `setup()` and only reads NVS; all radio work (WiFi, mDNS) runs in `tick()` on the Bluetooth task (stack raised to 8 KB), with backoff 5 s doubling to 60 s (at least 30 s after an authentication failure). No credentials means the radio is never started.
- SSID, password and API token are stored in **plaintext** NVS (no flash/NVS encryption). Logs never contain them; the `[WIFI]` serial lines show state, IP, RSSI and disconnect reason only.
- BLE provisioning (debug RX): `[0x04][ssid_len][ssid][pass_len][pass][token16]` and `[0x05]`. The PC generates the 16-char lowercase hex token. Accepted only while no credentials are stored or `WiFiManager::provisioning_window_open()` (the WiFi menu page, see below); so changing credentials later is only possible through that menu window. Pure parsing/validation lives in `wifi_credentials.*` (host tests).
- Dev builds: env `...-wifi-dev` (`DEBUG_WIFI_DEV_CREDS=1`) seeds credentials from the gitignored `include/spike_secrets.h` when NVS is empty and arms `push_enabled`; the API token comes from optional `SPIKE_API_TOKEN`, else the fixed public dev token in `wifi_manager.cpp` (a loud `[WIFI] WARNING` boot line appears while that fallback is stored). **Erase NVS after using the wifi-dev env** (`pio run -t erase`, or the BLE clear command `0x05` from a normal build): it seeds a public dev token and `push_enabled=true` that survive reflashing with a normal image.

### WiFi push update (chg008 pass 2-3)

- **Pieces:** `system/image_header` (header check), `system/update_sink_image` (ImageSink: `esp_ota_*` with sequential writes, SHA-256), `system/update_guard` (copy of the BLE OTA safety rules: busy check, lock_out, re-check, watchdog, task suspend, touch; release resumes tasks first), `network/update_http_logic` (pure: bearer parse, lockout, begin-JSON parser, session id) and `network/update_server` (esp_http_server on port 80). The BLE `OTAHandler` is untouched except for two checks: it refuses while `UpdateGuard::is_held()`, and the guard asks `is_updating_synchronized()` (takes the OTA op mutex) after claiming its slot, which closes the start race.
- **Serialisation:** one mutex in `UpdateServer` around every sink/guard call; handlers lock per chunk, never across a socket read; the tick only try-locks (inactivity abort after `UPDATE_INACTIVITY_TIMEOUT_MS`).
- **Gate:** the server runs only while WiFi is connected and `push_enabled` is set (checked every 2 s from the Bluetooth task); mDNS `_http._tcp` follows it. The Accept prompt hook is `UpdateAcceptPolicy` (see the R2a section: production builds fail closed until the R2b UI exists).
- **Endpoints:** `GET /api/info`; `POST /api/update/begin` (Bearer; `{"size","sha256"[,"build","version"]}` -> `{"session"}`); `PUT /api/update/data` (Bearer + `X-Session`, Content-Length == size) -> `{"status":"installed"}`, restart 800 ms later. Three bad tokens lock authentication for 60 s (doubling to 15 min, reset on success). Errors are `{"error","reason"}`; 409 busy, 413 size, 422 invalid image/SHA, 429 locked.
- **Tool:** `tools/wifi/grinder-wifi.py` (stdlib only), also `grinder.py wifi-info` / `wifi-upload`. Host and token from arguments, `GRINDER_WIFI_HOST`/`GRINDER_WIFI_TOKEN` or `~/.grinder-wifi.json`.
- **Former gap, fixed in R2a:** a push now stores `new_build_nr` / `new_fw_ver` like the BLE path (see the Accept prompt section).

- **No NVS while updating:** `tick()` skips all NVS reads (the push_enabled gate) while a session, the installed state or the guard is active. The data PUT authenticates against a RAM copy of the token taken at `begin` (zeroized when the session ends); only `begin` reads NVS, before the update takes the flash.
- **Early error replies:** every error reply except 401/429 is followed by a lingering close (discard up to 64 KB / 3 s of the request body, 1 s socket timeout) so the client receives the status instead of a TCP reset. The tool prints a hint when it still sees a reset.
- **Lockout:** a lock counts as expired when the unsigned distance to its end is 0 or larger than the longest lock (no re-arming after a millis() wrap).
- **Single httpd task limitations (accepted for now):** (1) one client trickling a header or body holds the only HTTP task until the 10 s receive timeout, so a LAN client can starve other requests; (2) the authentication lockout is global, so anyone on the LAN can lock the real user out with bad tokens (60 s, doubling to 15 min); (3) `/api/info` is not answered while a PUT is being received, so the tool must not poll it during an upload.
- **Measured throughput:** at -94 dBm (weak bench signal, `WIFI_PS_MIN_MODEM`) a push runs at 10-14 KB/s, about 4 minutes for the 2.2 MB image. Acceptable for a bootstrap; the 10 s receive timeout is the margin to watch on worse links.
- **Lingering close is a DoS lever:** any LAN client can keep the single HTTP task busy for about 3-4 s per lingering error reply (64 KB cap; the clock is checked between 1 s receives); 401 and 429 replies do not linger, but the pre-auth 403 (PUT without session) and 409 (busy) replies do, since they must reach the client. A PUT without a session is answered 403 before any token check (no lockout count), and a second `begin` while an update runs is answered 409 before authenticating (no NVS read).
- **Token staleness:** the RAM token copy is taken at `begin` and wiped when the session ends (success, error or timeout). If the token is re-provisioned mid-session, the running session keeps using the old one until it ends; the next `begin` reads NVS again.
- **Open items:** the Accept prompt, progress screen and the Receive/Confirm toggles exist (R2a/R2b, later sections); credentials/token provisioning tools exist since R3 (`grinder.py wifi-set` / `wifi-clear`, web flasher "WiFi Setup" tab, see the R3 section) but are host-tested only.

### WiFi menu page (chg008 R1)

- **Where:** Menu -> Settings -> "WiFi" (between Bluetooth and Display). Code is split out of the big files: `ui/screens/menu_screen_wifi.cpp` (page, window, refresh) and `ui/controllers/menu_controller_wifi.cpp` (Forget handler, 1 Hz refresh). Event `WIFI_FORGET`. No arm/Receive toggle yet (that comes with the Accept prompt, so token-only push is never reachable from the UI).
- **Content:** Status (Connected / Connecting... / Retry in Ns / Not set up, coloured), Network (shortened SSID), IP, Signal (`-62 dBm Good`), Name (`<host>.local`). "Not set up" block (shown without credentials) explains how to provision and warns when Bluetooth is off; "manage" block (with credentials) holds **Forget WiFi**, which opens a confirm dialog and then calls `WiFiManager::clear_credentials()` (SSID, password, token and push flag).
- **Pure text helpers:** `network/wifi_status_text.*` (host-tested): state text, RSSI quality (Excellent >= -55, Good >= -67, Fair >= -75, else Weak), SSID shortening (printable ASCII only, a run of other bytes becomes one `?`, max 20 chars with `...`). `WiFiState` moved there from `wifi_manager.h` so the helpers have no Arduino dependency.
- **Provisioning window:** `MenuScreen::sync_wifi_provisioning_window()` makes the window equal "menu visible AND WiFi page is the current page". It is called from the menu's page-change callback, from `show()` and from `hide()`, so every way of leaving the page or the MENU state closes it, and a menu shown again on the WiFi page reopens it. Serial log: `[UI] WiFi page entered/left (provisioning window open/closed)`.
- **Window security note:** the window stays open for as long as the WiFi page is on screen, including while the screen is dimmed (the page does not time out). During that time any BLE client in range can replace or clear the credentials through the unauthenticated debug write path (BLE link unencrypted). Accepted for now; leave the page when done.
- **Stale cache:** `WiFiManager::credentials_revision()` is incremented by `set_credentials()` and `clear_credentials()`; the 1 Hz refresh reloads the SSID/flag when it differs from the revision loaded last, so BLE provisioning or clearing while the page is open updates it. `clear_credentials()` returns false (nothing changed) if NVS cannot be opened; Forget then does not claim success.
- **NVS access:** the stored SSID and credentials flag are read on page entry (and after Forget, because the dialog hides the menu and `show()` re-syncs). The 1 Hz refresh only reads `WiFiManager` getters (state, IP, RSSI, retry time); labels are only touched when their text changed.
- **Not covered by tests:** the LVGL layout (280 px; Network, Signal and Name are stacked rows) needs a visual check on the device; "BLE write accepted only with the page open" needs a BLE client.

### WiFi Accept prompt, core part (chg008 R2a)

R2a is everything except LVGL. Without a UI controller (before R2b) **production builds fail closed**: the prompt cannot be answered, so every `begin` is refused with 409 `busy` ("Go to the home screen of the grinder"). Push stays disarmed by default.

- **Rendezvous:** `network/update_approval.*` is a pure state machine (IDLE / PENDING / ACCEPTED / DECLINED / EXPIRED / CANCELLED / UI_BUSY, injected time, wrap-safe, host-tested). `UpdateApprovalGate` (`update_approval_gate.*`, global `update_approval`) wraps it in a spinlock; the HTTP task files the request and polls every `UPDATE_PROMPT_POLL_MS` (50 ms) until it is no longer PENDING (the 30 s timeout expires it, so the wait always ends); the UI task will poll `pending_prompt()` and answer with `resolve()` / `resolve_busy()`. No LVGL call ever happens on the HTTP task. A late answer cannot overwrite an expiry or a cancel. Only a decline starts the 30 s back-off (`UPDATE_DECLINE_BACKOFF_MS`); expiry and busy do not.
- **Fail closed:** `UpdateApprovalGate::attach_ui(true)` is what the R2b controller will call. While no UI is attached the production policy answers `UI_BUSY` immediately (no wait, never accept).
- **Policy:** `UpdateAcceptPolicy::decide(request, peer_ip)` returns an `AcceptDecision`; the production implementation is `network/update_accept_policy.cpp`. Order in `handle_begin`: busy-before-auth check (guard held or an approval in use) -> bearer auth -> body parse -> grinder busy 409 (before bothering the user; the guard re-checks later) -> policy. Policy order: BLE client connected or data export running -> 409 "Disconnect Bluetooth first" (also with Confirm off) -> Confirm off (NVS `accept`=false) -> accept without prompt -> no UI -> UI_BUSY -> request (BACKOFF -> 429) -> wait. Replies come from the pure `accept_decision_reply()` (403 declined, 408 no_response, 409 busy, 429 declined_recently with `Retry-After` and `retry_after_s`).
- **Confirm setting:** `WiFiManager::accept_required()` / `set_accept_required()` (NVS `wifi/accept`, default true). It is served from a RAM copy loaded in `begin()` so `/api/info` ("confirm") and the policy never touch NVS while an update owns the flash. Turning it off only takes effect if the write succeeded; turning it on applies at once. `clear_credentials()` removes the key (back to the safe default). The start-up warning is logged only when Confirm is off.
- **Server tick:** a pending or just-answered approval counts as "in use": when the gate (WiFi up and push armed) disappears the tick withdraws the prompt (`cancel()`) and stops the server on a later round, never under a waiting handler (`httpd_stop` would wait for it). A second `begin` while an approval is in use is answered 409 before authentication. The single HTTP task is blocked for the whole wait, so `/api/info` and a second `begin` are only served after the first one was answered (documented limitation).
- **Prompt text:** `network/update_prompt_text.*` (host-tested): `prompt_sanitize` (printable ASCII only, every other byte `?`, max 32 characters), `prompt_format_size` (decimal units, `812 KB`, `2 MB`, `2.2 MB`) and `prompt_build_message` (4 lines, parts the client did not send are omitted, always fits `UPDATE_PROMPT_MESSAGE_BUF`). The peer address comes from `getpeername` on the request socket (IPv4 only, empty otherwise).
- **Progress:** `network/update_status.*` (`UpdateStatus` namespace, atomics plus a short reason string): AWAITING_APPROVAL / RECEIVING / VERIFYING / INSTALLED / FAILED, written by `UpdateServer` on every path (chunks, verify, install, sink/guard failures, the inactivity abort); `active()` is for the R2b `OtaDataExportController` guard. A FAILED status stays until the UI calls `acknowledge()` or the next update starts.
- **Expected-version marker:** `system/ota_marker.*` writes `new_build_nr` / `new_fw_ver` (namespace `grinder`, the BLE keys) with its own short-lived `Preferences` handle only after `guard_.acquire()` succeeded (so a concurrent BLE OTA's marker is never overwritten; the BLE path also writes after suspending the tasks) and before `sink_.begin()`; it is removed after `guard_.release()` on every later failure path (sink refusal, stream errors, verify failure, inactivity abort). Guard refusals never touch it. The BLE-connected check is repeated after the user accepted (a client may have connected meanwhile). After success the keys stay, so `check_ota_failure_after_boot()` reports a rolled-back or wrong-build update.
- **BLE exclusion:** `BluetoothManager::onConnect` disconnects a new client and `start_data_export()` refuses while `UpdateGuard::is_held()`; a connected BLE client makes `begin` answer 409 (the push does not disable BLE).
- **Dev knob:** `DEBUG_UPDATE_PROMPT_AUTOREPLY` (1 accept, 2 decline, 3 no reply; default 0) lets the HTTP side play the user; it is set to 1 only in the `...-wifi-dev` env and `#error`s without `DEBUG_WIFI_DEV_CREDS`. Other values are chosen with `-DDEBUG_UPDATE_PROMPT_AUTOREPLY=n` on top of that env.
- **Info disclosure:** `/api/info` is unauthenticated and now also reports `"confirm"` (whether Confirm is on), which tells a LAN client whether a tap is needed. Accepted; name, version, build and board were already public.
- **Tool:** `grinder-wifi.py upload` waits up to `PROMPT_WAIT_S` (45 s) for `begin`, prints "Confirm the update on the grinder screen (30 s)..." when `/api/info` says `confirm`, and explains 403/408/409/429 in plain words. The default firmware search now only considers the main env, `-debug` and `-wifi-dev` (not mock, selftest, spike, stress or deadman builds).
- **Not covered by tests:** the cross-thread wait on real hardware beyond the AUTOREPLY runs, the BLE-connected refusals (no BLE client from the dev PC), grind-in-progress refusal (no sensor), and everything LVGL (R2b).

### WiFi Accept prompt, UI part (chg008 R2b)

R2b is the LVGL side of R2a; with it the production build is no longer fail-closed (the controller calls `update_approval.attach_ui(true)` on its first frame) and push can be armed from the menu.

- **Controller:** `ui/controllers/wifi_update_controller.*` (`WifiUpdateUIController`, polled from `UIManager::update()` before `OtaDataExportController`; it returns true, which skips the rest of the frame, while the progress screen is up). No LVGL call happens on the HTTP task: the controller polls `update_approval.pending_prompt(&prompt, &generation)` (the generation is read under the same lock, so a replaced request is told apart from the one the dialog shows).
- **Accept dialog:** the existing confirm screen ("WIFI UPDATE", INSTALL in the accent colour, DECLINE), text from `prompt_build_message`. Allowed only while the UI state is READY or MENU, no blocking overlay is up and no update is active; otherwise `resolve_busy()` (HTTP 409 "Go to the home screen"). When the request expires or is cancelled while the dialog is up, `ConfirmUIController::dismiss()` closes it without running either callback; if some other screen took over the dialog (state no longer CONFIRM) the request is withdrawn. The decision table is the pure `update_prompt_action()` in `network/update_ui_logic.*` (host-tested).
- **Wake:** `ScreenTimeoutController::hold_awake(bool)` makes `update()` restore normal brightness and skip dimming. The controller holds while a request is pending, while the dialog is up and while the progress screen is shown, and releases it as soon as none of the three applies (so every resolution releases it). After the release, dimming resumes from the last touch time, so a prompt that expires on an unattended grinder dims again at once.
- **Progress screen:** the existing `ota_screen` (`show_ota_mode()`, title "WiFi update", arc = received/total), statuses "Receiving update...", "Verifying...", "Installed. Restarting...", "Failed: <reason>" (reason cut to `UPDATE_UI_REASON_MAX` = 16 characters, printable ASCII; plain "Update failed" without a reason). A failure stays `UPDATE_UI_FAILED_HOLD_MS` (3 s), then `UpdateStatus::acknowledge()` (so FAILED does not persist) and the UI returns to MENU if it was there, otherwise READY. `OtaDataExportController::update()` no longer treats the OTA screen as an aborted Bluetooth update while `UpdateStatus::active()` or FAILED is set (belt and braces: the WiFi controller consumes the frame first). While the progress screen is shown a new begin is answered 409 (state is not READY/MENU).
- **WiFi page "Updates" section:** Receive toggle (`WIFI_PUSH_TOGGLE`: `set_push_enabled`, and `approval_cancel` when switched off), Confirm toggle (`WIFI_CONFIRM_TOGGLE`: ON is immediate through `set_accept_required(true)`; OFF first shows the "TURN OFF CONFIRM" warning dialog and only the TURN OFF button stores it; the page re-syncs from the stored state when the menu returns, so CANCEL leaves it on), and a status line "Listening:" / "<name>.local" shown while `update_server.running()`. Without credentials both switches are greyed out (Confirm too, so an unattended Confirm-off cannot survive into a later provisioning; Forget also resets it). Toggle states are read on page entry (NVS for Receive, the RAM copy for Confirm), not per refresh. Forget additionally calls `update_approval.cancel()`.
- **Taps are bound to the request:** the dialog callbacks carry the request generation and `UpdateApprovalGate::resolve(accept, generation)` refuses a mismatch; `ConfirmUIController::guard_taps()` ignores taps (dialog stays, no callback) for `UPDATE_UI_TAP_GUARD_MS` (400 ms) after the dialog opens. Failure line: `Failed: <reason>` with the reason cut to 16 characters (longest text `Failed: ` + 16 = 24 characters). Listening line is two rows (`Listening:` / `<name>.local`). With Confirm off an update can preempt any UI state (EDIT, CALIBRATION, ...), by design; the progress screen then returns to MENU or READY.
- **Audit nit fixed:** in `UpdateServer::handle_begin` the expected-version marker is now cleared unconditionally in the `sink_.begin()` failure branch, after `guard_.release()`.
- **Known limits:** the single confirm slot means a Confirm-off warning dialog or any other dialog makes an incoming request fail with 409; the Confirm-off dialog is not protected against a request arriving while it is open (it gets 409, as designed).
- **Not covered by tests:** all LVGL behaviour (layout, dialog, dim wake, toggles) needs a look at the device; the pure decisions and texts are in `test_update_ui_logic.cpp`.

### WiFi provisioning tools (chg008 R3)

**Status: host-tested only. Nothing in this section has run against a real BLE link** (the dev PC has no working BLE adapter). Every BLE step below is "NOT RUN on hardware".

- **Firmware unchanged.** Only `test_native/test_wifi_credentials.cpp` gained `test_parse_python_builder_frames`: five literal frames produced by the Python builder (basic, open network, UTF-8 SSID and password, 32 B SSID + 63 B password = 114 B, spaces/punctuation) that `wifi_parse_provisioning` must accept with the expected fields.
- **One protocol, three implementations:** `tools/wifi_provision.py` (Python: frame builder, validation, reply classifier, hardware-info parser, config writer) and `tools/web-flasher/wifi-setup.js` (JS twin) must stay byte-identical. The shared list is `tools/tests/wifi_provision_vectors.json` (checked by `tools/tests/test_wifi_provision.py` and `node --test tools/tests/wifi-setup.test.js`, also each side's reply-text table); the C++ literals above are copied from it, so changing the protocol means regenerating the JSON and the C++ literals together. Lengths are UTF-8 byte counts, password empty or 8-63, NUL rejected, token `[0-9a-f]{16}`.
- **`grinder.py wifi-set <ssid>` / `wifi-clear`** run `tools/ble/grinder-ble.py` (bleak). `wifi-set` validates SSID and password BEFORE scanning, prompts for the password with `getpass` (or `--password-env NAME`, or `--open`), generates the token with `secrets.token_hex(8)`, enables the debug stream (0x01), writes the frame with response, waits up to 5 s for the reply line, disables the stream (0x02). Only on a "stored" reply it writes the token (and the host `grindbyweight.local` only if the file has none, so a custom host is kept) through `grinder-wifi.py`'s 0600 writer into `~/.grinder-wifi.json` (other keys kept), so `wifi-upload` works with no manual config. Then it polls the BLE hardware-info characteristic (`wifi`, `ip`) for up to 40 s and reports the IP or a "not connected yet" warning. On "locked" (credentials exist, WiFi page closed) it says "open Menu -> WiFi"; on no reply it does NOT touch the config file. The token is printed masked, the password never.
- **Web flasher "WiFi Setup" tab** (`index.html` + `wifi-setup.js`): same flow with Web Bluetooth (`requestDevice` filter name `GrindByWeight`, optional services OTA/debug/sysinfo), token from `crypto.getRandomValues`, Copy and "Download config" (JSON identical to what the Python writer produces), "New token" button, "Remove WiFi from grinder". The module has no console output, no browser storage and no network access (asserted by the node test). It verifies through BLE (hardware info) because the HTTPS page cannot call the plain-HTTP device.
- **Hardware-info read, not notify:** the hardware characteristic is only refreshed every 10 s while a client is connected and its notification is cut at the ATT MTU, so both tools poll `read` (long read) every 3 s and ignore values that do not parse (stale or empty before the first refresh).
- **Known weak spots for the first hardware run:**
  1. The reply text arrives as debug-stream notifications. At the default ATT MTU of 23 a notification carries 20 bytes, so a message like `BLE_DEBUG: WiFi command rejected, credentials already stored...` would be cut and neither tool could classify it ("No reply" is shown, nothing is saved). Bleak on Windows/Linux and Chrome on Android normally negotiate a bigger MTU (firmware requests 517); if a client does not, the replies need to be shortened or sent in chunks (firmware change, not done here).
  2. The 114-byte set frame is a GATT long write when the MTU is 23 (new path; the older commands are 2 bytes).
  3. `grinder.py` pipes the child's output, so the password prompt relies on `getpass` talking to the terminal directly (POSIX `/dev/tty`, Windows console); stdout of the tool is buffered until exit like the other BLE commands.
  4. Chrome Android only exposes Web Bluetooth over HTTPS or localhost.
- **Security:** the BLE link is unencrypted, so a sniffer in radio range sees SSID, password and token while no credentials exist or the WiFi page is open (documented in the tab text and README). The token is generated on the PC/browser and never read back from the device.
- **Manual checklist (phone or second adapter):** first-time set; set while credentials exist and the page is closed (must be refused with the "open Menu -> WiFi" message and the config file unchanged); set with Menu -> WiFi open; clear; wrong password (status stays Connecting/Retry on the grinder, tool says "not connected yet"); open network; 32-byte SSID with a non-ASCII character; `wifi-upload` with the stored token afterwards; Chrome Android and desktop Chrome for the web tab.

### Boot-health rollback (chg008 R4)

- **Rule:** an image installed by an update (WiFi or BLE, both end in `esp_ota_set_boot_partition`) is PENDING_VERIFY on its first boot. `system/boot_health.*` overrides the Arduino core's weak `verifyRollbackLater()` (C linkage, returns true) so `initArduino()` no longer confirms it at once. `boot_health_tick()` (called from `loop()`) calls `esp_ota_mark_app_valid_cancel_rollback()` when `setup()` has finished and `millis() >= BOOT_HEALTH_CONFIRM_MS` (30 s). A reset or crash before that makes the bootloader mark the image ABORTED and boot the previous one (factory slot if there is no other valid app). Criterion deliberately tiny: no WiFi/BLE/sensor health, so a healthy image cannot fail to confirm. Pure decisions in `boot_health_logic.*` (host tests `test_boot_health.cpp`).
- **Images without a pending state** (USB-flashed, erased otadata, factory slot, already confirmed, state unreadable) are a silent no-op: `[BOOT] image state: UNDEFINED/VALID/...` is logged and nothing else happens. Logs: `[BOOT] image state: PENDING_VERIFY (partition ota_1)`, later `[BOOT] image confirmed after N ms`.
- **Updates are refused while pending.** `esp_ota_begin()` fails with `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` on an unconfirmed image, so WiFi `begin` answers 409 "Firmware is still being verified, try again in a minute" and the BLE `start_ota` refuses with the same reason. Consequence: a second update needs the device to have run 30 s.
- **Marker lifetime:** `check_ota_failure_after_boot()` used to delete `new_build_nr` / `new_fw_ver` on a passed check at the first boot; the new image would thus consume the marker before crashing and the rolled-back image would never report the failure. Now a passed check keeps the marker while `boot_health_pending()`; `boot_health_tick()` clears it (`ota_marker_clear()`) right after the confirmation. After a rollback the previous image finds expected != current and shows the existing OTA_UPDATE_FAILED screen. A failed check still removes it at once. Caveat (pre-existing): if a version string is stored it is compared INSTEAD of the build, and many builds share a version, so the WiFi tool sends the build number only.
- **Fatal start-up failure:** if `task_manager.init()` fails, `boot_health_rollback_now()` calls `esp_ota_mark_app_invalid_rollback_and_reboot()` (no-op unless pending; returns if there is nothing to roll back to, then the old halt loop runs).
- **Expected behaviour:** a power cycle within 30 s of an update rolls back (the user sees "update failed"). The FIRST update from a firmware without R4 is not protected (the old firmware confirms the new image at once); protection starts with the next update. A hang in `setup()` (no reset) stays unconfirmed until the next reset/power cycle, which then rolls back.
- **WiFi tool fix:** `grinder-wifi.py upload` sent no build number when the image was not found in `firmware_cache/<main env>`, so no marker was stored for wifi-dev or test images. `find_cache_build` now searches every env directory of `firmware_cache` (main env first), and a timeout hint says "previous build is back, probably rolled back". Build only is sent, never a version. The server side (`http_parse_begin_request`, `ota_marker_store`) already accepted build-only (new host test with the tool's exact body).
- **Test env:** `...-boot-fail-test` (`DEBUG_BOOT_FAIL_TEST=1`, `-DDEBUG_BOOT_FAIL_TEST=2` for `esp_restart()`) crashes `DEBUG_BOOT_FAIL_DELAY_MS` (8 s) after boot, ONLY while the image is pending, so a USB-flashed copy never crash-loops. Not built by release.yml.
- **Confirmation failure:** if `esp_ota_mark_app_valid_cancel_rollback()` fails, `boot_health_tick()` logs it, keeps the image pending and the marker in place (a reset still rolls back and reports) and retries every 5 s.
- **Tool note:** after "Device is back" the WiFi tool reminds that the image is only confirmed after ~30 s of stable running (a crash before that rolls back); without a known build number it says a rollback cannot be detected.
- **Not covered:** the BLE path end to end (no BLE from the dev PC; reasoned from the shared bootloader mechanism), the bootloader's rollback itself (needs COM15).

### Known loose ends

- Hardware validation matrix on both boards (cold-boot visuals, forced UNKNOWN, OTA migration from old V1/V2 images) is tracked outside the repo.
- `LICENSE` still lists Arduino_GFX although the library is no longer used.

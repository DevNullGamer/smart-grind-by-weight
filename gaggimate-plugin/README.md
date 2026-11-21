# GLink grinder driver for GaggiMate (chg025, chg026)

A driver and a small plugin that make a Smart Grind By Weight grinder a grinder in GaggiMate's BluetoothDevices framework
(`lib/BluetoothDevices`, branch `feature/bluetooth-devices-grinder`, pinned at commit `2e64abd`, full SHA
`2e64abd78f77df01b8f623bf5ffc2542f34d3275`). GaggiMate's own `BluetoothDevicesPlugin` does the scan, the saved device, the
connect on sight, the disconnect at standby, Forget and the Bluetooth tab. The driver speaks GLink v1
(`docs/GLINK-PROTOCOL.md`) to the grinder, which is the BLE peripheral. What it does:

- pushes the dose of the profile selected in GaggiMate to the grinder's Profile Dose, with its grind size and rpm when the
  profile has them (a profile with only a dose is pushed too), compare-first, and selects that profile on the grinder's
  screen after a stored change. A size or rpm that the profile no longer has is cleared on the grinder. The rpm (GLink 1.2,
  display only) is sent only to a grinder that advertises `protoMinor` 2 or more;
- reports a finished grind, so that its weight temporarily becomes the dose of the selected GaggiMate profile;
- reports the grinder's state and the dose, size and rpm of the profile selected on the grinder.

Start and stop are not part of the driver (the framework has no interface for them yet); live data is parsed and held, not shown.

This folder is a prototype kept in the grinder repository. It is to be contributed upstream by the user under GaggiMate's
terms; it carries no licence or CLA text. The base is an unmerged branch of GaggiMate: the driver is pinned by SHA and
four small patches are carried (table below). The earlier own plugin (own scan, card and WebSocket requests, chg024,
pinned at b564759) is retired; it lives only in the grinder repository's history and in the snapshot branch
`keep/own-glink-plugin-chg024`.

**Status: built and host-tested; bench round 2 (2026-11-21) ran the real build with no bench variant. The link, the handshake,
the dose push and the select work on the bench; grind-dependent checks and the grinder-side link tests are still open.
chg026 (rpm, dose-only push, clearing) is host-tested only; its bench items are open.** See "Verified" and "Not verified".

## Layout

```
gaggimate-plugin/
  README.md
  docs/GLINK-PROTOCOL.md                 copy of the protocol (a test keeps it equal to ../docs/GLINK-PROTOCOL.md)
  src/display/plugins/grinderlink/
    config/bluetooth.h                   byte-identical copy of the firmware's src/config/bluetooth.h (constants only)
    bluetooth/glink_frame.{h,cpp}        shared with the firmware: fragmenter, reassembler, bad-fragment window
    bluetooth/glink_json.{h,cpp}         shared with the firmware: bounded JSON reader and writer
    bluetooth/glink_disconnect.{h,cpp}   shared with the firmware: names the HCI disconnect reason for the log
    GLinkMessages.{h,cpp}                pure: request builders; parsers for responses and broadcasts
    GLinkClient.{h,cpp}                  pure: session (handshake, one request at a time, timeouts, tracking)
    GLinkDevState.{h,cpp}                pure: devState text to IDLE / GRINDING / BUSY / UNKNOWN; when profile writes are accepted
    GLinkSettingsSync.{h,cpp}            pure: when the Profile Dose update (and the select after it) goes out
    GLinkResultGate.{h,cpp}              pure: per-link baseline; reports a settled grind result once
    GLinkDoseSave.{h,cpp}                pure: finished weight to profile dose: range, once, hold and expiry
    GLinkGrinderCore.{h,cpp}             pure: the session orchestrator (order, one request in flight, result path)
    RxRing.h                             pure: byte ring for notification packets
    GLinkGrinder.{h,cpp}                 device glue: the BluetoothGrinder driver (NimBLE); empty under GAGGIMATE_SIM
    GLinkPlugin.{h,cpp}                  device glue: registers the driver, applies a finished grind as the dose
  test/test_grinderlink/                 host tests (Unity style) against the shared vectors
  test/test_grinderlink/glink_sources.cpp  compiles the pure sources into one unit for `pio test -e native` only
  test/host/unity.h                      tiny Unity stand-in for the g++ run
  tools/tests/glink_vectors.json         copy of the shared conformance vectors
  patches/                               unified diffs against GaggiMate 2e64abd (four, below)
  scripts/run_host_tests.sh              builds and runs the host tests with g++ (globs the test and pure sources)
  scripts/check_patches.sh               read-only `git apply --check` of the patches against a checkout
  scripts/check_against_gaggimate.sh     clone 2e64abd into a NEW directory, apply, stage, `pio test`, `pio run`
```

Shared files (must stay byte-identical with the firmware; `tools/tests/test_gaggimate_plugin_contract.py` compares them,
line endings aside): `bluetooth.h`, `glink_frame.h`, `glink_frame.cpp`, `glink_json.h`, `glink_json.cpp`,
`GLINK-PROTOCOL.md`, `glink_vectors.json`, `glink_vectors.h`, `glink_disconnect.h`, `glink_disconnect.cpp`. They keep the
firmware's names so the includes inside them stay valid. The transport carries no grinder-specific object. The driver does
not use the firmware's `glink_messages.*` (that is the device side: request parser and response encoders).

The pure files have no Arduino, NimBLE, heap or `std::`; the clock is injected (milliseconds, wrap-safe). A file that is
under `GAGGIMATE_SIM` is empty in the simulator build.

## The driver (`GLinkGrinder`)

- **Registration.** `GLinkPlugin::setup()` installs the busy probe and calls `GLinkGrinder::registerDriver()`, which registers
  the driver id `plugin-glink-grinder` with the public `BluetoothDeviceRegistry`. `handles()` matches an advertised name that
  starts with `GrindByWeight` (the grinder advertises no service UUID). The framework creates one instance per connect
  attempt.
- **Connect** (loop task, blocking). It refuses before any radio use when GaggiMate is busy (a brew or grind runs, or the
  system is not ready; no probe counts as busy), when NimBLE already holds `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` clients
  (a `static_assert` requires at least 3), or within 10 s of a drop the driver itself caused (protocol error, ring
  overflow, failed handshake). GaggiMate's plugin then rescans and retries when it sees the grinder. After that it allocates
  the 8 KB notification ring and the session core with `new (std::nothrow)` (freed in `disconnect()` and the destructor),
  connects, finds the GLink service, subscribes, reads the MTU (packet size `min(MTU - 3, 512)`, else 20) and writes
  `appHello` inside `connect()` (the grinder drops a silent client after 10 s).
- **`update()`** runs about once a second while linked. It drains the whole ring into the core before the tick (a stalled
  loop must not fire the 5 s timeouts while the answer already sits in the ring), ticks the core, publishes the state, the
  reported settings and a finished grind to the framework, and logs. A ring overflow or a protocol error ends the link.
- **Settings.** `applySettings()` hands the profile's dose, grind size and rpm (each when it has one; feed rpm is ignored) to
  the core as the wanted Profile Dose and returns `isConnected()`. The grinder takes 5.0 to 60.0 g; a dose outside is not
  sent (log line). Grind size 0.0 to 999.9. rpm 0 to 9999, a whole number; an rpm outside that range is treated as absent
  (log line `rpm above 9999, not sent`), so a value the grinder holds for that profile is cleared. No grind unit is ever
  sent. `supportsGrindSize()` is true; `supportsDose()` is true (patch 0004); `supportsRpm()` is true only on a ready link to
  a grinder that advertised `protoMinor` 2 or more. Feed rpm is not supported.
- **Mirror rule.** A size or rpm that the GaggiMate profile does not have is sent as `null` (`grindSize:null`, and
  `rpm:null` only to a grinder that supports rpm), but only when the grinder holds one. The rpm is never sent to an older
  grinder: a 1.1 grinder refuses the whole update for the unknown key, so the dose would be refused too. One note
  (`rpm not supported by this grinder firmware, not sent`) is logged per wanted rpm.
- **Order inside the core** (one request in flight): stale profiles are read first (`profile_get`), then the dose push
  (`profile_update` of profile 3 with dose, size and rpm, only when entry 3 differs: compare-first), then `grindProfile
  select 3` after an update that stored a change, then `result_get` when `resultSeq` is newer. Writes wait for a grinder state that accepts them
  (IDLE or WARNING, so a user in the grinder's menu is not disturbed). Stored changes keep 1.1 s apart (spec 7.3: a
  `RATE_LIMITED` answer retries after 1.1 s); `NOT_READY`, `EDIT_IN_PROGRESS` and the like wait for a suitable state; at
  most 3 attempts per stage, then it gives up and logs the code. The select follows only an update that stored a change, so a
  re-push of equal values does not pull a user who moved elsewhere back.
- **Finished grind.** `GLinkResultGate` takes the grinder's `resultSeq` at link-up as the baseline; only a settled result
  (OK, OVERSHOOT, MAX_PULSES; not STOPPED or an error) with a newer sequence number is reported, once. A grind that finished
  before the link, or in a gap, is never reported. The driver calls `reportGrindFinished(grams)` from `update()` (loop task).
- **State mapping.** IDLE is IDLE, GRINDING is GRINDING, STARTING, DONE, SETTING, UPDATING and WARNING are BUSY, an unknown
  text is UNKNOWN. `getReportedSettings()` is the dose and size of the profile selected on the grinder.
- **`shutdown()` is not overridden**: GaggiMate entering standby only disconnects; it never powers the grinder off.
- **Disconnect reason.** A GAP listener (`ble_gap_event_listener_register`) stores the reason of the grinder's link in an
  atomic; `disconnect()` and a failed connect log it with `glink_disconnect_describe()`, for example
  `reason 0x08 (supervision timeout)`.
- **Logs** (serial, integers only): connect refusals, `Linked, packet size N, hello sent`, TX power at link-up, `Data channel
  ready` with the free internal heap before and after the connect, once the loop task's stack high-water mark, push progress
  (queued, stored and selecting, in sync, giving up with the code), the rpm notes above, `Grind finished, N.N g`, and
  `RSSI -NN dBm` every 5 s while linked. There is no card and no setting for any of it.

## The consumer plugin (`GLinkPlugin`)

The framework's `BluetoothDevicesPlugin::update()` fires `grinder:grind:finished` (`value` = grams) after patch 0001.
`GLinkPlugin` subscribes in `setup()`; the handler only records the value (range 5.0 to 40.0 g, latest wins). `loop()` calls
`applyPendingDose()`, the only place that writes `profile.dose`: on the loop task, under `controller->getProcessLock()`,
compare-first, and only when the mode is BREW, no brew or grind is active, no update or auto-tune runs and the selected
profile is not a utility profile. A value that cannot be applied yet is held for at most 5 minutes, then dropped. The write
is memory only, like Smart Dose: it is lost when the profile is reselected or the display restarts, and last writer wins
against Smart Dose. It is always on: no setting, no web endpoint. `GLinkPlugin.h` includes no GLink header
(`config/bluetooth.h` defines `BLE_*` macros that must not reach `Controller.cpp`).

## Patches (against GaggiMate 2e64abd, full SHA `2e64abd78f77df01b8f623bf5ffc2542f34d3275`)

| Patch | Files | What |
|---|---|---|
| `0001-grinder-finished-event.patch` | `lib/BluetoothDevices/src/BluetoothGrinder.h`, `src/display/plugins/BluetoothDevicesPlugin.{h,cpp}` | `reportGrindFinished` / `takeGrindFinished` on `BluetoothGrinder`; `pollGrinderEvents` fires `grinder:grind:finished` (`value` = grams). No effect without a grinder. Proposed upstream. |
| `0002-controller-register-glink-plugin.patch` | `src/display/core/Controller.cpp` | `#ifndef GAGGIMATE_SIM`: includes `GLinkPlugin.h` and registers `new GLinkPlugin()` after `&BLEDevices`, unconditionally (no setting). Goes away if the driver becomes a built-in. |
| `0003-web-glink-model-name.patch` | `web/src/utils/bluetoothDevices.js` | `[/^GrindByWeight/i, 'Smart Grind By Weight']` in `GRINDER_MODELS`, so the Bluetooth tab shows the model name instead of the raw name. Not a translated string: no locale change. |
| `0004-grinder-supports-dose.patch` | `lib/BluetoothDevices/src/BluetoothGrinder.h`, `src/display/plugins/BluetoothDevicesPlugin.cpp` | `virtual bool supportsDose() const { return false; }` on `BluetoothGrinder`; `syncGrinderProfile()` also proceeds when the connected grinder `supportsDose()` and the profile dose is above 0 (before, a profile with a dose and no grind size was skipped), and logs one line for a real skip. Other drivers (Motto) are unchanged except that a profile with a dose but no grind settings now logs one skip line (`has nothing the connected grinder can take, not sent`): the default is false. Added in chg026; can still be offered upstream. |

The patches are LF (the upstream blobs are LF); `.gitattributes` keeps them out of line-ending conversion. Without patch 0003
the tab shows the raw name `GrindByWeight`.

## The Bluetooth tab flow

1. On the grinder, Menu, Bluetooth, "GaggiMate link" on (touch only). It advertises `GrindByWeight` and keeps one client.
2. On GaggiMate, the Bluetooth tab lists it as a grinder (type grinder). The user connects it there; GaggiMate saves it as
   the saved grinder (one grinder slot: a paired Bookoo Motto 80 and this grinder exclude each other).
3. From then on GaggiMate's plugin connects whenever it sees the saved grinder, drops the link at standby and reconnects at
   wake-up on sight. Forget in the tab closes the link and stops it. There is no button of ours and no sticky disconnect.
4. After the connect GaggiMate pushes the selected profile (it also does so on every profile select and save). The grinder's
   PROFILE page appears the first time and shows the dose, the size and the rpm (the select is seen on the wire; the page
   itself was not yet seen on the bench, and the rpm line is not yet seen on a screen).
5. A finished grind on the grinder sets the selected profile's dose in memory (see the consumer plugin).

While linked the driver holds the grinder's single Bluetooth client slot: `grinder.py` upload and WiFi tools, the web flasher
and WiFi updates of the grinder are blocked until Forget, standby, or "GaggiMate link" off on the grinder.

Known limitations and things that do not map exactly:

- **G1 (a profile with a dose but no grind size was not pushed): fixed by patch 0004 (chg026).** The driver reports
  `supportsDose()`, so such a profile is pushed; a profile with nothing the grinder can take is skipped with one log line.
  Not yet re-checked on the bench.
- **rpm needs a grinder with `protoMinor` 2.** With an older grinder (chg025 image) the rpm is not sent and the dose and
  size still arrive; one note says so. A chg025 build of this plugin never sends an rpm and never clears one, so an rpm
  stored by a chg026 build stays on the grinder until the profile is removed there (Menu → Bluetooth → Remove GaggiMate
  profile) or this plugin is used again.
- The grinder shows the rpm as a second line under the grind size; its layout is checked by arithmetic only, not on a screen.
- Signal strength and TX power are in the serial log only; the tab does not render RSSI. The old "last saved dose" line is
  gone: the new dose is the profile dose on the GaggiMate display and one serial log line.
- A connect that is refused or fails during a shot raises GaggiMate's generic "found, but connection failed" banner on each retry.
- After a finished grind the profile dose is the measured weight; saving the profile then pushes that weight to the grinder
  as the new target (same as Smart Dose). The in-memory dose is lost when the profile is reselected.
- The old NVS keys `gl_a`, `gl_ad`, `gl_n` and `gl_sd` of the retired plugin stay in flash on development units. They are
  unused; the user pairs again in the Bluetooth tab.
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3: the controller link, a scale and the grinder use all three.

## Security

GLink has no authentication by decision (spec 1.1). The GaggiMate side adds no start and no capability the protocol does not
already give anyone in range; it only writes the Profile Dose. A grinder reply goes through the strict bounded reader, and a
weight reported by the grinder is range checked (5.0 to 40.0 g) before it can become a dose.

## Verified

Host side (Windows, MSYS2 g++, PlatformIO 6.2.0 with a private `PLATFORMIO_CORE_DIR`):

- `scripts/run_host_tests.sh`: 176 host tests pass, 0 failed (chg026 step 5; 145 before the rpm and dose-only work),
  compiled with `-Wall -Wextra -Werror`, plus a syntax check of `glink_sources.cpp` (all pure sources in one unit, as
  `pio test -e native` builds it).
- chg026, host only: the rpm wire format (build and parse of `rpm`, `protoMinor` in `baseInfo`, the 0 to 9999 range), the
  sync (rpm sent only when supported, the two notes, the mirror rule for size and rpm, compare-first) and the core's rpm
  handling (`GLinkGrinderCore`). The firmware side (`-0.5` as `INVALID_PARAM`, the store, the label, the spec vectors) is
  covered by the firmware's own suites. `GLinkGrinder.cpp` (`applySettings()`, `supportsRpm()`) is compiled only, not run.
- The shared files are byte-identical with the firmware (contract test).
- A3: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3 in the display env. It is the NimBLE-Arduino default in `nimconfig.h`; 2e64abd's
  `platformio.ini`, board files and sources do not override it.
- N5: a scan hit's `NimBLEAddress` carries the address type (`NimBLEScan` builds it from `disc.addr`, `NimBLEClient::connect`
  passes `address.getType()`). Read from the NimBLE-Arduino source, not run on hardware.
- Baseline build of a fresh clone of 2e64abd with the plugin staged and no patches (step 1): `pio test -e native -f
  test_grinderlink` passes and `pio run -e display` builds at RAM 29.7 % and flash 49.1 % (3217989 B). With patch 0001 only,
  flash is 3218661 B. No unpatched `display-headless` baseline was recorded.
- Step 5 build, driver with patches 0001 and 0002: `pio run -e display` builds at RAM 29.7 % (97360 B) and flash 49.5 %
  (3241649 B, +22988 B against the 0001 build); `pio run -e display-headless` builds at RAM 25.5 % (83436 B) and flash 35.7 %
  (2340461 B).
- Patch 0003 applies on a fresh clone after 0001 and 0002, and `node --test web/test/bluetooth-devices.test.js` passes (7 of 7:
  the 6 cases of the pinned tree plus the case for the new name, which the patch carries), and the staged `WEB_BUILD=1` web build and embed passes.

Bench round 2 (2026-11-21, unattended, no touch). Real build of the staged clone, no bench variant. The controller was
reflashed to protocol 16 (`v1.10.0-7-g2e64abd7-dir`), so GaggiMate reports READY. The grinder board was on its
CALIBRATION/MENU screens after a reset and reached READY by itself (no tap).

- B1 scan and connect (real build): the Bluetooth tab scan lists `GrindByWeight` (raw name; patch 0003 not checked in a
  browser), the link comes up, the hello is sent, the data channel is ready within about 2 to 3 s, packet size 244, TX power
  +9 dBm, loop task stack at least 4.5 KB free. Five links: internal heap per link is lost in the noise (WiFi and WebSocket
  traffic), with no downward trend. A connect to the grinder's random address worked (N5).
- B2 profile push (partial): dose 18 with grind size 15 gave `update ok` and `select ok`, and GaggiMate reported the profile
  in sync. An unchanged re-select sends nothing. 19.0 gave one update and one select. Two saves 220 ms apart were COALESCED
  into one update (the later value wins; which value was stored was not read back), not sent 1 s apart. 4.0 and 70.0 were
  skipped with `Profile dose outside 5.0 to 60.0 g, not sent to the grinder`.
- G1 (dose without grind size), before patch 0004: not pushed and no crash, no log line. Patch 0004 is not bench-tested yet.
- Disconnect reason line for a local close: `reason 0x16 (local host terminated)`.
- Forget while linked closes the link (reason 0x16), the grinder's link slot is free within 1 s, and no reconnect follows.
- B5 standby and wake through the API (`change-mode` 0, then 1): the link closes with 0x16, the grinder is back in
  `link_listening` within 6 s, and after wake GaggiMate resumes scanning and links 3 to 4 s later. The grinder was never
  powered off. Real screen standby was not run.
- B6 Forget, GaggiMate side: the saved grinder is cleared, the device is listed again as discovered, and nothing connects
  on its own. Grinder-side "GaggiMate link" off and on was not run.
- B7 grinder reset (not a carry-away): supervision timeout `reason 0x08`, one reconnect on sight 1 s after the grinder
  was seen again, no reconnect storm.
- B8 31.6 min link (controller and grinder only, no scale): no `Link lost`, 372 RSSI lines (about every 5 s), grinder
  polls `link_linked` in 325 of 328. This is not the three-connection case. With the default `standbyTimeout` of 900 s the
  screen goes to standby after 15 min and closes the link, so the rerun used 3600 s (restored to 900 afterwards).
- B9 (partial): the grinder's diagnostics follow `link_listening`, `link_connected`, `link_linked` and back to
  `link_listening` after standby or Forget. The PC sees no BLE devices, so the blocked and unblocked behaviour of
  `grinder.py` is not proven.

Round 1 (same day, controller at protocol 8, bench-only variant that accepted the mismatch state) reached a link and tested
Forget; its results are superseded by round 2.

The grinder dev board now holds a stored Profile Dose (index 3, about 21.0 g, grind size 15) and has profile 3 selected from
the bench. This could not be reverted with the available tooling.
- Clean-clone build of chg025 (`WEB_BUILD=1`, web UI embedded, before rpm): `-e display` RAM 29.7 % (97360 B), flash 61.5 % (4033485 B);
  `-e display-headless` RAM 25.5 % (83436 B), flash 47.8 % (3132309 B); `pio test -e native -f test_grinderlink` 145 of 145.
  The web UI is embedded, so the flash numbers are not comparable with the earlier step 5 numbers (49.5 %, 35.7 %).
- chg026 (rpm, dose-only push), staged fresh clone of 2e64abd with patches 0001 to 0004 and `WEB_BUILD=1`: `pio test -e native -f
  test_grinderlink` 176 of 176; `pio run -e display` RAM 29.7 %, flash 61.6 % (web UI embedded; chg025 was 61.5 %);
  `pio run -e display-headless` RAM 25.5 %, flash 47.8 %. Without the web UI (chg026 steps 1 and 4 builds): display flash
  49.5 %, headless flash 35.7 %.

## Not verified

- B2 remainder: the rate-limited retry path (`RATE_LIMITED`, 1.1 s) was not seen, because the two quick saves coalesced;
  the PROFILE page on the grinder's display was not seen (select seen on the wire only).
- B3 finished grind to dose and B10 Smart Dose race: need a real grind (no load cell or motor on the bench).
- B4 connect refusal during a shot: needs a GaggiMate shot with a machine; not attempted.
- B5 real standby and wake with the screen, and `grinder.py scan` / `info`: not run. The PC sees no BLE devices, so the
  tool path cannot be tested from this PC.
- B6 grinder-side "GaggiMate link" off and on: needs touch, not run.
- B7 link loss by carrying the screen away or powering the grinder off: not run. Only the reset path (0x08) was seen.
- B8 three connections together (a scale included), 30 min: not run. Round 2 had no scale.
- B9 `grinder.py` upload and scan blocked while linked, and the Motto 80 path: not run.
- Loop task stack and heap headroom under a real shot, and `parse_message` stack use with three connections.
- A refused connect during a shot (log lines, banner noise) and the cost of a blocking connect to the scale heartbeat.
- That the revert of the dose after a profile reselect works as inferred from Smart Dose.
- That a grinder in its menu (SETTING) holds a push until READY.
- chg026 on hardware (step 6): the rpm line on READY and EDIT (layout, no clipping, nothing over the buttons); rpm reaching
  the grinder and surviving a reboot; a rpm-only change (one update, the PROFILE page follows); `rpm:null` removing the
  line; rpm 10000 not sent and the grinder's value cleared; a removed Profile Dose leaving no stale rpm; the old and new
  combinations of the compatibility table (new plugin with the chg025 grinder image, old plugin with the new grinder); a
  dose-only profile after a profile with a size (the size line disappears).
- That the coalesced save stores the later value: the stored value was not read back.
- The Motto 80 path is untouched by the patches; it was not run on a device.
- No Linux or macOS run of the staging script; the `sim` env (`display-sim`) was not built (the driver and plugin are empty
  there by `GAGGIMATE_SIM`).
- `pio test -e native` for all suites: `test_ota_loadtest` does not compile on Windows (`arpa/inet.h`), unrelated to this
  plugin; the gate is the plugin suite.
- The patches were not applied to a second GaggiMate version; the branch is a proposal and may be force-pushed.
- The shared reader refuses exponents in numbers. The grinder never sends them; a client of another device would need a
  different reader.

## Still to do

1. The controller is at protocol 16 now. Run the rest of the bench (see "Not verified": a grind for B3 and B10, a shot for B4,
   grinder-side link tests, a scale for B8, a BLE-capable PC for B5 and B9) and move what passes to "Verified".
2. Decide upstream: the grind-finished event (patch 0001), the model name (patch 0003), and `supportsDose()` for dose-only
   profiles (patch 0004, G1), which the maintainer would have to take. The rpm (GLink 1.2) is in the protocol, not in a patch.
3. Optional follow-ups, not built: a quiet "busy" reason instead of `connect_failed` in GaggiMate's connect path, start and
   stop on `BluetoothGrinder` (the maintainer's interface), and moving the driver into `lib/BluetoothDevices`.

## Running the checks

```
sh gaggimate-plugin/scripts/run_host_tests.sh                       # needs g++ (C++17)
sh gaggimate-plugin/scripts/check_patches.sh /path/to/gaggimate     # read-only
sh gaggimate-plugin/scripts/check_against_gaggimate.sh DEST [SOURCE]  # DEST: new or empty directory; needs git, network for libraries, and pio unless STAGE_ONLY
python3 -m pytest tools/tests/test_gaggimate_plugin_contract.py     # PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 in CI
```

The staged check (`check_against_gaggimate.sh`) stages the plugin into a fresh clone. Under Git Bash `STAGE_ONLY` defaults to
1: it stages and prints the PowerShell commands, because `pio` must run from PowerShell (Git Bash corrupts the PlatformIO tool
packages). `WEB_BUILD=1` also builds and embeds the web UI (needs `npm`). Never point `DEST` at your own GaggiMate checkout.
The full procedure and the rules are in `docs/DEVELOPMENT.md` ("The staged check").

On Windows keep DEST and `PLATFORMIO_CORE_DIR` paths short (for example a `subst` drive): the Xtensa GCC 8.4 toolchain fails
with `CreateProcess: No such file or directory` on long paths. `PLATFORMIO_CORE_DIR` keeps the toolchain downloads away from
`~/.platformio`.

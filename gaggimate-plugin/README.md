# GrinderLink plugin for GaggiMate (prototype, parts 8a and 8b)

A GaggiMate display plugin that talks GLink v1 (`docs/GLINK-PROTOCOL.md`) to a Smart Grind By Weight grinder over BLE:
read the grinder state and live weight, send a dose and an optional grind size to the grinder's Profile Dose, start
and stop a grind, read the result. GaggiMate acts as the BLE central; the grinder is the peripheral.

This folder is a prototype kept in the grinder repository. It is to be contributed upstream by the user under
GaggiMate's terms; it carries no licence or CLA text. The `grindControl` object of the protocol is provisional.

**Status (chg025, step 1): the plugin is being re-based onto GaggiMate 2e64abd (full SHA `2e64abd78f77df01b8f623bf5ffc2542f34d3275`)
as a driver for GaggiMate's BluetoothDevices grinder framework. Step 1 removed the old glue (`GLinkPlugin`, `GLinkReconnect`),
the settings card and the five b564759 patches. The sections below that describe them (the plugin, the WebSocket API,
the patches, the chg024 dose save) are stale until step 7 rewrites them. Nothing has run on a display or against a grinder
yet** (plan step 9). See "Verified" and "Not verified".

## Layout

```
gaggimate-plugin/
  README.md
  docs/GLINK-PROTOCOL.md                 copy of the protocol (a test keeps it equal to ../docs/GLINK-PROTOCOL.md)
  src/display/plugins/grinderlink/
    config/bluetooth.h                   byte-identical copy of the firmware's src/config/bluetooth.h (constants only)
    bluetooth/glink_frame.{h,cpp}        shared with the firmware: fragmenter, reassembler, bad-fragment window
    bluetooth/glink_json.{h,cpp}         shared with the firmware: bounded JSON reader and writer
    bluetooth/glink_disconnect.{h,cpp}   shared with the firmware: names the HCI disconnect reason for the log (chg023)
    GLinkMessages.{h,cpp}                request builders; parsers for responses and broadcasts
    GLinkClient.{h,cpp}                  session: handshake, one request at a time, timeouts, start decision, tracking
    GLinkDoseSave.{h,cpp}                pure policy: grind result to profile dose, once per result, range, hold and expiry (chg024)
  test/test_grinderlink/                 host tests (Unity style) against the shared vectors
  test/test_grinderlink/glink_sources.cpp  compiles the pure sources into the test for `pio test -e native` only
  test/host/unity.h                      tiny Unity stand-in for the g++ run
  tools/tests/glink_vectors.json         copy of the shared conformance vectors
  patches/                               unified diffs against GaggiMate 2e64abd (empty in step 1)
  scripts/run_host_tests.sh              builds and runs the host tests with g++ (globs the test and pure sources)
  scripts/check_patches.sh               read-only `git apply --check` of the patches against a checkout
  scripts/check_against_gaggimate.sh     clone 2e64abd into a NEW directory, apply, stage, `pio test`, `pio run`
```

Shared files (must stay byte-identical with the firmware; `tools/tests/test_gaggimate_plugin_contract.py` compares
them, line endings aside): `bluetooth.h`, `glink_frame.h`, `glink_frame.cpp`, `glink_json.h`, `glink_json.cpp`,
`GLINK-PROTOCOL.md`, `glink_vectors.json`, `glink_vectors.h`, `glink_disconnect.h`, `glink_disconnect.cpp`. They keep the firmware's names (so the includes inside
them stay valid) instead of the `GLinkFrame.*` names the plan used. The transport carries no grinder-specific object, so
a driver for another device (for example a Motto80) could reuse it unchanged; only the GLink driver exists.
The plugin does not use the firmware's `glink_messages.*` (that is the device side: request parser and response
encoders).

`GLinkMessages` and `GLinkClient` are pure C++ (no Arduino, no NimBLE, no heap). `GLinkPlugin` implements `Sink` (one
RX write) and `Listener` and feeds `on_connected`, `on_notify`, `on_disconnected` and `tick`.

Things the pure code does on purpose: requests are checked with the grinder's limits before they are sent (dose 5.0
to 60.0 g, size 0.0 to 999.9, unit of up to 6 characters from the allowed set, a unit only together with a size);
one request at a time with a 5 s timeout; `stop` goes out at any time; a successful `start` is followed by the
"did it happen" decision of spec 5.5 (first `periodInfo` past `STARTING`, else lead-in plus 2 s); profiles are re-read
when `profileRev` changes (the first `periodInfo` of a connection counts as changed); the result counter is compared
with `resultSeq`; the sequence counter runs 1 to 65535 like `tools/ble/glink_client.py`; the list reply of `grindProfile`
`get` (a JSON array, which the strict shared reader refuses) is split into entries before the reader sees it; the
grinder's error `message` texts (longer than the reader's 32 byte string limit) are shortened for the reader and kept
whole in `Failure::message`.

## The plugin (`GLinkPlugin`)

Registered by `Controller::setup` only when `grinderLinkActive` is on (patch 0002), so the toggle takes effect after a
reboot; the whole file is empty under `GAGGIMATE_SIM`.

- **Threads.** WebSocket requests arrive on the AsyncTCP task and only copy the request into a queue (a stop goes to the
  front). Everything else runs on the main `loop()` task: the NimBLE calls, the `Client`, the replies and the events.
  NimBLE callbacks (host task) only set a flag, add a scan hit or put a notification packet in an 8 KB byte ring (`RxRing.h`, about 370 packets of 20 bytes or 33 of 244).
  A ring overflow drops the link instead of feeding the reassembler a gap. (An earlier 8 packet ring overflowed on the hello
  reply of a grinder that sends 20-byte packets: baseInfo alone is 17 of them.)
- **Memory.** The `Client` (about 3 KB with its parse target), the packet ring and one scratch packet are allocated with
  `new (std::nothrow)` in `setup()`. `parse_message` needs about 3 KB of stack on the loop task; the plugin logs the
  task's stack high-water mark once, at the first ready link, so the margin can be read from the first hardware run.
- **Link.** The grinder is chosen by Scan (name prefix `GrindByWeight`, 6 s window) and Select, and stored in the settings
  as `gl_ad` (`aa:bb:cc:dd:ee:ff`, or `aa:bb:cc:dd:ee:ff/r` for a random address) and `gl_n`. Connect is on demand
  (Connect button; blocking, 5 s timeout). **Auto-reconnect (chg023, `GLinkReconnect.{h,cpp}`, a pure policy):** the plugin
  re-makes the link by itself, only while GaggiMate is idle (same predicate as Connect: not brewing, grinding or busy) and
  not in standby; the first attempt comes right after wake-up and at boot once idle. Backoff 3 s, 10 s, then every 30 s
  forever; an automatic attempt times out after 3 s (each attempt blocks the plugin's `loop()` for up to that long, only
  when idle). A link that stayed up under 30 s does not reset the backoff. The Disconnect button (or
  `req:grinderlink:disconnect`) keeps it off until GaggiMate has been in standby or restarted (RAM only, not saved);
  select, forget and a manual Connect re-arm it. The old 60 s idle drop is gone; the card's keep-alive is still accepted and
  does nothing. `startOutcome`, `startTimedOut` and `stopUnconfirmed` are cleared only by a manual connect. While the link
  is up it holds the grinder's single BLE client slot: `grinder.py` upload and WiFi tools, the web flasher and WiFi updates
  of the grinder are blocked until you press Disconnect or turn the grinder's "GaggiMate link" off. Connect is answered
  `BUSY` while GaggiMate is active (grinding or brewing) or not ready, so the scale
  heartbeat on the main loop is not stalled mid-shot. **Residual stall:** an idle connect still blocks `loop()` for up to
  about 5 s (manual) or 3 s (automatic) plus service discovery and the CCCD write, and the 3 s retry throttle counts from the end
  of the attempt; a shot that starts during an automatic connect stalls the loop for the rest of it; queued Connects share one attempt's answer. The ATT MTU is read after the connect: packet size is `min(MTU - 3, 512)`, else 20.
- **Never replays a start.** A start is sent from one place, only for an explicit `req:grinderlink:start`, always with
  `expectDose` (the request's dose, else the dose cached from the grinder for that profile, else `NO_DOSE`). Requests
  queued before a link change are answered `STALE`; a request in flight when the link drops is answered `DISCONNECTED`;
  nothing is stored for later.
- **After a timeout.** A start that gets no answer sets `startOutcome` to `unknown`; the `Client` then refuses a new start
  until a `periodInfo` has been seen, and that `periodInfo` decides `started` (STARTING or GRINDING) or `not_seen`
  (spec 7.5). A stop without answer sets `stopUnconfirmed` until a `periodInfo` shows the grinder is not grinding.
- **GaggiMate events.** `controller:grind:start` and `controller:grind:end` are hooked as a sketch that only logs; GaggiMate
  has no dose of its own to send, so the prototype is driven from the card.

### WebSocket API

Replies are `res:grinderlink:<name>` with the request's `rid`, `ok`, and for failures `error` (the grinder's code, or one
of `BUSY`, `NOT_CONNECTED`, `CONNECT_FAILED`, `DISCONNECTED`, `STALE`, `NO_DOSE`, `REFUSED_LOCALLY`, `TIMEOUT`,
`INVALID_PARAM`, `BLE_NOT_READY`, `SCAN_FAILED`), `message` and `field`. Most replies carry the status object.

| Request | Fields | What |
|---|---|---|
| `req:grinderlink:status` | | status object |
| `req:grinderlink:scan` | | 6 s scan; hits arrive in `found` of the state events |
| `req:grinderlink:select` | `address`, `addrType`, `name` | store the grinder (drops the current link) |
| `req:grinderlink:forget` | | drop the link, clear address and name |
| `req:grinderlink:connect` / `disconnect` | | manual connect (re-arms auto-reconnect; `keepalive:true` is accepted and does nothing) / end the link and keep auto-reconnect off until standby or restart |
| `req:grinderlink:profile:select` | `profile` | `grindProfile` `select` |
| `req:grinderlink:profile:update` | `profile`, `dose`, `grindSize` (number or null), `grindUnit` (string or null) | `grindProfile` `update` |
| `req:grinderlink:start` | `profile`, `dose` (optional, else the cached dose) | `grindControl` `start` with `expectDose` |
| `req:grinderlink:stop` | | `grindControl` `stop` (also while another request waits) |
| `req:grinderlink:result` | | `grindResult` `get` |

Events: `evt:grinderlink:state` (the status object, at most 4 per second, only when something changed) and
`evt:grinderlink:result` (a new result). The status object has `active`, `address`, `name`, `link`
(`idle`, `scanning`, `connecting`, `linked`, `ready`), `error`, `found`, `startOutcome`, `startNote`, `stopUnconfirmed`
and, while linked, `device`, `period`, `profiles`, `lastResult`, `rssi` (dBm) and `txPower` (dBm). RSSI is read about every
1 s while a browser is connected, else every 5 s. The card shows `Signal: -62 dBm · TX power: +9 dBm` (new msgids `Signal`
and `TX power` in the eight catalogs of patch 0005).

### Scan and the scale plugin

The NimBLE scan is a singleton that `BLEScalePlugin` owns while no scale is connected. A GrinderLink scan (refused unless the controller link is up, because `BleClientTransport` shares the scan singleton and
needs it to find the controller again) stops it, sets its
own callbacks for 6 s and stops it again; the scale plugin's `maintainScan()` sees a stopped scan and restarts it with its
own callbacks (the code and comments show this, it has not run). A connect also stops the scan (NimBLE does that itself).

### Security

The WebSocket API is unauthenticated, like the rest of GaggiMate's web interface, and `req:grinderlink:start` can start a
grinder motor (after the grinder's own 3 s touch-to-cancel lead-in). Anyone who can reach the GaggiMate page can use it.
Scan hits are cut to printable ASCII before they reach the page. The NimBLE scan callback stores hits in fixed buffers,
but NimBLE-Arduino 1.4 itself hands it `std::string` copies of name and address, so it is not allocation-free.

### Bluetooth connections

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3 in the display env (the default in NimBLE-Arduino 1.4.3 `nimconfig.h`; neither
`platformio.ini` nor the board files override it), and `GLinkPlugin.cpp` has a `static_assert` that it is at least 3.
The controller link, a scale and the grinder use all three, with no spare. `connectLink` refuses (and says so in `error`)
when NimBLE already holds three clients. Raising it (`-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=4`) costs heap and is the
maintainer's call.

## Save grind weight as dose (chg024)

Setting `gl_sd` (`grinderLinkSaveDose`, default on), shown as the checkbox "Save grind weight as dose" on the card. When a
grind finishes on the grinder with a settled weight (results OK, OVERSHOOT or MAX_PULSES, spec 5.6), the plugin writes
the grinder's `finalWeight` into the dose of the profile selected in GaggiMate, the same way GaggiMate's Smart Dosing
does. Rules:

- once per result: a result is taken only if its sequence number is newer than the last one seen on this link. The
  first `resultSeq` read at link-up is the baseline, so a grind that finished before the link came up, or during a
  link gap, is never written. A grinder reboot is re-baselined on the next link;
- only 5.0 to 40.0 g is written. A value outside that range is skipped, not clamped (the skip reason is shown in
  `doseNote`);
- the value is held (latest result wins, up to 5 min) while a shot runs, the mode is not BREW, the machine is busy
  (updating, auto-tuning) or the selected profile is a utility profile; after 5 min it is dropped;
- memory only: the dose reverts when the profile is reselected or the display restarts. Saving the profile in the
  GaggiMate UI persists it, as it does for a Smart Dosing value;
- last writer wins against Smart Dosing: both write the same field, whichever ran last holds;
- the status object gets `doseSaved` (grams of the last dose applied by this plugin; absent until the first apply since the
  display started; a compare-first skip may mean the dose already had that value) and `doseNote` (`waiting` while a value is held, else the reason the last result was not written:
  `disabled`, `not_settled` or `out_of_range`; absent otherwise);
- the GaggiMate setup wizard does not save the GrinderLink toggles (`grinderLinkActive`, `grinderLinkSaveDose` are not in
  its key list).

The listener callbacks only record the result; the write happens in `GLinkPlugin::applyPendingDose()`, called from
`loop()` under GaggiMate's process lock. The policy is pure code in `GLinkDoseSave.{h,cpp}`. Nothing has been run on a
grinder for this feature (see "Not verified").

## Patches (against GaggiMate 2e64abd, full SHA `2e64abd78f77df01b8f623bf5ffc2542f34d3275`)

The b564759 patches (settings, card, locales) are retired with the old glue. The chg025 set is in the chg025 plan,
section 3.6; so far:

| Patch | Files | What |
|---|---|---|
| `0001-grinder-finished-event.patch` | `lib/BluetoothDevices/src/BluetoothGrinder.h`, `src/display/plugins/BluetoothDevicesPlugin.{h,cpp}` | `reportGrindFinished` / `takeGrindFinished` on `BluetoothGrinder`; `pollGrinderEvents` fires `grinder:grind:finished` (`value` = grams). No effect without a grinder. |
| `0002-controller-register-glink-plugin.patch` | `src/display/core/Controller.cpp` | `#ifndef GAGGIMATE_SIM`: includes `GLinkPlugin.h` and registers `new GLinkPlugin()` after `&BLEDevices`, unconditionally (no setting). |

Still to come: web model name.
The patches are LF (the upstream blobs are LF); `.gitattributes` keeps them out of line-ending conversion.

## Verified

chg025 step 1, fresh clone of GaggiMate 2e64abd with no patches (`check_against_gaggimate.sh`, pio run from PowerShell):

- A3, the connection limit: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3 in the display env. It is the NimBLE-Arduino default in
  `.pio/libdeps/display/NimBLE-Arduino/src/nimconfig.h` (`#ifndef` block); 2e64abd's `platformio.ini`, board files and
  sources do not override it. The `static_assert` in the driver stays.
- N5, the address type: a scan hit's `NimBLEAddress` carries the type. `NimBLEScan` builds it from `disc.addr`
  (`NimBLEAddress(ble_addr_t)` stores `addr.type`), `NimBLEAdvertisedDevice::setAddress` keeps it, `getAddress()` returns it,
  and `NimBLEClient::connect` passes `address.getType()` to the peer address. Read from the NimBLE-Arduino source, not run on
  hardware (bench B1 confirms it).
- Host tests (`run_host_tests.sh`): the glob build compiles the test files and the pure sources. In step 1 it stops at the
  stale `test_glink_reconnect.cpp` (and `test_main.cpp` calls its runner) until the Tester's change. In a scratch copy without
  that file and its call: 71 tests pass, 0 failed (99 before, the 28 reconnect tests are gone).

Checked on the developer PC (Windows, MSYS2 g++, PlatformIO 6.2.0 with a private `PLATFORMIO_CORE_DIR`), for the b564759 driver:

- the shared files are byte-identical with the firmware (contract test);
- `scripts/run_host_tests.sh`: 99 host tests pass, compiled with `-Wall -Wextra -Werror`: the request builders
  reproduce the vector requests byte for byte and refuse what the grinder refuses; every response, `periodInfo`, `baseInfo`
  and `grindResult` text of the vectors parses to the values in the vector JSON; the hello is the golden handshake
  frames; sessions with a fake link cover the handshake, the 5 s ready and response timeouts (also across the clock
  wrap), one request at a time, response matching by object name, the stop reply, the start decision (grinding,
  cancelled, timeout), profile and result tracking, a grinder reboot, disconnect and reconnect, ten bad fragments,
  the sequence wrap, a failing link write, and packet sizes of 9, 20, 100 and 512 bytes;
- chg024 A1, a fresh clone at b564759 (the user's checkout untouched): the five patches apply in order, and the
  display and display-headless images build with the plugin compiled in;
- chg024 B4, a fresh staged clone of the current code (dose save included) at b564759 with `WEB_BUILD=1`: all five
  patches apply, the web UI builds and embeds (the bundle holds the new msgids), `pio test -e native -f test_grinderlink`
  runs 99 of 99, and `pio run -e display` and `-e display-headless` build (no warnings from the plugin files).
  Nothing was flashed or run on a device;
- the card passes a JSX syntax-only transform (esbuild); its request and event names match what the plugin registers (contract test).
- the dose-save policy (`GLinkDoseSave`, chg024 B1) has host tests in the plugin suite of `run_host_tests.sh`.

## Not verified

- Nothing ran on a GaggiMate display or against a real grinder: the NimBLE connect, service discovery, subscribe, MTU read,
  notification flow, write with response, the scan hand-over with the scale plugin and the stack use of
  `parse_message` on the loop task are compiled but untested (plan step 9).
- The web card was built with the web toolchain at b564759 (chg024 B2: `npm ci`, lint, build), but not rendered and not
  run against the plugin. The locale entries (eight catalogs, including the dose-save texts) were written by hand, not
  produced by `lingui extract`, and the translations are not reviewed by a native speaker. The `Not connected` entry
  duplicated an upstream msgid and was dropped from the patch (the card's `t`Not connected`` resolves via upstream's entry).
- Save grind weight as dose (chg024): nothing has run on a grinder. The dose path (grinder result to profile dose on the
  display) is covered by the host tests of the policy and by the text guards of the contract test only. The revert after a profile reselect is inferred from Smart Dose and is not tested.
- `scripts/check_patches.sh` was not re-run against the user's own checkout for b564759 (that checkout has uncommitted work).
- `pio test -e native` for all suites: `test_ota_loadtest` does not compile on Windows (`arpa/inet.h`), which is not
  related to this plugin; the other suites passed. The gate is the plugin suite.
- No Linux or macOS run of the staging script; the `sim` env (`display-sim`) was not built (the plugin is excluded there by
  `GAGGIMATE_SIM`).
- Disconnect reasons (chg023): the plugin registers a NimBLE GAP listener (`ble_gap_event_listener_register`, from the
  ESP-IDF NimBLE host headers the firmware compiles against) and asks the client for its connection handle
  (`NimBLEClient::getConnId()`, and `BLE_HS_EALREADY`). NimBLE-Arduino 1.4.3 is not in the checkout used here, so these
  calls were not compiled against it. The reason is logged as `Grinder link lost, reason 0x08 (supervision timeout)`;
  a hardware run must show that line (or `the stack reported no reason`).
- Radio diagnostics (chg023 1b): at link-up the log shows `TX power +N dBm` (`esp_ble_tx_power_get()` decoded by `glink_tx_power_level_dbm()`; `NimBLEDevice::getPower()` returns -128 for levels outside its table)
  and every 5 s while linked `Grinder link RSSI -NN dBm` (`NimBLEClient::getRssi()`, read in the loop task; 0 means the read
  failed and is skipped). Both calls are in NimBLE-Arduino 1.4.3 (checked in the staged clone, not compiled here).
- Auto-reconnect on hardware (chg023): reconnect after a power cycle, no attempt in standby, first attempt after wake-up,
  the 3/10/30 s backoff, Disconnect staying off, no connect in the middle of a shot, the cost of the RSSI read, the loop
  task's stack headroom and the interplay with the scale (scan sharing) are covered by host tests of the policy only.
- Whether three Bluetooth connections are enough in real use (controller, scale, grinder) is unknown.
- The shared reader refuses exponents in numbers. The grinder never sends them; a client of another device would need a
  different reader.
- The patches were not applied to a second GaggiMate version.

## Still to do

1. Hardware loop against the grinder (plan step 9): scan, select, connect, set dose, start with the lead-in, stop, result;
   read the logged stack high-water mark; try it with a scale connected.
2. Run `lingui extract` (the web UI build itself was done in chg024 B2).
3. Decide with the GaggiMate maintainer whether the connection limit should rise.

## Running the checks

```
sh gaggimate-plugin/scripts/run_host_tests.sh                       # needs g++ (C++17)
sh gaggimate-plugin/scripts/check_patches.sh /path/to/gaggimate     # read-only
sh gaggimate-plugin/scripts/check_against_gaggimate.sh DEST [SOURCE]  # DEST: new or empty directory; needs git, network for libraries, and pio unless STAGE_ONLY
python3 -m pytest tools/tests/test_gaggimate_plugin_contract.py     # PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 in CI
```

The staged check (`check_against_gaggimate.sh`) stages the plugin into a fresh clone. Under Git Bash `STAGE_ONLY`
defaults to 1: it stages and prints the PowerShell commands, because `pio` must run from PowerShell (Git Bash corrupts
the PlatformIO tool packages). `WEB_BUILD=1` also builds and embeds the web UI (needs `npm`). Never point `DEST` at your
own GaggiMate checkout. The full procedure and the rules are in `docs/DEVELOPMENT.md` ("The staged check").

On Windows keep DEST and `PLATFORMIO_CORE_DIR` paths short (for example a `subst` drive): the Xtensa GCC 8.4 toolchain fails with
`CreateProcess: No such file or directory` on long paths. `PLATFORMIO_CORE_DIR` keeps the toolchain downloads away from
`~/.platformio`.

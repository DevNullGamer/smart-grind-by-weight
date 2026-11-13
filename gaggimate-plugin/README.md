# GrinderLink plugin for GaggiMate (prototype, parts 8a and 8b)

A GaggiMate display plugin that talks GLink v1 (`docs/GLINK-PROTOCOL.md`) to a Smart Grind By Weight grinder over BLE:
read the grinder state and live weight, send a dose and an optional grind size to the grinder's Profile Dose, start
and stop a grind, read the result. GaggiMate acts as the BLE central; the grinder is the peripheral.

This folder is a prototype kept in the grinder repository. It is to be contributed upstream by the user under
GaggiMate's terms; it carries no licence or CLA text. The `grindControl` object of the protocol is provisional.

**Status: parts 8a (core) and 8b (NimBLE glue, web card, patches, staging script) exist and build inside GaggiMate
b78beee. Nothing has run on a display or against a grinder yet** (plan step 9). See "Verified" and "Not verified".

## Layout

```
gaggimate-plugin/
  README.md
  docs/GLINK-PROTOCOL.md                 copy of the protocol (a test keeps it equal to ../docs/GLINK-PROTOCOL.md)
  src/display/plugins/grinderlink/
    config/bluetooth.h                   byte-identical copy of the firmware's src/config/bluetooth.h (constants only)
    bluetooth/glink_frame.{h,cpp}        shared with the firmware: fragmenter, reassembler, bad-fragment window
    bluetooth/glink_json.{h,cpp}         shared with the firmware: bounded JSON reader and writer
    GLinkMessages.{h,cpp}                request builders; parsers for responses and broadcasts
    GLinkClient.{h,cpp}                  session: handshake, one request at a time, timeouts, start decision, tracking
    GLinkPlugin.{h,cpp}                  GaggiMate Plugin: NimBLE central, WebSocket requests and events (device only)
  web/src/pages/Settings/GrinderLinkCard.jsx   the settings card (toggle, scan/select/forget, dose, start/stop, status)
  test/test_grinderlink/                 host tests (Unity style) against the shared vectors
  test/test_grinderlink/glink_sources.cpp  compiles the pure sources into the test for `pio test -e native` only
  test/host/unity.h                      tiny Unity stand-in for the g++ run
  tools/tests/glink_vectors.json         copy of the shared conformance vectors
  patches/                               unified diffs against GaggiMate b78beee
  scripts/run_host_tests.sh              builds and runs the host tests with g++
  scripts/check_patches.sh               read-only `git apply --check` of the patches against a checkout
  scripts/check_against_gaggimate.sh     clone b78beee into a NEW directory, apply, stage, `pio test`, `pio run`
```

Shared files (must stay byte-identical with the firmware; `tools/tests/test_gaggimate_plugin_contract.py` compares
them, line endings aside): `bluetooth.h`, `glink_frame.h`, `glink_frame.cpp`, `glink_json.h`, `glink_json.cpp`,
`GLINK-PROTOCOL.md`, `glink_vectors.json`, `glink_vectors.h`. They keep the firmware's names (so the includes inside
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
  (Connect button; blocking, 5 s timeout), the card keeps it alive every 20 s with `keepalive:true`, and the plugin drops an
  unused link after 60 s, but never while a start, stop or request is outstanding or the grinder is STARTING or GRINDING.
  The link is never re-made by itself: a keep-alive only refreshes the idle timer and is answered `NOT_CONNECTED` when the
  link is gone. Connect is answered `BUSY` while GaggiMate is active (grinding or brewing) or not ready, so the scale
  heartbeat on the main loop is not stalled mid-shot. **Residual stall:** an idle connect still blocks `loop()` for up to
  about 5 s (connect timeout) plus service discovery and the CCCD write, and the 3 s retry throttle counts from the end
  of the attempt; queued Connects share one attempt's answer. The ATT MTU is read after the connect: packet size is `min(MTU - 3, 512)`, else 20.
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
| `req:grinderlink:connect` / `disconnect` | | link on demand / keep-alive; end the link |
| `req:grinderlink:profile:select` | `profile` | `grindProfile` `select` |
| `req:grinderlink:profile:update` | `profile`, `dose`, `grindSize` (number or null), `grindUnit` (string or null) | `grindProfile` `update` |
| `req:grinderlink:start` | `profile`, `dose` (optional, else the cached dose) | `grindControl` `start` with `expectDose` |
| `req:grinderlink:stop` | | `grindControl` `stop` (also while another request waits) |
| `req:grinderlink:result` | | `grindResult` `get` |

Events: `evt:grinderlink:state` (the status object, at most 4 per second, only when something changed) and
`evt:grinderlink:result` (a new result). The status object has `active`, `address`, `name`, `link`
(`idle`, `scanning`, `connecting`, `linked`, `ready`), `error`, `found`, `startOutcome`, `startNote`, `stopUnconfirmed`
and, while linked, `device`, `period`, `profiles` and `lastResult`.

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

## Patches (against GaggiMate b78beee)

| Patch | What |
|---|---|
| `0001-settings-grinderlink.patch` | `Settings`: `grinderLinkActive` (`gl_a`), `grinderLinkAddress` (`gl_ad`), `grinderLinkName` (`gl_n`) |
| `0002-controller-register-plugin.patch` | `Controller::setup`: register `GLinkPlugin` when active (inside the device-only block) |
| `0003-webui-grinderlink-settings.patch` | `WebUIPlugin`: accept `grinderLinkActive` in the settings POST; return the three values in the settings JSON |
| `0004-webui-grinderlink-card-hook.patch` | web settings page: render `GrinderLinkCard` in the plugins tab; `grinderLinkActive` is a checkbox key; address and name are read-only keys (not saved, exported or imported) |
| `0005-locales-grinderlink.patch` | the card's strings in the seven catalogs (appended; `lingui extract` will reorder them) |

Address and name are set by the plugin (WebSocket requests), not by the settings POST.
The patches are LF (the upstream blobs are LF); `.gitattributes` keeps them out of line-ending conversion.
The card file itself is copied by the staging script, not patched in.

## Verified

Checked on the developer PC (Windows, MSYS2 g++, PlatformIO 6.2.0 with a private `PLATFORMIO_CORE_DIR`):

- the shared files are byte-identical with the firmware (contract test);
- `scripts/run_host_tests.sh`: 49 host tests pass, compiled with `-Wall -Wextra -Werror`: the request builders
  reproduce the vector requests byte for byte and refuse what the grinder refuses; every response, `periodInfo`, `baseInfo`
  and `grindResult` text of the vectors parses to the values in the vector JSON; the hello is the golden handshake
  frames; sessions with a fake link cover the handshake, the 5 s ready and response timeouts (also across the clock
  wrap), one request at a time, response matching by object name, the stop reply, the start decision (grinding,
  cancelled, timeout), profile and result tracking, a grinder reboot, disconnect and reconnect, ten bad fragments,
  the sequence wrap, a failing link write, and packet sizes of 9, 20, 100 and 512 bytes;
- `scripts/check_against_gaggimate.sh` on a fresh clone of the user's local checkout at b78beee (new empty directory,
  the user's checkout untouched): the five patches apply in order; `pio test -e native -f test_grinderlink` (real Unity)
  passes all 44 plugin test cases; `pio run -e display` links with `GLinkPlugin.cpp` compiled in, without a warning from
  the plugin's files; `pio run -e display-headless` builds as well (display: flash 3140529 bytes, 47.9 %; headless: 2210661 bytes, 33.7 %);
- the three C++ patches also pass `git apply --check` on the user's checkout, read-only (`scripts/check_patches.sh`);
- the card passes a JSX syntax-only transform (esbuild); its request and event names match what the plugin registers (contract test).

## Not verified

- Nothing ran on a GaggiMate display or against a real grinder: the NimBLE connect, service discovery, subscribe, MTU read,
  notification flow, write with response, the idle drop, the scan hand-over with the scale plugin and the stack use of
  `parse_message` on the loop task are compiled but untested (plan step 9).
- The web card was only syntax-checked: not built with the web toolchain (`npm`, Vite, Lingui), not rendered, not run
  against the plugin. The new locale entries were written by hand (translations are mine; none were reviewed) and not
  produced by `lingui extract`.
- `pio test -e native` for all suites: `test_ota_loadtest` does not compile on Windows (`arpa/inet.h`), which is not
  related to this plugin; the other suites passed. The gate is the plugin suite.
- No Linux or macOS run of the staging script; the `sim` env (`display-sim`) was not built (the plugin is excluded there by
  `GAGGIMATE_SIM`).
- Whether three Bluetooth connections are enough in real use (controller, scale, grinder) is unknown.
- The shared reader refuses exponents in numbers. The grinder never sends them; a client of another device would need a
  different reader.
- The patches were not applied to a second GaggiMate version.

## Still to do

1. Hardware loop against the grinder (plan step 9): scan, select, connect, set dose, start with the lead-in, stop, result;
   read the logged stack high-water mark; try it with a scale connected.
2. Build the web UI with GaggiMate's toolchain and run `lingui extract`.
3. Decide with the GaggiMate maintainer where a dose can come from (`controller:grind:start` hook) and whether the
   connection limit should rise.

## Running the checks

```
sh gaggimate-plugin/scripts/run_host_tests.sh                       # needs g++ (C++17)
sh gaggimate-plugin/scripts/check_patches.sh /path/to/gaggimate     # read-only
sh gaggimate-plugin/scripts/check_against_gaggimate.sh DEST [SOURCE]  # DEST: new or empty directory; needs git, pio, network for libraries
python3 -m pytest tools/tests/test_gaggimate_plugin_contract.py     # PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 in CI
```

On Windows keep DEST and `PLATFORMIO_CORE_DIR` paths short (for example a `subst` drive): the Xtensa GCC 8.4 toolchain fails with
`CreateProcess: No such file or directory` on long paths. `PLATFORMIO_CORE_DIR` keeps the toolchain downloads away from
`~/.platformio`.

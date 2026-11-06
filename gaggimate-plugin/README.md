# GrinderLink plugin for GaggiMate (prototype, part 8a)

A GaggiMate display plugin that talks GLink v1 (`docs/GLINK-PROTOCOL.md`) to a Smart Grind By Weight grinder over BLE:
read the grinder state and live weight, send a dose and an optional grind size to the grinder's Profile Dose, start
and stop a grind, read the result. GaggiMate acts as the BLE central; the grinder is the peripheral.

This folder is a prototype kept in the grinder repository. It is to be contributed upstream by the user under
GaggiMate's terms; it carries no licence or CLA text. The `grindControl` object of the protocol is provisional.

**Status: part 8a (core) only.** Everything that needs the GaggiMate source tree to compile (the NimBLE glue
`GLinkPlugin`, the Preact card, the locales) is part 8b and does not exist yet. See "Still to do".

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
  test/test_grinderlink/                 host tests (Unity style) against the shared vectors
  test/host/unity.h                      tiny Unity stand-in for the g++ run
  tools/tests/glink_vectors.json         copy of the shared conformance vectors
  patches/                               unified diffs against GaggiMate b78beee
  scripts/run_host_tests.sh              builds and runs the host tests with g++
  scripts/check_patches.sh               read-only `git apply --check` of the patches against a checkout
```

Shared files (must stay byte-identical with the firmware; `tools/tests/test_gaggimate_plugin_contract.py` compares
them, line endings aside): `bluetooth.h`, `glink_frame.h`, `glink_frame.cpp`, `glink_json.h`, `glink_json.cpp`,
`GLINK-PROTOCOL.md`, `glink_vectors.json`, `glink_vectors.h`. They keep the firmware's names (so the includes inside
them stay valid) instead of the `GLinkFrame.*` names the plan used. The transport carries no grinder-specific object, so
a driver for another device (for example a Motto80) could reuse it unchanged; only the GLink driver exists.
The plugin does not use the firmware's `glink_messages.*` (that is the device side: request parser and response
encoders).

`GLinkMessages` and `GLinkClient` are pure C++ (no Arduino, no NimBLE, no heap). The future glue implements `Sink` (one
RX write) and `Listener`, and feeds `on_connected`, `on_notify`, `on_disconnected` and `tick`.

Things the pure code does on purpose: requests are checked with the grinder's limits before they are sent (dose 5.0
to 60.0 g, size 0.0 to 999.9, unit of up to 6 characters from the allowed set, a unit only together with a size);
one request at a time with a 5 s timeout; `stop` goes out at any time; a successful `start` is followed by the
"did it happen" decision of spec 5.5 (first `periodInfo` past `STARTING`, else lead-in plus 2 s); profiles are re-read
when `profileRev` changes (the first `periodInfo` of a connection counts as changed); the result counter is compared
with `resultSeq`; the sequence counter runs 1 to 65535 like `tools/ble/glink_client.py`; the list reply of `grindProfile`
`get` (a JSON array, which the strict shared reader refuses) is split into entries before the reader sees it; the
grinder's error `message` texts (longer than the reader's 32 byte string limit) are shortened for the reader and kept
whole in `Failure::message`.

Glue notes: `Client` holds its receive buffer and a parse target (about 3 KB), and `parse_message` needs about 3 KB of
stack on top (a JSON document plus a 1 KB copy), so the glue must allocate `Client` statically or on the heap and watch
the stack of the task that calls `on_notify`. `Client::start` always sends `expectDose`. The `grinderLinkActive` toggle
registers the plugin in `Controller::setup`, so it takes effect after a reboot.

## Patches (against GaggiMate b78beee)

| Patch | What |
|---|---|
| `0001-settings-grinderlink.patch` | `Settings`: `grinderLinkActive` (`gl_a`), `grinderLinkAddress` (`gl_ad`), `grinderLinkName` (`gl_n`) |
| `0002-controller-register-plugin.patch` | `Controller::setup`: register `GLinkPlugin` when active (inside the device-only block) |
| `0003-webui-grinderlink-settings.patch` | `WebUIPlugin`: accept `grinderLinkActive` in the settings POST; return the three values in the settings JSON |

Address and name are set by the plugin (WebSocket requests, part 8b), not by the settings POST.
The patches are LF (the upstream blobs are LF); `.gitattributes` keeps them out of line-ending conversion.

## Verified

Checked in this repository on the developer PC (Windows, MSYS2 g++, no GaggiMate build):

- the shared files are byte-identical with the firmware (contract test);
- `scripts/run_host_tests.sh`: 39 host tests pass, compiled with `-Wall -Wextra -Werror`: the request builders
  reproduce the vector requests byte for byte and refuse what the grinder refuses; every response, `periodInfo`, `baseInfo`
  and `grindResult` text of the vectors parses to the values in the vector JSON; the hello is the golden handshake
  frames; sessions with a fake link cover the handshake, the 5 s ready and response timeouts (also across the clock
  wrap), one request at a time, response matching by object name, the stop reply, the start decision (grinding,
  cancelled, timeout), profile and result tracking, a grinder reboot, disconnect and reconnect, ten bad fragments,
  the sequence wrap, a failing link write, and packet sizes of 9, 20, 100 and 512 bytes;
- the three patches pass `git apply --check` on a GaggiMate checkout at b78beee (the user's checkout, read-only), alone and
  together (`scripts/check_patches.sh`).

## Not verified

- Nothing was compiled inside the GaggiMate tree: no `pio test -e native`, no `pio run -e display`, no
  `display-headless`. The patches add an `#include` of `GLinkPlugin.h`, which does not exist yet, so the patched tree
  does not build until part 8b.
- No NimBLE code exists yet; nothing ran on a GaggiMate display or against a real grinder.
- Whether the display build has enough BLE connections for a third central link
  (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` of at least 3) is unknown; the plugin is meant to connect on demand.
- The shared reader refuses exponents in numbers. The grinder never sends them; a client of another device would need a
  different reader.
- The patches were not applied to a second GaggiMate version.

## Still to do (part 8b)

1. `GLinkPlugin.{h,cpp}`: `Plugin` subclass with the NimBLE central (scan by the name `GrindByWeight`, connect on demand,
   discover the service, check RX writable and TX notify, subscribe, ATT MTU to packet size, `Sink` and `Listener`,
   `tick` from `loop()`), WebSocket requests and events `req:grinderlink:*` / `evt:grinderlink:*`, persistence of
   address and name, the sketch hook on `controller:grind:start` / `controller:grind:end`; `#ifndef GAGGIMATE_SIM`.
2. `web/src/pages/Settings/GrinderLinkCard.jsx` (toggle, Scan/Select/Forget, dose and grind size, Start/Stop, status,
   last result), the patch that hooks it into the settings page, the locale strings.
3. A check script that stages the plugin into a checkout, runs `pio test -e native` (the tests include `glink_vectors.h`,
   which looks for `tools/tests/glink_vectors.json` below the working directory) and `pio run -e display`
   (and `display-headless` if time allows), with a BLOCKED report if the toolchain or network is missing.
4. Apply the patches to a clean clone at b78beee as well, and build.
5. Hardware loop against the grinder (plan step 9).

## Running the checks

```
sh gaggimate-plugin/scripts/run_host_tests.sh                       # needs g++ (C++17)
sh gaggimate-plugin/scripts/check_patches.sh /path/to/gaggimate     # read-only
python3 -m pytest tools/tests/test_gaggimate_plugin_contract.py     # PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 in CI
```

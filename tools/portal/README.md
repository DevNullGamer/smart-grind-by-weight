# Web panel page and dev server

The grinder serves a small web panel (page plus JSON API, contract in `docs/PORTAL-API.md`). This folder holds the
tools for working on the page without a grinder.

> **With the web interface on, anyone on the local network can change settings and start the grinder.** There is no
> login and no token for the panel; the only gate is Menu -> WiFi -> Web interface on the grinder's screen.

## Files

| File | What |
|---|---|
| `web/portal/page.src.html` | The page shell: landmarks, skip link, inline SVG icon sprite, status bar with STOP, name, state and weight, navigation (bottom bar, pill bar or sidebar), panels (Grind with the Last grind card, Settings, Insights, Device with the About card), and one marker per part below. |
| `web/portal/portal.css` | Design tokens (light and dark via `prefers-color-scheme`) and layout for phone, tablet and desktop. Marker `/*@@PORTAL_CSS@@*/`. |
| `web/portal/portal-client.js` | The API client (request queue, error mapping; `toCurl` is a tested helper the page does not use). Tested by `node --test`. Never logs, never touches storage, no token. Marker `/*@@PORTAL_CLIENT@@*/`. |
| `web/portal/portal-view.js` | Pure view logic (formatting, controls and their reasons, banners, the lead-in, the settings form). Loadable by node and tested there. Marker `/*@@PORTAL_VIEW@@*/`. |
| `web/portal/portal-app.js` | DOM glue only (events, `textContent`); logic belongs in the view or client module. Marker `/*@@PORTAL_APP@@*/`. |
| `web/portal/index.html` | **Generated and committed**: the page with all parts inlined. This is the file the firmware embeds (`tools/build-scripts/embed_portal.py`, 64 KB gzip budget; the limit in `test_page_fits_the_embed_budget` is 48 KB since chg021 V0; the page is about 35 KB gzip). |
| `tools/portal/build_page.py` | Builds `index.html` from the sources. Run it after editing any of them (`--check` only reports). It fails on a missing or duplicated marker and on `</script` / `</style` inside a part. A pytest fails when `index.html` is stale. The build strips full-line `//` comments (JS), `/* */` blocks (CSS, markers kept) and indentation from the parts; the sources keep them. `--no-strip` builds the unstripped page for debugging only (it writes `index.html` unless `--check`; rebuild without the flag before committing). |
| `tools/portal/snap.py` | Dev tool, not run by CI: starts the dev server, sets a state, and takes headless Chrome/Edge screenshots per width, colour scheme and state (`--list`, `--step`, `--out`, `--widths`, `--schemes`, `--states`, `--reduced-motion`, `--browser`). Output goes to `<out>/<step>/`, default under `~/.claude/workspace/chgreq/smart-grind-by-weight/chg021/shots`, outside the repo. It opens the Grind section only; Edge headless cannot go below about 500 px, so true 320 and 360 px shots need Chrome with device-metrics emulation over the DevTools protocol. |
| `tools/portal/portal_model.py` | The contract as pure Python (scanner, field table, clamping, guards, decisions). Tested against `tools/tests/portal_api_vectors.json`, the vectors the firmware tests use. |
| `tools/portal/dev_server.py` | Local server: the page plus a simulated grinder behind the real contract. Standard library only. |

## Run the page with no hardware

```
python3 tools/portal/dev_server.py            # http://127.0.0.1:8080
python3 tools/portal/dev_server.py --port 9000 --ui edit --start-block load_cell_no_data
```

Open **http://127.0.0.1:8080** (use the IP address: like the grinder, the server refuses `localhost` as a DNS name
with `bad_host`). The server rebuilds the page from the sources at start, so restart it after editing them.

What it simulates:

- Everything in the contract: Host / Origin / `Content-Type` / `X-Grinder` guards, body rules, clamping and
  `adjusted`, profile targets refused out of range, `device_name` rules (the reply carries `address` on a change),
  the decision order (`command_in_flight`, `update_in_progress`, `busy`, `not_ready`, the start / motor test blocks)
  and `stop` always accepted.
- A firmware update holding the grinder (`--update-held`): the status reports `update_in_progress` in `start_block`
  and `motor_test_block`, as the firmware does, except where a disabled motor or a latched fault comes first.
- A grind: `start` is accepted with `lead_in_ms` 3000 (the lead-in counts as in flight; `stop` cancels it). While the
  lead-in waits, every look at the simulator decides the start again, as the firmware does every frame: the start is
  **dropped** at once (the screen stays as it is, nothing runs, nothing is in flight) if the screen is no longer READY,
  an update holds the grinder or a start block applies. `drop_start` drops it when the lead-in ends. Otherwise
  it runs at about 3 g/s (weight mode) or for the profile time (time mode), shows `grind_complete` for 3 s (writes get
  `not_ready` meanwhile) and returns to `ready`. `motor_test` keeps the motor busy for 1.5 s (firmware: 1.0 s).
  `tare` zeroes the weight.
- Insights: `/api/stats` and `/api/history` follow the simulated grinds (a finished grind adds an entry with its target, a final weight 0.1 g over in two pulses, result `complete`; a stopped grind adds nothing; the newest 10 are kept). The history lives in memory only.
- `/api/diagnostics` answers fixed example values (a calibrated V2 board found by probe, 10 samples per second, Bluetooth off, signal -58 dBm); change them with the `diagnostics` key of `/dev/state`.
- Settings live in memory only; a restart resets them.

Not simulated: STOP bypassing `latency_ms` (the grinder's STOP skips the UI task mailbox), real timing, hardware faults, gzip (the page is sent plain), the mDNS rename (only the reply), the
touch cancel of the lead-in and the overlay, and the flash store behind the history (the list lives in memory; `history_persistent` only
changes what the route reports).

### Flags

| Flag | Default | Simulates |
|---|---|---|
| `--host ADDR` | `127.0.0.1` | Address to listen on. `0.0.0.0` opens the simulated grinder to the whole network (the server warns). |
| `--port N` | `8080` | Port to listen on. |
| `--web-off` | off | Web interface switch off: everything but `/api/info` answers 403 `web_interface_off`. |
| `--ui STATE` | `ready` | Screen state at start: `ready`, `menu`, `edit`, `calibration`, `confirm`, `purge_confirm`, `autotuning`, `ota_update`. |
| `--update-held` | off | A firmware update holds the grinder (`update_in_progress`). |
| `--start-block CODE` | none | Grinder-side refusal of a start: `motor_disabled`, `update_in_progress`, `load_cell_not_connected`, `load_cell_no_data`, `load_cell_rate_invalid`, `busy`, `motor_fault`. |
| `--motor-test-block CODE` | none | Grinder-side refusal of the motor test (same codes). |
| `--drop-start` | off | Every remote start is dropped when its lead-in ends (another screen took over). |
| `--legacy-status` | off | `GET /api/status` leaves out the `warnings` member, like a firmware from before it existed. The page then shows no warnings list; the `warnings` key of `/dev/state` still holds its value. |
| `--no-insights` | off | `GET /api/stats` and `GET /api/history` answer 404 from the server (no JSON), like a firmware from before they existed. The page keeps the Insights tab hidden and shows no error. |
| `--no-history` | off | Only `GET /api/history` answers 404, like a firmware from before chg016 step 12: the Insights tiles work, the recent-grinds list is hidden. |
| `--no-diagnostics` | off | Only `GET /api/diagnostics` answers 404 from the server (no JSON), like a firmware from before chg021 step G3. Everything else is unchanged. |

The default listens on `127.0.0.1` only.

### `/dev/state`

Dev-only, **not part of the contract**. `GET` reads the simulated state, `POST` changes it. A `POST` needs the same two
headers as the API (`Content-Type: application/json` and `X-Grinder: 1`); there is no Host or Origin check, and `GET`
is open. `/dev/state` itself is never delayed by `latency_ms`.

```
curl http://127.0.0.1:8080/dev/state
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://127.0.0.1:8080/dev/state -d '{"web_interface":false}'
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://127.0.0.1:8080/dev/state -d '{"latency_ms":6000}'
```

A `POST` answers the whole state. An unknown key, a wrong type or an out-of-range value is `400` with
`{"error":"dev_state","reason":...}` and changes nothing.

| Key | Type | Effect |
|---|---|---|
| `web_interface` | bool | The Web interface switch (`false`: 403 `web_interface_off` on everything but `/api/info`). |
| `ui_state` | one of the screen states above | The screen. Changing it ends a grind or a motor run; a remote start still in its lead-in is dropped at once unless the screen is `ready`. |
| `update_held` | bool | A firmware update holds the grinder (`update_in_progress`). |
| `in_flight` | bool | Another request is still being handled: every command and write answers 409 `command_in_flight` (`stop` is never refused). |
| `start_block` | `null` or a block code | The start refusal the status reports and a start answers with. |
| `motor_test_block` | `null` or a block code | The same for the motor test. |
| `drop_start` | bool | Every remote start is dropped when its lead-in ends (`--drop-start`). |
| `ui_busy` | bool | `GET` and `PATCH /api/settings`, `GET /api/stats`, `GET /api/history` and `GET /api/diagnostics` answer 503 `ui_busy`. |
| `store_failed` | bool | A `PATCH` that would change a value answers 500 `store_failed` and stores nothing. A `PATCH` that changes nothing still succeeds. |
| `latency_ms` | whole number 0 to 30000 | Every reply except `/dev/state` waits this long before it is answered. This delays STOP too; on the grinder STOP bypasses the UI task mailbox and is not slowed (a known simulator difference). |
| `warnings` | list of warning codes, `[]` for none | The active grinder warnings the status reports in `warnings`, always in priority order and without duplicates: `board_unrecognized`, `motor_fault`, `motor_disabled`, `hx711_not_connected`, `hx711_no_data`, `hx711_sample_rate_invalid`, `mechanical_instability`, `load_cell_noisy`, `load_cell_not_calibrated`. The Grind tab lists them, highest priority first. |
| `insights` | bool | `false` is `--no-insights`: `/api/stats` and `/api/history` answer 404 (back on with `true`). |
| `history_route` | bool | `false` is `--no-history`: only `/api/history` answers 404 (back on with `true`). |
| `history_persistent` | bool | The `persistent` member of `/api/history`. `false` is a grinder whose flash store is unavailable: the list shows a "not being saved" notice. |
| `history_fill` | whole number 0 to 10 | Replaces the history with that many sample grinds (newest first, ages from 2 minutes to a day, every result, a time-mode grind, positive and negative errors; the simulated uptime is raised to fit the oldest age). `10` is a full list, `0` empties it. |
| `history_clear` | `true` | Empties the history (`false` is refused). A finished grind adds an entry again. |
| `history_restart` | `true` | Marks every entry as recorded before the last restart, as a reboot does: `age_s` becomes `null` (the page says "before the last restart"). |
| `stats` | object of statistics members | Sets any of the 13 members of `/api/stats` (numbers from 0 to 1e12; `motor_runtime_s` and `powered_minutes` keep growing from the value set) to see unusual numbers. The `GET` shows the current document under `stats`. |
| `stats_reset` | `true` | Zeroes every statistic, as after a reset on the grinder (`false` is refused). The next finished grind is the first one. |
| `diagnostics_route` | bool | `false` is `--no-diagnostics`: only `/api/diagnostics` answers 404 (back on with `true`). |
| `diagnostics` | object of diagnostics members | Sets any of the 11 members of `/api/diagnostics` (`calibrated`, `noise_ok`, `push_armed`: bool; `cal_factor`, `std_dev_g`, `sample_rate_sps`, `motor_latency_ms`: any number, also `NaN`, `Infinity` and huge values, which the reply bounds like the firmware; `wifi_rssi_dbm`: `null` or a whole number; `ble`, `board`, `board_source`: one of the fixed words). The `GET` shows the document the route would answer under `diagnostics`. |
| `weight` | `null` or a number from -1000 to 1000 | Holds the weight the status reports at this value (`null` releases it). A tare sets the held value to 0. |

## Reproduce each page state

The states the page shows, and how to get each one from the dev server. Each `POST` is one `curl` with the two headers
above.

| Page state | How to reproduce | What to expect |
|---|---|---|
| Offline (fetch error or timeout) | Stop the server (Ctrl+C). Start it again and press "Retry now". | Chip "Offline", last values greyed with "Last seen N s ago", controls disabled except STOP. |
| Web interface off (403) | `--web-off`, or `{"web_interface":false}` (back on with `true`). | When the switch flips while the page is open: full-width "The web interface is switched off" card; polling slows and resumes by itself. Loaded with `--web-off` (or reloaded while off) the grinder refuses the page itself, as the firmware does: the raw `web_interface_off` JSON, no card. |
| Busy (`busy`, 409 `busy`) | Start a grind (`{"cmd":"start","profile":1}`): busy from the end of the 3 s lead-in until the grind ends (about 6 s for a double at 18 g). `stop` ends it early. A motor test (`{"cmd":"motor_test"}`) is busy for 1.5 s. | Start, Tare, Motor test and Save disabled with "A grind or motor run is active"; STOP emphasised. |
| Not ready (409 `not_ready`) | `{"ui_state":"edit"}` (back to `ready`). | Start, Tare and Motor test disabled: "Go back to the main screen on the grinder". |
| Update running (`update_in_progress`) | `--update-held`, or `{"update_held":true}` (back with `false`). | Banner "Firmware update running"; everything but STOP disabled. |
| Motor fault (`motor_fault`) | `{"motor_test_block":"motor_fault"}` (back with `null`). | Error banner with the restart hint; Motor test disabled with the reason. |
| Motor disabled (`motor_disabled`) | `{"start_block":"motor_disabled","motor_test_block":"motor_disabled"}` (and `null` again; the firmware sets both). | Error banner "Motor disabled"; Start and Motor test disabled with the reason. |
| Warnings list | `{"warnings":["motor_fault","load_cell_not_calibrated"]}` (back with `[]`). `--legacy-status` leaves the member out. | A "Grinder warnings" list on the Grind tab (soft alert rows with a level icon, a tag and what to do), highest priority first; hidden when empty or when the member is absent. `motor_fault` and `motor_disabled` also give the error banner, and Start and Motor test are disabled with the reason; the list does not repeat the banner's code. |
| `ui_busy` 503 | `{"ui_busy":true}`, then reload or change a setting. | Settings cannot be read: the Settings tab stays on "Loading the settings." (no warning); the poller retries. Back to `false` to clear. |
| `command_in_flight` 409 | `{"in_flight":true}`, then press Start or Tare. | "The grinder is still busy with the previous request." STOP still works. Back to `false`. |
| `store_failed` 500 | `{"store_failed":true}`, then change a setting and Save. | Error message under the Save bar ("... reload the settings"); the page keeps the value you typed, nothing is stored. The settings are read again in the background, and a later reply never overwrites your entry. |
| `invalid_device_name` 400 | Type `Kitchen` as the device name and Save (or `PATCH` `{"device_name":"Kitchen"}`). | Message beside the device name field. |
| `out_of_range` 400 | Set a single or double target below 5 g and Save (or `PATCH` `{"single_weight_g":4}`). | Message beside the offending field. |
| Dropped start | `--drop-start` (or `{"drop_start":true}`), then press Start. | The countdown runs out and then "The start was dropped on the grinder (check its screen)". |
| Slow grinder, in progress and timeout | `{"latency_ms":1500}` shows the requests in progress. `{"latency_ms":6000}` gives the timeout: the client waits 5 s, then "The grinder took too long to answer". Reset with `{"latency_ms":0}`. | In progress: the page shows no waiting state; Start, Tare and Motor test stay enabled until the reply arrives. Timeout: the page takes the offline path (chip, stale readout, backed-off polling). |
| Insights tiles | Open the page (the tab shows as soon as the first reply arrives), pick Insights. A finished grind (`start`, then wait out the lead-in and the grind) changes the numbers within a few seconds; `{"stats":{"total_grinds":4294967295,"total_weight_kg":1e9,"powered_minutes":257698037759}}` gives very large numbers. | Seven tiles (grinds, coffee ground, motor run time, powered on, average error, average pulses, extra time-mode pulses) and the split by final weight; "Updated N s ago". |
| Empty data (statistics) | `{"stats_reset":true}`. | "No grinds recorded yet. The totals appear after the first grind." and no tiles. |
| Statistics not available | `--no-insights`, or `{"insights":false}`. | The Insights tab stays hidden, no error; if the tab was shown before, it disappears. |
| Statistics cannot be read | `{"ui_busy":true}` (back with `false`), or stop the server while the tab is shown. | The tiles stay with "The numbers could not be refreshed ... from N s ago" (or "The grinder is not reachable"); the page asks again every 10 s. |
| Empty history | Open the page on a fresh server (the list starts empty), or `{"history_clear":true}`. | In Insights, under the tiles: "No grinds recorded yet. Finished grinds are listed here and kept across restarts." With `{"history_persistent":false}` too: "No grinds recorded since the grinder started" and the not-saved notice. |
| Recent grinds | `{"history_fill":3}` for three grinds, `{"history_fill":10}` for a full list. A finished grind (`start`, wait) adds the newest one on top within a few seconds. | A list, newest first: relative age, profile, result badge, final weight against the target and the error with its sign, plus a bar chart of the errors (up, down, or on target). Overshoot and timeout grinds carry a visible badge. |
| History after a restart | `{"history_restart":true}`. | Every age reads "before the last restart". |
| History not being saved | `{"history_persistent":false}`. | A notice above the list: "History is not being saved on the grinder; it is lost at the next restart." |
| History not available | `--no-history`, or `{"history_route":false}`. | The tiles stay, the recent-grinds list and its heading are hidden, no error. |
| Last grind, running | Press Start (Double): after the 3 s lead-in and about a second of grinding. | A "This grind" card between the hero and Profiles with the weight curve (a dashed target line when this page started the grind in weight mode) and a caption "Weight from ... to ... in ...". Hidden until three weights are in. |
| Last grind, finished | Let the grind above finish (the grinder returns to `ready`). | The card becomes "Last grind": final weight against the target, result badge, error, time, pulses, motor time and "just now" (it keeps ageing), the curve stays. One `GET /api/history` follows the end; the card says "Reading the result from the grinder." until it arrives. |
| Last grind, stopped | Press Start, then STOP while it grinds (`{"history_fill":3}` first so there is an older grind). | The curve stays; after the second look at the history (about 6 s) a neutral "Not recorded" badge with "This grind did not finish, so the grinder did not record it. It was probably stopped." The older grind is never shown as the result. |
| Last grind, no history route | `--no-history`, then a grind. | The curve stays with "This grinder firmware does not list finished grinds, so there is no result to show." |
| Device, About card | Open the Device tab. The dev server answers `/api/info` with `board` "dev" and `confirm` true. | Rows Name, Address, Firmware, Build, Board, Uptime (counts, hidden while the grinder is not reachable) and "Confirm on the grinder: On" with a sentence. A member that is missing, null or of the wrong type hides its row (try it by intercepting `/api/info` in the browser's developer tools). |

## Manual browser check

A human pass after changing the page (the automated tests do not render it). Start the dev server, then:

1. Open http://127.0.0.1:8080 at 320 and 360 px (phone), 768 px (tablet), 1024, 1280 and 1440 px (desktop) wide, in the
   light and the dark scheme (browser or OS setting), on each of the four sections. Nothing overflows sideways, text is
   readable, STOP is always reachable. `tools/portal/snap.py` takes the Grind-section shots; for the other sections and
   the true narrow widths drive headless Chrome over the DevTools protocol (device-metrics emulation, then click the tab).
2. Walk every page state in the table above at each width and in both schemes; the chip, banner and disabled reasons
   match the "What to expect" column.
3. Keyboard only: Tab through the page in order (skip link, STOP, navigation, controls, save bar). Every focus ring is
   visible, including on the checked segmented control and on a busy STOP; Enter and Space activate buttons.
4. Offline recovery: stop the server (Ctrl+C), see "Offline" and the stale readout, restart it, press "Retry now" and
   check the page recovers without a reload.

## Run the page against a real grinder

Turn Web interface on at the grinder (Menu -> WiFi), then open `http://grindbyweight.local` (or the IP address the
WiFi page shows) in a browser on the same network. The page is served by the grinder, so it is same origin. To work
on a changed page without flashing, use the dev server; to try the embedded build, build and upload the firmware as
usual.

## Page notes

- No token, no stored state: the page keeps nothing in `sessionStorage`, `localStorage` or cookies.
- Values are written with `textContent` only; the page never builds markup from device data.

## Tests

```
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest tools/tests/test_portal_dev_server.py
node --test tools/tests/portal-client.test.js tools/tests/portal-view.test.js tools/tests/portal-lastgrind.test.js
```

`test_portal_dev_server.py` also checks that every flag and every `/dev/state` key is documented here.

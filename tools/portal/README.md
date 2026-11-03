# Web panel page and dev server

The grinder serves a small web panel (page plus JSON API, contract in `docs/PORTAL-API.md`). This folder holds the
tools for working on the page without a grinder.

> **With the web interface on, anyone on the local network can change settings and start the grinder.** There is no
> login and no token for the panel; the only gate is Menu -> WiFi -> Web interface on the grinder's screen.

## Files

| File | What |
|---|---|
| `web/portal/page.src.html` | The page shell: landmarks, skip link, inline SVG icon sprite, header with STOP and the status strip, tab bar, panels, and one marker per part below. |
| `web/portal/portal.css` | Design tokens (light and dark via `prefers-color-scheme`) and layout for phone, tablet and desktop. Marker `/*@@PORTAL_CSS@@*/`. |
| `web/portal/portal-client.js` | The API client (request queue, error mapping, curl export). Tested by `node --test`. Never logs, never touches storage, no token. Marker `/*@@PORTAL_CLIENT@@*/`. |
| `web/portal/portal-view.js` | Pure view logic (formatting, controls and their reasons, banners, the lead-in, the settings form). Loadable by node and tested there. Marker `/*@@PORTAL_VIEW@@*/`. |
| `web/portal/portal-app.js` | DOM glue only (events, `textContent`); logic belongs in the view or client module. Marker `/*@@PORTAL_APP@@*/`. |
| `web/portal/index.html` | **Generated and committed**: the page with all parts inlined. This is the file the firmware embeds (`tools/build-scripts/embed_portal.py`, 64 KB gzip budget; chg016 aims below 32 KB). |
| `tools/portal/build_page.py` | Builds `index.html` from the sources. Run it after editing any of them (`--check` only reports). It fails on a missing or duplicated marker and on `</script` / `</style` inside a part. A pytest fails when `index.html` is stale. |
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
- A grind: `start` is accepted with `lead_in_ms` 3000 (the lead-in counts as in flight; `stop` cancels it). When the
  lead-in runs out the grinder decides again: the start is **dropped** (the screen stays as it is, nothing runs) if
  the screen is no longer READY, an update holds the grinder, a start block applies, or `drop_start` is set. Otherwise
  it runs at about 3 g/s (weight mode) or for the profile time (time mode), shows `grind_complete` for 3 s (writes get
  `not_ready` meanwhile) and returns to `ready`. `motor_test` keeps the motor busy for 1.5 s (firmware: 1.0 s).
  `tare` zeroes the weight.
- Settings live in memory only; a restart resets them.

Not simulated: real timing, hardware faults, gzip (the page is sent plain), the mDNS rename (only the reply), the
touch cancel of the lead-in and the overlay, and the Insights data (the routes come in later steps).

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
| `ui_state` | one of the screen states above | The screen. Changing it ends a grind or a motor run; a remote start still in its lead-in is decided when the lead-in ends. |
| `update_held` | bool | A firmware update holds the grinder (`update_in_progress`). |
| `in_flight` | bool | Another request is still being handled: every command and write answers 409 `command_in_flight` (`stop` is never refused). |
| `start_block` | `null` or a block code | The start refusal the status reports and a start answers with. |
| `motor_test_block` | `null` or a block code | The same for the motor test. |
| `drop_start` | bool | Every remote start is dropped when its lead-in ends (`--drop-start`). |
| `ui_busy` | bool | `GET` and `PATCH /api/settings` answer 503 `ui_busy`. |
| `store_failed` | bool | A `PATCH` that would change a value answers 500 `store_failed` and stores nothing. A `PATCH` that changes nothing still succeeds. |
| `latency_ms` | whole number 0 to 30000 | Every reply except `/dev/state` waits this long before it is answered. |
| `weight` | `null` or a number from -1000 to 1000 | Holds the weight the status reports at this value (`null` releases it). A tare sets the held value to 0. |

## Reproduce each page state

The states the page shows, and how to get each one from the dev server. Each `POST` is one `curl` with the two headers
above.

| Page state | How to reproduce | What to expect |
|---|---|---|
| Offline (fetch error or timeout) | Stop the server (Ctrl+C). Start it again and press "Retry now". | Chip "Offline", last values greyed with "Last seen N s ago", controls disabled except STOP. |
| Web interface off (403) | `--web-off`, or `{"web_interface":false}` (back on with `true`). | Full-width "The web interface is switched off" card; polling slows and resumes by itself. |
| Busy (`busy`, 409 `busy`) | Start a grind (`{"cmd":"start","profile":1}`): busy from the end of the 3 s lead-in until the grind ends (about 6 s for a double at 18 g). `stop` ends it early. A motor test (`{"cmd":"motor_test"}`) is busy for 1.5 s. | Start, Tare, Motor test and Save disabled with "A grind or motor run is active"; STOP emphasised. |
| Not ready (409 `not_ready`) | `{"ui_state":"edit"}` (back to `ready`). | Start, Tare and Motor test disabled: "Go back to the main screen on the grinder". |
| Update running (`update_in_progress`) | `--update-held`, or `{"update_held":true}` (back with `false`). | Banner "Firmware update running"; everything but STOP disabled. |
| Motor fault (`motor_fault`) | `{"motor_test_block":"motor_fault"}` (back with `null`). | Error banner with the restart hint; Motor test disabled with the reason. |
| Motor disabled (`motor_disabled`) | `{"start_block":"motor_disabled"}` (and `null` again). | Error banner "Motor disabled"; Start and Motor test disabled with the reason. |
| `ui_busy` 503 | `{"ui_busy":true}`, then reload or change a setting. | Settings cannot be read or saved; a soft warning; the poller retries. Back to `false` to clear. |
| `command_in_flight` 409 | `{"in_flight":true}`, then press Start or Tare. | "The grinder is still busy with the previous request." STOP still works. Back to `false`. |
| `store_failed` 500 | `{"store_failed":true}`, then change a setting and Save. | Error message, the settings are reloaded and show what is stored; nothing changed. |
| `invalid_device_name` 400 | Type `Kitchen` as the device name and Save (or `PATCH` `{"device_name":"Kitchen"}`). | Message beside the device name field. |
| `out_of_range` 400 | Set a single or double target below 5 g and Save (or `PATCH` `{"single_weight_g":4}`). | Message beside the offending field. |
| Dropped start | `--drop-start` (or `{"drop_start":true}`), then press Start. | The countdown runs out and then "The start was dropped on the grinder (check its screen)". |
| Slow grinder, in progress and timeout | `{"latency_ms":1500}` shows the requests in progress. `{"latency_ms":6000}` gives the timeout: the client waits 5 s, then "The grinder took too long to answer". Reset with `{"latency_ms":0}`. | In progress: buttons wait with their reason. Timeout: the page takes the offline path (chip, stale readout, backed-off polling). |
| Empty data (history, statistics) | Not reproducible yet: the dev server has no history or statistics routes until chg016 steps 8 and 10. | "No grinds recorded since the grinder started" and "Statistics not available" appear then. |

## Run the page against a real grinder

Turn Web interface on at the grinder (Menu -> WiFi), then open `http://grindbyweight.local` (or the IP address the
WiFi page shows) in a browser on the same network. The page is served by the grinder, so it is same origin. To work
on a changed page without flashing, use the dev server; to try the embedded build, build and upload the firmware as
usual.

## Page notes

- No token, no stored state: the page keeps nothing in `sessionStorage`, `localStorage` or cookies.
- "Copy as curl" fills a read-only box with the command for the last action (or the pending changes) and tries to
  copy it. Browsers block the clipboard API on plain HTTP, so the text is selected and copied with the legacy
  command, with a "Press Ctrl+C" hint if that is blocked as well.
- Values are written with `textContent` only; the page never builds markup from device data.

## Tests

```
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest tools/tests/test_portal_dev_server.py
node --test tools/tests/portal-client.test.js tools/tests/portal-view.test.js
```

`test_portal_dev_server.py` also checks that every flag and every `/dev/state` key is documented here.

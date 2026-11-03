# Web panel reference page and dev server

The grinder serves a small web panel (page plus JSON API, contract in `docs/PORTAL-API.md`). This folder holds the
tools for working on the page without a grinder.

> **With the web interface on, anyone on the local network can change settings and start the grinder.** There is no
> login and no token for the panel; the only gate is Menu -> WiFi -> Web interface on the grinder's screen.

## Files

| File | What |
|---|---|
| `web/portal/page.src.html` | The page shell: landmarks, skip link, inline SVG icon sprite, header with STOP, tab bar, panels, and one marker per part below. |
| `web/portal/portal.css` | Design tokens (light and dark via `prefers-color-scheme`) and layout for phone, tablet and desktop. Marker `/*@@PORTAL_CSS@@*/`. |
| `web/portal/portal-client.js` | The API client (request queue, error mapping, curl export). Tested by `node --test`. Never logs, never touches storage, no token. Marker `/*@@PORTAL_CLIENT@@*/`. |
| `web/portal/portal-view.js` | Pure view helpers (formatting, later the control and state logic), loadable by node. Marker `/*@@PORTAL_VIEW@@*/`. |
| `web/portal/portal-app.js` | DOM glue only (events, `textContent`); logic belongs in the view or client module. Marker `/*@@PORTAL_APP@@*/`. |
| `web/portal/index.html` | **Generated and committed**: the page with all parts inlined. This is the file the firmware embeds (`tools/build-scripts/embed_portal.py`, 64 KB gzip budget; chg016 aims below 32 KB). |
| `tools/portal/build_page.py` | Builds `index.html` from the sources. Run it after editing any of them (`--check` only reports). It fails on a missing or duplicated marker and on `</script` / `</style` inside a part. A pytest fails when `index.html` is stale. |
| `tools/portal/portal_model.py` | The contract as pure Python (scanner, field table, clamping, guards, decisions). Tested against `tools/tests/portal_api_vectors.json`, the vectors the firmware tests use. |
| `tools/portal/dev_server.py` | Local server: the page plus a simulated grinder behind the real contract. Standard library only. |

## Run the page with no hardware

```
python3 tools/portal/dev_server.py            # http://127.0.0.1:8080
python3 tools/portal/dev_server.py --port 9000 --ui menu --start-block load_cell_no_data
```

Open **http://127.0.0.1:8080** (use the IP address: like the grinder, the server refuses `localhost` as a DNS name
with `bad_host`). The server rebuilds the page from the sources at start, so restart it after editing them.

What it simulates:

- Everything in the contract: Host / Origin / `Content-Type` / `X-Grinder` guards, body rules, clamping and
  `adjusted`, profile targets refused out of range, `device_name` rules (the reply carries `address` on a change),
  the decision order (`command_in_flight`, `update_in_progress`, `busy`, `not_ready`, the start / motor test blocks)
  and `stop` always accepted.
- A grind: `start` is accepted with `lead_in_ms` 3000 (the lead-in counts as in flight; `stop` cancels it), then
  runs at about 3 g/s (weight mode) or for the profile time (time mode), shows `grind_complete` for 3 s (writes get
  `not_ready` meanwhile) and returns to `ready`. `motor_test` keeps the motor busy for 1.5 s (firmware: 1.0 s). `tare` zeroes the weight.
  Settings live in memory only; a restart resets them.

Not simulated: real timing, hardware faults, gzip (the page is sent plain), the mDNS rename (only the reply), the touch cancel of the lead-in and the overlay.

### Flags and dev-only controls

Flags: `--web-off` (Web interface switch off, everything but `/api/info` answers 403 `web_interface_off`),
`--ui {ready,menu,edit,...}`, `--update-held`, `--start-block CODE`, `--motor-test-block CODE`, `--host`, `--port`.
The default listens on `127.0.0.1` only; `--host 0.0.0.0` opens the simulated grinder to the whole network.

While running, change the simulated state with `/dev/state` (dev-only, **not part of the contract**; POST needs the same two headers as the API, there is no Host/Origin check, GET is open):

```
curl http://127.0.0.1:8080/dev/state
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://127.0.0.1:8080/dev/state -d '{"web_interface":false}'
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://127.0.0.1:8080/dev/state -d '{"ui_state":"edit","update_held":false,"in_flight":true}'
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://127.0.0.1:8080/dev/state -d '{"start_block":"motor_disabled","motor_test_block":"motor_fault"}'
```

Keys: `web_interface`, `update_held`, `in_flight` (bool), `ui_state` (`ready`, `menu`, `edit`, `calibration`,
`confirm`, `purge_confirm`, `autotuning`, `ota_update`), `start_block` / `motor_test_block` (`null` or an error code).

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
node --test tools/tests/portal-client.test.js
```

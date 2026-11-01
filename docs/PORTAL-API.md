# Web panel API (contract)

The grinder serves an internal web panel and a small JSON API on the **local network**. This document is the
contract between the firmware and any page that talks to it (the reference page, a replacement portal, scripts).
The same rules are executable: `tools/tests/portal_api_vectors.json` holds the shared test vectors, run by
`test_native/test_portal_api.cpp` and `test_portal_guard.cpp` against `src/network/portal_api.*` and
`src/network/portal_guard.*`, and `tools/tests/test_portal_api_contract.py` keeps the vectors in line with the
firmware sources (menu tables, `UiSettings`, `user.h`, `ProfileController`).

> **Safety: with the Web interface on, anyone on the local network can change settings and start the grinder.**
> There is no login. Remote actions (including starting the motor, with nobody at the grinder) are possible from
> every device on the same WiFi. The only protection is the **Web interface** switch on the grinder's own
> screen (Menu -> WiFi). Leave it off unless you want this. Do not forward port 80 to the grinder.

## 1. Transport and the Web interface switch

- HTTP (no TLS), port 80, `http://grindbyweight.local` (mDNS name, see `wifi/name`) or the grinder's IP address,
  which the WiFi page shows. Requests and replies are UTF-8 JSON. API replies carry `Cache-Control: no-store`.
- The panel is enabled only by the **Web interface** toggle on the on-screen WiFi page (own NVS key, default
  **off**, changed only on the touchscreen, independent of **Receive** = firmware push). Nothing can switch it on
  remotely.
- The HTTP server runs while WiFi is connected and (Web interface on **or** Receive on). Every route checks its own
  switch, so Receive keeps protecting firmware pushes whether or not the panel is on, and the panel does not
  need Receive:

| Route | Needs | When the switch is off |
|---|---|---|
| `GET /api/info` | nothing (open, unchanged) | answers |
| `GET /` (the page) | Web interface | 403 `web_interface_off` |
| `GET /api/status`, `GET /api/settings`, `GET /api/schema` | Web interface | 403 `web_interface_off` |
| `PATCH /api/settings` | Web interface | 403 `web_interface_off` |
| `POST /api/command` (including `stop`) | Web interface | 403 `web_interface_off` |
| `/api/update/begin`, `/api/update/data` | Receive (+ the existing token and Accept dialog) | 403 `receive_off` (the update routes check Receive themselves because the server also runs for the web interface alone) |

The Web interface switch is kept in RAM (loaded at boot, written only by the on-screen toggle), so the HTTP task
never reads NVS. **Forget WiFi switches it off again**, like Receive. The switch only decides whether the web routes
answer; the server itself follows within about 2 s.

There is **no token** for the panel (the existing token keeps protecting firmware push only). The page is served
by the grinder itself, so it is same origin: no CORS headers are sent and none are needed.

## 2. Browser guards

The grinder cannot tell a person from a web page, so a few cheap checks keep a browser tab on the same network
from being turned against it (cross-site requests, DNS rebinding). They cost a legitimate client nothing.
Order of all checks for a request: route switch -> Host -> Origin -> (state-changing only) Content-Type ->
`X-Grinder` -> body parser -> decision on the UI task.

| Check | Rule | Refusal |
|---|---|---|
| Host (every request) | `<label>(.<label>)*.local` (labels of `a-z0-9-`, 1-63 chars) or an IPv4 / bracketed IPv6 literal, each with an optional `:port`; case-insensitive; at most 128 chars. A DNS name such as `evil.com` is refused (DNS rebinding). | 403 `bad_host` |
| Origin (every request, when present) | exactly `http://` + the Host value (case-insensitive). `null`, `https://...`, other sites, a trailing `/` are refused. Absent is fine (scripts, same-origin GET). | 403 `bad_origin` |
| Content-Type (`PATCH`, `POST`) | `application/json`, optional parameters such as `; charset=utf-8` | 415 `unsupported_media_type` |
| `X-Grinder: 1` (`PATCH`, `POST`) | header present with the value `1` | 403 `missing_custom_header` |

The last two make every state-changing call a request a foreign web page cannot send without a CORS preflight,
which the grinder never answers.

## 3. Routes

| Method and path | Body | Success reply |
|---|---|---|
| `GET /` (also `/index.html`) | | the embedded page: gzip, `Content-Encoding: gzip`, `ETag` (build number + content CRC, `If-None-Match` answers 304), `Cache-Control: no-cache`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, a Content-Security-Policy (`default-src 'self'` ... `frame-ancestors 'none'`), `Connection: close`. At most 64 KB gzipped (the build fails above that). |
| `GET /api/info` | | unchanged (name, version, build, board, busy, confirm) |
| `GET /api/status` | | status object (section 7) |
| `GET /api/schema` | | `{"fields":[...],"profiles":[...]}`: everything a page needs to render the settings form |
| `GET /api/settings` | | `{"values":{<18 keys>},"current_profile":N}` |
| `PATCH /api/settings` | flat JSON object, see 4 and 5 | 200 `{"ok":true,"values":{...},"current_profile":N,"adjusted":["key",...]}` (the applied state); with a changed `device_name` also `"address":"<name>.local"` |
| `POST /api/command` | `{"cmd":"...","profile":N}`, see 6 | 200 or 202 `{"ok":true,"cmd":"..."}` |

Any error is `{"error":"<code>","reason":"<text>"}` with the HTTP status of section 8. Unknown paths and wrong
methods are answered by the HTTP server (404 / 405) before any of this applies. The buffer sizes the firmware
needs are 1 KB for the values and patch reply, 4 KB for the schema.

## 4. Settings and profile fields

One flat namespace: the 11 menu settings (the 10 settings of the Settings pages plus the grind mode), the 6
profile targets (single/double/custom, weight and time) and the **device setting** `device_name`. Names, types and limits of every field are also served
by `GET /api/schema`. Values in `GET /api/settings` use the same JSON types as the table; numbers are written with at most 3 decimals (trailing zeros trimmed, at least one decimal kept, e.g. `9.0`, `21.5`).

| Key | Type | Unit | Range | Step | Default | Rule outside the range |
|---|---|---|---|---|---|---|
| `ble_startup` | bool | | true/false | | true | n/a |
| `logging` | bool | | true/false | | false | n/a |
| `swipe` | bool | | true/false | | false | n/a |
| `auto_start` | bool | | true/false | | false | n/a |
| `auto_return` | bool | | true/false | | false | n/a |
| `brightness_normal` | int | % | 15 - 100 | 1 | 100 | clamped (below 15 -> 15, above 100 -> 100) |
| `brightness_screensaver` | int | % | 15 - 100 | 1 | 35 | clamped like `brightness_normal` |
| `purge_mode` | enum | | `"keep"` (Prime, stored 0), `"remove"` (Purge, stored 1) | | `"remove"` | other string -> `invalid_value` |
| `purge_amount_g` | number | g | 0.1 - 2.5 | 0.1 | 1.0 | clamped into the range, then snapped to a tenth |
| `freshness_hours` | number | h | 0.5 - 48 | steps 0.5, 1, 2, 3, 4, 8, 12, 24, 48 | 8.0 | snapped to the nearest step (a tie goes to the lower step), so below 0.5 -> 0.5 and above 48 -> 48 |
| `grind_mode` | enum | | `"weight"` (stored 0), `"time"` (stored 1) | | `"weight"` | other string -> `invalid_value` |
| `single_weight_g` | number | g | 5 - 1000 | | 9.0 | **refused** (`out_of_range`) |
| `single_time_s` | number | s | 0.5 - 25 | | 5.0 | **refused** (`out_of_range`) |
| `double_weight_g` | number | g | 5 - 1000 | | 18.0 | **refused** |
| `double_time_s` | number | s | 0.5 - 25 | | 10.0 | **refused** |
| `custom_weight_g` | number | g | 5 - 1000 | | 21.5 | **refused** |
| `custom_time_s` | number | s | 0.5 - 25 | | 12.0 | **refused** |
| `device_name` | string | | 1 - 24 characters of `a-z`, `0-9`, `-`; no leading or trailing hyphen | | `"grindbyweight"` | **refused** (`invalid_device_name`), never clamped or folded |

Profile names are fixed: index 0 `SINGLE`, 1 `DOUBLE`, 2 `CUSTOM` (`GET /api/schema` lists them with the keys of
their two targets). `current_profile` (0-2, the selection on the screen) is returned by `GET /api/settings` but
is **read-only**: a body containing it is refused as `unknown_key`. Also not exposed (always `unknown_key`):
WiFi credentials and token, board override, factory reset, calibration, autotune, firmware update.

Clamping is exactly what the menu does with the same value:

- Settings are **clamped**, never refused for being out of range. The reply lists the keys whose value was
  clamped or snapped in `adjusted`, with the applied value in `values`.
- `brightness_*`: a number is rounded to a whole percent (half away from zero), limited to the slider range
  0-100, then raised to the display minimum 15 (`HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT`). Stored as percent/100.
- `purge_amount_g`: limited to 0.1-2.5 and rounded to the 0.1 g slider position (`purge_slider_from_amount`).
- `freshness_hours`: nearest of the nine menu steps.
- **Profile targets are refused, not clamped**, like `ProfileController::set_profile_weight/time` (valid means
  min <= value <= max on the stored float). Any value in range is stored as given; the screen shows one decimal.
- Brightness read-back: the menu reads a stored fraction back with `(int)(fraction * 100)`, which truncates, so
  53 % and 59 % read back as 52 % and 58 % (also on the grinder's own slider). `values` always reports what the
  menu would show.

### Device name (`device_name`)

`device_name` is the mDNS host name: the grinder answers at `http://<device_name>.local`. It is a **device setting**,
not a menu setting: it is stored in the existing NVS key `wifi/name` and applied by `WiFiManager`, on the UI-task
side of the mailbox like every other write (the HTTP task never touches NVS or WiFi). Rules:

- Default `grindbyweight` (unchanged). 1-24 characters of lowercase `a-z`, `0-9` and hyphen, no leading or trailing
  hyphen (a double hyphen is fine). This is the rule `WiFiManager` already applies when it loads the name
  (`wifi_validate_hostname`, which would allow up to 63 characters) with the length limited to 24.
- **Uppercase input is refused, not folded to lowercase.** Any invalid name (empty, 25+ characters, underscore, dot,
  space, leading/trailing hyphen) is refused with 400 `invalid_device_name`; it is never clamped or corrected.
  A non-ASCII byte anywhere in a body is `malformed_json` (like in every string), a non-string is `wrong_type`.
- Applying it restarts **only mDNS**; WiFi stays connected, so the IP address does not change. The `PATCH` reply
  contains `"address":"<name>.local"` and the page must be reopened at that address (the old name stops
  answering). `GET /api/settings` and `GET /api/schema` include `device_name` (schema group `device`, type
  `string`, `min`/`max` are the length limits).
- Setting the name it already has is accepted and changes nothing.
- **Duplicate names on the LAN are the user's responsibility.** The grinder does not detect them; with two grinders
  on one name, use the IP address or give them different names.

## 5. Request body rules (`PATCH /api/settings`, `POST /api/command`)

A body is **one flat JSON object** of simple values, at most 512 bytes (the firmware also caps the member count at 24, a defensive limit that cannot be reached: there are only 18 known keys and duplicates are refused). Anything else is
refused and nothing is applied (all or nothing, also for a patch with one good and one bad field).

| Rule | Refusal |
|---|---|
| empty body (no bytes) | 400 `empty_body` |
| more than 512 bytes | 413 `body_too_large` |
| not an object, truncated, trailing bytes, trailing/leading comma, unquoted or empty or over-32-byte key, string with an escape (`\`), control or non-ASCII byte, bad literal (`tru`), bad number grammar | 400 `malformed_json` |
| `NaN`, `Infinity`, `-Infinity`, hex, `+5`, `.5`, `5.`, leading zeros, a number token over 32 characters | 400 `malformed_json` |
| a value that is an object or an array | 400 `nested_value` |
| a key that appears twice (also with different values) | 400 `duplicate_key` |
| a key that is not in the field table (case-sensitive) | 400 `unknown_key` |
| `{}` for `PATCH` | 400 `empty_patch` |
| JSON type does not match the field (bool needs true/false, number fields need a number, enum fields a string, `null` is never valid) | 400 `wrong_type` |
| enum string that is not a choice or longer than 24 characters; a number that does not fit a finite double (`1e999`) | 400 `invalid_value` |
| profile target outside its limits | 400 `out_of_range` |

Members are examined in document order and the first problem wins: per member the key (duplicate, then known),
then the value (shape, then type, then range). Whitespace between tokens (space, tab, CR, LF) is allowed.
Numbers may use an exponent (`1.8e1`). Integer fields (`brightness_*`) accept any number and round it.

Examples (every request needs `Content-Type: application/json` and `X-Grinder: 1`):

```
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X PATCH http://grindbyweight.local/api/settings \
     -d '{"brightness_normal":80,"grind_mode":"time","double_time_s":9.5}'
curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST http://grindbyweight.local/api/command \
     -d '{"cmd":"start","profile":1}'
```

## 6. Writes and commands

A write or command is handed to the UI task (the HTTP task never touches the screen, the grind state, the profiles
or NVS) and runs through the same handlers as the touchscreen, so an open menu page, the screen brightness and the
grind mode shown on the screen follow. **One request at a time** is in flight; a second one while another is being
handled is refused with `command_in_flight`. If the UI task does not pick a request up within about 500 ms the reply
is 503 `ui_busy`. A value that could not be persisted is 500 `store_failed`.

**Settings/profile writes** (`PATCH`) are refused unless the grinder is on the READY screen or a menu page (never
while an EDIT screen, a dialog, a grind or an update is active). Decision order: `command_in_flight`, `busy`, `not_ready`.

**Commands** (`POST /api/command`, body `{"cmd":"<name>"[,"profile":0..2]}`). Unknown members (a free weight or
time, for example) are `unknown_key`: a start only takes a stored profile.

| `cmd` | Body | Effect | Success reply |
|---|---|---|---|
| `stop` | no `profile` | Same as the STOP button: stops a running grind, autotune or motor run, and cancels a pending remote start. | 200 |
| `tare` | no `profile` | Tares the scale (zeroes the reading); only while idle on the READY screen. | 200 |
| `start` | `profile` (0-2) required | Starts a grind with that stored profile's weight/time in the **current grind mode**, like the START button, after the touch-to-cancel lead-in below. | 202 `{"ok":true,"cmd":"start","profile":1,"lead_in_ms":3000}` |
| `motor_test` | no `profile` | Runs the same motor test as Menu -> Motor Test (same refusals, same result handling). | 202 |

A missing `cmd` or a `start` without `profile` is `missing_field`; `profile` with any other command is
`unexpected_field`; an unknown name (case-sensitive) is `unknown_command`; a wrong JSON type is `wrong_type`;
`profile` must be a whole number 0-2 (`invalid_value` for 1.5 or a non-finite value, `out_of_range` for 3 or -1).

**Decision** for the commands other than `stop`, in this order, on the UI task right before acting (the first
that applies is the refusal):

1. `command_in_flight`: another write/command is still being handled.
2. `update_in_progress`: a firmware update holds the grinder.
3. `busy`: a grind, autotune or motor output is active.
4. `not_ready`: `tare` and `start` need the READY screen with no dialog; `motor_test` needs READY or a menu page.
5. the command's own block, the same checks the touchscreen uses: for `start` `GrindController::start_block()`
   (`motor_disabled`, `update_in_progress`, `load_cell_not_connected`, `load_cell_no_data`, `load_cell_rate_invalid`);
   for `motor_test` `UIManager::motor_pulse_block()` (`motor_disabled`, `update_in_progress`, `busy`, `motor_fault`).

**`stop` is always accepted** (while the Web interface is on): it skips the whole decision, is never blocked by
`command_in_flight`, `busy`, a lock-out or a fault, and only ever stops things.

**Lead-in counts as in flight (rule for step 7):** while the lead-in runs, the remote start must count as an in-flight
command (or the overlay must classify the UI as `OTHER`), so `motor_test`, `tare`, a second `start` and `PATCH` are
refused during it (`command_in_flight` or `not_ready`); only `stop` and a touch are accepted. This is an explicit
acceptance test for step 7.

**Remote start lead-in:** after an accepted `start` the grinder screen shows "Remote start in N s, touch to cancel"
for 3 seconds (`lead_in_ms`). A touch on the grinder or a `stop` request cancels it; otherwise the normal grind
starts, with all of its own safety checks. There is no confirmation tap, no cooldown and no extra marker beyond
the normal log line. Time mode has no sensor checks: a start without a cup runs the set time (at most 25 s).

## 7. Status

`GET /api/status` is a snapshot published by the UI task (RAM, no hardware access):

```
{"ui_state":"ready","grind_phase":"idle","busy":false,"weight_g":0.0,"motor_available":true,
 "start_block":null,"motor_test_block":null,"uptime_s":123,"build":123,"web_interface":true}
```

| Field | Type | Meaning |
|---|---|---|
| `ui_state` | string | lower-case name of the screen state (`ready`, `grinding`, `grind_complete`, `grind_timeout`, `edit`, `menu`, `calibration`, `confirm`, `purge_confirm`, `autotuning`, `ota_update`, `ota_update_failed`) |
| `grind_phase` | string | lower-case name of the grind phase (`idle`, `predictive`, ...) |
| `busy` | bool | grind, autotune or motor output active |
| `weight_g` | number or null | current weight; null while the reading is not valid |
| `motor_available` | bool | the board is established and the motor can be driven |
| `start_block` | string or null | the error code a `start` would be refused with right now (`null` = would be accepted by the grinder-side checks) |
| `motor_test_block` | string or null | the same for `motor_test` |
| `uptime_s` | int | seconds since boot |
| `build` | int or null | firmware build number |
| `web_interface` | bool | always true (the route is only served while the switch is on) |

## 8. Error codes

Every refusal, with its HTTP status. `status` is what the client sees; `error` is the stable machine-readable code.

| `error` | HTTP | Cause |
|---|---|---|
| `empty_body` | 400 | the body has no bytes |
| `body_too_large` | 413 | more than 512 bytes |
| `malformed_json` | 400 | not a flat JSON object of simple values (section 5) |
| `nested_value` | 400 | a value is an object or an array |
| `duplicate_key` | 400 | a key appears more than once |
| `unknown_key` | 400 | a key that is not a field (or not a command member) |
| `unexpected_field` | 400 | `profile` sent with a command other than `start` |
| `missing_field` | 400 | `cmd` missing, or `start` without `profile` |
| `empty_patch` | 400 | `PATCH` body `{}` |
| `wrong_type` | 400 | JSON type does not match the field |
| `invalid_value` | 400 | enum choice unknown/too long, number not finite, `profile` not a whole number |
| `out_of_range` | 400 | profile target outside its limits, `profile` not 0-2 |
| `unknown_command` | 400 | `cmd` is not one of the four commands |
| `invalid_device_name` | 400 | `device_name` is not 1-24 characters of `a-z`, `0-9`, `-`, or starts/ends with a hyphen (uppercase is refused, not folded) |
| `web_interface_off` | 403 | the Web interface switch on the grinder is off |
| `bad_host` | 403 | Host header is not a `.local` name or an IP literal |
| `bad_origin` | 403 | Origin header present and not `http://` + Host |
| `missing_custom_header` | 403 | `X-Grinder: 1` missing on a `PATCH`/`POST` |
| `unsupported_media_type` | 415 | `PATCH`/`POST` without `Content-Type: application/json` |
| `command_in_flight` | 409 | another write/command is still being handled |
| `busy` | 409 | a grind, autotune or motor run is active |
| `not_ready` | 409 | the screen is not in a state that allows this (EDIT, dialog, calibration, ...) |
| `update_in_progress` | 409 | a firmware update holds the grinder |
| `motor_disabled` | 409 | the board is not confirmed, the motor is disabled |
| `motor_fault` | 409 | the motor output fault is latched until reboot (motor test) |
| `load_cell_not_connected` | 409 | load cell not connected (start) |
| `load_cell_no_data` | 409 | load cell delivers no data (start) |
| `load_cell_rate_invalid` | 409 | load cell sample rate invalid (start) |
| `ui_busy` | 503 | the UI task did not pick the request up within about 500 ms |
| `store_failed` | 500 | a value could not be persisted |

## 9. Change rules

Adding a setting means: the menu table and `UiSettings` first, then one row in the field table of
`portal_api.cpp`, the vectors, the tables above (the pytest guard fails until all agree). Never rename a key or an
error code, pages rely on them. Never add a token requirement, a CORS header, or a command that takes a free
target weight or time without a new plan: the panel is open on the local network by design.

# Web panel API (contract)

The grinder serves an internal web panel and a small JSON API on the **local network**. This document is the
contract between the firmware and any page that talks to it (the page the grinder serves, a replacement portal, scripts).
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
- The panel is enabled only by the **Web interface** toggle on the on-screen WiFi page (Menu -> WiFi, the "Web" row
  under "Web interface"; greyed out until WiFi is set up; own NVS key, default **off**, changed only on the
  touchscreen, independent of **Receive** = firmware push). Nothing can switch it on remotely.
- The HTTP server runs while WiFi is connected and (Web interface on **or** Receive on). Every route checks its own
  switch, so Receive keeps protecting firmware pushes whether or not the panel is on, and the panel does not
  need Receive:

| Route | Needs | When the switch is off |
|---|---|---|
| `GET /api/info` | nothing (open, unchanged) | answers |
| `GET /` (the page) | Web interface | 403 `web_interface_off` |
| `GET /api/status`, `GET /api/settings`, `GET /api/schema`, `GET /api/stats`, `GET /api/history`, `GET /api/diagnostics` | Web interface | 403 `web_interface_off` |
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
| `GET /api/stats` | | lifetime statistics object (section 7, "Statistics") |
| `GET /api/history` | | the newest finished grinds (section 7, "Recent grinds") |
| `GET /api/diagnostics` | | read-only health numbers: load cell, motor latency, signal, Bluetooth, board, firmware push (section 7, "Diagnostics") |
| `GET /api/schema` | | `{"fields":[...],"profiles":[...]}`: everything a page needs to render the settings form |
| `GET /api/settings` | | `{"values":{<20 keys>},"current_profile":N}` |
| `PATCH /api/settings` | flat JSON object, see 4 and 5 | 200 `{"ok":true,"values":{...},"current_profile":N,"adjusted":["key",...]}` (the applied state); with a changed `device_name` also `"address":"<name>.local"` |
| `POST /api/command` | `{"cmd":"...","profile":N}`, see 6 | 200 or 202 `{"ok":true,"cmd":"..."}` |

All four commands are served by `POST /api/command`: `stop`, `tare`, `start` and `motor_test`.

The reads (`/api/status`, `/api/settings`, `/api/stats`, `/api/history`, `/api/diagnostics`) copy a snapshot the UI task publishes (see section 7); with no fresh
snapshot (the UI task is stuck, for example during a long blocking screen operation) they answer 503 `ui_busy`. `GET /api/schema` is constant and
needs no snapshot.

Any error is `{"error":"<code>","reason":"<text>"}` with the HTTP status of section 8. Unknown paths and wrong
methods are answered by the HTTP server (404 / 405) before any of this applies. The buffer sizes the firmware
needs are 1 KB for the values and patch reply, 4 KB for the schema, 640 bytes for the status (the longest possible status is 470 bytes), 512 bytes for the statistics (the longest possible reply is 334 bytes), 2560 bytes for the history (ten worst-case entries are about 2 KB) and 320 bytes for the diagnostics (the longest possible reply is 253 bytes).

## 4. Settings and profile fields

One flat namespace: the 13 menu settings (the 10 settings of the Settings pages, the two screen timeouts plus the grind mode), the 6
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
| `screen_dim_min` | int | min | 0 - 60 | steps 1, 2, 5, 10, 15, 30, 60; 0 = Never | 5 | snapped to the nearest step (a tie goes to the lower step), above 60 -> 60; 0 or less -> 0 (Never) |
| `screen_off_min` | int | min | 0 - 480 | steps 10, 30, 60, 120, 240, 480; 0 = Never | 60 | snapped like `screen_dim_min`, above 480 -> 480; 0 or less -> 0 (Never) |
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
- `screen_dim_min`, `screen_off_min`: a number is rounded to a whole minute (half rounds up), then 0 or less (also
  anything that rounds to 0, such as 0.4) means Never (0); otherwise the nearest listed step wins, a tie goes to the
  lower step (45 -> 30 for dim, 20 -> 10 for off), and above the largest step it is the largest step (not Never).
  The menu offers the same steps ("Dim after", "Screen off"); a stored value that is not a step reads back as the
  default (5 / 60). The grinder dims or turns the screen off after this much time without a touch or a weight change
  of at least 1 g (details in `docs/DOC.md`, "Screen saver and screen off"); a write is picked up at once, but it
  never wakes or darkens the screen by itself, and a screen that is off stays off.
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
- Setting the name it already has is accepted and changes nothing (no write, no mDNS restart, no `address` in the
  reply).
- A stored name longer than 24 characters (the WiFi code accepts up to 63, so it can only come from outside this
  API) is reported shortened to 24 characters with trailing hyphens removed, so the value `GET` returns always
  passes the validator. Sending that value renames the grinder to the shortened name.
- **Duplicate names on the LAN are the user's responsibility.** The grinder does not look for them and does not
  tell the page about them; with two grinders on one name, use the IP address or give them different names. The
  mDNS responder may itself pick another name when it finds a conflict (for example `<name>-2`), so the address in
  the reply is the name that was stored, and the WiFi page and the IP address are the reliable ways to find the
  grinder. The DHCP host name registered at the router only changes at the next WiFi reconnect.

## 5. Request body rules (`PATCH /api/settings`, `POST /api/command`)

A body is **one flat JSON object** of simple values, at most 512 bytes (the firmware also caps the member count at 24, a defensive limit that cannot be reached: there are only 20 known keys and duplicates are refused). Anything else is
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
while an EDIT screen, a dialog, a grind or an update is active). Decision order: `command_in_flight`, `update_in_progress`
(a firmware update owns the flash, so nothing is written), `busy`, `not_ready`.

**Apply order and partial failure.** The body is validated completely first (all or nothing, section 5). Then the
fields it names are written one by one in a fixed order: ascending field index (the order of the table in section
4: the settings including `grind_mode` and the two screen timeouts, then the six profile targets) and the **device name last**, whatever the order
in the body. A field whose value already equals the stored one is not written. The device name goes last because it
is the disruptive change (the page must be reopened at the new address), so it only happens when every other field
of the patch was stored. The write **stops at the first** field that cannot be persisted: the fields before it stay
applied, the failing one and everything after it are not touched (the grinder keeps the value it had: a profile target or
grind mode that could not be stored is rolled back in memory too, so the screen and the next start agree with what is saved), and the reply is 500 `store_failed` (no `values`
in it). After a `store_failed` or a `ui_busy` the page must read `GET /api/settings` to see what is stored. The
grinder's screen follows what was applied (brightness, auto actions, the cached screen timeouts, the READY tab values and grind mode, an open
menu page) before the reply is sent. A screen that is off stays dark, and the new brightness is used when it wakes. The `values` of a successful reply are re-read from the stored state, so a
brightness reads back as the menu would show it (section 4).

**Commands** (`POST /api/command`, body `{"cmd":"<name>"[,"profile":0..2]}`). Unknown members (a free weight or
time, for example) are `unknown_key`: a start only takes a stored profile.

| `cmd` | Body | Effect | Success reply |
|---|---|---|---|
| `stop` | no `profile` | Same as the STOP button: stops a running grind, autotune or motor run, and cancels a pending remote start. | 200 `{"ok":true,"cmd":"stop"}` |
| `tare` | no `profile` | Tares the scale (zeroes the reading); only while idle on the READY screen. Same as the Scale page's TARE button: the TARING overlay appears and the tare itself runs a moment later. | 200 `{"ok":true,"cmd":"tare"}`: the tare was started (it takes at most `GRIND_TARE_TIMEOUT_MS`, 3 s, and the reading is zero when it is done). Until then every other command and write is refused: `not_ready` if it arrives before the UI task starts the tare, otherwise `ui_busy` after about 500 ms (the UI task is blocked in the tare). `stop` is never refused. |
| `start` | `profile` (0-2) required | Starts a grind with that stored profile's weight/time in the **current grind mode**, like the START button, after the touch-to-cancel lead-in below. | 202 `{"ok":true,"cmd":"start","profile":1,"lead_in_ms":3000}` |
| `motor_test` | no `profile` | Runs the same motor test as Menu -> Motor Test after its RUN tap (the 1 s output pulse, the "MOTOR RUNNING" overlay with STOP, the statistics), without the confirm dialog and without a lead-in. | 202 `{"ok":true,"cmd":"motor_test"}`: the pulse is running (it ends by itself after 1 s; a `stop` ends it early). |

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
   (`motor_fault` while the deadman fault is latched, `motor_disabled`, `update_in_progress`, `load_cell_not_connected`,
   `load_cell_no_data`, `load_cell_rate_invalid`);
   for `motor_test` `UIManager::motor_pulse_block()` (`motor_disabled`, `update_in_progress`, `busy`, `motor_fault`).

**Motor test.** There is no lead-in and no confirm dialog for `motor_test` (the lead-in is the remote start's only):
the request itself is the confirmation, and the output runs for 1 s, so the risk is small. The decision above is
made on the UI task and `UIManager::motor_pulse_block()` is checked once more inside the run, immediately before the
pulse, so a lock-out, a fault or a disabled motor that appeared in between still refuses it (a pulse that cannot
be started after that check is answered `busy`). It is accepted on the READY screen and on a menu page, never over a
dialog or an overlay. While it runs the grinder counts as `busy`, and until the 100 ms poll notices the end it still counts as `not_ready`
(the overlay is up), so a second `motor_test`, `start`, `tare` or `PATCH` is refused. The overlay ends it with its
STOP button, like the touch run. When it ends the result notice
is shown only if a menu page is the current screen (as on the touch run, which can only start there); started from
the READY screen the overlay simply goes away and the run is recorded in the statistics. There is no cooldown:
repeated runs are allowed, each one through the same timer and `finish_motor_test()` path, which always deletes its
poll timer.

**`stop` is always accepted** (while the Web interface is on): it skips the whole decision, is never blocked by
`command_in_flight`, `busy`, a lock-out or a fault, and only ever stops things. Stopping while nothing runs is a
harmless 200. The browser guards of section 2 still apply (a stop is a `POST`).

**How stop reaches the grinder.** `stop` is not a mailbox request and does not wait for the UI task, which may be
busy (a blocking screen operation such as a tare) and would otherwise delay or lose it. The HTTP task itself calls
`portal_stop_now()` (`src/network/portal_stop.*`), which does three things, in this order, using only entry points
built to be called from any task:

1. `GrindController::request_stop()` sets an atomic flag. The grind control task (Core 0, its own task, independent of
   the UI task) consumes it at the top of every control cycle (20 ms) and runs the same `stop_grind()` as the STOP
   button, so the control loop cannot restart the motor behind the stop. A request made while no grind runs is
   discarded in the same cycle and never stops a later grind.
2. `Grinder::stop()` cuts the motor output at once. It is mutex-serialised and, like the deadman, carries on without
   the lock if it cannot get it quickly. This also ends a Motor Test pulse (the test screen notices the output has
   ended and finishes itself) and a pulse of the autotune. Worst case between the request and a grind that is
   really stopped: one control cycle, during which the control task could issue one more pulse, which the next
   cycle's `stop_grind()` cuts.
3. A counter in `portal_command_gate` is bumped. The UI task notices it on its next frame and cancels an autotune run
   (the autotune is driven from the UI task, exactly like its CANCEL button). If the UI task is stuck, the motor and the grind
   are stopped anyway; only the autotune screen change waits.

Two edge cases:

- A web stop during a Motor Test ends the pulse early, and the test is recorded as ended early with the elapsed
  time (measured on the bench: 576 ms for a stop at about 0.3 s), the same as a lock-out stop.
- A stop that arrives in the roughly 20 ms while `start_grind()` is being processed on the UI task, before the phase
  is set, is consumed (nothing was running yet) and the grind then starts.

The HTTP task still never calls LVGL, profiles, settings or NVS. The `UpdateGuard` lock-out needs no special case: it
already holds the motor off, and `stop` never checks it.

**Lead-in counts as in flight:** while the lead-in runs, the remote start counts as an in-flight command, so
`motor_test`, `tare`, a second `start` and `PATCH` are refused during it with `command_in_flight` (the overlay also
classifies the screen as not ready, so `not_ready` is the answer if the in-flight flag were ever missing); only
`stop` and a touch are accepted.

**Remote start lead-in:** after an accepted `start` the grinder screen shows an overlay "Remote start in N s, touch
to cancel" for 3 seconds (`lead_in_ms`; the reply is sent at once, 202). Any touch on the grinder (the overlay also
has a STOP button) or a `stop` request cancels it; a cancelled start is logged and nothing else happens. When the
time is up the grinder decides again on what is true then (READY screen, not busy, no update, `start_block` NONE)
and, if it still passes, starts through the START button's path with the profile's stored target and the current
grind mode (the profile selected on the screen does not change). If something took the grinder over during the
lead-in (auto start, a firmware update, a load cell fault, another screen) the start is dropped and only logged
(`remote start dropped: <code>`); when the lead-in runs out and a start block applies then, the grinder shows the
same "can't start" notice as the START button. The 202 therefore means "accepted, will start unless cancelled", not "the
grind is running": read `GET /api/status` for the outcome. There is no confirmation tap, no cooldown and no extra
marker beyond the normal log lines (`[PORTAL] remote start ...`). Time mode has no sensor checks: a start without a
cup runs the set time (at most 25 s). If the touch driver is not available, the lead-in is always cancelled
(fail-safe: nobody could cancel it by touch), so the start is dropped and logged (`remote start cancelled: no touch driver`). This is host-reviewed only, not tested on hardware.

- The overlay names the profile ("Remote start: DOUBLE in 3 s, touch to cancel") and keeps the screen awake; the
  grinding and complete screens show the name of the profile that was started, not the one selected on READY.
- A stop that was accepted before the start was queued does not cancel it. A stop that arrives during the lead-in,
  including just before it ends, does.
- The lead-in itself is not visible in `GET /api/status`: for those 3 seconds it still reads `ui_state` `ready` and
  `busy` false, and `start_block` is unchanged. A page should wait out `lead_in_ms` and then watch for `grinding`.
- A weight-mode remote start with purge set to remove stops at `purge_confirm` until someone taps at the grinder,
   exactly like a touch start (the purge itself runs; the main grind waits for the tap).

## 7. Status

`GET /api/status` is a snapshot published by the UI task (RAM, no hardware access), refreshed about every 250 ms
(`GET /api/settings` shows the stored settings as of the last publish: at most 10 s old, or right after a write
through this API or when the menu is left; a change made on the touchscreen shows up within that time):

```
{"ui_state":"ready","grind_phase":"idle","busy":false,"weight_g":0.0,"motor_available":true,
 "start_block":null,"motor_test_block":null,"uptime_s":123,"build":123,"web_interface":true,"warnings":[]}
```

| Field | Type | Meaning |
|---|---|---|
| `ui_state` | string | lower-case name of the screen state (`ready`, `grinding`, `grind_complete`, `grind_timeout`, `edit`, `menu`, `calibration`, `confirm`, `purge_confirm`, `autotuning`, `ota_update`, `ota_update_failed`) |
| `grind_phase` | string | lower-case name of the grind phase: `idle`, `initializing`, `setup`, `taring`, `tare_confirm`, `predictive`, `pulse_decision`, `pulse_execute`, `pulse_settling`, `final_settling`, `time_grinding`, `time_additional_pulse`, `completed`, `timeout`, `prime`, `prime_settling`, `purge_confirm` |
| `busy` | bool | grind, autotune or motor output active |
| `weight_g` | number or null | current weight; null while the reading is not valid |
| `motor_available` | bool | the board is established and the motor can be driven |
| `start_block` | string or null | the error code a `start` would be refused with right now (`null` = would be accepted by the grinder-side checks) |
| `motor_test_block` | string or null | the same for `motor_test` |
| `uptime_s` | int | seconds since boot |
| `build` | int or null | firmware build number |
| `web_interface` | bool | always true (the route is only served while the switch is on) |
| `warnings` | array of strings | the grinder's active warnings (the diagnostics behind the warning icon), highest priority first, `[]` when there are none; see below |

`warnings` is an additive member (the contract version does not change): it is always the last member, and a page
written before it existed ignores it. Every entry is one of the fixed codes below (at most 25 characters, nine in total,
each at most once); the grinder never sends free text, the wording belongs to the page. The list is built by the UI
task from the diagnostics of the grinder (the same source as the warning icon) and shares the snapshot's age of at most
250 ms. A client should tolerate a code it does not know (a later firmware may add one) and an absent member (an older
firmware). The codes, in priority order:

| Code | Meaning |
|---|---|
| `board_unrecognized` | the hardware board (V1 or V2) could not be identified; the motor stays off until it is chosen on the grinder |
| `motor_fault` | the motor output was cut off by the deadman; latched until the grinder is restarted |
| `motor_disabled` | the grinder output is off (board not confirmed, or the grinder failed to initialise) |
| `hx711_not_connected` | the load cell amplifier is missing or does not answer |
| `hx711_no_data` | the load cell amplifier is connected but delivers no samples |
| `hx711_sample_rate_invalid` | the load cell amplifier is wired for the wrong sample rate |
| `mechanical_instability` | the grinder reported repeated mechanical anomalies during grinding |
| `load_cell_noisy` | sustained excessive noise on the load cell (60 s or more) |
| `load_cell_not_calibrated` | the load cell has not been calibrated |

### Statistics (`GET /api/stats`)

The grinder's lifetime statistics, the same totals as the Lifetime Stats page on its touchscreen. Read-only, gated by
the Web interface switch like every other read (403 `web_interface_off`, `bad_host`, `bad_origin` as usual). The
reply is a copy the UI task takes from the statistics every 5 seconds and whenever the screen state changes (a finished
grind shows up within about 5 s); it has its own age limit of 15 s, after which the route answers 503 `ui_busy`
(the UI task is stuck, or the statistics were never published since the switch went on). The firmware has no
other error for this route; an older firmware without it answers 404 from the HTTP server, which a page treats as
"statistics not available".

```
{"total_grinds":412,"single":180,"double":200,"custom":32,"weight_mode":390,"time_mode":22,"total_weight_kg":8.43,
 "motor_runtime_s":5120,"avg_abs_error_g":0.12,"total_pulses":1100,"avg_pulses":2.7,"time_pulses":14,"powered_minutes":22890}
```

| Field | Type | Meaning |
|---|---|---|
| `total_grinds` | int | successful grinds since the statistics were last reset (timeouts, overshoots and stopped grinds are not counted) |
| `single`, `double`, `custom` | int | the same grinds by final weight, **not** by profile: up to 10 g, up to 22 g, more |
| `weight_mode`, `time_mode` | int | the same grinds by grind mode |
| `total_weight_kg` | number | total ground weight in kilograms |
| `motor_runtime_s` | int | motor run time in whole seconds (grinds, time pulses and motor tests), rounded down |
| `avg_abs_error_g` | number | mean absolute difference between final and target weight over the weight-mode grinds, `0.0` with no samples |
| `total_pulses` | int | pulses summed over all grinds: every grind adds its pulse count, a time-mode grind adds 0 (its extra pulses are `time_pulses`) |
| `avg_pulses` | number | `total_pulses` divided by all counted grinds (time-mode grinds count with 0 pulses), `0.0` with no samples |
| `time_pulses` | int | additional pulses given after time-mode grinds |
| `powered_minutes` | int | minutes the grinder has been powered on (hours times 60 plus the minutes remainder) |

Integers are plain (up to 4294967295, `powered_minutes` up to 257698037759); the three fractional members follow the
number rule of section 4 (at most 3 decimals, one kept) and are bounded: a value that is not finite or is negative is
`0.0`, a value above 1000000000 is `1000000000.0`, so a reply is always valid JSON. Statistics that were reset (factory
reset) simply read as zeros. The members are additive: a client ignores members
it does not know and treats a missing one as unknown, not as zero. Nothing here uses the clock or `millis()`.

### Recent grinds (`GET /api/history`)

The newest finished grinds, newest first, at most 10. Read-only, gated by the Web interface switch like every other
read. The grinder keeps these grinds in a small file in its flash memory (two files of 16 records, written once per
finished grind), so the list survives a restart and a power cut. A **factory reset clears it** (the files carry an id that
is stored with the settings, so old grinds never appear next to freshly reset statistics); Menu "Purge logs" does not.
The grinder has no wall clock, so there are no dates: a grind carries its age and nothing else about time.

The reply is a copy the UI task takes from the history whenever it changes (within about 250 ms of the grind being
recorded) and every 5 seconds otherwise; its own age limit is 15 s. The route answers 503 `ui_busy` when the copy is
older than that, when it was never published since the switch went on, when the boot load of the history has not finished,
or when the reply buffer cannot be allocated; a page asks again. A firmware without the route answers 404 from the HTTP
server, which a page treats as "recent grinds not available" (the statistics may still exist).

```
{"count":2,"persistent":true,"grinds":[
 {"id":57,"age_s":120,"profile":1,"mode":"weight","target_g":18.0,"target_s":null,"final_g":18.1,"time_ms":14230,
  "motor_ms":9800,"pulses":2,"result":"complete"},
 {"id":56,"age_s":null,"profile":0,"mode":"time","target_g":null,"target_s":5.0,"final_g":14.5,"time_ms":5000,
  "motor_ms":4700,"pulses":0,"result":"max_pulses"}]}
```

An empty history is `{"count":0,"persistent":true,"grinds":[]}`.

| Field | Type | Meaning |
|---|---|---|
| `count` | int | the number of entries in `grinds` (0 to 10) |
| `persistent` | bool | `true` while the grinds are being saved to flash. `false` when the flash store is unavailable or the last write failed: the list is then kept in memory only and is lost at the next restart (a page says so). It turns `true` again after a successful write. |
| `grinds` | array | newest first |
| `id` | int | the grind's number as the grinder counts them (it wraps; not an ordering key, the array order is) |
| `age_s` | int or null | seconds since the grind ended, counted on the grinder's uptime clock and never negative. `null` for a grind recorded before the last restart (its age cannot be known without a clock). It grows between two requests without a new grind. |
| `profile` | int | 0 `SINGLE`, 1 `DOUBLE`, 2 `CUSTOM`: the button that was used (not the weight bucket of the statistics) |
| `mode` | string | `"weight"` or `"time"` |
| `target_g` | number or null | the target weight in grams; `null` in time mode |
| `target_s` | number or null | the target time in seconds; `null` in weight mode |
| `final_g` | number | the settled weight in grams (may be negative with a bad reading) |
| `time_ms` | int | duration of the grind |
| `motor_ms` | int | time the motor was on |
| `pulses` | int | correction pulses (time mode: additional pulses) |
| `result` | string | `complete`, `max_pulses` (finished after the maximum number of pulses), `overshoot`, `timeout`, or `unknown` (also for a code a later firmware adds) |

**Error** is not sent: a page computes it as `final_g - target_g` (positive is over the target, negative is under) for a
weight-mode grind. Only finished grinds are listed: a grind that was stopped is not recorded, and timeouts and overshoots
are (with their `result`), although the statistics do not count them. Weights are bounded and rounded like the
rest of the API (section 4): a value that is not finite is `0.0`, the magnitude is cut at 1000000, a value under 0.0005
is `0.0` (so there is never `-0.0`), and numbers have at most 3 decimals. `profile` is cut to 2. `age_s` is
`uptime - ended` on the grinder's clock `millis() / 1000` (it restarts from 0 after about 49 days; an entry then looks
0 s old rather than negative). The grinder sends no free text and nothing about the network or the device.
Everything is additive: a client ignores members it does not know and tolerates an unknown `result`.

**Not guaranteed:** after a power cut during a write, or when a file turns out torn, the list can lose the grinds that
were recorded before the cut in the older file (the newest ones in the active file are kept; the exact guarantee is in
`docs/DESIGN-NOTES.md`, "Persistent history (chg016)"). A filesystem that is not available at boot keeps the history in
memory only (`persistent` is `false`).

### Diagnostics (`GET /api/diagnostics`)

Read-only health numbers of the grinder, the same ones its Diagnostics and WiFi pages show, for a Health card on the page.
Gated by the Web interface switch like every other read (403 `web_interface_off`, `bad_host`, `bad_origin` as usual).
Nothing here can be changed through the API; Receive, Bluetooth, the GaggiMate link and the board choice stay touch-only.
The reply is a copy the UI task gathers about every 2 seconds while the Web interface is on; it has its own age limit of
15 s (`PORTAL_DIAGNOSTICS_STALE_MS`; the UI task republishes every `PORTAL_DIAGNOSTICS_REFRESH_MS`, 2 s), after which the route answers 503
`ui_busy`, as it does before the first publish. The HTTP task only copies the slot (own spinlock, plain data, no call out of
the lock) and encodes into a heap `ReplyBuffer`; it logs its stack headroom once after the first reply, like the other
read routes. A firmware without the route answers 404 from the HTTP server, which a page treats as "health not
available". The route is additive: the contract version does not change.

```
{"calibrated":true,"cal_factor":1234.56,"std_dev_g":0.012,"noise_ok":true,"sample_rate_sps":10.0,"motor_latency_ms":50,
 "wifi_rssi_dbm":-58,"ble":"off","board":"V2","board_source":"probe","push_armed":false}
```

| Field | Type | Meaning |
|---|---|---|
| `calibrated` | bool | the load cell has been calibrated |
| `cal_factor` | number | the load cell calibration factor; may be negative, `0.0` when unknown |
| `std_dev_g` | number | load cell noise as a standard deviation in grams over the default 500 ms window (`get_standard_deviation_g()`), never negative |
| `noise_ok` | bool | `noise_level_diagnostic()`, the stateful sustained-2 s noise check; the same verdict the Diagnostics page words as "OK" against "Too High" |
| `sample_rate_sps` | number | the load cell sample rate detected at init (`get_detected_sample_rate_sps()`), never negative |
| `motor_latency_ms` | int | the grinder's measured motor response latency in whole milliseconds (0 to 1000000) |
| `wifi_rssi_dbm` | int or null | WiFi signal strength in dBm (-127 to 0); `null` while the grinder is not connected to WiFi (the firmware's `rssi()` reads 0 then; 0 dBm is never sent for "not connected") |
| `ble` | string | one word for the Bluetooth state, see below |
| `board` | string | the detected board: `V1`, `V2` or `UNKNOWN` (the word `/api/info` reports as `board`) |
| `board_source` | string | how the board was established: `none`, `probe`, `user`, `cached` or `default` |
| `push_armed` | bool | the Receive toggle (firmware push) is on; read-only here |

`ble` is one of `off`, `advertising`, `connected` (the plain Bluetooth modes) and `link_listening`, `link_connected`,
`link_linked` (the GaggiMate link mode: waiting, a client connected, a client linked). A client shows a word it does not
know as received (a later firmware may add one). The numbers follow the number rule of section 4 (at most 3 decimals,
one kept) and are bounded so a reply is always valid JSON: a value that is not finite is `0.0`, a negative `std_dev_g` or
`sample_rate_sps` is `0.0`, magnitudes are cut at 1000000000, a value under 0.0005 is `0.0` (never `-0.0`), the latency is
rounded and cut to 0..1000000, the signal is cut to -127..0. `board` and `board_source` only ever carry the words above.
The grinder sends no SSID, address or other free text. Everything is additive: a client ignores members it does not know
and treats a missing one as unknown.

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
| `motor_fault` | 409 | the motor output fault is latched until reboot (motor test and start) |
| `load_cell_not_connected` | 409 | load cell not connected (start) |
| `load_cell_no_data` | 409 | load cell delivers no data (start) |
| `load_cell_rate_invalid` | 409 | load cell sample rate invalid (start) |
| `ui_busy` | 503 | the UI task did not pick a write up within about 500 ms or finish it within 2 s (the write may still complete in the second case), or a read found no fresh snapshot (for `/api/history` also: the boot load of the history has not finished, or the reply buffer could not be allocated) |
| `store_failed` | 500 | a value could not be persisted; fields before it in the apply order stay applied (section 6) |

## 9. Change rules

Adding a setting means: the menu table and `UiSettings` first, then one row in the field table of
`portal_api.cpp`, the vectors, the tables above (the pytest guard fails until all agree). Never rename a key or an
error code, pages rely on them.

Adding a **read route** (like `/api/stats`, `/api/history` and `/api/diagnostics`) means: `GET` only, `PortalRoute::API_READ` (the Web
interface gate, the Host and Origin guards and `Cache-Control: no-store` come with it), its own snapshot slot that the
UI task fills and the HTTP task only copies (own lock, own age limit, 503 `ui_busy` when stale), a reply buffer from
`ReplyBuffer` and never a large local on the HTTP task's 6 KB stack, a pure encoder with a truncation test at every
byte and a worst-case size margin, encoder vectors in `portal_api_vectors.json`, the Python mirror in
`tools/portal/portal_model.py` and `dev_server.py`, a row in the gate table and the routes table above, and a free
handler slot (`UPDATE_HTTP_MAX_URI_HANDLERS`, 13 of 16 used, `/api/diagnostics` being the 13th). The HTTP task never touches the filesystem, NVS, LVGL or the
controllers for it. Additive members never change the contract version. Never add a token requirement, a CORS header, or a command that takes a free
target weight or time without a new plan: the panel is open on the local network by design.

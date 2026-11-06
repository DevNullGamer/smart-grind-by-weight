# GLink: Grinder Link protocol v1 (contract)

GLink is the Bluetooth Low Energy protocol of the Smart Grind By Weight grinder for other devices, first of all a
GaggiMate. It reads the grinder state and the live weight, selects a profile, sets the "Profile Dose" (grams) and an
optional display-only grind size, starts and stops a grind, and delivers the final weight with a result code.

This document is the contract between the grinder firmware and any client. The same rules are executable:
`tools/tests/glink_vectors.json` holds the conformance vectors (golden frame bytes, reader cases, request/response
pairs for every object, operation and error code, number and grind size cases), and `tools/tests/test_glink_contract.py`
keeps the document, the vectors and the constants in `src/config/bluetooth.h` equal.

| | |
|---|---|
| Protocol version (`proto` in `baseInfo`) | 1 |
| Status | v1 frozen for review by the GaggiMate maintainer; `grindControl` is **provisional** (see 5.5) |
| Conventions | the Bookoo Motto80 BLE protocol (transport, envelopes, handshake, broadcasts, error object) |
| Document language | English; field names and codes are case-sensitive |

> **Safety: GLink has no authentication and no pairing. Anyone within Bluetooth range can start your grinder.**
> The Motto80 protocol, whose transport GLink copies, withholds remote start for safety reasons ("under safety
> evaluation"). GLink adds a start without any authentication, so its trust model is weaker than Motto80's. While the
> "GaggiMate link" (or Bluetooth) is on, anyone in radio range (typically 10 to 30 m, more with an antenna) can read
> the state and the live weight, change the Profile Dose and the displayed grind size, change the selected profile,
> stop a running grind, occupy the one BLE connection, and **start a weight-mode grind of a profile whose stored dose
> is 5.0 to 60.0 g**. Nothing is encrypted. The only protections are listed in 1.1, and none of them is security. Do
> not use this link where a stranger within radio range, or a service situation with the hopper open and the grinder
> not switched off, is a concern.

## 1. Scope and trust model

GLink is a local, short-range, cooperative protocol. It is not a security boundary.

### 1.1 What an unauthenticated peer can do

In range of the grinder, with the link enabled, any BLE central can:

- see the state, the live weight, the serial code (`snCode`) and the profiles, and eavesdrop on everything (nothing is
  encrypted);
- change the Profile Dose (5.0 to 60.0 g) and the displayed grind size, and change the selected profile. This is a
  nuisance: the user reads the number on the screen before pressing START;
- stop any running grind or motor test (`stop` is always accepted, also before the handshake);
- occupy the single BLE connection, which blocks GaggiMate, `grinder.py`, the web flasher and WiFi updates;
- **start a grind.** Preconditions: the READY screen, no dialog, no start block, no update. A start grinds the stored
  weight of one profile in weight mode (5.0 to 60.0 g). The existing grind safety aborts (stale sensor, tare, no flow,
  stall, negative weight, 60 s timeout, motor deadman) all apply; no safety logic is bypassed. Realistic consequences:
  wasted beans, noise, a spill if no cup is present, and the burrs turning if someone is servicing the hopper without
  having switched the grinder off. The last case is the real physical risk.

What the protocol does to limit this, honestly stated:

- Every start has a fixed lead-in of 3 s (`GLINK_START_LEAD_IN_MS`) in which a person at the grinder can cancel it by
  touch (or STOP, or a `stop` request). The decision is made again at the end of the lead-in. The lead-in cannot be
  skipped, but a cancelled lead-in does **not** stop the client from sending another `start` immediately. The only
  remedies are switching the "GaggiMate link" (or Bluetooth) off in the grinder's menu, or cutting power.
- Starts are accepted only on the READY screen, only for a stored dose of 5.0 to 60.0 g, and only one start is in
  flight.
- The handshake gate and its timeout (section 2) stop nothing: any central can send `appHello`.
- Writes that change a stored value are limited to one per second. This slows flash wear but does **not** prevent it:
  the NVS partition is 20 KB, and a sustained one write per second could wear it out in months to a couple of years
  (an estimate, not a measurement).

A future authentication layer can be added as one more business object without breaking v1 (see 9).

### 1.2 Compatibility with Motto80

GLink copies the Motto80 transport and conventions (GATT shape, fragment frame v1, `request` / `response` /
`broadcast` envelopes, `appHello`, `baseInfo` and `periodInfo`, the `{code, message, field}` error object, no request
ids, no pairing). A client library that implements Motto80's frame layer, envelope handling and handshake can be reused
unchanged. The business objects differ (a grinder with a scale instead of a motorised burr grinder), and the service
UUIDs differ, so a driver for GLink is a separate driver on top of the same transport. Section 10 lists every
difference and extension.

## 2. GATT

| Purpose | UUID | Properties |
|---|---|---|
| Service | `53474257-0001-4632-901b-4752494e4421` | Primary service |
| RX (client to device) | `53474257-0002-4632-901b-4752494e4421` | Write, Write Without Response |
| TX (device to client) | `53474257-0003-4632-901b-4752494e4421` | Notify (CCCD 0x2902) |

UUID scheme (analogue of Motto80's `4d543830-0001-4b80-8f00-424f4f4b4f4f`): the first group `53474257` is ASCII "SGBW"
(Smart Grind By Weight), the last group `4752494e4421` is ASCII "GRIND!", the third and fourth group (`4632`, `901b`) were
drawn at random in the shape of a version-4 UUID with RFC variant bits, and the second group numbers the attributes
(`0001` service, `0002` RX, `0003` TX). The three UUIDs are checked against all other UUIDs in
`src/config/bluetooth.h`, the Motto80 UUIDs and the Nordic UART UUIDs: there is no collision.

- **Discovery.** The device name is `GrindByWeight` (unchanged), the address is a public address. The service UUID is
  **not** in the advertisement, so a client finds the device by name or MAC and then discovers the services. `snCode`
  in `baseInfo` tells several grinders apart. Verify name and MAC before you connect.
- **No pairing, no link encryption, no bonding.** The grinder accepts one connection at a time. Do not send secrets over
  this channel.
- **Write mode.** Writing RX with response is recommended; Write Without Response is accepted. Either way the client
  must keep the fragment order.
- **Connection sequence.** Connect, discover the service, check that RX is writable and TX has Notify, subscribe to TX,
  send `appHello`, wait for `periodInfo` (the device sends it at least every 2 s, so "no `periodInfo` within 5 s" means
  the channel is not ready).
- **Disconnect.** See 7.4: every disconnect clears all per-connection state. After reconnecting, subscribe to TX again
  and resend `appHello`.
- **Handshake gate.** Until `appHello` has been processed, every request except `appHello` and `grindControl` `stop`
  is answered with `INVALID_STATE` ("handshake required"; this is the only use of `INVALID_STATE`). A client that has
  not sent `appHello` within 10 s (`GLINK_HANDSHAKE_TIMEOUT_MS`) of connecting is disconnected. A handshaken client may
  stay connected silently.
- **Update exclusion.** While a firmware update runs the grinder refuses new connections, and the requests of an
  existing connection are answered as described in 7.1 (`UPDATING`, except `stop` and `appHello`).

## 3. Fragment frame v1

Identical to Motto80. Each RX write and each TX notification carries exactly one fragment. All multi-byte integers are
little-endian.

### 3.1 Header

| Offset | Length | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | `magic` | `0xA5` |
| 1 | 1 | `version` | `0x01` |
| 2 | 2 | `sequence` | same for every fragment of one JSON message; not consecutive across messages; the sender may reuse a value; never use it to correlate a request with a response |
| 4 | 2 | `totalLength` | byte length of the whole UTF-8 JSON message, 1 to 4096 |
| 6 | 2 | `offset` | byte offset of this fragment's payload within the whole message |
| 8 | N | `payload` | a slice of the UTF-8 JSON, N at least 1 |

### 3.2 Sender rules (both directions)

- The first fragment has offset 0. Each next fragment's offset equals the number of payload bytes already sent. The
  message is complete when `offset + N == totalLength`.
- A packet (header plus payload) is at most `min(ATT MTU - 3, 512)` bytes. If the MTU is not known, use 20 bytes
  (`GLINK_DEFAULT_PACKET_BYTES`, valid at the default MTU of 23): 12 payload bytes per fragment.
- Send the fragments of a message in order and without interleaving another message. The device never interleaves
  messages, so a client needs only one reassembler.
- Compact JSON (no whitespace between tokens) is recommended on the wire. No checksum or CRC.
- A request is at most 512 bytes (`GLINK_MAX_REQUEST_BYTES`); a response or broadcast is at most 1024 bytes
  (`GLINK_MAX_REPLY_BYTES`). The device declares `maxRequestBytes` in `baseInfo`.

### 3.3 Receiver rules (both directions)

Valid rules for the device and for a client reassembling TX:

1. A fragment with bad magic or version, a packet of 8 bytes or fewer (no payload), `totalLength` of 0 or above 4096,
   `offset >= totalLength`, or a payload running past `totalLength` is a **bad fragment**: it is dropped, and so is any
   message in progress.
2. A fragment with `offset == 0` starts a new message and drops an incomplete one. One message is reassembled at a time.
3. A fragment with `offset > 0` is valid only if a message is in progress and its `sequence` and `totalLength` equal the
   message's and its offset equals the bytes received so far. Otherwise it is a bad fragment and the message in
   progress is dropped.
4. An incomplete message expires when more than 2000 ms (`GLINK_REASSEMBLY_TIMEOUT_MS`) pass between two fragments. A
   gap of exactly 2000 ms is still allowed. The next fragment of an expired message is a bad fragment (rule 3).
5. **Device only.** A first fragment with `totalLength` above 512 is answered once with `PAYLOAD_TOO_LARGE` (object
   `error`). The remaining fragments of that message (same `sequence` and `totalLength`) are discarded silently and are
   not bad fragments. This discard state ends like a message in progress: after a gap of more than 2000 ms between
   fragments, or when a fragment with `offset == 0` arrives. A continuing fragment after it ended is a bad fragment.
6. **Device only.** Ten (`GLINK_BAD_FRAME_LIMIT`) bad fragments within 10 s (`GLINK_BAD_FRAME_WINDOW_MS`) disconnect the
   client; a bad fragment counts together with the earlier ones that are less than 10 000 ms older.
7. The reassembled bytes are one UTF-8 JSON message, judged by section 4.

### 3.4 Limits and constants

All of these are defined in `src/config/bluetooth.h` with exactly these names and values.

| Constant | Value | Meaning |
|---|---|---|
| `GLINK_PROTO_VERSION` | 1 | `proto` in `baseInfo` |
| `GLINK_FRAME_MAGIC` | 165 | `0xA5` |
| `GLINK_FRAME_VERSION` | 1 | `0x01` |
| `GLINK_FRAME_HEADER_BYTES` | 8 | header size |
| `GLINK_DEFAULT_PACKET_BYTES` | 20 | packet size when the MTU is unknown |
| `GLINK_MAX_PACKET_BYTES` | 512 | upper bound of the packet size |
| `GLINK_MAX_MESSAGE_BYTES` | 4096 | `totalLength` above this is a bad fragment |
| `GLINK_MAX_REQUEST_BYTES` | 512 | larger requests get `PAYLOAD_TOO_LARGE` |
| `GLINK_MAX_REPLY_BYTES` | 1024 | largest response or broadcast |
| `GLINK_REASSEMBLY_TIMEOUT_MS` | 2000 | gap that expires an incomplete message |
| `GLINK_HANDSHAKE_TIMEOUT_MS` | 10000 | no `appHello` after connect: disconnect |
| `GLINK_BAD_FRAME_LIMIT` | 10 | bad fragments that disconnect |
| `GLINK_BAD_FRAME_WINDOW_MS` | 10000 | window of `GLINK_BAD_FRAME_LIMIT` |
| `GLINK_JSON_MAX_DEPTH` | 4 | object nesting in a request |
| `GLINK_JSON_KEY_MAX_LEN` | 32 | longest key |
| `GLINK_JSON_STRING_MAX_LEN` | 32 | longest string value, in bytes |
| `GLINK_JSON_NUMBER_MAX_LEN` | 15 | longest number token, in characters |
| `GLINK_PROFILE_COUNT_MAX` | 4 | profile indices 0 to 3 |
| `GLINK_PROFILE_WRITABLE_INDEX` | 3 | the only profile writable over BLE |
| `GLINK_DOSE_MIN_G` | 5.0 | smallest dose for writes and BLE starts |
| `GLINK_DOSE_MAX_G` | 60.0 | largest dose for writes and BLE starts |
| `GLINK_GRIND_SIZE_MAX` | 999.9 | largest `grindSize` |
| `GLINK_GRIND_UNIT_MAX_LEN` | 6 | longest `grindUnit` after trimming |
| `GLINK_START_LEAD_IN_MS` | 3000 | lead-in of every BLE start |
| `GLINK_WRITE_MIN_INTERVAL_MS` | 1000 | minimum time between two stored changes |
| `GLINK_UI_REPLY_TIMEOUT_MS` | 1000 | the UI task must answer within this, else `UI_BUSY` |
| `GLINK_PERIOD_HEARTBEAT_MS` | 2000 | `periodInfo` when nothing changed |
| `GLINK_PERIOD_GRINDING_FAST_MS` | 250 | `periodInfo` while GRINDING, MTU at least `GLINK_FAST_MTU_MIN` |
| `GLINK_PERIOD_GRINDING_SLOW_MS` | 1000 | `periodInfo` while GRINDING, smaller MTU |
| `GLINK_FAST_MTU_MIN` | 100 | MTU from which the fast cadence is used |

### 3.5 Worked example

The handshake request (compact, 43 bytes) with sequence 1 and 20-byte packets (12 payload bytes each) is four
fragments, offsets 0, 12, 24 and 36. The bytes of each packet (hex) are:

```text
A5 01 01 00 2B 00 00 00 7B 22 72 65 71 75 65 73 74 22 3A 7B
A5 01 01 00 2B 00 0C 00 22 61 70 70 48 65 6C 6C 6F 22 3A 7B
A5 01 01 00 2B 00 18 00 22 6F 70 22 3A 22 68 61 6E 64 73 68
A5 01 01 00 2B 00 24 00 61 6B 65 22 7D 7D 7D
```

Reading the first header: `A5` magic, `01` version, `01 00` sequence 1, `2B 00` totalLength 43, `00 00` offset 0. The payload
`7B 22 72 65 71 75 65 73 74 22 3A 7B` is `{"request":{`. The second packet has offset `0C 00` = 12, the third `18 00` = 24,
the fourth `24 00` = 36 and carries the last 7 bytes (`ake"}}}`): 36 + 7 = 43, so the message is complete. With any
negotiated ATT MTU of 54 or more the same request is a single 51-byte packet (see the vectors,
`handshake_single_packet_seq_0x0102`, which also shows a sequence of 0x0102 stored as `02 01`).

## 4. JSON envelopes

Every message is one UTF-8 JSON object with exactly one top-level member:

| Top-level | Direction | Content |
|---|---|---|
| `request` | client to device | exactly one business object |
| `response` | device to client | the result of one request, under the object name of the request |
| `broadcast` | device to client | one or more business objects (in v1 the device sends one per message) |

There are no request ids. A client sends one request at a time and matches the response by the object name. A second
request while one is pending is answered with `BUSY`.

Success:

```json
{"response":{"grindProfile":{"result":"success","data":{"selectProfile":2}}}}
```

Failure (`field` only when the error belongs to one field; paths use dots from the business object, for example
`selector.value` or `selector.colour`; `message` is short English text for logs and **not** a contract, `code` is):

```json
{"response":{"grindProfile":{"result":"fail","error":{"code":"OUT_OF_RANGE","message":"dose out of range","field":"dose"}}}}
```

Only the first error is returned and the whole request is rejected: nothing is applied. If the failing request cannot be
addressed to a known object (broken JSON, unknown object, no object, too large, a bad top level), the failure is
returned under the pseudo-object `error`:

```json
{"response":{"error":{"result":"fail","error":{"code":"MALFORMED_JSON","message":"malformed json"}}}}
```

### 4.1 Order of checks

The first failing step is the answer:

1. Frame level (section 3.3). A message above 512 bytes: `PAYLOAD_TOO_LARGE`.
2. Reader (4.2): `MALFORMED_JSON`.
3. Envelope. Any top-level member other than `request` (for example `response` or `broadcast` sent by a client): `INVALID_PARAM`,
   field is that key (the first one in message order). A top-level object without members: `MISSING_FIELD`, field `request`.
   Then `request` must be an object (`INVALID_PARAM`, field `request`) with exactly one member (zero: `MISSING_FIELD`; more:
   `INVALID_PARAM`; field `request` in both), whose name is a request object (`UNKNOWN_OBJECT`) and whose value is an
   object (`INVALID_PARAM`, field is the object name, answered under that object).
4. `grindControl` `stop` is executed here (5.5), whatever follows, even before the handshake and while a request is
   pending. A message is a stop if it passes the reader and the envelope and the object is `grindControl` with `op` equal
   to `stop`; other members of the object are ignored.
5. Handshake gate: `INVALID_STATE`, unless the object is `appHello`.
6. Pending request: `BUSY`.
7. Operation and fields on the BLE task: `op` (`MISSING_FIELD`, `INVALID_PARAM`, `UNKNOWN_OP`), unknown keys
   (`INVALID_PARAM`, field is the dotted path of the key), required fields, types, and ranges, in the order the fields
   are listed in section 5.
8. The state checks and the write or start decision on the UI task (section 7). The UI task answers within 1000 ms
   (`GLINK_UI_REPLY_TIMEOUT_MS`), otherwise `UI_BUSY`; a request that timed out is **not** applied (7.4).

### 4.2 Reader rules for incoming JSON (strict)

The device accepts only a conservative subset of JSON in requests. Violations are `MALFORMED_JSON` (no `field`).

1. The message is one object; only whitespace (space, tab, CR, LF) may surround tokens and follow the object. No BOM, no
   NUL, no text after the closing brace, not empty.
2. **No string escapes.** A backslash anywhere in a string is refused, so `\"`, `\\`, `\n` and `\uXXXX` never reach the
   device. Raw control characters (below 0x20) in a string are refused. Bytes of 0x80 and above are not validated by
   the reader; the fields that have a charset refuse them (`grindUnit`).
3. Keys consist of `A-Za-z0-9_` only, 1 to 32 characters. Duplicate keys in one object, at any level, are refused.
4. String values are at most 32 bytes.
5. Nesting is at most 4 objects deep: the top-level object is depth 1, so `request` (2), the business object (3) and
   `selector` (4). Requests contain **no arrays**.
6. Numbers follow the JSON grammar `-?(0|[1-9][0-9]*)(\.[0-9]+)?` **without exponent** (`1e2` is refused, as are `NaN`,
   `Infinity`, `+1`, `.5`, `5.`, `018`), and a number token has at most 15 characters.
7. Literals are `true`, `false` and `null`, nothing else (lower case).

After the reader, the business rules of section 5 apply. Unknown keys inside `grindProfile` and `grindControl` (and
inside their `selector`) are refused with `INVALID_PARAM` and the dotted path of the key as `field` (`colour`,
`selector.colour`), so a typo never silently does nothing. The only exception is `stop` (4.1). Unknown keys inside
`appHello` are ignored. Keys that are not allowed for the given `op` count as unknown.

An **integer** field accepts any number token whose value is a whole number: `3`, `3.0`, `-0` (zero). `3.5` is
`INVALID_PARAM`. The value is compared as a number of up to 15 characters, with no wrap-around: `4294967296` and
`4294967299` are `OUT_OF_RANGE` for an index, not 0 and 3.

### 4.3 Numbers the device sends

Weights, doses, targets and grind sizes are always written with exactly one decimal (`18.0`, `-0.3`, `0.0`). Counters,
times and indices are plain integers. A client must accept any JSON number.

### 4.4 Rounding of incoming decimals

`dose`, `expectDose` and `grindSize` are rounded to 0.1 on the **decimal digits as written**, half away from zero (not on
a binary float), and the range is checked **after** rounding. `18.04` becomes `18.0`, `18.05` becomes `18.1`, `4.95`
becomes `5.0` and is valid, `4.94` becomes `4.9` and is `OUT_OF_RANGE`, `-0.04` becomes `0.0` (valid for `grindSize`).
Two values that round to the same number are the same value (no flash write, `changed` is false).

### 4.5 Injection safety

The device echoes only fixed strings: error `message` texts and enumerated vocabulary values. The only client-controlled
strings that appear in any output are the key names in an error `field`, which the reader limits to `A-Za-z0-9_`, and the
stored `grindUnit`, which is limited to a fixed charset (5.4). A device implementation must never pass `grindUnit` (or
any client string) as a printf or LVGL format string; it is data.

## 5. Objects

| Object | Used in | Operations |
|---|---|---|
| `appHello` | `request` | `handshake` |
| `baseInfo` | `broadcast` | (none) |
| `periodInfo` | `broadcast` | (none) |
| `grindProfile` | `request`, `response` | `get`, `select`, `update` |
| `grindControl` | `request`, `response` | `start`, `stop` |
| `grindResult` | `request`, `response`, `broadcast` | `get` |

### 5.1 `appHello`

```json
{"request":{"appHello":{"op":"handshake"}}}
```

`op` must be `handshake` (`MISSING_FIELD`, `UNKNOWN_OP`). On success there is no direct response; a failure is answered
like any other request. Afterwards the device sends the broadcast `baseInfo` once and then `periodInfo`, each as its own
message. A repeated `appHello` is allowed and sends both broadcasts again. Unknown keys are ignored.

### 5.2 `baseInfo` (broadcast, once per handshake)

| Field | Type | Meaning |
|---|---|---|
| `releaseVer` | int | firmware build number (`BUILD_NUMBER`) |
| `snCode` | string | serial code, 12 lower-case hex digits from the chip's factory MAC |
| `wifiName` | null | always `null` (privacy; present for Motto80 compatibility) |
| `device` | string | `SGBW` |
| `proto` | int | GLink protocol version, 1 |
| `fwVersion` | string | firmware version, for example `1.4.1` |
| `board` | string | `V1`, `V2` or `UNKNOWN` |
| `doseMin` | number | smallest dose for writes and starts, 5.0 (g) |
| `doseMax` | number | largest dose for writes and starts, 60.0 (g) |
| `maxRequestBytes` | int | largest request, 512 |

```json
{"broadcast":{"baseInfo":{"releaseVer":640,"snCode":"7c2a3f10b4d8","wifiName":null,"device":"SGBW","proto":1,"fwVersion":"1.4.1","board":"V2","doseMin":5.0,"doseMax":60.0,"maxRequestBytes":512}}}
```

Clients must ignore unknown fields (additive changes keep `proto`).

### 5.3 `periodInfo` (broadcast)

Sent right after `baseInfo`, immediately on a change (section 8), and at least every 2 s.

| Field | Type | Present | Meaning |
|---|---|---|---|
| `devState` | string | always | see 6.1 |
| `weight` | number or null | always | live weight in g, 0.1 resolution, may be negative; `null` if the sensor reading is invalid |
| `selectProfile` | int | always | selected profile, 0 to 3 (3 only while the Profile Dose exists) |
| `phase` | string | while GRINDING | grind phase, see 6.2 |
| `target` | number | while STARTING, GRINDING | target in g (`mode` WEIGHT) or s (`mode` TIME), one decimal |
| `mode` | string | with target | `WEIGHT` or `TIME` |
| `leadInMs` | int | while STARTING | milliseconds until the lead-in ends |
| `blocked` | string or null | always | the reason a `start` would be refused right now, or `null`; see 6.5 |
| `resultSeq` | int | always | `seq` of the last `grindResult`, 0 if none since boot |
| `profileRev` | int | always | profile revision, see below |

Fields marked "while ..." are absent otherwise (not `null`). A client must tolerate unknown `devState` and `phase`
strings (any non-empty string) without disconnecting or failing to parse the message.

`profileRev` is a counter that increases by at least one whenever the stored dose, grind size or grind unit of any
profile changes, or profile 3 comes into or goes out of existence, **from any origin** (BLE, touch, web panel, factory
reset). It is 0 after boot, never decreases while the grinder runs, and every change is sent immediately, subject to the cap of section 8 while GRINDING. A client
re-reads the profiles with `grindProfile` `get` when it differs from the value it last saw; this is how it learns about
edits made on the grinder. Selecting a profile changes `selectProfile`, not `profileRev`. If profile 3 is selected and
then removed on the grinder, `selectProfile` reports the profile the grinder fell back to (0 to 2; which one is the
grinder's choice) and `profileRev` increases.

```json
{"broadcast":{"periodInfo":{"devState":"GRINDING","weight":7.3,"selectProfile":3,"phase":"PREDICTIVE","target":18.0,"mode":"WEIGHT","blocked":"NOT_READY","resultSeq":4,"profileRev":2}}}
```

### 5.4 `grindProfile`

The grinder has the profiles SINGLE (0), DOUBLE (1) and CUSTOM (2), which exist always, and the **Profile Dose** PROFILE
(3). Over BLE only profile 3 is writable; 0 to 2 can be read and selected. Profile 3 **exists only after a client has
set a dose**: until then `get all` returns three entries, `select` and `start` of 3 are refused with `PROFILE_NOT_SET`,
and `periodInfo.selectProfile` is never 3. It persists across reboots. It is removed on the grinder itself (touch only,
or a factory reset); removal is not part of this protocol. A client sees a removal through `profileRev` and
`selectProfile`.

Entry (`data` of `get` and `update`):

| Field | Type | Meaning |
|---|---|---|
| `index` | int | 0 to 3 |
| `name` | string | `SINGLE`, `DOUBLE`, `CUSTOM`, `PROFILE` |
| `dose` | number | stored weight in g. May be above 60.0 for profiles set by touch (the BLE range applies to writes and starts) |
| `grindSize` | number or null | display-only grind size, 0.0 to 999.9, or `null` if none |
| `grindUnit` | string or null | display label of the size, at most 6 characters, or `null` |
| `writable` | bool | true only for index 3 |

**`get`** returns all entries (`data` is an array, profile 3 only if it exists) when `selector` is absent or
`{"type":"all"}`, and one entry (`data` is the entry object) for `{"type":"index","value":N}`. `selector` must be an
object (`INVALID_PARAM`, field `selector`) without unknown keys; `selector.type` is required (`MISSING_FIELD`) and must be
`all` or `index` (`INVALID_PARAM`, field `selector.type`); `all` with a `value` is `INVALID_PARAM` (field
`selector.value`); `index` needs `value` (`MISSING_FIELD`), an integer (`INVALID_PARAM`) from 0 to 3 (`OUT_OF_RANGE`);
index 3 before it exists is `NOT_FOUND` (field `selector.value`).

```json
{"request":{"grindProfile":{"op":"get","selector":{"type":"index","value":3}}}}
```

```json
{"response":{"grindProfile":{"result":"success","data":{"index":3,"name":"PROFILE","dose":18.0,"grindSize":14.5,"grindUnit":"um","writable":true}}}}
```

**`select`**: `{"op":"select","index":N}`, `index` an integer 0 to 3. Success `data` is `{"selectProfile":N}`. The READY
screen moves to that profile page. Selecting the profile that is already selected succeeds and writes nothing.

**`update`**: `{"op":"update","index":3, ...}` with at least one of the following. Absent fields stay unchanged.

| Field | Type | Rules |
|---|---|---|
| `dose` | number | 5.0 to 60.0 g after rounding (4.4); `null` and other types are `INVALID_PARAM`. Required when profile 3 does not exist yet (`MISSING_FIELD`, field `dose`) |
| `grindSize` | number or null | 0.0 to 999.9 after rounding; `null` clears the size **and** the unit |
| `grindUnit` | string or null | trimmed (spaces); up to 6 characters of `A-Z a-z 0-9` space `.` `/` `%` `-` `_`; empty or `null` means no unit. Checked in this order after trimming: type and any other character (including non-ASCII) is `INVALID_PARAM`, then more than 6 characters is `OUT_OF_RANGE`. A unit needs a size: setting a unit while the resulting size is `null` is `INVALID_PARAM` (field `grindUnit`) |

Order of checks for `update`: `index` (`MISSING_FIELD`, `INVALID_PARAM`, `OUT_OF_RANGE`), then `PROFILE_READ_ONLY` (field
`index`) for 0 to 2, then the fields in the order `dose`, `grindSize`, `grindUnit`, then "nothing to update"
(`MISSING_FIELD`, no field), then the state rules (7.1), the create-needs-dose and unit-needs-size rules, `RATE_LIMITED`,
and last the flash write (`STORE_FAILED`). A refused update changes nothing.

Success `data` is the resulting entry plus `changed` (a value differs from before) and `stored` (flash was written).
In v1 they are always equal; a request that changes nothing succeeds with both `false` and costs no flash write.

```json
{"request":{"grindProfile":{"op":"update","index":3,"dose":18.0,"grindSize":14.5,"grindUnit":"um"}}}
```

```json
{"response":{"grindProfile":{"result":"success","data":{"index":3,"name":"PROFILE","dose":18.0,"grindSize":14.5,"grindUnit":"um","writable":true,"changed":true,"stored":true}}}}
```

**Grind size is display only.** The grinder shows it as text on the profile page and the edit screen. It has no effect on
grinding and cannot be edited on the grinder.

### 5.5 `grindControl` (PROVISIONAL)

> **Provisional.** The Motto80 start/stop interface is not public yet ("under safety evaluation"). The object name, the
> operations and the field layout of `grindControl` are open for alignment with the Motto80/GaggiMate maintainer and may
> change before they are final. The rest of this document is not affected. Clients should isolate their `grindControl`
> code.

**`start`**: `{"op":"start","profile":N[,"expectDose":D]}`. `profile` is required, an integer 0 to 3. `expectDose` is
optional, a number (5.0 to 60.0 after rounding) that must equal the stored dose of that profile (rounded to 0.1);
GaggiMate sends `update` first and `start` afterwards, and `expectDose` guards against a stale or racing value. A start
grinds the **stored weight** of the profile, always in **weight mode**, whatever the grind-mode setting of the grinder
is. There is no "grams in the start message" in v1. A start changes neither `selectProfile` nor any stored value.

Success means "accepted, the lead-in has begun": `data` is `{"leadInMs":3000}` (always 3000, the lead-in cannot be
skipped). While the lead-in runs, `devState` is `STARTING` (with `leadInMs` and `target` in `periodInfo`) and a person
at the grinder can cancel it. How the grinder presents the pending start is up to the grinder and not part of this
protocol. A touch, the STOP button or a `stop` request cancels the lead-in. If the grinder state changes during the
lead-in (for example an update starts, a load cell fault appears, or another screen takes over), the start is dropped
silently at once. At the end of the lead-in the decision is made again. If it fails because of a start block (motor,
update lock, load cell), the grinder opens its confirmation dialog for it: `devState` is `SETTING` and `blocked` is
`NOT_READY` until the user acknowledges it on the grinder. If it fails for any other reason nothing is shown. Otherwise
the grind begins (`GRINDING`).

A lead-in that ends without a grind (cancelled, dropped, refused at the end) sends no `grindResult` and has no result
code. **Did the start happen?** After a successful `start` response, the first `periodInfo` whose `devState` is not
`STARTING` decides: `GRINDING` means the grind started; anything else means it did not, and `devState` and `blocked` give
the reason. If no such `periodInfo` arrives within `leadInMs` plus 2 s, treat the start as not happened and re-read the
state.

The order of the start refusals is in 7.2. Failures use the specific codes of 6.4.

```json
{"request":{"grindControl":{"op":"start","profile":3,"expectDose":18.0}}}
```

```json
{"response":{"grindControl":{"result":"success","data":{"leadInMs":3000}}}}
```

**`stop`**: `{"op":"stop"}`. It is executed as soon as the message passes the reader and the envelope (4.1 step 4):
other members of the object are ignored, it works before the handshake, while a request is pending and during an update.
It stops a running grind, a motor test and an autotune run, or cancels a lead-in, and answers at once with
`{"accepted":true}` (also when nothing was running). A stopped grind ends with a `grindResult` of code `STOPPED` and the
last live weight.

If the link is lost while grinding, the grind continues. The result can be read with `grindResult` `get` (compare
`periodInfo.resultSeq` with the last `seq` you saw).

### 5.6 `grindResult`

Broadcast once per finished grind, whoever started it (BLE, touch, web panel), to the client that is connected and
handshaken at that moment. `get` returns the last result (RAM only; `seq` restarts at boot). `get` before any result
since boot is `NOT_FOUND`.

| Field | Type | Meaning |
|---|---|---|
| `seq` | int | result counter, 1-based since boot (the value `periodInfo.resultSeq` shows) |
| `code` | string | result code, see 6.3 |
| `origin` | string | `BLE`, `TOUCH` or `WEB` |
| `profile` | int | profile index 0 to 3 |
| `mode` | string | `WEIGHT` or `TIME` |
| `target` | number | g in `WEIGHT` mode, s in `TIME` mode |
| `finalWeight` | number | final weight in g |
| `weightSettled` | bool | true for `OK`, `OVERSHOOT`, `MAX_PULSES` (the final settled weight); for errors, `TIMEOUT` and `STOPPED` it is the last live reading |
| `pulses` | int | number of correction pulses |
| `durationMs` | int | grind duration |
| `motorMs` | int | time the motor ran |

```json
{"broadcast":{"grindResult":{"seq":5,"code":"OK","origin":"BLE","profile":3,"mode":"WEIGHT","target":18.0,"finalWeight":18.1,"weightSettled":true,"pulses":2,"durationMs":14230,"motorMs":11800}}}
```

In a rare race a `STOPPED` result may be followed by the real outcome of the same grind with a higher `seq`; the latest `seq` wins.

A client that reconnects, or missed the broadcast, compares `periodInfo.resultSeq` with the last `seq` it saw and
fetches the result with `get` when it is newer.

## 6. Vocabularies

Clients must tolerate unknown non-empty strings in `devState`, `phase`, `blocked`, `code` of results and `origin`.

### 6.1 `devState`

| Value | Meaning |
|---|---|
| `IDLE` | READY screen, no grind running, a start would be accepted |
| `STARTING` | remote-start lead-in running |
| `GRINDING` | any grind phase, including the PURGE_CONFIRM wait (see `phase`) |
| `DONE` | grind complete or timed out, result not yet acknowledged on the grinder (the cup is probably still on the scale) |
| `SETTING` | menu, edit, calibration, confirmation dialogs and other screens |
| `UPDATING` | a firmware update is running or being offered |
| `WARNING` | READY screen, but a start would be refused for a device reason (`blocked` says which) |
| `UNKNOWN` | fallback |

`IDLE`, `GRINDING`, `SETTING`, `UPDATING`, `WARNING` and `UNKNOWN` are Motto80 names; `STARTING` and `DONE` are
extensions.

### 6.2 `phase`

Present while `devState` is `GRINDING`. These are the grind controller phases except IDLE, COMPLETED and TIMEOUT (which
are `DONE`).

| Value | Meaning |
|---|---|
| `INITIALIZING` | preparing the grind |
| `SETUP` | file and logger setup |
| `TARING` | taring the scale |
| `TARE_CONFIRM` | confirming the tare |
| `PREDICTIVE` | main grinding with flow prediction |
| `PULSE_DECISION` | deciding whether a correction pulse is needed |
| `PULSE_EXECUTE` | running a correction pulse |
| `PULSE_SETTLING` | waiting for the weight to settle after a pulse |
| `FINAL_SETTLING` | waiting for the final weight to settle |
| `TIME_GRINDING` | time-based grinding |
| `TIME_ADDITIONAL_PULSE` | additional pulse in time mode |
| `PRIME` | priming grind (purge/prime before a weight grind) |
| `PRIME_SETTLING` | settling after the priming grind |
| `PURGE_CONFIRM` | waiting for a tap on the grinder to confirm the purge (use Prime mode with remote starts) |

### 6.3 Result `code`

| Value | Meaning |
|---|---|
| `OK` | target reached |
| `OVERSHOOT` | finished above the target |
| `MAX_PULSES` | finished after the maximum number of correction pulses |
| `TIMEOUT` | the 60 s grind timeout |
| `ERR_STALLED` | stall abort |
| `ERR_NO_FLOW` | no flow while priming or while grinding |
| `ERR_SENSOR` | stale or invalid scale |
| `ERR_TARE` | tare failed |
| `ERR_MOTOR` | motor fault |
| `ERR_NEG_WEIGHT` | negative weight abort |
| `STOPPED` | stopped by touch, web or BLE |
| `UPDATE` | aborted because an update started |

### 6.4 Error codes

First error only. `Kind` is `common` (the Motto80 common set), `glink` (specific to GLink) or `start` (also a possible
`blocked` value, see 6.5).

| Code | Kind | Used when |
|---|---|---|
| `INVALID_PARAM` | common | wrong type, unknown key or top-level member, bad selector, more than one object, a non-integer index |
| `MISSING_FIELD` | common | `op`, `index`, `profile`, `dose` (create), `selector.type`, `selector.value` or `request` missing; nothing to update |
| `OUT_OF_RANGE` | common | a number or index outside its range, a unit that is too long, a stored dose outside 5 to 60 g at start |
| `INVALID_STATE` | common | handshake required (the only use) |
| `NOT_FOUND` | common | profile 3 not set (`get`), no result since boot |
| `CONFLICT` | common | `expectDose` differs from the stored dose |
| `MALFORMED_JSON` | glink | the reader rules of 4.2 are violated |
| `UNKNOWN_OBJECT` | glink | the object name is not a request object |
| `UNKNOWN_OP` | glink | `op` is not an operation of the object |
| `PAYLOAD_TOO_LARGE` | glink | a request above 512 bytes |
| `BUSY` | glink | the previous request of this client is still being processed |
| `UI_BUSY` | glink | the UI task did not answer within 1000 ms; the request was not applied |
| `STORE_FAILED` | glink | the flash write failed; the RAM value is rolled back |
| `RATE_LIMITED` | glink | a second stored change within 1000 ms |
| `PROFILE_READ_ONLY` | glink | `update` of profile 0, 1 or 2 |
| `EDIT_IN_PROGRESS` | glink | `update` while the profile is edited on the grinder |
| `PROFILE_NOT_SET` | glink | `select` or `start` of profile 3 before a dose has been set |
| `NOT_READY` | start | the screen or grinder state does not allow the request (7.1): not on the READY screen, a grind or motor run is active, another screen or dialog is shown |
| `UPDATING` | start | a firmware update is running or the grinder is locked out for one |
| `MOTOR_DISABLED` | start | the board is not established, so the motor is disabled |
| `LOAD_CELL_NOT_CONNECTED` | start | the load cell is not connected |
| `LOAD_CELL_NO_DATA` | start | the load cell sends no data |
| `LOAD_CELL_RATE_INVALID` | start | the load cell sample rate is invalid |
| `COMMAND_IN_FLIGHT` | start | a remote-start lead-in is running |

### 6.5 `blocked`

`periodInfo.blocked` is the code a `start` request would return for a valid profile right now, judged on the grinder state
only (not on `PROFILE_NOT_SET`, `OUT_OF_RANGE` or `CONFLICT`, which depend on the request). The values are the codes of
kind `start`; `null` means a start would pass the state checks.

| `devState` | `blocked` |
|---|---|
| `IDLE` | `null` |
| `STARTING` | `COMMAND_IN_FLIGHT` |
| `GRINDING`, `DONE`, `SETTING`, `UNKNOWN` | `NOT_READY` |
| `UPDATING` | `UPDATING` |
| `WARNING` | `MOTOR_DISABLED`, `UPDATING`, `LOAD_CELL_NOT_CONNECTED`, `LOAD_CELL_NO_DATA` or `LOAD_CELL_RATE_INVALID` |

## 7. State and write rules

### 7.1 What each state allows

The decision is made on the UI task from the real state. The rows are evaluated from the top: the first row that
applies decides. `accept` means the request continues to its own validation and is applied. Every other entry is the
error code returned. `get` covers `grindProfile` `get` and `grindResult` `get`. `stop` and `appHello` are not
subject to this table.

| State | `get` | `update` | `select` | `start` |
|---|---|---|---|---|
| `LEAD_IN` | accept | `COMMAND_IN_FLIGHT` | `COMMAND_IN_FLIGHT` | `COMMAND_IN_FLIGHT` |
| `UPDATING` | `UPDATING` | `UPDATING` | `UPDATING` | `UPDATING` |
| `READY` | accept | accept | accept | decide |
| `MENU` | accept | accept | `NOT_READY` | `NOT_READY` |
| `EDIT` | accept | `EDIT_IN_PROGRESS` | `NOT_READY` | `NOT_READY` |
| `GRINDING` | accept | `NOT_READY` | `NOT_READY` | `NOT_READY` |
| `DONE` | accept | `NOT_READY` | `NOT_READY` | `NOT_READY` |
| `OTHER` | accept | `NOT_READY` | `NOT_READY` | `NOT_READY` |

`LEAD_IN` is a running remote start, `UPDATING` any firmware update or its prompt, `READY` the
READY screen without a dialog, `DONE` the grind-complete and timeout screens, `OTHER` calibration, dialogs and every other
screen. A grind, autotune or motor run in progress counts as `GRINDING` or `OTHER`. This follows the rule of the web
panel (a write is accepted on the READY screen and in the menu only). There are two differences: on `EDIT`, `update`
gets its own code, and the web panel also accepts a profile change on a menu page whereas `select` in `MENU` is
`NOT_READY`. A start on a locked-out READY screen shows `PROFILE_NOT_SET` or `MOTOR_DISABLED` first (7.2); the
lock-out itself is reported as `UPDATING` after those. Because `UPDATING` is decided on the UI task, a request that fails validation on the BLE task first
(section 4.1, step 7) gets that error instead. `update` is refused while grinding rather than stored for later; a client
may retry.

### 7.2 Order of the start refusals

On the UI task, after the BLE task has checked `profile` and `expectDose` (`MISSING_FIELD`, `INVALID_PARAM`,
`OUT_OF_RANGE`), the first applicable refusal is returned. The state checks (1 to 3) are the ones of the web panel's
start decision, where `NOT_READY` also covers a running grind, autotune or motor output; 7 to 10 are the grinder's own
start block, in its order; the profile checks are placed between the two groups:

1. `COMMAND_IN_FLIGHT`
2. `UPDATING` (an update is held)
3. `NOT_READY` (not on the READY screen, or busy)
4. `PROFILE_NOT_SET`
5. `OUT_OF_RANGE` (field `dose`: the stored dose of the profile is outside 5.0 to 60.0 g)
6. `CONFLICT` (field `expectDose`)
7. `MOTOR_DISABLED`
8. `UPDATING` (the grinder is locked out for an update)
9. `LOAD_CELL_NOT_CONNECTED`
10. `LOAD_CELL_NO_DATA`
11. `LOAD_CELL_RATE_INVALID`

### 7.3 Stored changes

A request that changes a stored value (`update`, or `select` of another profile) is refused with `RATE_LIMITED` when the
previous stored change is less than 1000 ms (`GLINK_WRITE_MIN_INTERVAL_MS`) old. A request that changes nothing is never
rate limited and never writes flash. A profile set above 60 g by touch can be started by touch only; a BLE start of it is
`OUT_OF_RANGE`.

### 7.4 Disconnect and teardown

Every disconnect, whoever caused it, clears all per-connection state: the handshake, the TX subscription, the
reassembler (including the oversize discard state of 3.3 rule 5), the bad-fragment window, and any response or broadcast
not yet sent. A request that was already handed to the UI task:

- is claimed atomically by exactly one side. If the UI task claims it before the 1000 ms deadline, it is applied exactly
  as decided, even if the client has disconnected meanwhile (the response is then dropped). If the deadline passes first,
  the BLE task answers `UI_BUSY` and marks the request expired, and the UI task must **not** apply it afterwards;
- a lead-in already running continues or is cancelled by the normal rules (7.1, 5.5); a disconnect does not cancel it.

### 7.5 Client timeouts and retries

- Keep at most one request pending; the device answers a second one with `BUSY`.
- Wait up to 5 s for a response. `grindProfile` `get`, `update` and `select`, and `grindResult` `get` are safe to repeat.
  `start` is not: after a timeout, look at `periodInfo` (`STARTING` or `GRINDING` means it was taken) before sending it
  again.
- After `RATE_LIMITED`, retry no earlier than 1 s later. After `UI_BUSY` or `BUSY`, retry shortly.
- Ignore a response whose object name does not match the pending request.

## 8. Cadence and MTU guidance

The cadence of `periodInfo` is device-defined and a client must not depend on it. The device sends `periodInfo`:

- immediately when `devState`, `phase`, `selectProfile`, `blocked`, `resultSeq` or `profileRev` changes. While `GRINDING`
  these immediate sends are still subject to a minimum interval of 250 ms (`GLINK_PERIOD_GRINDING_FAST_MS`) when the ATT
  MTU is at least 100, otherwise 1000 ms (`GLINK_PERIOD_GRINDING_SLOW_MS`), except for a change of `devState`, which is
  sent at once;
- while `GRINDING` also at that cadence, because the live weight changes (weight changes alone do **not** trigger a
  send in any other state);
- as a heartbeat every 2000 ms (`GLINK_PERIOD_HEARTBEAT_MS`) when nothing was sent (so "`periodInfo` within 5 s" always
  holds on a live link, and the live weight outside a grind is at most 2 s old).

Throughput at the default MTU of 23 (20-byte packets, 12 payload bytes): `baseInfo` (about 190 bytes) is 16
notifications, `periodInfo` (about 170 bytes) is 15, `grindProfile` `get` of four profiles (about 400 bytes) is 34. At an
MTU of 185 or more each is one to three packets. The device uses the negotiated MTU if it learns it and assumes 23
otherwise. It sends at most 2 fragments per 20 ms and never blocks; if a notification fails it drops the rest of that
message and sends a fresh one later. The device never interleaves two messages, but it may replace a pending
`periodInfo` by a newer one.

## 9. Versioning

`proto` in `baseInfo` is the protocol version. Additive changes (new optional fields, new `devState` or `phase` values,
new error codes, new operations, new objects) keep `proto` at 1, so clients ignore what they do not know. A breaking
change gets a new service UUID and a new `proto`. A future authentication layer would be an extra business object (for
example an `auth` handshake) and would not change the frame or the existing objects. `grindControl` may change before
it is declared final (5.5).

## 10. Differences from Motto80

Re-read against the original README and `mt80_gatt_client.py` (BooKooCode/OpenSource, `bookoo_motto80`, README dated
2026-09-02) on 2026-11-05.

Extensions and differences:

| Topic | Motto80 | GLink v1 |
|---|---|---|
| Service UUIDs | `4d543830-000N-4b80-8f00-424f4f4b4f4f` | `53474257-000N-4632-901b-4752494e4421` |
| Device name | `BOOKOO MT80 xxxxxxxx` | `GrindByWeight` |
| Request size | `totalLength` 1 to 4096 | requests above 512 bytes get `PAYLOAD_TOO_LARGE` (4096 stays the frame limit); replies and broadcasts at most 1024 bytes |
| Handshake timeout | not specified | no `appHello` within 10 s: disconnect; other requests before it get `INVALID_STATE` except `stop` |
| Bad fragments | not specified | ten within 10 s disconnect the client |
| `periodInfo` | 5 Hz | on change, at most 4 Hz while grinding (1 Hz at a small MTU), heartbeat every 2 s |
| `baseInfo` | `releaseVer` (a version encoded as an integer, for example `102040416` = v1.2.4.0416), `snCode`, `wifiName` | the same three, but `releaseVer` is the firmware **build number** and `wifiName` is always `null`, plus `device`, `proto`, `fwVersion`, `board`, `doseMin`, `doseMax`, `maxRequestBytes` |
| `devState` | adds `HIGHSPEEDCLEAN`, `BOOTGUIDE`, `ADJUSTMENTPURGE` | adds `STARTING` and `DONE`; the three Motto80 extras are not used |
| Business objects | `geneSetting`, `grindSection`, `grindPreset` | `grindProfile`, `grindControl`, `grindResult` |
| Ops and selectors | `get set add delete restore update reorder`, selectors `all keys uid tombstones activeStats deviceStats uidStats` | `get select update` / `start stop` / `get`; selectors `all` and `index` only |
| Strictness | not specified | the reader of 4.2 (no escapes, duplicate keys refused, depth 4, no exponent, no arrays in requests, unknown keys refused in `grindProfile` and `grindControl`) |
| Unaddressable failures | not specified | answered under the pseudo-object `error` |
| Start and stop | not available ("under safety evaluation") | `grindControl`, provisional |
| Grind size | `bladeGap`: the stored burr-gap setting in integer um, range 0 to 999 (writing it does not move a physical gap) | `grindSize` (one decimal, optional `grindUnit`), display only. A new name, because `bladeGap` is already defined as a burr-gap value in um and a driver would apply that meaning to a free label with another unit |
| Result | not available | `grindResult` with the final weight (the Motto80 has no scale) |
| Change tracking | none | `periodInfo.profileRev` |

Same as Motto80: fragment frame v1 (header, little-endian, no CRC, 2 s reassembly gap, offset 0 restarts, packet at most
`min(ATT MTU - 3, 512)`), the three envelopes, no request ids, success and failure shapes, first error only and whole
request rejected, `field` only when it applies, the common error codes, no pairing, no encryption, no authentication,
"every disconnect clears subscription and handshake", discovery by name or MAC without the service UUID in the
advertisement, `periodInfo` within 5 s as "channel ready", clients tolerate unknown `devState` strings.

Details that only the original sources show (they were not in the summarised notes) and that GLink adopts: a fragment
must carry at least one payload byte; `offset` must be below `totalLength`; `totalLength` must not change within a
message; a message with `offset > 0` after the previous message completed is rejected; the Python client sends 20-byte
packets and writes with response; the Motto80 string limits are in UTF-8 bytes (GLink strings are ASCII and limited to
32 bytes); the reference client treats the first `periodInfo` within 5 s of the handshake as success.

## 11. Client checklist and conformance vectors

A client that follows this list works with any GLink v1 grinder:

1. Find the device by name `GrindByWeight` (and check the MAC); discover the service `53474257-0001-4632-901b-4752494e4421`.
2. Check RX is writable and TX has Notify; subscribe to TX; then send `appHello`.
3. Reassemble TX fragments with one reassembler (section 3.3); parse each message as JSON.
4. Wait for `baseInfo` (check `device` is `SGBW` and `proto` is 1) and `periodInfo` (within 5 s).
5. Send one request at a time and match the response by object name; handle `result` `fail` by `error.code`; follow 7.5.
6. Never rely on the `periodInfo` rate; ignore unknown fields and unknown `devState` / `phase` strings.
7. Re-read the profiles when `profileRev` changes, and treat the first `periodInfo` of every connection as changed (a
   rebooted grinder resets `profileRev` to 0).
8. For a dose: send `grindProfile` `update` (dose and optional grind size), check `changed`, then `grindControl` `start`
   with `expectDose`; expect `leadInMs` 3000 and decide as described in 5.5 whether the grind started; send `stop` at
   any time.
9. Read the result from the `grindResult` broadcast, or with `get` (compare `resultSeq`).
10. Keep the `grindControl` code replaceable (provisional).

Conformance data (every implementation, C++ and Python, must run all `reader_cases` and all other vector lists):
`tools/tests/glink_vectors.json` has the golden frame bytes (including the 43-byte handshake of 3.5),
single-packet and multi-packet receiver cases, the bad-fragment window cases, the reader cases, the dose, grind size and
unit cases (valid and invalid, with the expected error code and field), and one request/response pair for every object,
operation and error code, plus `periodInfo` and `grindResult` broadcast examples.

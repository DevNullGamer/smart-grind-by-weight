"""Contract guard for GLink v1 (chg020, docs/GLINK-PROTOCOL.md).

Pure text and data checks, no build and no hardware. The spec's tables (UUIDs, constants, vocabularies, state rules)
must equal the vectors (glink_vectors.json) and the constants in src/config/bluetooth.h; every example JSON in the
document must parse under the document's own reader rules; the worked fragment example must be byte-correct. The
vectors are also executed here against small reference models (frame reassembler, number/unit validators) so they
cannot drift from the rules they claim to pin. Later steps run the same vectors against the C++ and Python codecs.
"""

import json
import re
import struct
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
DOC = (ROOT / "docs" / "GLINK-PROTOCOL.md").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "config" / "bluetooth.h").read_text(encoding="utf-8")
V = json.loads((Path(__file__).resolve().parent / "glink_vectors.json").read_text(encoding="utf-8"))
LIMITS = V["limits"]
UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
HEX_LINE = re.compile(r"^(?:[0-9A-F]{2} )*[0-9A-F]{2}$")


# ---- helpers: document ----------------------------------------------------------------------------------------

def section(heading_prefix: str) -> str:
    """Text of the section whose heading line starts with heading_prefix, up to the next heading of the same or a
    higher level."""
    lines = DOC.splitlines()
    for start, line in enumerate(lines):
        if line.startswith(heading_prefix):
            level = len(line) - len(line.lstrip("#"))
            end = len(lines)
            for i in range(start + 1, len(lines)):
                nxt = lines[i]
                if nxt.startswith("#") and len(nxt) - len(nxt.lstrip("#")) <= level and not nxt.startswith("#!"):
                    end = i
                    break
            return "\n".join(lines[start:end])
    raise AssertionError(f"heading {heading_prefix!r} not found in GLINK-PROTOCOL.md")


def table_rows(text: str) -> list[list[str]]:
    rows = []
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows[1:]  # drop the header row


def bare(cell: str) -> str:
    return cell.replace("`", "").strip()


def first_column(text: str) -> list[str]:
    return [bare(r[0]) for r in table_rows(text)]


def blocked_map() -> dict[str, set]:
    """devState -> allowed `blocked` values from the table of section 6.5 (None is JSON null)."""
    allowed: dict[str, set] = {}
    for states, values in table_rows(section("### 6.5 `blocked`")):
        parsed = {None} if bare(values) == "null" else set(re.findall(r"`([A-Z_]+)`", values))
        for state in re.findall(r"`([A-Z]+)`", states):
            allowed[state] = parsed
    return allowed


def header_defines() -> dict[str, str]:
    return {m.group(1): m.group(2).strip() for m in re.finditer(r"^#define\s+(\w+)\s+(.+?)\s*(?://.*)?$", HEADER, re.M)}


def number(text: str) -> Decimal:
    text = text.strip()
    if text.lower().startswith("0x"):
        return Decimal(int(text, 16))
    return Decimal(text.rstrip("f"))


# ---- reference models -------------------------------------------------------------------------------------------

class Malformed(Exception):
    pass


NUMBER_RE = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?")
KEY_RE = re.compile(r"[A-Za-z0-9_]{1,32}")


def strict_parse(text: str, *, request: bool = True):
    """The reader rules of section 4.2. request=True also applies the request-only limits (string length, depth, no
    arrays); numbers become Decimal, everything else the Python equivalent."""
    pos = 0
    ws = " \t\r\n"

    def skip():
        nonlocal pos
        while pos < len(text) and text[pos] in ws:
            pos += 1

    def string():
        nonlocal pos
        assert text[pos] == '"'
        pos += 1
        start = pos
        while True:
            if pos >= len(text):
                raise Malformed("unterminated string")
            c = text[pos]
            if c == '"':
                break
            if c == "\\" or ord(c) < 0x20:
                raise Malformed("escape or control character")
            pos += 1
        value = text[start:pos]
        pos += 1
        if request and len(value.encode("utf-8")) > LIMITS["GLINK_JSON_STRING_MAX_LEN"]:
            raise Malformed("string too long")
        return value

    def value(depth):
        nonlocal pos
        skip()
        if pos >= len(text):
            raise Malformed("unexpected end")
        c = text[pos]
        if c == "{":
            if depth + 1 > (LIMITS["GLINK_JSON_MAX_DEPTH"] if request else 8):
                raise Malformed("too deep")
            pos += 1
            obj = {}
            skip()
            if pos < len(text) and text[pos] == "}":
                pos += 1
                return obj
            while True:
                skip()
                if pos >= len(text) or text[pos] != '"':
                    raise Malformed("key expected")
                key = string()
                if not KEY_RE.fullmatch(key):
                    raise Malformed("bad key")
                if key in obj:
                    raise Malformed("duplicate key")
                skip()
                if pos >= len(text) or text[pos] != ":":
                    raise Malformed("colon expected")
                pos += 1
                obj[key] = value(depth + 1)
                skip()
                if pos < len(text) and text[pos] == ",":
                    pos += 1
                    continue
                if pos < len(text) and text[pos] == "}":
                    pos += 1
                    return obj
                raise Malformed("comma or brace expected")
        if c == "[":
            if request:
                raise Malformed("arrays are not allowed in requests")
            pos += 1
            items = []
            skip()
            if pos < len(text) and text[pos] == "]":
                pos += 1
                return items
            while True:
                items.append(value(depth + 1))
                skip()
                if pos < len(text) and text[pos] == ",":
                    pos += 1
                    continue
                if pos < len(text) and text[pos] == "]":
                    pos += 1
                    return items
                raise Malformed("comma or bracket expected")
        if c == '"':
            return string()
        for literal, result in (("true", True), ("false", False), ("null", None)):
            if text.startswith(literal, pos):
                pos += len(literal)
                return result
        match = NUMBER_RE.match(text, pos)
        if not match or len(match.group(0)) > LIMITS["GLINK_JSON_NUMBER_MAX_LEN"]:
            raise Malformed("bad number")
        pos = match.end()
        if pos < len(text) and text[pos] not in ws + ",}]":
            raise Malformed("bad number")
        return Decimal(match.group(0))

    result = value(0)
    skip()
    if pos != len(text):
        raise Malformed("trailing data")
    if not isinstance(result, dict):
        raise Malformed("top level must be an object")
    return result


def compact(text: str) -> str:
    """Compact form of a JSON text (for comparing doc examples with vector messages)."""
    return json.dumps(json.loads(text), separators=(",", ":"), ensure_ascii=False)


def round1(d: Decimal) -> Decimal:
    q = d.quantize(Decimal("0.1"), rounding=ROUND_HALF_UP)  # ties away from zero, on the decimal digits
    return Decimal("0.0") if q == 0 else q


def ref_number_field(json_value: str, field: str, lo: Decimal, hi: Decimal, nullable: bool) -> dict:
    try:
        parsed = strict_parse('{"v":' + json_value + "}")["v"]
    except Malformed:
        return {"ok": False, "error": "MALFORMED_JSON"}
    if parsed is None and nullable:
        return {"ok": True, "value": None}
    if not isinstance(parsed, Decimal):
        return {"ok": False, "error": "INVALID_PARAM", "field": field}
    q = round1(parsed)
    if q < lo or q > hi:
        return {"ok": False, "error": "OUT_OF_RANGE", "field": field}
    return {"ok": True, "value": format(q, ".1f")}


UNIT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ./%-_")


def ref_unit(json_value: str) -> dict:
    try:
        parsed = strict_parse('{"v":' + json_value + "}")["v"]
    except Malformed:
        return {"ok": False, "error": "MALFORMED_JSON"}
    if parsed is None:
        return {"ok": True, "value": None}
    if not isinstance(parsed, str):
        return {"ok": False, "error": "INVALID_PARAM", "field": "grindUnit"}
    trimmed = parsed.strip(" ")
    if any(c not in UNIT_CHARS for c in trimmed):
        return {"ok": False, "error": "INVALID_PARAM", "field": "grindUnit"}
    if len(trimmed) > LIMITS["GLINK_GRIND_UNIT_MAX_LEN"]:
        return {"ok": False, "error": "OUT_OF_RANGE", "field": "grindUnit"}
    return {"ok": True, "value": trimmed or None}


def frames(payload: bytes, seq: int, packet_len: int) -> list[str]:
    cap = packet_len - LIMITS["GLINK_FRAME_HEADER_BYTES"]
    return [(struct.pack("<BBHHH", LIMITS["GLINK_FRAME_MAGIC"], LIMITS["GLINK_FRAME_VERSION"], seq, len(payload), off)
             + payload[off:off + cap]).hex() for off in range(0, len(payload), cap)]


class Reassembler:
    """The receiver rules of section 3.3 (device flavour: rule 5 included)."""

    def __init__(self):
        self.buf = None
        self.seq = self.total = self.last_ms = 0
        self.discard = None
        self.discard_ms = 0

    def _drop(self):
        self.buf = None

    def disconnect(self):
        """Section 7.4: a disconnect clears the reassembler, including the oversize discard state."""
        self.buf = None
        self.discard = None

    def feed(self, t_ms: int, packet: bytes):
        head = LIMITS["GLINK_FRAME_HEADER_BYTES"]
        if self.buf is not None and t_ms - self.last_ms > LIMITS["GLINK_REASSEMBLY_TIMEOUT_MS"]:
            self._drop()
        if self.discard and t_ms - self.discard_ms > LIMITS["GLINK_REASSEMBLY_TIMEOUT_MS"]:
            self.discard = None
        if len(packet) <= head:
            self._drop()
            return "bad_fragment", None
        magic, version, seq, total, off = struct.unpack_from("<BBHHH", packet)
        payload = packet[head:]
        if (magic != LIMITS["GLINK_FRAME_MAGIC"] or version != LIMITS["GLINK_FRAME_VERSION"] or total == 0
                or total > LIMITS["GLINK_MAX_MESSAGE_BYTES"] or off >= total or len(payload) > total - off):
            self._drop()
            return "bad_fragment", None
        if off > 0 and self.discard == (seq, total):
            self.discard_ms = t_ms
            return "discarded", None
        if off == 0:
            self.discard = None
            if total > LIMITS["GLINK_MAX_REQUEST_BYTES"]:
                self._drop()
                self.discard = (seq, total)
                self.discard_ms = t_ms
                return "payload_too_large", None
            self.buf, self.seq, self.total = bytearray(), seq, total
        elif self.buf is None or seq != self.seq or total != self.total or off != len(self.buf):
            self._drop()
            return "bad_fragment", None
        self.buf += payload
        self.last_ms = t_ms
        if len(self.buf) == self.total:
            message = bytes(self.buf).decode("utf-8")
            self._drop()
            return "complete", message
        return "partial", None


def disconnects(bad_at_ms: list[int]) -> bool:
    window = LIMITS["GLINK_BAD_FRAME_WINDOW_MS"]
    for i, t in enumerate(bad_at_ms):
        recent = [x for x in bad_at_ms[:i + 1] if x > t - window]
        if len(recent) >= LIMITS["GLINK_BAD_FRAME_LIMIT"]:
            return True
    return False


# ---- UUIDs and constants -----------------------------------------------------------------------------------------

def test_uuids_equal_in_doc_vectors_and_header():
    defines = header_defines()
    for key, macro in (("service", "GLINK_SERVICE_UUID"), ("rx", "GLINK_RX_CHAR_UUID"), ("tx", "GLINK_TX_CHAR_UUID")):
        uuid = V["uuids"][key]
        assert UUID_RE.match(uuid), uuid
        assert defines[macro].strip('"') == uuid
        assert f"`{uuid}`" in section("## 2. GATT")
    service, rx, tx = (V["uuids"][k] for k in ("service", "rx", "tx"))
    # scheme: SGBW ... GRIND!, attribute numbers 0001/0002/0003, one shared base for the two middle groups
    for uuid, number_ in ((service, "0001"), (rx, "0002"), (tx, "0003")):
        groups = uuid.split("-")
        assert bytes.fromhex(groups[0]) == b"SGBW" and bytes.fromhex(groups[4]) == b"GRIND!"
        assert groups[1] == number_
        assert groups[2][0] == "4" and groups[3][0] in "89ab"  # version-4 shape with RFC variant bits
    assert service.split("-")[2:] == rx.split("-")[2:] == tx.split("-")[2:]


def test_uuid_collision_check_against_existing_and_known_uuids():
    defines = header_defines()
    glink = {defines[m].strip('"').lower() for m in ("GLINK_SERVICE_UUID", "GLINK_RX_CHAR_UUID", "GLINK_TX_CHAR_UUID")}
    others = [v.strip('"').lower() for k, v in defines.items() if UUID_RE.match(v.strip('"').lower()) and
              not k.startswith("GLINK_")]
    assert len(others) >= 15  # the OTA, data, debug and sysinfo UUIDs
    assert not glink & set(others), "a GLink UUID equals an existing UUID"
    assert len({u.split("-")[0] for u in glink}) == 1
    assert glink.pop().split("-")[0] not in {u.split("-")[0] for u in others}, "the first group is shared with another service"
    # the UUIDs of the protocol we follow and of the Nordic UART service
    foreign = {"4d543830-0001-4b80-8f00-424f4f4b4f4f", "4d543830-0002-4b80-8f00-424f4f4b4f4f",
               "4d543830-0003-4b80-8f00-424f4f4b4f4f", "6e400001-b5a3-f393-e0a9-e50e24dcca9e"}
    assert not set(V["uuids"].values()) & foreign


def test_constants_equal_in_doc_vectors_and_header():
    defines = header_defines()
    header_numeric = {k: number(v) for k, v in defines.items() if k.startswith("GLINK_") and not UUID_RE.match(v.strip('"').lower())}
    assert header_numeric == {k: Decimal(str(v)) for k, v in LIMITS.items()}, "bluetooth.h and the vector limits differ"
    doc_rows = table_rows(section("### 3.4 Limits and constants"))
    doc = {bare(r[0]): number(bare(r[1])) for r in doc_rows}
    assert len(doc) == len(doc_rows), "duplicate constant in the doc table"
    assert doc == header_numeric, "bluetooth.h and the doc table differ"


def test_constants_are_consistent_with_each_other_and_the_firmware():
    assert LIMITS["GLINK_FRAME_HEADER_BYTES"] == 8 and LIMITS["GLINK_FRAME_MAGIC"] == 0xA5 and LIMITS["GLINK_FRAME_VERSION"] == 1
    assert LIMITS["GLINK_DEFAULT_PACKET_BYTES"] == 20 and LIMITS["GLINK_MAX_PACKET_BYTES"] == 512
    assert LIMITS["GLINK_MAX_REQUEST_BYTES"] <= LIMITS["GLINK_MAX_MESSAGE_BYTES"] and LIMITS["GLINK_MAX_REPLY_BYTES"] <= 4096
    assert LIMITS["GLINK_PROFILE_WRITABLE_INDEX"] == LIMITS["GLINK_PROFILE_COUNT_MAX"] - 1
    assert LIMITS["GLINK_HANDSHAKE_TIMEOUT_MS"] > 5000 > LIMITS["GLINK_PERIOD_HEARTBEAT_MS"], "periodInfo within 5 s"
    # the BLE dose range stays inside what the profile allows on the device, and the lead-in is the web panel's
    user = (ROOT / "src" / "config" / "user.h").read_text(encoding="utf-8")
    portal = (ROOT / "src" / "config" / "portal.h").read_text(encoding="utf-8")
    low = float(re.search(r"USER_MIN_TARGET_WEIGHT_G\s+([0-9.]+)", user).group(1))
    high = float(re.search(r"USER_MAX_TARGET_WEIGHT_G\s+([0-9.]+)", user).group(1))
    assert low <= LIMITS["GLINK_DOSE_MIN_G"] < LIMITS["GLINK_DOSE_MAX_G"] <= high
    assert int(re.search(r"PORTAL_LEAD_IN_MS\s+(\d+)", portal).group(1)) == LIMITS["GLINK_START_LEAD_IN_MS"]
    # the profile names follow the firmware
    controller = (ROOT / "src" / "controllers" / "profile_controller.cpp").read_text(encoding="utf-8")
    for index, name in enumerate(V["profile_names"][:3]):
        assert re.search(rf'strcpy\(profiles\[{index}\]\.name,\s*"{name}"\)', controller)
    assert V["profile_names"][3] == "PROFILE"


# ---- vocabularies ------------------------------------------------------------------------------------------------

def test_vocabularies_equal_in_doc_and_vectors():
    assert first_column(section("### 6.1 `devState`")) == V["devState"]
    assert first_column(section("### 6.2 `phase`")) == V["phase"]
    assert first_column(section("### 6.3 Result `code`")) == V["resultCode"]
    errors = table_rows(section("### 6.4 Error codes"))
    assert [bare(r[0]) for r in errors] == V["errorCode"]
    assert [bare(r[0]) for r in errors if bare(r[1]) == "start"] == V["blocked"]
    assert {bare(r[1]) for r in errors} == {"common", "glink", "start"}
    assert [bare(r[0]) for r in errors if bare(r[1]) == "common"] == V["errorCode"][:6]
    assert "MOTOR_FAULT" not in V["errorCode"] and "MOTOR_FAULT" not in DOC
    assert [bare(r[0]) for r in errors if bare(r[1]) == "glink"] == V["errorCode"][6:17]
    for name in V["origin"] + V["mode"]:
        assert f"`{name}`" in DOC
    assert len(set(V["errorCode"])) == len(V["errorCode"]) == 24


def test_phase_vocabulary_follows_the_grind_controller_enum():
    text = (ROOT / "src" / "controllers" / "grind_controller.h").read_text(encoding="utf-8")
    body = re.search(r"enum class GrindPhase\b[^{]*\{(.*?)\};", text, re.S).group(1)
    phases = [m.group(1) for line in body.splitlines() if (m := re.match(r"\s*([A-Z][A-Z0-9_]*)\b", line.split("//")[0]))]
    assert [p for p in phases if p not in ("IDLE", "COMPLETED", "TIMEOUT")] == V["phase"]


def test_blocked_table_maps_every_devstate():
    allowed = blocked_map()
    assert set(allowed) == set(V["devState"])
    for values in allowed.values():
        assert values <= set(V["blocked"]) | {None}
    assert allowed["IDLE"] == {None} and allowed["STARTING"] == {"COMMAND_IN_FLIGHT"}
    assert set().union(*allowed.values()) - {None} == set(V["blocked"])


def test_object_and_operation_tables_equal_vectors():
    rows = {bare(r[0]): r for r in table_rows(section("## 5. Objects").split("### 5.1")[0])}
    assert set(rows) == set(V["objects"]) | set(V["broadcast_objects"])
    for obj, ops in V["objects"].items():
        assert re.findall(r"`(\w+)`", rows[obj][2]) == ops, obj
        assert "request" in rows[obj][1]
    for obj in set(V["broadcast_objects"]) - {"grindResult"}:
        assert "(none)" in rows[obj][2] and "broadcast" in rows[obj][1]


def test_write_rules_and_start_order_equal_vectors():
    doc_rules = [{"state": bare(r[0]), "get": bare(r[1]), "update": bare(r[2]), "select": bare(r[3]), "start": bare(r[4])}
                 for r in table_rows(section("### 7.1 What each state allows"))]
    assert doc_rules == V["write_rules"]
    codes = set(V["errorCode"])
    for rule in V["write_rules"]:
        for op in ("get", "update", "select", "start"):
            assert rule[op] in codes | {"accept", "decide"}
    states = [r["state"] for r in V["write_rules"]]
    assert states[:2] == ["LEAD_IN", "UPDATING"], "the precedence of 7.1: lead-in first, then update"
    by_state = {r["state"]: r for r in V["write_rules"]}
    # the web panel's write rule: accepted on READY and in the menu only; INVALID_STATE is not a state refusal
    assert {s for s, r in by_state.items() if r["update"] == "accept"} == {"READY", "MENU"}
    assert all(r[op] != "INVALID_STATE" for r in V["write_rules"] for op in ("get", "update", "select", "start"))
    assert by_state["OTHER"]["update"] == by_state["OTHER"]["select"] == by_state["OTHER"]["start"] == "NOT_READY"
    assert by_state["UPDATING"]["get"] == "UPDATING" and by_state["LEAD_IN"]["get"] == "accept"
    order = re.findall(r"^\d+\. `([A-Z_]+)`", section("### 7.2 Order of the start refusals"), re.M)
    assert order == V["start_order"]
    assert set(order) <= codes and set(order) >= set(V["blocked"])
    # the real chain: portal_command_decide (in flight, update, busy/screen) then the grinder's start block
    # (motor, update lock, load cell), with the profile checks between the two groups
    assert order[:3] == ["COMMAND_IN_FLIGHT", "UPDATING", "NOT_READY"]
    assert order[3:6] == ["PROFILE_NOT_SET", "OUT_OF_RANGE", "CONFLICT"]
    assert order[6:] == ["MOTOR_DISABLED", "UPDATING", "LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NO_DATA", "LOAD_CELL_RATE_INVALID"]
    block = (ROOT / "src" / "controllers" / "grind_start_check.cpp").read_text(encoding="utf-8")
    assert [m for m in re.findall(r"GrindStartBlock::([A-Z_]+);", block.split("grind_start_block_reason")[0])] == [
        "MOTOR_DISABLED", "UPDATE_IN_PROGRESS", "LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NO_DATA", "LOAD_CELL_RATE_INVALID", "NONE"]
    guard = (ROOT / "src" / "network" / "portal_guard.cpp").read_text(encoding="utf-8")
    decide = guard[guard.index("PortalError portal_command_decide"):]
    assert decide.index("command_in_flight") < decide.index("update_held") < decide.index("in.busy") < decide.index("start_block")


# ---- frames ------------------------------------------------------------------------------------------------------

def test_golden_frames_are_byte_correct():
    for case in V["frame_cases"]:
        payload = case["payload"].encode("utf-8")
        assert len(payload) == case["payload_len"], case["name"]
        assert case["packets"] == frames(payload, case["sequence"], case["packet_len"]), case["name"]
        assert case["payload_len"] <= LIMITS["GLINK_MAX_REQUEST_BYTES"]
        strict_parse(case["payload"])  # every golden payload is a valid request
        for packet in case["packets"]:
            assert len(packet) // 2 <= min(case["packet_len"], LIMITS["GLINK_MAX_PACKET_BYTES"])
    by_name = {c["name"]: c for c in V["frame_cases"]}
    # the Motto80 worked example: 43 bytes, 4 fragments at offsets 0/12/24/36, first header A5 01 01 00 2B 00 00 00
    hello = by_name["handshake_20"]
    assert hello["payload"] == '{"request":{"appHello":{"op":"handshake"}}}' and hello["payload_len"] == 43
    assert len(hello["packets"]) == 4 and hello["packets"][0].startswith("a50101002b000000")
    assert [int.from_bytes(bytes.fromhex(p)[6:8], "little") for p in hello["packets"]] == [0, 12, 24, 36]
    assert by_name["handshake_single_packet_seq_0x0102"]["packets"][0].startswith("a50102012b000000")
    assert len(by_name["request_512_bytes_packet_512"]["packets"]) == 2
    assert by_name["request_512_bytes_packet_512"]["sequence"] == 0xFFFF


def test_worked_example_in_the_doc_is_byte_correct():
    block = re.search(r"```text\n(.*?)```", section("### 3.5 Worked example"), re.S).group(1).strip().splitlines()
    assert all(HEX_LINE.match(line) for line in block)
    doc_packets = [line.replace(" ", "").lower() for line in block]
    golden = {c["name"]: c for c in V["frame_cases"]}["handshake_20"]["packets"]
    assert doc_packets == golden
    assert b"".join(bytes.fromhex(p)[8:] for p in doc_packets).decode() == '{"request":{"appHello":{"op":"handshake"}}}'
    text = section("### 3.5 Worked example")
    assert "43 bytes" in text and "51-byte packet" in text


def test_single_packet_cases_judged_by_an_empty_reassembler():
    for case in V["single_packet_cases"]:
        outcome, message = Reassembler().feed(0, bytes.fromhex(case["packet"]))
        assert outcome == case["outcome"], case["name"]
    outcomes = {c["outcome"] for c in V["single_packet_cases"]}
    assert outcomes == {"bad_fragment", "payload_too_large", "partial", "complete"}


def test_reassembly_cases_follow_the_receiver_rules():
    for case in V["reassembly_cases"]:
        reassembler = Reassembler()
        for step in case["steps"]:
            if step.get("event") == "disconnect":
                reassembler.disconnect()
                continue
            outcome, message = reassembler.feed(step["t_ms"], bytes.fromhex(step["packet"]))
            assert outcome == step["outcome"], (case["name"], step)
            assert message == step.get("message"), (case["name"], step)
    names = {c["name"] for c in V["reassembly_cases"]}
    for needed in ("offset_zero_replaces_incomplete_message", "gap_of_exactly_2000_ms_is_allowed",
                   "gap_of_2001_ms_expires_the_message", "out_of_order_drops_the_message",
                   "oversize_discard_state_expires_after_a_gap_over_2000_ms", "disconnect_clears_the_reassembler",
                   "disconnect_clears_the_oversize_discard_state"):
        assert needed in names


def test_bad_fragment_window_cases():
    for case in V["bad_fragment_window_cases"]:
        assert disconnects(case["bad_at_ms"]) == case["disconnect"], case["name"]
    assert {c["disconnect"] for c in V["bad_fragment_window_cases"]} == {True, False}


def test_reader_cases():
    names = [c["name"] for c in V["reader_cases"]]
    assert len(names) == len(set(names)) >= 30
    for case in V["reader_cases"]:
        if case["accepted"]:
            strict_parse(case["text"])
        else:
            with pytest.raises(Malformed):
                strict_parse(case["text"])
    for needed in ("depth_exactly_4", "depth_5_refused", "string_32_bytes", "string_33_bytes", "key_32_chars", "key_33_chars",
                   "duplicate_key_top_level", "duplicate_key_depth_2", "duplicate_key_depth_3", "array_value", "array_top_level",
                   "trailing_closing_brace", "trailing_text", "trailing_nul", "leading_bom", "empty_message", "key_with_hyphen",
                   "key_with_space", "string_16_two_byte_characters_is_32_bytes",
                   "string_17_two_byte_characters_is_34_bytes", "raw_control_byte_in_string", "backslash_u_escape", "backslash_quote_escape"):
        assert needed in names, needed
    assert {c["accepted"] for c in V["reader_cases"]} == {True, False}
    # the limits the cases sit on
    sizes = {c["name"]: c["text"] for c in V["reader_cases"]}
    assert len(sizes["string_32_bytes"].split('"')[3]) == LIMITS["GLINK_JSON_STRING_MAX_LEN"]
    assert len(sizes["key_32_chars"].split('"')[1]) == LIMITS["GLINK_JSON_KEY_MAX_LEN"]
    assert sizes["depth_exactly_4"].count("{") == LIMITS["GLINK_JSON_MAX_DEPTH"]


# ---- numbers, grind size and unit ---------------------------------------------------------------------------------

def run_cases(cases, fn):
    names = [c["name"] for c in cases]
    assert len(names) == len(set(names))
    for case in cases:
        assert fn(case["json"]) == case["expect"], case["name"]


def test_dose_cases():
    run_cases(V["dose_cases"], lambda j: ref_number_field(j, "dose", Decimal("5.0"), Decimal("60.0"), False))
    lo, hi = LIMITS["GLINK_DOSE_MIN_G"], LIMITS["GLINK_DOSE_MAX_G"]
    assert (lo, hi) == (5.0, 60.0)
    got = {c["json"]: c["expect"] for c in V["dose_cases"]}
    assert got["18.05"]["value"] == "18.1" and got["18.04"]["value"] == "18.0" and got["4.95"]["value"] == "5.0"
    assert got["4.94"]["error"] == "OUT_OF_RANGE" and got["60.05"]["error"] == "OUT_OF_RANGE"
    kinds = {c["expect"].get("error") for c in V["dose_cases"]}
    assert kinds == {None, "OUT_OF_RANGE", "MALFORMED_JSON", "INVALID_PARAM"}


def test_grind_size_cases():
    run_cases(V["grind_size_cases"], lambda j: ref_number_field(j, "grindSize", Decimal("0.0"), Decimal(str(LIMITS["GLINK_GRIND_SIZE_MAX"])), True))
    tokens = {c["json"] for c in V["grind_size_cases"]}
    for needed in ("-0.1", "0", "999.9", "1000", "12.34", "null"):  # the cases the plan names
        assert needed in tokens, needed


def test_grind_unit_cases():
    run_cases(V["grind_unit_cases"], ref_unit)
    tokens = {c["json"] for c in V["grind_unit_cases"]}
    for needed in ('""', '"clicks"', '"abcdefg"', '"   "', '"é"', '"a\\"b"', '"a\\\\b"', '"a\tb"', "null"):
        assert needed in tokens, needed
    assert {c["expect"].get("error") for c in V["grind_unit_cases"]} == {None, "OUT_OF_RANGE", "MALFORMED_JSON", "INVALID_PARAM"}
    inner = lambda c: len(c["json"]) - 2  # noqa: E731  (characters between the quotes)
    assert any(inner(c) == 7 and c["expect"].get("error") == "OUT_OF_RANGE" for c in V["grind_unit_cases"])
    assert any(inner(c) == 6 and c["expect"].get("ok") for c in V["grind_unit_cases"])


# ---- pairs and broadcasts ----------------------------------------------------------------------------------------

def parse_response(text: str):
    doc = strict_parse(text, request=False)
    assert list(doc) == ["response"] and len(doc["response"]) == 1
    (obj, body), = doc["response"].items()
    assert len(text.encode("utf-8")) <= LIMITS["GLINK_MAX_REPLY_BYTES"]
    return obj, body


def test_pairs_are_well_formed():
    names = [p["name"] for p in V["pairs"]]
    assert len(names) == len(set(names))
    codes = set(V["errorCode"])
    for pair in V["pairs"]:
        name = pair["name"]
        request_obj = None
        if pair["request"] is not None:
            assert pair["request"].encode("utf-8") and len(pair["request"].encode("utf-8")) <= LIMITS["GLINK_MAX_REQUEST_BYTES"]
            expect_malformed = pair["response"] is not None and '"MALFORMED_JSON"' in pair["response"]
            if expect_malformed:
                with pytest.raises(Malformed):
                    strict_parse(pair["request"])
            else:
                doc = strict_parse(pair["request"])
                if list(doc) == ["request"] and isinstance(doc["request"], dict) and len(doc["request"]) == 1:
                    request_obj = next(iter(doc["request"]))
        else:
            reassembler = Reassembler()
            outcomes = [reassembler.feed(i, bytes.fromhex(p))[0] for i, p in enumerate(pair["request_frames"])]
            assert outcomes[0] == "payload_too_large"
        if pair["response"] is None:
            assert pair["broadcasts"], name
        else:
            obj, body = parse_response(pair["response"])
            if request_obj in V["objects"]:
                assert obj == request_obj, name
            else:
                assert obj == "error", name
            assert body["result"] in ("success", "fail")
            if body["result"] == "success":
                assert "data" in body and "error" not in body
            else:
                err = body["error"]
                assert err["code"] in codes, name
                assert isinstance(err["message"], str) and err["message"]
                assert set(err) <= {"code", "message", "field"}
                assert "data" not in body
        for message in pair.get("broadcasts", []):
            doc = strict_parse(message, request=False)
            assert list(doc) == ["broadcast"]
            assert len(message.encode("utf-8")) <= LIMITS["GLINK_MAX_REPLY_BYTES"]
            if "periodInfo" in doc["broadcast"]:
                validate_period(message)
        assert pair["state"]


def test_pairs_cover_every_object_operation_and_error_code():
    covered = set()
    seen_codes = set()
    for pair in V["pairs"]:
        if pair["request"] is not None and "MALFORMED_JSON" not in (pair["response"] or ""):
            doc = strict_parse(pair["request"])
            request = doc.get("request")
            if isinstance(request, dict) and len(request) == 1:
                obj, body = next(iter(request.items()))
                op = body.get("op") if isinstance(body, dict) else None
                response_ok = pair["response"] is None or '"result":"success"' in pair["response"]
                if response_ok and op in V["objects"].get(obj, []):
                    covered.add((obj, op))
        if pair["response"]:
            _, body = parse_response(pair["response"])
            if body["result"] == "fail":
                seen_codes.add(body["error"]["code"])
    assert covered == {(o, op) for o, ops in V["objects"].items() for op in ops}
    assert seen_codes == set(V["errorCode"]), f"no pair for {set(V['errorCode']) ^ seen_codes}"


def test_pairs_follow_the_documented_rules():
    by_name = {p["name"]: p for p in V["pairs"]}
    # every field named in an error exists as a request field or a selector path of that request
    for pair in V["pairs"]:
        if pair["request"] is None or not pair["response"] or '"result":"fail"' not in pair["response"]:
            continue
        err = parse_response(pair["response"])[1]["error"]
        if "field" in err and "MALFORMED_JSON" not in pair["response"]:
            doc = strict_parse(pair["request"])
            flat = set()
            stack = [("", doc)]
            while stack:
                prefix, node = stack.pop()
                if isinstance(node, dict):
                    for k, val in node.items():
                        flat.add(prefix + k)
                        stack.append((prefix + k + ".", val))
            leaf = err["field"].split(".")[-1]
            allowed_missing = err["code"] in ("MISSING_FIELD", "NOT_FOUND", "PROFILE_NOT_SET", "OUT_OF_RANGE", "CONFLICT")
            assert any(f.endswith(leaf) for f in flat) or allowed_missing, (pair["name"], err["field"])
    # a BLE-task validation error beats UPDATING (7.1)
    assert '"code":"OUT_OF_RANGE"' in by_name["profile_update_validation_beats_updating"]['response']
    assert "update in progress" in by_name["profile_update_validation_beats_updating"]["state"]
    # handshake pairs: two broadcasts, in the documented order
    hello = by_name["appHello_handshake"]
    assert [list(strict_parse(m, request=False)["broadcast"])[0] for m in hello["broadcasts"]] == ["baseInfo", "periodInfo"]
    # the stop request is accepted before the handshake
    assert '"accepted":true' in by_name["control_stop_before_handshake"]["response"]
    # an unchanged update is not a stored change
    assert '"changed":false,"stored":false' in by_name["profile_update_unchanged"]["response"]
    assert '"changed":true,"stored":true' in by_name["profile_update_create"]["response"]


PERIOD_ORDER = ("devState", "weight", "selectProfile", "phase", "target", "mode", "leadInMs", "blocked", "resultSeq", "profileRev")


def validate_period(message: str) -> str:
    """One periodInfo message against the table of 5.3 and the rules of 6.5. Returns its devState."""
    fields = table_rows(section("### 5.3 `periodInfo`"))
    always = [bare(r[0]) for r in fields if bare(r[2]) == "always"]
    when = {bare(r[0]): bare(r[2]) for r in fields if bare(r[2]) != "always"}
    assert when == {"phase": "while GRINDING", "target": "while STARTING, GRINDING", "mode": "with target",
                    "leadInMs": "while STARTING"}
    assert [bare(r[0]) for r in fields] == [k for k in PERIOD_ORDER], "documented member order"
    info = strict_parse(message, request=False)["broadcast"]["periodInfo"]
    state = info["devState"]
    assert state in V["devState"] and set(always) <= set(info), message
    assert set(info) <= set(always) | set(when), message
    assert ("phase" in info) == (state == "GRINDING") and (("leadInMs" in info) == (state == "STARTING")), message
    assert ("target" in info) == (state in ("STARTING", "GRINDING")) and ("mode" in info) == ("target" in info), message
    if "phase" in info:
        assert info["phase"] in V["phase"]
    if "mode" in info:
        assert info["mode"] in V["mode"]
    assert info["blocked"] in blocked_map()[state], message
    assert 0 <= info["selectProfile"] <= LIMITS["GLINK_PROFILE_WRITABLE_INDEX"]
    assert info["weight"] is None or isinstance(info["weight"], Decimal)
    for counter in ("resultSeq", "profileRev"):
        assert info[counter] == info[counter].to_integral_value() >= 0, message
    assert list(info) == [k for k in PERIOD_ORDER if k in info], "documented member order"
    return state


def test_base_info_period_info_and_result_fields_equal_the_doc_tables():
    base_pair = next(p for p in V["pairs"] if p["name"] == "appHello_handshake")
    base = strict_parse(base_pair["broadcasts"][0], request=False)["broadcast"]["baseInfo"]
    assert list(base) == first_column(section("### 5.2 `baseInfo`"))
    assert base["proto"] == LIMITS["GLINK_PROTO_VERSION"] and base["device"] == "SGBW" and base["wifiName"] is None
    assert float(base["doseMin"]) == LIMITS["GLINK_DOSE_MIN_G"] and float(base["doseMax"]) == LIMITS["GLINK_DOSE_MAX_G"]
    assert int(base["maxRequestBytes"]) == LIMITS["GLINK_MAX_REQUEST_BYTES"] and re.fullmatch(r"[0-9a-f]{12}", base["snCode"])

    seen_states = {validate_period(case["message"]) for case in V["period_cases"]}
    assert seen_states == set(V["devState"])
    pair_messages = [m for p in V["pairs"] for m in p.get("broadcasts", []) if "periodInfo" in m]
    assert pair_messages, "periodInfo inside pairs is validated too"

    result_fields = first_column(section("### 5.6 `grindResult`"))
    for item in V["result_broadcasts"]:
        result = strict_parse(item["message"], request=False)["broadcast"]["grindResult"]
        assert list(result) == result_fields, item["name"]
        assert result["code"] in V["resultCode"] and result["origin"] in V["origin"] and result["mode"] in V["mode"]
    assert {r["code"] for r in (strict_parse(i["message"], request=False)["broadcast"]["grindResult"] for i in V["result_broadcasts"])} >= {"OK", "STOPPED"}

    entry_table = re.search(r"Entry \(`data` of `get` and `update`\):\n\n(.*?)\n\n", section("### 5.4 `grindProfile`"), re.S).group(1)
    entry_pair = next(p for p in V["pairs"] if p["name"] == "profile_get_all_with_profile_3")
    entries = parse_response(entry_pair["response"])[1]["data"]
    assert len(entries) == 4
    for i, entry in enumerate(entries):
        assert list(entry) == first_column(entry_table) and entry["index"] == i and entry["name"] == V["profile_names"][i]
        assert entry["writable"] == (i == LIMITS["GLINK_PROFILE_WRITABLE_INDEX"])


# ---- document ----------------------------------------------------------------------------------------------------

def test_every_json_example_in_the_doc_parses_and_matches_a_vector():
    blocks = re.findall(r"```json\n(.*?)\n```", DOC, re.S)
    assert len(blocks) >= 12
    known = set()
    for pair in V["pairs"]:
        known.update(filter(None, [pair["request"], pair["response"]] + pair.get("broadcasts", [])))
    for group in ("period_cases", "result_broadcasts"):
        known.update(item["message"] for item in V[group])
    compacted = set()
    for text in known:
        try:
            compacted.add(compact(text))
        except ValueError:
            pass  # the deliberately malformed requests
    known = compacted
    for block in blocks:
        top = next(iter(json.loads(block)))
        assert top in ("request", "response", "broadcast"), block
        limit = LIMITS["GLINK_MAX_REQUEST_BYTES"] if top == "request" else LIMITS["GLINK_MAX_REPLY_BYTES"]
        assert len(compact(block).encode("utf-8")) <= limit
        strict_parse(block, request=(top == "request"))
        assert compact(block) in known, f"doc example not in the vectors: {block[:90]}"


def test_doc_has_the_required_sections_and_statements():
    headings = re.findall(r"^## (\d+)\. (.+)$", DOC, re.M)
    assert [int(n) for n, _ in headings] == list(range(1, 12))
    titles = [t for _, t in headings]
    assert titles[0].startswith("Scope and trust model") and titles[1] == "GATT" and titles[2].startswith("Fragment frame v1")
    assert titles[3].startswith("JSON envelopes") and titles[4] == "Objects" and titles[5] == "Vocabularies"
    assert titles[6].startswith("State and write rules") and titles[7].startswith("Cadence and MTU")
    assert titles[8] == "Versioning" and titles[9] == "Differences from Motto80" and titles[10].startswith("Client checklist")
    # the unauthenticated-start risk, stated plainly, at the top and in section 1
    assert "Anyone within Bluetooth range can start your grinder" in DOC.split("## 1.")[0]
    assert "**start a grind.**" in section("## 1. Scope and trust model")
    assert "no authentication and no pairing" in DOC.split("## 1.")[0].lower()
    # grindControl is provisional, at the status line, the section and the checklist
    assert "provisional" in DOC.split("## 1.")[0].lower()
    assert "(PROVISIONAL)" in section("### 5.5 `grindControl`") and "Provisional." in section("### 5.5 `grindControl`")
    assert "replaceable (provisional)" in section("## 11.")
    # the user decisions that shape the contract
    text = section("### 5.4 `grindProfile`")
    assert "exists only after a client has" in text and "`PROFILE_NOT_SET`" in text and "touch only" in text
    assert "S-B" not in DOC and "REMOTE_START_OFF" not in DOC and "Allow remote start" not in DOC
    assert "always 3000" in section("### 5.5 `grindControl`")
    assert V["version"] == 1 and "proto" in DOC


# ---- prose numbers ------------------------------------------------------------------------------------------------

WORD_NUMBERS = {"Ten": 10}


def test_numbers_in_the_prose_equal_the_constants():
    # "N ms (`GLINK_X`)", "N s (`GLINK_X`)", "N bytes (`GLINK_X`)" anywhere in the document
    found = re.findall(r"(\d[\d ]*\d|\d)\s+(ms|s|bytes)\s+\(`(GLINK_\w+)`[,)]", DOC)
    named = {name for _, _, name in found}
    for needed in ("GLINK_START_LEAD_IN_MS", "GLINK_REASSEMBLY_TIMEOUT_MS", "GLINK_HANDSHAKE_TIMEOUT_MS",
                   "GLINK_BAD_FRAME_WINDOW_MS", "GLINK_WRITE_MIN_INTERVAL_MS", "GLINK_UI_REPLY_TIMEOUT_MS",
                   "GLINK_PERIOD_HEARTBEAT_MS", "GLINK_PERIOD_GRINDING_FAST_MS", "GLINK_PERIOD_GRINDING_SLOW_MS",
                   "GLINK_MAX_REQUEST_BYTES", "GLINK_MAX_REPLY_BYTES", "GLINK_DEFAULT_PACKET_BYTES"):
        assert needed in named, f"{needed} is not stated next to its number in the prose"
    for number_, unit, name in found:
        value = int(number_.replace(" ", "")) * (1000 if unit == "s" else 1)
        assert value == LIMITS[name], f"{number_} {unit} next to {name}: the constant is {LIMITS[name]}"
    for word, name in re.findall(r"(\w+) \(`(GLINK_BAD_FRAME_LIMIT)`\)", DOC):
        assert WORD_NUMBERS[word] == LIMITS[name]
    # dose and grind size ranges in prose
    ranges = set(re.findall(r"(\d+\.\d) to (\d+\.\d) g", DOC))
    dose = (f"{LIMITS['GLINK_DOSE_MIN_G']:.1f}", f"{LIMITS['GLINK_DOSE_MAX_G']:.1f}")
    assert ranges <= {dose, ("0.0", f"{LIMITS['GLINK_GRIND_SIZE_MAX']:.1f}")}, ranges
    assert dose in ranges
    assert re.findall(r"0\.0 to (\d+\.\d)", DOC) == [f"{LIMITS['GLINK_GRIND_SIZE_MAX']:.1f}"] * len(
        re.findall(r"0\.0 to (\d+\.\d)", DOC))
    # the lead-in: "3 s" in prose and 3000 in the examples
    assert LIMITS["GLINK_START_LEAD_IN_MS"] == 3000 and '"leadInMs":3000' in DOC
    assert all(int(n) * 1000 == LIMITS["GLINK_START_LEAD_IN_MS"] for n in re.findall(r"lead-in of (\d+) s", DOC))
    # structural limits stated in words
    assert f"at most {LIMITS['GLINK_JSON_MAX_DEPTH']} objects deep" in DOC
    assert f"1 to {LIMITS['GLINK_JSON_KEY_MAX_LEN']} characters" in DOC
    assert f"at most {LIMITS['GLINK_JSON_STRING_MAX_LEN']} bytes" in DOC
    assert f"at most {LIMITS['GLINK_JSON_NUMBER_MAX_LEN']} characters" in DOC
    assert f"up to {LIMITS['GLINK_GRIND_UNIT_MAX_LEN']} characters" in DOC
    assert f"more than {LIMITS['GLINK_GRIND_UNIT_MAX_LEN']} characters" in DOC
    assert f"less than {LIMITS['GLINK_BAD_FRAME_WINDOW_MS']:,} ms".replace(",", " ") in DOC
    assert f"at least {LIMITS['GLINK_FAST_MTU_MIN']}" in DOC
    assert f"1 to {LIMITS['GLINK_MAX_MESSAGE_BYTES']}" in DOC and f"above {LIMITS['GLINK_MAX_MESSAGE_BYTES']}" in DOC
    assert f"above {LIMITS['GLINK_MAX_REQUEST_BYTES']}" in DOC


def test_unit_charset_sentence_equals_the_reference_charset():
    row = next(r for r in table_rows(section("### 5.4 `grindProfile`")) if bare(r[0]) == "grindUnit" and "trimmed" in r[2])[2]
    sentence = re.search(r"of (`A-Z a-z 0-9`.*?) ;?empty", row.replace("; empty", " empty")).group(1)
    chars = set()
    for token in re.findall(r"`([^`]+)`", sentence):
        if token == "A-Z a-z 0-9":
            chars |= set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
        else:
            chars.add(token)
    chars.add(" ")  # the word "space" between the backticked tokens
    assert "space" in sentence and chars == UNIT_CHARS


def test_flash_wear_statement_uses_the_real_partition_size():
    partitions = (ROOT / "partitions.csv").read_text(encoding="utf-8")
    assert re.search(r"^nvs,\s*data,\s*nvs,\s*0x9000,\s*20K", partitions, re.M)
    assert "NVS partition is 20 KB" in DOC and "slows flash wear but does **not** prevent it" in DOC
    assert "Motto80 protocol, whose transport GLink copies, withholds remote start" in DOC
    assert "stop nothing: any central can send `appHello`" in DOC
    assert "does **not** stop the client from sending another `start` immediately" in DOC

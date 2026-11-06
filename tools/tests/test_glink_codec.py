"""GLink v1 Python codec (tools/ble/glink_codec.py) against the shared conformance vectors.

The C++ twin (src/bluetooth/glink_*.cpp, test_native/test_glink_*.cpp) runs the same vectors; both must pass. Besides
the vectors this file pins the rules written out in docs/GLINK-PROTOCOL.md sections 3, 4 and 7 and feeds malformed input.
"""

import json
import random
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ble"))

import glink_codec as g  # noqa: E402

V = json.loads((Path(__file__).resolve().parent / "glink_vectors.json").read_text(encoding="utf-8"))


def msg_bytes(text: str) -> bytes:
    return text.encode("utf-8")


# ---- constants and vocabularies ---------------------------------------------------------------------------------

def test_constants_equal_the_vector_limits():
    pairs = {
        "GLINK_PROTO_VERSION": g.PROTO_VERSION, "GLINK_FRAME_MAGIC": g.FRAME_MAGIC, "GLINK_FRAME_VERSION": g.FRAME_VERSION,
        "GLINK_FRAME_HEADER_BYTES": g.FRAME_HEADER_BYTES, "GLINK_DEFAULT_PACKET_BYTES": g.DEFAULT_PACKET_BYTES,
        "GLINK_MAX_PACKET_BYTES": g.MAX_PACKET_BYTES, "GLINK_MAX_MESSAGE_BYTES": g.MAX_MESSAGE_BYTES,
        "GLINK_MAX_REQUEST_BYTES": g.MAX_REQUEST_BYTES, "GLINK_MAX_REPLY_BYTES": g.MAX_REPLY_BYTES,
        "GLINK_REASSEMBLY_TIMEOUT_MS": g.REASSEMBLY_TIMEOUT_MS, "GLINK_BAD_FRAME_LIMIT": g.BAD_FRAME_LIMIT,
        "GLINK_BAD_FRAME_WINDOW_MS": g.BAD_FRAME_WINDOW_MS, "GLINK_JSON_MAX_DEPTH": g.JSON_MAX_DEPTH,
        "GLINK_JSON_KEY_MAX_LEN": g.JSON_KEY_MAX_LEN, "GLINK_JSON_STRING_MAX_LEN": g.JSON_STRING_MAX_LEN,
        "GLINK_JSON_NUMBER_MAX_LEN": g.JSON_NUMBER_MAX_LEN, "GLINK_PROFILE_COUNT_MAX": g.PROFILE_COUNT_MAX,
        "GLINK_PROFILE_WRITABLE_INDEX": g.PROFILE_WRITABLE_INDEX, "GLINK_GRIND_UNIT_MAX_LEN": g.GRIND_UNIT_MAX_LEN,
        "GLINK_START_LEAD_IN_MS": g.START_LEAD_IN_MS, "GLINK_WRITE_MIN_INTERVAL_MS": g.WRITE_MIN_INTERVAL_MS,
    }
    for name, value in pairs.items():
        assert V["limits"][name] == value, name
    assert V["limits"]["GLINK_DOSE_MIN_G"] * 10 == g.DOSE_MIN_TENTHS
    assert V["limits"]["GLINK_DOSE_MAX_G"] * 10 == g.DOSE_MAX_TENTHS
    assert round(V["limits"]["GLINK_GRIND_SIZE_MAX"] * 10) == g.GRIND_SIZE_MAX_TENTHS


def test_vocabularies_equal_the_vectors():
    assert g.ERRORS == V["errorCode"]
    assert g.DEV_STATES == V["devState"]
    assert g.PHASES == V["phase"]
    assert g.RESULT_CODES == V["resultCode"]
    assert g.ORIGINS == V["origin"]
    assert g.MODES == V["mode"]
    assert g.PROFILE_NAMES == V["profile_names"]
    assert g.OBJECTS == V["objects"]


# ---- fragment frame ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("case", V["frame_cases"], ids=lambda c: c["name"])
def test_fragmenter_matches_golden_frames(case):
    packets = g.fragments(msg_bytes(case["payload"]), case["sequence"], case["packet_len"])
    assert [p.hex() for p in packets] == case["packets"]
    assert g.fragment_count(len(msg_bytes(case["payload"])), case["packet_len"]) == len(case["packets"])


def test_fragmenter_rejects_bad_arguments():
    assert g.fragment_count(0, 20) == 0
    assert g.fragment_count(4097, 20) == 0
    assert g.fragment_count(10, 8) == 0
    assert g.fragment_count(10, 0) == 0
    assert g.fragment_count(10, 100000) == 1
    assert g.fragment_count(10, 14) == 2
    assert g.fragments(b"", 1, 20) == []


@pytest.mark.parametrize("length", [1, 11, 12, 13, 24, 100, 511, 512, 513, 1024, 4096])
@pytest.mark.parametrize("packet", [9, 20, 21, 185, 512])
def test_fragmenter_round_trips_through_the_reassembler(length, packet):
    message = bytes((i * 7 + 3) & 0xFF for i in range(length))
    r = g.Reassembler(capacity=4096)
    parts = g.fragments(message, 0xFFFF, packet)
    assert parts
    for i, p in enumerate(parts):
        assert r.feed(p, i) == (g.COMPLETE if i == len(parts) - 1 else g.PARTIAL)
    assert r.message == message


def test_header_parse_checks():
    good = bytes.fromhex("a50102012b0000007b")
    assert g.header_parse(good) == (0x0102, 43, 0)
    for n in range(len(good) - 0):
        if n <= g.FRAME_HEADER_BYTES:
            assert g.header_parse(good[:n]) is None
    assert g.header_parse(b"\xa4" + good[1:]) is None
    assert g.header_parse(good[:1] + b"\x02" + good[2:]) is None
    assert g.header_parse(None) is None
    assert g.header_write(0x0102, 0x0304, 0x0506).hex() == "a501020104030605"


def packet(seq, total, offset, n):
    return g.header_write(seq, total, offset) + b"x" * n


@pytest.mark.parametrize("case", V["single_packet_cases"], ids=lambda c: c["name"])
def test_single_packet_cases(case):
    assert g.Reassembler().feed(bytes.fromhex(case["packet"]), 0) == case["outcome"]


@pytest.mark.parametrize("case", V["reassembly_cases"], ids=lambda c: c["name"])
def test_reassembly_cases(case):
    r = g.Reassembler()
    for step in case["steps"]:
        if step.get("event") == "disconnect":
            r.reset()
            continue
        assert r.feed(bytes.fromhex(step["packet"]), step["t_ms"]) == step["outcome"], step
        if "message" in step:
            assert r.message == msg_bytes(step["message"])


def test_request_frames_vector_is_payload_too_large():
    seen = 0
    for pair in V["pairs"]:
        if "request_frames" in pair:
            assert g.Reassembler().feed(bytes.fromhex(pair["request_frames"][0]), 0) == g.PAYLOAD_TOO_LARGE
            seen += 1
    assert seen


@pytest.mark.parametrize("case", V["bad_fragment_window_cases"], ids=lambda c: c["name"])
def test_bad_fragment_window_cases(case):
    w = g.BadFrameWindow()
    result = False
    for t in case["bad_at_ms"]:
        result = w.record(t)
    assert result == case["disconnect"]


def test_bad_fragment_window_edges():
    w = g.BadFrameWindow()
    assert not any(w.record(100 * i) for i in range(9))
    assert w.record(900)
    assert w.record(901)
    w.reset()
    assert not any(w.record(0) for _ in range(9))
    assert not w.record(g.BAD_FRAME_WINDOW_MS)
    w.reset()
    assert not any(w.record(0xFFFFFFF0 + i) for i in range(9))
    assert w.record(5)


def test_total_length_bounds():
    r = g.Reassembler()
    assert r.feed(packet(1, 0, 0, 5), 0) == g.BAD_FRAGMENT
    assert r.feed(packet(1, 512, 0, 12), 0) == g.PARTIAL
    assert r.feed(packet(1, 513, 0, 12), 0) == g.PAYLOAD_TOO_LARGE
    assert r.feed(packet(1, 4096, 0, 12), 0) == g.PAYLOAD_TOO_LARGE
    assert r.feed(packet(1, 4097, 0, 12), 0) == g.BAD_FRAGMENT
    assert r.feed(packet(1, 65535, 0, 12), 0) == g.BAD_FRAGMENT
    assert r.feed(packet(1, 1, 0, 1), 0) == g.COMPLETE
    assert len(r.message) == 1


def test_offset_rules():
    r = g.Reassembler()
    assert r.feed(packet(2, 40, 0, 12), 0) == g.PARTIAL
    assert r.feed(packet(2, 40, 13, 12), 1) == g.BAD_FRAGMENT  # gap
    assert r.feed(packet(2, 40, 12, 12), 2) == g.BAD_FRAGMENT  # the message is gone
    assert r.feed(packet(2, 40, 0, 12), 3) == g.PARTIAL
    assert r.feed(packet(2, 40, 11, 12), 4) == g.BAD_FRAGMENT  # overlap
    assert r.feed(packet(3, 20, 0, 12), 5) == g.PARTIAL
    assert r.feed(packet(3, 20, 12, 9), 6) == g.BAD_FRAGMENT  # longer than the remaining total
    assert r.feed(packet(3, 20, 0, 12), 7) == g.PARTIAL
    assert r.feed(packet(3, 20, 12, 8), 8) == g.COMPLETE
    assert r.feed(packet(3, 20, 20, 1), 9) == g.BAD_FRAGMENT  # offset == total


def test_new_offset_zero_mid_message_restarts():
    r = g.Reassembler()
    assert r.feed(packet(1, 30, 0, 12), 0) == g.PARTIAL
    assert r.feed(packet(9, 24, 0, 12), 1) == g.PARTIAL
    assert r.feed(packet(1, 30, 12, 12), 2) == g.BAD_FRAGMENT
    assert r.feed(packet(9, 24, 0, 12), 3) == g.PARTIAL
    assert r.feed(packet(9, 24, 12, 12), 4) == g.COMPLETE


@pytest.mark.parametrize("gap,expected", [(1999, g.COMPLETE), (2000, g.COMPLETE), (2001, g.BAD_FRAGMENT)])
def test_expiry_boundary(gap, expected):
    r = g.Reassembler()
    assert r.feed(packet(5, 24, 0, 12), 1000) == g.PARTIAL
    assert r.feed(packet(5, 24, 12, 12), 1000 + gap) == expected


def test_expiry_is_measured_between_fragments():
    r = g.Reassembler()
    assert r.feed(packet(5, 36, 0, 12), 0) == g.PARTIAL
    assert r.feed(packet(5, 36, 12, 12), 1500) == g.PARTIAL
    assert r.feed(packet(5, 36, 24, 12), 3000) == g.COMPLETE


def test_clock_wraps():
    r = g.Reassembler()
    assert r.feed(packet(5, 24, 0, 12), 0xFFFFFF00) == g.PARTIAL
    assert r.feed(packet(5, 24, 12, 12), 0x10) == g.COMPLETE


def test_sequence_wrap_and_match():
    r = g.Reassembler()
    for seq in (65534, 65535, 0, 1):
        assert r.feed(packet(seq, 24, 0, 12), 0) == g.PARTIAL
        assert r.feed(packet(seq, 24, 12, 12), 1) == g.COMPLETE
    assert r.feed(packet(65535, 24, 0, 12), 0) == g.PARTIAL
    assert r.feed(packet(0, 24, 12, 12), 1) == g.BAD_FRAGMENT


def test_oversize_discard_ends_on_last_fragment():
    r = g.Reassembler()
    assert r.feed(packet(7, 600, 0, 300), 0) == g.PAYLOAD_TOO_LARGE
    assert r.feed(packet(7, 600, 300, 300), 1) == g.DISCARDED
    assert r.feed(packet(7, 600, 300, 300), 2) == g.BAD_FRAGMENT
    assert r.feed(packet(8, 600, 0, 300), 3) == g.PAYLOAD_TOO_LARGE
    assert r.feed(packet(9, 600, 300, 300), 4) == g.BAD_FRAGMENT


def test_bad_fragment_ends_discard_state():
    r = g.Reassembler()
    assert r.feed(packet(7, 1000, 0, 12), 0) == g.PAYLOAD_TOO_LARGE
    assert r.feed(b"\x00\x01\x02", 1) == g.BAD_FRAGMENT
    assert r.feed(packet(7, 1000, 12, 12), 2) == g.BAD_FRAGMENT


def test_fuzzed_frames_never_exceed_the_capacity():
    rnd = random.Random(12345)
    r = g.Reassembler(capacity=100)
    t = 0
    for _ in range(20000):
        n = g.FRAME_HEADER_BYTES + rnd.randrange(40)
        magic = rnd.randrange(256) if rnd.randrange(8) == 0 else g.FRAME_MAGIC
        version = rnd.randrange(256) if rnd.randrange(8) == 0 else g.FRAME_VERSION
        seq, total = rnd.randrange(3), rnd.randrange(130)
        off = rnd.randrange(130) if rnd.randrange(3) == 0 else 0
        pkt = bytes([magic, version, seq & 255, seq >> 8, total & 255, total >> 8, off & 255, off >> 8]) + bytes(n - 8)
        t += rnd.randrange(2500)
        if r.feed(pkt, t) == g.COMPLETE:
            assert len(r.message) <= 100


# ---- JSON reader ------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("case", V["reader_cases"], ids=lambda c: c["name"])
def test_reader_cases(case):
    assert (g.parse_json(msg_bytes(case["text"])) is not None) == case["accepted"]


@pytest.mark.parametrize("token", ["0", "-0", "5", "-5", "0.5", "-0.5", "18.04", "123456789012345", "1234567890123.5",
                                   "0.0000000001"])
def test_numbers_accepted(token):
    assert g.parse_json('{"a":%s}' % token) is not None


@pytest.mark.parametrize("token", ["1e999", "1E5", "1e2", ".5", "5.", "-", "-.5", "+1", "01", "00", "-01", "0x10", "NaN",
                                   "Infinity", "-Infinity", "1_0", "1,5", "1.5.5", "1234567890123456", "12345678901234.5",
                                   "--1", "1-"])
def test_numbers_refused(token):
    assert g.parse_json('{"a":%s}' % token) is None


@pytest.mark.parametrize("text", ['{"a":"x}', '{"a":"x"', '{"a"', '{"a":', "{", "}", '{"a":1 "b":2}', '{"a":1;}',
                                  '{"a":nul}', '{"a":nulll}', '{"a":tru}', '{"a":truee}', '{"a":falsey}',
                                  '{"a":{"b":}}', '{"a":{,}}', '{,"a":1}', '{"a":1}{"b":2}'])
def test_structure_refused(text):
    assert g.parse_json(text) is None


def test_reader_bytes_and_nul():
    assert g.parse_json(b'{"a":"\xff\xfe"}') is not None  # bytes of 0x80 and above are not validated
    assert g.parse_json(b'{"a":"\xff\x01"}') is None
    assert g.parse_json(b'{"a":"x\x00"}') is None
    assert g.parse_json(b'{"a":1}\x00') is None
    assert g.parse_json(b'{"a":1\x00}') is None
    assert g.parse_json(b"") is None
    assert g.parse_json(None) is None


def test_depth_and_node_limits():
    assert g.parse_json('{"a":{"b":{"c":{}}}}') is not None
    assert g.parse_json('{"a":{"b":{"c":{"d":{}}}}}') is None
    many = "{" + ",".join('"k%02d":1' % i for i in range(86)) + "}"
    assert g.parse_json(many) is not None
    assert g.parse_json("{" + ",".join('"k%03d":1' % i for i in range(200)) + "}") is None
    assert g.parse_json(" " * 70000) is None


def test_number_helpers():
    def tenths(token):
        return g.parse_json('{"a":%s}' % token).get("a").tenths()

    cases = {"18": 180, "18.0": 180, "18.04": 180, "18.05": 181, "17.95": 180, "18.049999": 180, "4.95": 50, "4.94": 49,
             "60.04": 600, "60.05": 601, "-0": 0, "-0.04": 0, "-0.05": -1, "-0.1": -1, "0.0000000001": 0,
             "999999999999999": 9999999999999990, "999.95": 10000, "999.94": 9999}
    for token, want in cases.items():
        assert tenths(token) == want, token

    def integer(token):
        return g.parse_json('{"a":%s}' % token).get("a").integer()

    assert integer("3") == 3 and integer("3.0") == 3 and integer("3.000") == 3 and integer("-0") == 0
    assert integer("-1") == -1
    assert integer("3.5") is None and integer("3.01") is None and integer("0.1") is None
    assert integer("4294967296") == 4294967296 and integer("4294967299") == 4294967299


def test_fuzzed_json_never_raises():
    base = ('{"request":{"grindProfile":{"op":"update","index":3,"dose":18.0,"grindSize":14.5,"grindUnit":"um",'
            '"selector":{"type":"index","value":3}}}}')
    alphabet = '{}[]":,-.0123456789eE+ \\\t\nnulltruefalse\x01\x80\xff'
    rnd = random.Random(4242)
    accepted = 0
    for _ in range(5000):
        s = list(base)
        for _ in range(1 + rnd.randrange(4)):
            pos = rnd.randrange(len(s)) if s else 0
            op = rnd.randrange(4)
            if op == 0 and s:
                s[pos] = rnd.choice(alphabet)
            elif op == 1:
                s.insert(pos, rnd.choice(alphabet))
            elif op == 2 and s:
                del s[pos]
            else:
                s = s[:pos + 1]
        data = "".join(s).encode("latin-1", "replace") or b"{"
        if g.parse_json(data) is not None:
            accepted += 1
        req, failure = g.parse_request(data)  # must always end in a request or a failure
        assert (req is None) != (failure is None)
    assert accepted


# ---- request parser ---------------------------------------------------------------------------------------------

def update_request(field, js):
    return '{"request":{"grindProfile":{"op":"update","index":3,"%s":%s}}}' % (field, js)


def check_value_case(field, case):
    req, failure = g.parse_request(update_request(field, case["json"]))
    exp = case["expect"]
    assert (failure is None) == exp["ok"], case["name"]
    if not exp["ok"]:
        assert failure.code == exp["error"], case["name"]
        assert failure.field == exp.get("field", ""), case["name"]
        return
    value = exp["value"]
    if field == "dose":
        assert g.fmt_tenths(req.dose_tenths) == value
    elif field == "grindSize":
        assert req.has_grind_size
        if value is None:
            assert req.grind_size_null
        else:
            assert not req.grind_size_null and g.fmt_tenths(req.grind_size_tenths) == value
    else:
        assert req.has_grind_unit and req.grind_unit == (value or "")


@pytest.mark.parametrize("case", V["dose_cases"], ids=lambda c: c["name"])
def test_dose_cases(case):
    check_value_case("dose", case)


@pytest.mark.parametrize("case", V["grind_size_cases"], ids=lambda c: c["name"])
def test_grind_size_cases(case):
    check_value_case("grindSize", case)


@pytest.mark.parametrize("case", V["grind_unit_cases"], ids=lambda c: c["name"])
def test_grind_unit_cases(case):
    check_value_case("grindUnit", case)


@pytest.mark.parametrize("case", V["dose_cases"], ids=lambda c: c["name"])
def test_expect_dose_uses_the_dose_rules(case):
    text = '{"request":{"grindControl":{"op":"start","profile":1,"expectDose":%s}}}' % case["json"]
    req, failure = g.parse_request(text)
    exp = case["expect"]
    assert (failure is None) == exp["ok"]
    if exp["ok"]:
        assert g.fmt_tenths(req.expect_dose_tenths) == exp["value"]
    else:
        assert failure.code == exp["error"]
        assert failure.field == ("expectDose" if "field" in exp else "")


def test_oversize_request_is_payload_too_large():
    text = b'{"request":{"appHello":{"op":"handshake"}}}'
    text = text + b" " * (g.MAX_REQUEST_BYTES - len(text))
    assert g.parse_request(text)[1] is None
    assert g.parse_request(text + b" ")[1].code == "PAYLOAD_TOO_LARGE"


def test_parsed_fields():
    req, _ = g.parse_request('{"request":{"grindProfile":{"op":"update","index":3.0,"dose":18.05,'
                             '"grindSize":14.5,"grindUnit":"  um "}}}')
    assert (req.object, req.op, req.index, req.dose_tenths, req.grind_size_tenths, req.grind_unit) == (
        "grindProfile", "update", 3, 181, 145, "um")
    req, _ = g.parse_request('{"request":{"grindProfile":{"op":"get","selector":{"type":"index","value":2}}}}')
    assert req.selector_index and req.selector_value == 2
    req, _ = g.parse_request('{"request":{"grindProfile":{"op":"get"}}}')
    assert not req.selector_index
    req, _ = g.parse_request('{"request":{"grindControl":{"op":"start","profile":2,"expectDose":21.5}}}')
    assert (req.op, req.index, req.has_expect_dose, req.expect_dose_tenths) == ("start", 2, True, 215)


def fail_of(text):
    req, failure = g.parse_request(text)
    assert req is None
    return failure


def test_failures_carry_the_object():
    assert fail_of('{"request":{"grindProfile":{"op":"nope"}}}').object == "grindProfile"
    assert fail_of('{"request":{"zzz":{}}}').object is None
    f = fail_of('{"request":{"grindProfile":5}}')
    assert (f.object, f.field) == ("grindProfile", "grindProfile")


def test_op_variants():
    assert fail_of('{"request":{"grindProfile":{"op":5}}}').code == "INVALID_PARAM"
    assert fail_of('{"request":{"grindProfile":{"op":null}}}').code == "INVALID_PARAM"
    assert fail_of('{"request":{"grindProfile":{"op":"GET"}}}').code == "UNKNOWN_OP"
    assert fail_of('{"request":{"grindProfile":{"op":"start"}}}').code == "UNKNOWN_OP"
    assert fail_of('{"request":{"grindResult":{"op":"stop"}}}').code == "UNKNOWN_OP"
    for ok in ('{"request":{"appHello":{"op":"handshake","client":"x"}}}', '{"request":{"grindResult":{"op":"get","x":1}}}'):
        assert g.parse_request(ok)[1] is None
    req, failure = g.parse_request('{"request":{"grindControl":{"op":"stop","profile":99,"dose":"x"}}}')
    assert failure is None and req.op == "stop"
    assert fail_of('{"request":{"grindControl":{"op":"stop"},"grindResult":{}}}').code == "INVALID_PARAM"
    assert fail_of('{"request":{"grindControl":{"op":"stop"}},"x":1}').code == "INVALID_PARAM"
    f = fail_of('{"request":{"grindProfile":{"op":"select","colour":1}}}')
    assert (f.code, f.field) == ("INVALID_PARAM", "colour")
    assert fail_of('{"request":{"grindProfile":{"op":"select","index":1,"dose":18.0}}}').field == "dose"
    assert fail_of('{"request":{"grindProfile":{"op":"get","index":1}}}').field == "index"


def test_envelope_order():
    assert fail_of('{"b":1,"a":2,"request":{}}').field == "b"
    assert fail_of('{"request":{},"a":2,"b":1}').field == "a"
    f = fail_of('{"request":{"zzz":1,"yyy":2}}')
    assert (f.code, f.field) == ("INVALID_PARAM", "request")
    assert fail_of('{"request":{"grindProfile":{"op":"update","index":3,"dose":61,"grindSize":-5}}}').field == "dose"
    assert fail_of('{"request":{"grindProfile":{"op":"update","index":1,"dose":61}}}').code == "PROFILE_READ_ONLY"
    f = fail_of('{"request":{"grindProfile":{"op":"update","index":3,"grindSize":-5,"grindUnit":"a;"}}}')
    assert f.field == "grindSize"


def test_field_path_with_a_long_key():
    key = "k" * 32
    f = fail_of('{"request":{"grindProfile":{"op":"get","selector":{"%s":1}}}}' % key)
    assert f.field == "selector." + key
    assert g.encode_failure(f)


# ---- pairs: a small stand-in for the UI task answers each request from the state described in the vector ----------

NOW = 100000


class Model:
    def __init__(self, state: str):
        self.state = state
        self.handshaken = "handshaken" in state
        self.screen = g.Screen.READY
        for needle, screen in (("MENU", g.Screen.MENU), ("GRINDING", g.Screen.GRINDING), ("grind-complete", g.Screen.DONE),
                               ("EDIT screen", g.Screen.EDIT), ("lead-in running", g.Screen.LEAD_IN),
                               ("update in progress", g.Screen.UPDATING), ("calibration screen", g.Screen.OTHER)):
            if needle in state:
                self.screen = screen
        self.block = g.StartBlock.NONE
        for needle, block in (("motor disabled", g.StartBlock.MOTOR_DISABLED),
                              ("load cell not connected", g.StartBlock.LOAD_CELL_NOT_CONNECTED),
                              ("load cell sends no data", g.StartBlock.LOAD_CELL_NO_DATA),
                              ("load cell sample rate invalid", g.StartBlock.LOAD_CELL_RATE_INVALID)):
            if needle in state:
                self.block = block
        self.have_last = "a stored change 300 ms ago" in state
        self.last_store = NOW - 300
        self.store_fails = "flash write fails" in state
        self.has_result = "a grind finished" in state
        self.pending = "still being processed" in state
        self.ui_busy = "does not answer" in state
        self.selected = int(state.split("selected ")[1][0]) if "selected " in state else 1
        self.dose = [90, 180, 215]
        if "profile 2 = " in state:
            self.dose[2] = round(float(state.split("profile 2 = ")[1].split(" ")[0]) * 10)
        self.p3 = g.ProfileState()
        if "profile 3 = " in state or "profile 3 set" in state:
            self.p3 = g.ProfileState(exists=True, dose_tenths=180)
            if "profile 3 = " in state:
                rest = state.split("profile 3 = ")[1]
                self.p3.dose_tenths = round(float(rest.split(" ")[0]) * 10)
                if "no grind size" not in state and " g " in rest:
                    after = rest.split(" g ", 1)[1].split(" ")
                    self.p3.has_grind_size = True
                    self.p3.grind_size_tenths = round(float(after[0]) * 10)
                    self.p3.grind_unit = after[1].split(",")[0] if len(after) > 1 else ""
        elif self.screen == g.Screen.MENU and "profile 3" not in state:
            # "handshaken, MENU" does not say, but its update pair changes a profile 3 with a size and a unit.
            self.p3 = g.ProfileState(True, 180, True, 145, "um")

    def entry(self, index):
        if index < 3:
            return g.ProfileEntry(index=index, dose_tenths=self.dose[index])
        return g.ProfileEntry(3, self.p3.dose_tenths, self.p3.has_grind_size, self.p3.grind_size_tenths, self.p3.grind_unit)


def sample_result():
    return g.ResultInfo(5, "OK", "BLE", 3, "WEIGHT", 180, 181, True, 2, 14230, 11800)


def respond(text, m: Model):
    req, f = g.parse_request(text)
    if req is not None:
        f = g.gate(m.handshaken, m.pending, req)
    if f is not None:
        return g.encode_failure(f)
    obj = req.object
    if req.op == "stop":
        return g.encode_stop()
    if req.op == "handshake":
        return ""
    if m.ui_busy:
        return g.encode_error(obj, "UI_BUSY", g.MSG["UI_BUSY"])
    interval_ok = g.write_interval_ok(m.have_last, m.last_store, NOW)
    if req.op == "get":
        f = g.state_check(m.screen, g.OpClass.GET, obj)
        if f:
            return g.encode_failure(f)
        if obj == "grindResult":
            if not m.has_result:
                return g.encode_error(obj, "NOT_FOUND", g.MSG["NO_RESULT"])
            return g.encode_result_get(sample_result())
        if req.selector_index:
            if req.selector_value == 3 and not m.p3.exists:
                return g.encode_error(obj, "NOT_FOUND", g.MSG["PROFILE_NOT_SET"], "selector.value")
            return g.encode_profile_entry(m.entry(req.selector_value))
        entries = [m.entry(i) for i in range(3)] + ([m.entry(3)] if m.p3.exists else [])
        return g.encode_profile_list(entries)
    if req.op == "select":
        _, f = g.decide_select(m.screen, req.index, m.p3.exists, m.selected, interval_ok)
        return g.encode_failure(f) if f else g.encode_select(req.index)
    if req.op == "update":
        plan, f = g.decide_update(m.screen, m.p3, req, interval_ok)
        if f:
            return g.encode_failure(f)
        if plan.changed and m.store_fails:
            return g.encode_error(obj, "STORE_FAILED", g.MSG["STORE_FAILED"])
        return g.encode_profile_update(plan.result, plan.changed, plan.changed)
    inputs = g.StartInputs(req.index < 3 or m.p3.exists, m.dose[req.index] if req.index < 3 else m.p3.dose_tenths,
                           req.has_expect_dose, req.expect_dose_tenths, m.block)
    f = g.decide_start(m.screen, inputs)
    return g.encode_failure(f) if f else g.encode_start()


PAIRS = [p for p in V["pairs"] if p["request"] is not None]


@pytest.mark.parametrize("pair", PAIRS, ids=lambda p: p["name"])
def test_pairs(pair):
    got = respond(pair["request"], Model(pair["state"]))
    assert got == (pair["response"] or "")


def test_pair_payload_too_large():
    for pair in V["pairs"]:
        if pair["request"] is None:
            assert g.encode_error(None, "PAYLOAD_TOO_LARGE", g.MSG["TOO_LARGE"]) == pair["response"]


def test_every_error_code_is_exercised_by_a_pair():
    text = "".join(p["response"] or "" for p in V["pairs"])
    for code in g.ERRORS:
        assert '"code":"%s"' % code in text, code


# ---- broadcasts -------------------------------------------------------------------------------------------------

def test_handshake_broadcasts():
    for pair in V["pairs"]:
        if "broadcasts" not in pair:
            continue
        base = g.encode_base_info(g.BaseInfo(640, "7c2a3f10b4d8", "1.4.1", "V2"))
        period = g.encode_period_info(g.PeriodInfo("IDLE", True, 0, 1))
        assert [base, period] == pair["broadcasts"]


def period_from_json(o):
    return g.PeriodInfo(
        dev_state=o["devState"], weight_valid=o["weight"] is not None,
        weight_tenths=round(o["weight"] * 10) if o["weight"] is not None else 0, select_profile=o["selectProfile"],
        phase=o.get("phase", ""), has_target="target" in o, target_tenths=round(o.get("target", 0) * 10),
        mode=o.get("mode", "WEIGHT"), has_lead_in="leadInMs" in o, lead_in_ms=o.get("leadInMs", 0),
        blocked=o["blocked"] or "", result_seq=o["resultSeq"], profile_rev=o["profileRev"])


@pytest.mark.parametrize("case", V["period_cases"], ids=lambda c: c["name"])
def test_period_cases(case):
    p = period_from_json(json.loads(case["message"])["broadcast"]["periodInfo"])
    assert g.encode_period_info(p) == case["message"]
    # 6.5: blocked is what the rule table says for its devState (a WARNING carries its start block).
    block = {code: b for b in g.StartBlock if (code := g.start_block_error(b)[0])}.get(p.blocked, g.StartBlock.NONE)
    assert g.blocked_for(p.dev_state, block) == p.blocked


@pytest.mark.parametrize("case", V["result_broadcasts"], ids=lambda c: c["name"])
def test_result_broadcasts(case):
    o = json.loads(case["message"])["broadcast"]["grindResult"]
    r = g.ResultInfo(o["seq"], o["code"], o["origin"], o["profile"], o["mode"], round(o["target"] * 10),
                     round(o["finalWeight"] * 10), o["weightSettled"], o["pulses"], o["durationMs"], o["motorMs"])
    assert g.encode_result_broadcast(r) == case["message"]


def test_encoders_respect_the_reply_limit():
    entries = [g.ProfileEntry(i, 9999, True, 9999, "clicks") for i in range(4)]
    assert 0 < len(g.encode_profile_list(entries)) <= g.MAX_REPLY_BYTES
    assert g.encode_profile_list(entries * 10) == ""  # too long: refused like the C++ encoder (0 bytes)


def test_unit_without_size_is_never_written():
    assert '"grindSize":null,"grindUnit":null' in g.encode_profile_entry(g.ProfileEntry(3, 180, False, 0, "um"))


def test_encoded_messages_round_trip_as_json():
    # Every encoder output is valid JSON (the Python json module is the independent check).
    samples = [g.encode_stop(), g.encode_start(), g.encode_select(2), g.encode_result_get(sample_result()),
               g.encode_error("grindProfile", "OUT_OF_RANGE", 'a "quoted" \\ text\n', "dose"),
               g.encode_profile_update(g.ProfileEntry(3, -5, True, -3, 'a"b'), True, False)]
    for s in samples:
        json.loads(s)


# ---- decisions --------------------------------------------------------------------------------------------------

def test_state_table_matches_vectors():
    cols = ["get", "update", "select", "start"]
    for row in V["write_rules"]:
        screen = g.Screen[row["state"]]
        for c, name in enumerate(cols):
            f = g.state_check(screen, g.OpClass(c), "grindProfile")
            want = row[name]
            if want in ("accept", "decide"):
                assert f is None, (row["state"], name)
            else:
                assert f is not None and f.code == want, (row["state"], name)
                assert f.message and f.object == "grindProfile"
    assert len(V["write_rules"]) == len(g.Screen)


def test_gate():
    def req(obj, op):
        return g.Request(object=obj, op=op)

    assert g.gate(False, False, req("appHello", "handshake")) is None
    assert g.gate(False, True, req("grindControl", "stop")) is None
    assert g.gate(True, True, req("grindControl", "stop")) is None
    for obj in ("grindProfile", "grindControl", "grindResult"):
        f = g.gate(False, True, req(obj, "get"))
        assert f.code == "INVALID_STATE" and f.object == obj
    assert g.gate(True, True, req("grindProfile", "get")).code == "BUSY"
    assert g.gate(True, True, req("grindControl", "start")).code == "BUSY"
    assert g.gate(True, False, req("grindProfile", "get")) is None
    assert g.gate(True, True, req("appHello", "handshake")).code == "BUSY"


def test_write_interval():
    assert g.write_interval_ok(False, 0, 0)
    assert not g.write_interval_ok(True, 5000, 5000)
    assert not g.write_interval_ok(True, 5000, 5999)
    assert g.write_interval_ok(True, 5000, 6000)
    assert not g.write_interval_ok(True, 0xFFFFFF00, 0x100)
    assert g.write_interval_ok(True, 0xFFFFFF00, 0x300)


def good_start(**kw):
    return g.StartInputs(**{"profile_exists": True, "stored_dose_tenths": 180, **kw})


def test_start_order_matches_vectors():
    order = V["start_order"]
    assert len(order) == 11
    triggers = [
        (g.Screen.LEAD_IN, {}), (g.Screen.UPDATING, {}), (g.Screen.MENU, {}),
        (g.Screen.READY, {"profile_exists": False}), (g.Screen.READY, {"stored_dose_tenths": 700}),
        (g.Screen.READY, {"has_expect_dose": True, "expect_dose_tenths": 190}),
        (g.Screen.READY, {"block": g.StartBlock.MOTOR_DISABLED}), (g.Screen.READY, {"block": g.StartBlock.UPDATE_LOCK}),
        (g.Screen.READY, {"block": g.StartBlock.LOAD_CELL_NOT_CONNECTED}),
        (g.Screen.READY, {"block": g.StartBlock.LOAD_CELL_NO_DATA}),
        (g.Screen.READY, {"block": g.StartBlock.LOAD_CELL_RATE_INVALID}),
    ]
    for i, (screen, kw) in enumerate(triggers):
        assert g.decide_start(screen, good_start(**kw)).code == order[i], i
    # Priority: with every later condition also present, refusal i is still the one reported.
    for i in range(len(triggers)):
        screen = triggers[i][0] if i <= 2 else g.Screen.READY
        kw = {}
        if i <= 3:
            kw["profile_exists"] = False
        if i <= 4:
            kw["stored_dose_tenths"] = 700
        if i <= 5:
            kw.update(has_expect_dose=True, expect_dose_tenths=190)
        kw["block"] = triggers[max(i, 6)][1]["block"]
        assert g.decide_start(screen, good_start(**kw)).code == order[i], i


def test_start_failure_details():
    assert g.decide_start(g.Screen.READY, good_start()) is None
    assert g.decide_start(g.Screen.READY, good_start(has_expect_dose=True, expect_dose_tenths=180)) is None
    for dose in (50, 600):
        assert g.decide_start(g.Screen.READY, good_start(stored_dose_tenths=dose, has_expect_dose=True, expect_dose_tenths=dose)) is None
    assert g.decide_start(g.Screen.READY, good_start(profile_exists=False)).field == "profile"
    assert g.decide_start(g.Screen.READY, good_start(stored_dose_tenths=49)).field == "dose"
    assert g.decide_start(g.Screen.READY, good_start(stored_dose_tenths=601)).code == "OUT_OF_RANGE"
    f = g.decide_start(g.Screen.READY, good_start(has_expect_dose=True, expect_dose_tenths=181))
    assert (f.code, f.field, f.object) == ("CONFLICT", "expectDose", "grindControl")
    assert g.decide_start(g.Screen.READY, good_start(block=g.StartBlock.LOAD_CELL_NO_DATA)).field == ""


def state_with(dose, size, size10, unit):
    return g.ProfileState(True, dose, size, size10, unit)


def upd(**kw):
    return g.Request(object="grindProfile", op="update", index=3, **kw)


def test_update_plan():
    cur = state_with(180, True, 145, "um")
    plan, f = g.decide_update(g.Screen.READY, cur, upd(has_dose=True, dose_tenths=185), True)
    assert f is None and plan.changed and plan.result.dose_tenths == 185 and plan.result.grind_unit == "um"
    plan, f = g.decide_update(g.Screen.READY, cur, upd(has_dose=True, dose_tenths=180, has_grind_size=True,
                                                       grind_size_tenths=145, has_grind_unit=True, grind_unit="um"), False)
    assert f is None and not plan.changed
    plan, f = g.decide_update(g.Screen.READY, cur, upd(has_grind_size=True, grind_size_null=True), True)
    assert plan.changed and not plan.result.has_grind_size and plan.result.grind_unit == ""
    plan, f = g.decide_update(g.Screen.READY, cur, upd(has_grind_unit=True, grind_unit=""), True)
    assert plan.changed and plan.result.has_grind_size and plan.result.grind_unit == ""
    bare = state_with(180, False, 0, "")
    plan, f = g.decide_update(g.Screen.READY, bare, upd(has_grind_size=True, grind_size_tenths=100, has_grind_unit=True,
                                                        grind_unit="clicks"), True)
    assert plan.result.grind_unit == "clicks" and plan.result.grind_size_tenths == 100
    plan, f = g.decide_update(g.Screen.READY, bare, upd(has_grind_size=True, grind_size_tenths=0), True)
    assert plan.changed and plan.result.has_grind_size


def test_update_refusals():
    cur = state_with(180, True, 145, "um")
    none = g.ProfileState()
    f = g.decide_update(g.Screen.READY, none, upd(has_grind_size=True, grind_size_tenths=145), True)[1]
    assert (f.code, f.field) == ("MISSING_FIELD", "dose")
    plan, f = g.decide_update(g.Screen.READY, none, upd(has_dose=True, dose_tenths=180), True)
    assert plan.changed
    f = g.decide_update(g.Screen.READY, state_with(180, False, 0, ""), upd(has_grind_unit=True, grind_unit="um"), True)[1]
    assert (f.code, f.field) == ("INVALID_PARAM", "grindUnit")
    assert g.decide_update(g.Screen.READY, cur, upd(has_grind_size=True, grind_size_null=True, has_grind_unit=True,
                                                    grind_unit="um"), True)[1].code == "INVALID_PARAM"
    assert g.decide_update(g.Screen.READY, none, upd(has_dose=True, dose_tenths=180, has_grind_unit=True,
                                                     grind_unit="um"), True)[1].code == "INVALID_PARAM"
    assert g.decide_update(g.Screen.EDIT, none, upd(has_grind_unit=True, grind_unit="um"), True)[1].code == "EDIT_IN_PROGRESS"
    assert g.decide_update(g.Screen.GRINDING, cur, upd(has_dose=True, dose_tenths=190), True)[1].code == "NOT_READY"
    assert g.decide_update(g.Screen.MENU, cur, upd(has_dose=True, dose_tenths=190), True)[1] is None
    assert g.decide_update(g.Screen.READY, cur, upd(has_dose=True, dose_tenths=190), False)[1].code == "RATE_LIMITED"
    assert g.decide_update(g.Screen.READY, cur, upd(has_dose=True, dose_tenths=180), False)[1] is None


def test_select():
    assert g.decide_select(g.Screen.READY, 2, False, 1, True) == (True, None)
    assert g.decide_select(g.Screen.READY, 1, False, 1, False) == (False, None)
    assert g.decide_select(g.Screen.READY, 2, False, 1, False)[1].code == "RATE_LIMITED"
    f = g.decide_select(g.Screen.READY, 3, False, 1, True)[1]
    assert (f.code, f.field) == ("PROFILE_NOT_SET", "index")
    assert g.decide_select(g.Screen.READY, 3, True, 0, True) == (True, None)
    assert g.decide_select(g.Screen.MENU, 2, False, 1, True)[1].code == "NOT_READY"
    assert g.decide_select(g.Screen.EDIT, 2, False, 1, True)[1].code == "NOT_READY"
    assert g.decide_select(g.Screen.GRINDING, 3, False, 1, True)[1].code == "NOT_READY"


def test_dev_state_and_blocked():
    B = g.StartBlock
    assert g.dev_state(g.Screen.LEAD_IN, B.NONE) == "STARTING"
    assert g.dev_state(g.Screen.UPDATING, B.MOTOR_DISABLED) == "UPDATING"
    assert g.dev_state(g.Screen.READY, B.NONE) == "IDLE"
    assert g.dev_state(g.Screen.READY, B.LOAD_CELL_NO_DATA) == "WARNING"
    for s in (g.Screen.MENU, g.Screen.EDIT, g.Screen.OTHER):
        assert g.dev_state(s, B.NONE) == "SETTING"
    assert g.dev_state(g.Screen.GRINDING, B.NONE) == "GRINDING"
    assert g.dev_state(g.Screen.DONE, B.NONE) == "DONE"
    # 6.5: blocked is the code a start would return right now.
    for screen in g.Screen:
        for block in g.StartBlock:
            blocked = g.blocked_for(g.dev_state(screen, block), block)
            f = g.decide_start(screen, good_start(block=block))
            assert blocked == (f.code if f else ""), (screen, block)
    assert g.blocked_for("UNKNOWN", B.NONE) == "NOT_READY"
    assert g.blocked_for("DONE", B.MOTOR_DISABLED) == "NOT_READY"


def test_start_block_error_and_blocked_vocabulary():
    assert g.start_block_error(g.StartBlock.NONE) == ("", "")
    start_codes = {g.start_block_error(b)[0] for b in g.StartBlock if b != g.StartBlock.NONE}
    for value in V["blocked"]:
        assert value in start_codes or value in ("NOT_READY", "COMMAND_IN_FLIGHT")


def test_result_mapping_is_complete():
    for outcome in g.OUTCOMES:
        for reason in g.ABORT_REASONS:
            for stopped in (False, True):
                code = g.result_code(outcome, reason, stopped)
                assert code in g.RESULT_CODES
                if stopped and outcome in ("UNKNOWN", "TIMEOUT"):
                    assert code == "STOPPED"
                elif outcome == "SUCCESS":
                    assert code == "OK"
                elif outcome in ("OVERSHOOT", "MAX_PULSES"):
                    assert code == outcome
                elif outcome in ("UNKNOWN", "TIMEOUT"):
                    assert code == "TIMEOUT"
                else:
                    want = {"STALL": "ERR_STALLED", "PRIME_FLOW": "ERR_NO_FLOW", "PREDICTIVE_FLOW": "ERR_NO_FLOW",
                            "TARE": "ERR_TARE", "MOTOR_FAULT": "ERR_MOTOR", "NEG_WEIGHT": "ERR_NEG_WEIGHT",
                            "UPDATE": "UPDATE"}.get(reason, "ERR_SENSOR")
                    assert code == want


def test_result_mapping_pinned():
    assert g.result_code("UNKNOWN", "NONE", True) == "STOPPED"
    assert g.result_code("UNKNOWN", "NONE", False) == "TIMEOUT"
    assert g.result_code("SUCCESS", "NONE", True) == "OK"  # a real outcome wins over a late stop
    assert g.result_code("ERROR", "MOTOR_FAULT", True) == "ERR_MOTOR"
    assert g.result_code("ERROR", "NONE", False) == "ERR_SENSOR"
    for code in g.RESULT_CODES:
        assert g.result_weight_settled(code) == (code in ("OK", "OVERSHOOT", "MAX_PULSES"))

"""Independent re-implementation check for GLink v1 (chg020 step 1).

Written from the specification (docs/GLINK-PROTOCOL.md) without importing anything from test_glink_contract.py: its
own tiny frame encoder, receiver, JSON reader and decimal-text rounding are checked against the document and the
vectors, and a mutation test proves that the contract test fails when a COPY of the doc, header or vectors drifts.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
DOC = (ROOT / "docs" / "GLINK-PROTOCOL.md").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "config" / "bluetooth.h").read_text(encoding="utf-8")
VECTORS_PATH = Path(__file__).resolve().parent / "glink_vectors.json"
V = json.loads(VECTORS_PATH.read_text(encoding="utf-8"))
HELLO = b'{"request":{"appHello":{"op":"handshake"}}}'


# ---- own helpers ------------------------------------------------------------------------------------------------

def le16(n):
    return bytes([n & 0xFF, (n >> 8) & 0xFF])


def encode(payload: bytes, seq: int, packet_len: int):
    """Spec 3.1/3.2: header A5 01 seq tot off, payload slices of packet_len - 8."""
    step = packet_len - 8
    return [bytes([0xA5, 0x01]) + le16(seq) + le16(len(payload)) + le16(off) + payload[off:off + step]
            for off in range(0, len(payload), step)]


def doc_section(start: str, end: str) -> str:
    a = DOC.index(start)
    return DOC[a:DOC.index(end, a + len(start))]


def first_cells(text: str):
    """First cell of every table row, backticks removed, header and separator rows skipped."""
    out = []
    for line in text.splitlines():
        if line.startswith("|") and not re.match(r"^\|[\s:|-]+\|$", line):
            out.append(line.strip("|").split("|")[0].strip().strip("`"))
    return out[1:]


def all_messages():
    msgs = []
    for p in V["pairs"]:
        msgs += [m for m in [p["request"], p["response"]] if m] + p.get("broadcasts", [])
    msgs += [c["message"] for c in V["period_cases"]] + [c["message"] for c in V["result_broadcasts"]]
    return msgs


# ---- independent JSON reader (spec 4.2), recursive descent over bytes ------------------------------------------

class Bad(Exception):
    pass


def read_request(raw: bytes):
    s = raw
    n = len(s)
    i = 0

    def ws():
        nonlocal i
        while i < n and s[i] in b" \t\r\n":
            i += 1

    def key():
        nonlocal i
        j = i + 1
        while j < n and s[j] != 0x22:
            if not (chr(s[j]).isascii() and (chr(s[j]).isalnum() or s[j] == 0x5F)):
                raise Bad("key char")
            j += 1
        if j >= n or not 1 <= j - i - 1 <= 32:
            raise Bad("key length")
        k = s[i + 1:j].decode()
        i = j + 1
        return k

    def string():
        nonlocal i
        j = i + 1
        while j < n and s[j] != 0x22:
            if s[j] == 0x5C or s[j] < 0x20:
                raise Bad("escape/control")
            j += 1
        if j >= n:
            raise Bad("unterminated")
        if j - i - 1 > 32:
            raise Bad("string long")
        v = s[i + 1:j].decode("utf-8")
        i = j + 1
        return v

    def num():
        nonlocal i
        j = i
        if j < n and s[j] == 0x2D:
            j += 1
        if j >= n or not 0x30 <= s[j] <= 0x39:
            raise Bad("digit")
        if s[j] == 0x30:
            j += 1
        else:
            while j < n and 0x30 <= s[j] <= 0x39:
                j += 1
        if j < n and s[j] == 0x2E:
            j += 1
            k = j
            while j < n and 0x30 <= s[j] <= 0x39:
                j += 1
            if j == k:
                raise Bad("fraction")
        if j - i > 15:
            raise Bad("number long")
        if j < n and s[j] not in b" \t\r\n,}":
            raise Bad("number tail")
        t = s[i:j].decode()
        i = j
        return ("num", t)

    def val(depth):
        nonlocal i
        ws()
        if i >= n:
            raise Bad("eof")
        c = s[i]
        if c == 0x7B:
            if depth + 1 > 4:
                raise Bad("depth")
            i += 1
            out = {}
            ws()
            if i < n and s[i] == 0x7D:
                i += 1
                return out
            while True:
                ws()
                if i >= n or s[i] != 0x22:
                    raise Bad("key expected")
                k = key()
                if k in out:
                    raise Bad("dup")
                ws()
                if i >= n or s[i] != 0x3A:
                    raise Bad("colon")
                i += 1
                out[k] = val(depth + 1)
                ws()
                if i < n and s[i] == 0x2C:
                    i += 1
                    continue
                if i < n and s[i] == 0x7D:
                    i += 1
                    return out
                raise Bad("sep")
        if c == 0x22:
            return string()
        for lit, v in ((b"true", True), (b"false", False), (b"null", None)):
            if s.startswith(lit, i):
                i += len(lit)
                return v
        return num()  # arrays, '+', '.', NaN and everything else land here and fail

    top = val(0)
    ws()
    if i != n or not isinstance(top, dict):
        raise Bad("trailing or not object")
    return top


def round_text(token: str):
    """Round a JSON number token to 0.1 on its decimal digits, half away from zero. Returns (tenths:int)."""
    neg = token.startswith("-")
    body = token.lstrip("-")
    whole, _, frac = body.partition(".")
    frac = (frac + "00")  # at least two digits
    tenths = int(whole) * 10 + int(frac[0])
    if int(frac[1]) >= 5:
        tenths += 1
    return -tenths if neg else tenths


def fmt(tenths: int):
    return f"{tenths // 10}.{tenths % 10}"


# ---- worked example ---------------------------------------------------------------------------------------------

def test_worked_example_byte_by_byte_with_own_encoder():
    assert len(HELLO) == 43
    pk = encode(HELLO, 1, 20)
    assert [len(p) for p in pk] == [20, 20, 20, 15]
    assert [p[6] for p in pk] == [0, 12, 24, 36] and all(p[7] == 0 for p in pk)
    assert all(p[:6] == bytes([0xA5, 1, 1, 0, 43, 0]) for p in pk)
    lines = re.search(r"### 3\.5 Worked example.*?```text\n(.*?)```", DOC, re.S).group(1).strip().splitlines()
    doc = [bytes(int(x, 16) for x in line.split()) for line in lines]
    assert doc == pk
    assert pk[0][8:] == b'{"request":{' and pk[3][8:] == b'ake"}}}' and len(pk[3][8:]) == 7
    # vector file agrees
    golden = {c["name"]: c for c in V["frame_cases"]}
    assert [bytes.fromhex(p) for p in golden["handshake_20"]["packets"]] == pk
    # single packet at MTU >= 54: 8 + 43 = 51 bytes; sequence 0x0102 is stored 02 01
    single = encode(HELLO, 0x0102, 54 - 3)
    assert len(single) == 1 and len(single[0]) == 51 and single[0][2:4] == bytes([2, 1])
    assert [bytes.fromhex(p) for p in golden["handshake_single_packet_seq_0x0102"]["packets"]] == single
    # the doc statement "MTU of 54 or more": MTU 53 (packet 50) needs two fragments
    assert len(encode(HELLO, 1, 53 - 3)) == 2


@pytest.mark.parametrize("case", V["frame_cases"], ids=lambda c: c["name"])
def test_every_golden_frame_case_with_own_encoder(case):
    assert [bytes.fromhex(p) for p in case["packets"]] == encode(case["payload"].encode(), case["sequence"], case["packet_len"])
    assert len(case["payload"].encode()) == case["payload_len"]
    assert read_request(case["payload"].encode())


# ---- own receiver over the vectors ------------------------------------------------------------------------------

class Rx:
    def __init__(self):
        self.msg = None
        self.disc = None

    def feed(self, t, p):
        if self.msg and t - self.msg["t"] > 2000:
            self.msg = None
        if self.disc and t - self.disc["t"] > 2000:
            self.disc = None
        if len(p) < 9:
            self.msg = None
            return "bad_fragment", None
        seq = p[2] | p[3] << 8
        tot = p[4] | p[5] << 8
        off = p[6] | p[7] << 8
        pay = p[8:]
        if p[0] != 0xA5 or p[1] != 1 or tot == 0 or tot > 4096 or off >= tot or off + len(pay) > tot:
            self.msg = None
            return "bad_fragment", None
        if off > 0 and self.disc and (self.disc["seq"], self.disc["tot"]) == (seq, tot):
            self.disc["t"] = t
            return "discarded", None
        if off == 0:
            self.disc = None
            self.msg = None
            if tot > 512:
                self.disc = {"seq": seq, "tot": tot, "t": t}
                return "payload_too_large", None
            self.msg = {"seq": seq, "tot": tot, "buf": b"", "t": t}
        else:
            m = self.msg
            if not m or m["seq"] != seq or m["tot"] != tot or len(m["buf"]) != off:
                self.msg = None
                return "bad_fragment", None
        self.msg["buf"] += pay
        self.msg["t"] = t
        if len(self.msg["buf"]) == tot:
            out = self.msg["buf"].decode()
            self.msg = None
            return "complete", out
        return "partial", None


@pytest.mark.parametrize("case", V["single_packet_cases"], ids=lambda c: c["name"])
def test_single_packet_cases_with_own_receiver(case):
    assert Rx().feed(0, bytes.fromhex(case["packet"]))[0] == case["outcome"]


@pytest.mark.parametrize("case", V["reassembly_cases"], ids=lambda c: c["name"])
def test_reassembly_cases_with_own_receiver(case):
    rx = Rx()
    for step in case["steps"]:
        if step.get("event") == "disconnect":
            rx = Rx()
            continue
        out, msg = rx.feed(step["t_ms"], bytes.fromhex(step["packet"]))
        assert out == step["outcome"], step
        assert msg == step.get("message"), step


def test_gap_boundaries_with_own_encoder():
    pk = encode(HELLO, 7, 20)
    for gap, expect in ((1999, "partial"), (2000, "partial"), (2001, "bad_fragment")):
        rx = Rx()
        assert rx.feed(0, pk[0])[0] == "partial"
        assert rx.feed(gap, pk[1])[0] == expect, gap
    # offset not equal to bytes received, sequence change, total change, truncated headers at every length
    rx = Rx(); rx.feed(0, pk[0])
    assert rx.feed(1, pk[2])[0] == "bad_fragment"
    rx = Rx(); rx.feed(0, pk[0])
    assert rx.feed(1, pk[1][:2] + bytes([9, 0]) + pk[1][4:])[0] == "bad_fragment"
    for n in range(0, 9):
        assert Rx().feed(0, pk[0][:n])[0] == "bad_fragment", n
    for total, expect in ((0, "bad_fragment"), (512, "partial"), (513, "payload_too_large"), (4096, "payload_too_large"),
                          (4097, "bad_fragment")):
        first = bytes([0xA5, 1, 1, 0]) + le16(total) + le16(0) + b"x" * 12
        assert Rx().feed(0, first)[0] == expect, total


def test_bad_fragment_window_with_own_counter():
    for case in V["bad_fragment_window_cases"]:
        times = case["bad_at_ms"]
        hit = any(sum(1 for u in times[:k + 1] if t - u < 10000) >= 10 for k, t in enumerate(times))
        assert hit == case["disconnect"], case["name"]
    boundary = [0] + list(range(1000, 9000, 1000)) + [9999]  # 10 bad fragments, the first 9999 ms older: counts
    assert any(sum(1 for u in boundary[:k + 1] if t - u < 10000) >= 10 for k, t in enumerate(boundary))
    boundary[-1] = 10000  # exactly 10 000 ms older: the first one no longer counts
    assert not any(sum(1 for u in boundary[:k + 1] if t - u < 10000) >= 10 for k, t in enumerate(boundary))


# ---- reader ------------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("case", V["reader_cases"], ids=lambda c: c["name"])
def test_reader_cases_with_own_reader(case):
    raw = case["text"].encode("utf-8")
    if case["accepted"]:
        read_request(raw)
    else:
        with pytest.raises(Bad):
            read_request(raw)


@pytest.mark.parametrize("text", [b'{"a":1e2}', b'{"a":.5}', b'{"a":5.}', b'{"a":NaN}', b'{"a":-}', b'{"a":+1}', b'{"a":018}',
                                  b'{"a":[1]}', b'{"a":"\\n"}', b'{"a":1}x', b'', b'{}{}', b'{"a":1234567890123456}',
                                  b'{"a":{"b":{"c":{"d":{}}}}}'])
def test_extra_refusals(text):
    with pytest.raises(Bad):
        read_request(text)


@pytest.mark.parametrize("text", [b'{"a":-0}', b' {"a" : -0.0 }\r\n', b'{"a":123456789012345}', b'{"a":{"b":{"c":{"d":1}}}}',
                                  b'{"a":true,"b":false,"c":null}'])
def test_extra_acceptances(text):
    read_request(text)


# ---- every JSON example in the doc ------------------------------------------------------------------------------

def test_doc_json_examples_parse_and_are_golden():
    blocks = re.findall(r"```json\n(.*?)\n```", DOC, re.S)
    assert len(blocks) >= 12
    golden = set(m.replace(" ", "") for m in all_messages() if m.startswith("{"))
    for b in blocks:
        raw = b.strip().encode()
        d = json.loads(b)
        assert len(d) == 1
        if "request" in d:
            assert len(raw) <= 512
            read_request(raw)
        else:
            assert len(raw) <= 1024
        assert re.sub(r"\s", "", b) in {re.sub(r"\s", "", g) for g in golden}, b[:80]


# ---- UUIDs -------------------------------------------------------------------------------------------------------

def test_uuid_collision_over_all_source_files():
    uu = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
    ours = {V["uuids"][k] for k in ("service", "rx", "tx")}
    assert ours == {"53474257-0001-4632-901b-4752494e4421", "53474257-0002-4632-901b-4752494e4421",
                    "53474257-0003-4632-901b-4752494e4421"}
    for u in ours:
        assert HEADER.count(u) == 1 and f"`{u}`" in DOC
    others = set()
    for path in (ROOT / "src").rglob("*"):
        if path.suffix in (".h", ".cpp", ".hpp", ".c") and path.is_file():
            others |= {x.lower() for x in uu.findall(path.read_text(encoding="utf-8", errors="ignore"))}
    others -= ours
    assert len(others) >= 15
    assert not ours & others
    assert not any(o.startswith("53474257") for o in others)
    assert not ours & {"4d543830-0001-4b80-8f00-424f4f4b4f4f", "4d543830-0002-4b80-8f00-424f4f4b4f4f",
                       "4d543830-0003-4b80-8f00-424f4f4b4f4f", "6e400001-b5a3-f393-e0a9-e50e24dcca9e",
                       "6e400002-b5a3-f393-e0a9-e50e24dcca9e", "6e400003-b5a3-f393-e0a9-e50e24dcca9e"}
    assert bytes.fromhex("53474257") == b"SGBW" and bytes.fromhex("4752494e4421") == b"GRIND!"


# ---- vocabularies appear in the vectors -------------------------------------------------------------------------

def test_doc_tables_values_appear_in_the_vectors():
    dev = first_cells(doc_section("### 6.1", "### 6.2"))
    phase = first_cells(doc_section("### 6.2", "### 6.3"))
    res = first_cells(doc_section("### 6.3", "### 6.4"))
    err_text = doc_section("### 6.4", "### 6.5")
    err = first_cells(err_text)
    start_kind = [ln.strip("|").split("|")[0].strip().strip("`") for ln in err_text.splitlines()
                  if ln.startswith("|") and ln.strip("|").split("|")[1].strip() == "start"]
    assert (dev, phase, res, err, start_kind) == (V["devState"], V["phase"], V["resultCode"], V["errorCode"], V["blocked"])
    msgs = all_messages()
    text = "\n".join(msgs)
    # every error code is the code of at least one failing response; every devState and blocked value is in a periodInfo
    for code in err:
        assert f'"code":"{code}"' in text, code
    infos = [json.loads(m)["broadcast"]["periodInfo"] for m in msgs if m.startswith('{"broadcast":{"periodInfo"')]
    assert {i["devState"] for i in infos} == set(dev)
    assert {i["blocked"] for i in infos if i["blocked"]} <= set(start_kind)
    assert {"IDLE", "STARTING", "GRINDING", "DONE", "SETTING", "UPDATING", "WARNING", "UNKNOWN"} == set(dev)
    # blocked values the periodInfo examples use must be consistent with the 6.5 table
    allowed = {"IDLE": {None}, "STARTING": {"COMMAND_IN_FLIGHT"}, "UPDATING": {"UPDATING"}}
    for i in infos:
        if i["devState"] in allowed:
            assert i["blocked"] in allowed[i["devState"]], i
        if i["devState"] in ("GRINDING", "DONE", "SETTING", "UNKNOWN"):
            assert i["blocked"] == "NOT_READY"
        if i["devState"] == "WARNING":
            assert i["blocked"] in {"MOTOR_DISABLED", "UPDATING", "LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NO_DATA", "LOAD_CELL_RATE_INVALID"}
    # result broadcasts carry codes from the table
    for item in V["result_broadcasts"]:
        assert json.loads(item["message"])["broadcast"]["grindResult"]["code"] in res


def test_every_object_and_op_has_a_success_pair():
    seen = set()
    for p in V["pairs"]:
        if not p["request"] or not p["response"] or '"result":"success"' not in p["response"]:
            continue
        d = json.loads(p["request"])["request"]
        (obj, body), = d.items()
        seen.add((obj, body.get("op")))
    assert {("appHello", "handshake")} - seen == set() or any(p["name"] == "appHello_handshake" for p in V["pairs"])
    for obj, op in (("grindProfile", "get"), ("grindProfile", "select"), ("grindProfile", "update"),
                    ("grindControl", "start"), ("grindControl", "stop"), ("grindResult", "get")):
        assert (obj, op) in seen, (obj, op)


# ---- numbers -----------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("token,expect", [("18.04", "18.0"), ("18.05", "18.1"), ("4.95", "5.0"), ("4.94", "4.9"),
                                          ("59.95", "60.0"), ("60.04", "60.0"), ("60.05", "60.1"), ("-0.04", "0.0"),
                                          ("17.95", "18.0"), ("18.049999", "18.0"), ("999.94", "999.9"),
                                          ("999.95", "1000.0"), ("-0.05", "-0.1"), ("0.05", "0.1"), ("12.35", "12.4")])
def test_rounding_on_decimal_text(token, expect):
    t = round_text(token)
    assert ("-" if t < 0 else "") + fmt(abs(t)) == expect


def test_dose_vectors_with_own_rounding():
    for c in V["dose_cases"]:
        e = c["expect"]
        tok = c["json"]
        if not re.fullmatch(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?", tok) or len(tok) > 15:
            assert e.get("error") in ("MALFORMED_JSON", "INVALID_PARAM"), c["name"]
            continue
        t = round_text(tok)
        if 50 <= t <= 600:
            assert e == {"ok": True, "value": fmt(t)}, c["name"]
        else:
            assert e == {"ok": False, "error": "OUT_OF_RANGE", "field": "dose"}, c["name"]
    # the values the task names: dose ranges after rounding
    for tok, ok in (("18.04", True), ("18.05", True), ("4.95", True), ("4.94", False), ("59.95", True), ("60.04", True),
                    ("60.05", False), ("17.95", True)):
        assert (50 <= round_text(tok) <= 600) == ok, tok


def test_grind_size_vectors_with_own_rounding():
    for c in V["grind_size_cases"]:
        e = c["expect"]
        tok = c["json"]
        if tok == "null":
            assert e == {"ok": True, "value": None}
            continue
        if not re.fullmatch(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?", tok):
            assert e["error"] in ("MALFORMED_JSON", "INVALID_PARAM"), c["name"]
            continue
        t = round_text(tok)
        if 0 <= t <= 9999:
            assert e == {"ok": True, "value": fmt(t)}, c["name"]
        else:
            assert e == {"ok": False, "error": "OUT_OF_RANGE", "field": "grindSize"}, c["name"]
    tokens = {c["json"] for c in V["grind_size_cases"]}
    assert {"-0.1", "0", "999.9", "1000", "-0.04"} <= tokens


def test_unit_vectors_with_own_validator():
    ok_chars = re.compile(r"[A-Za-z0-9 ./%\-_]*")
    for c in V["grind_unit_cases"]:
        e = c["expect"]
        raw = ('{"v":' + c["json"] + "}").encode("utf-8")
        try:
            v = read_request(raw)["v"]
        except Bad:
            assert e == {"ok": False, "error": "MALFORMED_JSON"}, c["name"]
            continue
        if v is None:
            assert e == {"ok": True, "value": None}
        elif not isinstance(v, str):
            assert e == {"ok": False, "error": "INVALID_PARAM", "field": "grindUnit"}, c["name"]
        else:
            t = v.strip(" ")
            if not ok_chars.fullmatch(t):
                assert e == {"ok": False, "error": "INVALID_PARAM", "field": "grindUnit"}, c["name"]
            elif len(t) > 6:
                assert e == {"ok": False, "error": "OUT_OF_RANGE", "field": "grindUnit"}, c["name"]
            else:
                assert e == {"ok": True, "value": t or None}, c["name"]


# ---- drift: the contract test must fail on a mutated COPY --------------------------------------------------------

COPY_FILES = ["docs/GLINK-PROTOCOL.md", "src/config/bluetooth.h", "src/config/user.h", "src/config/portal.h",
              "src/controllers/profile_controller.cpp", "src/controllers/grind_controller.h",
              "src/controllers/grind_start_check.cpp", "src/network/portal_guard.cpp", "partitions.csv",
              "tools/tests/test_glink_contract.py", "tools/tests/glink_vectors.json"]


def _mutations():
    def vec(fn):
        def go(files, d):
            fn(d)
            files["tools/tests/glink_vectors.json"] = json.dumps(d, indent=1)
        return go

    def text(path, old, new, count=1):
        def go(files, d):
            assert old in files[path], (path, old)
            files[path] = files[path].replace(old, new, count)
        return go

    def case(group, name, update):
        def fn(d):
            next(c for c in d[group] if c["name"] == name).update(update)
        return vec(fn)

    return {
        "uuid_digit_in_header": text("src/config/bluetooth.h", "53474257-0002-4632", "53474257-0002-4633"),
        "uuid_digit_in_doc": text("docs/GLINK-PROTOCOL.md", "53474257-0003-4632-901b-4752494e4421` | Notify",
                                  "53474257-0003-4632-901b-4752494e4422` | Notify"),
        "limit_constant_in_header": text("src/config/bluetooth.h", "#define GLINK_BAD_FRAME_LIMIT 10", "#define GLINK_BAD_FRAME_LIMIT 11"),
        "limit_constant_in_doc_table": text("docs/GLINK-PROTOCOL.md", "| `GLINK_DOSE_MAX_G` | 60.0 |", "| `GLINK_DOSE_MAX_G` | 61.0 |"),
        "limit_in_vectors": vec(lambda d: d["limits"].update(GLINK_REASSEMBLY_TIMEOUT_MS=2500)),
        "error_code_name_in_doc": text("docs/GLINK-PROTOCOL.md", "| `RATE_LIMITED` | glink |", "| `RATE_LIMIT` | glink |"),
        "error_code_missing_in_vectors": vec(lambda d: d["errorCode"].remove("CONFLICT")),
        "state_table_cell": text("docs/GLINK-PROTOCOL.md", "| `READY` | accept | accept | accept | decide |",
                                 "| `READY` | accept | accept | NOT_READY | decide |"),
        "state_table_edit_cell": text("docs/GLINK-PROTOCOL.md", "| `EDIT` | accept | `EDIT_IN_PROGRESS` |", "| `EDIT` | accept | `NOT_READY` |"),
        "devstate_removed_in_doc": text("docs/GLINK-PROTOCOL.md", "| `DONE` | grind complete or timed out", "| `FINISHED` | grind complete or timed out"),
        "blocked_table_cell": text("docs/GLINK-PROTOCOL.md", "| `STARTING` | `COMMAND_IN_FLIGHT` |", "| `STARTING` | `NOT_READY` |"),
        "result_code_in_doc": text("docs/GLINK-PROTOCOL.md", "| `ERR_STALLED` | stall abort |", "| `ERR_STALL` | stall abort |"),
        "worked_example_byte": text("docs/GLINK-PROTOCOL.md", "A5 01 01 00 2B 00 0C 00", "A5 01 01 00 2B 00 0D 00"),
        "golden_frame_byte": vec(lambda d: d["frame_cases"][0]["packets"].__setitem__(1, d["frame_cases"][0]["packets"][1].replace("000c00", "000d00", 1))),
        "dose_vector": case("dose_cases", "rounds_half_away_18_05", {"expect": {"ok": True, "value": "18.0"}}),
        "reader_vector": case("reader_cases", "depth_5_refused", {"accepted": True}),
        "reassembly_vector": vec(lambda d: d["reassembly_cases"][0]["steps"][-1].update(outcome="partial")),
        "doc_example_json": text("docs/GLINK-PROTOCOL.md", '"selectProfile":2}}}}', '"selectProfile":2,"x":1}}}}'),
        "doc_lead_in_prose": text("docs/GLINK-PROTOCOL.md", "fixed lead-in of 3 s (`GLINK_START_LEAD_IN_MS`)", "fixed lead-in of 4 s (`GLINK_START_LEAD_IN_MS`)"),
        "pair_removed": vec(lambda d: d.update(pairs=[p for p in d["pairs"] if '"UNKNOWN_OP"' not in (p["response"] or "")])),
        "start_order_in_doc": text("docs/GLINK-PROTOCOL.md", "5. `OUT_OF_RANGE` (field", "5. `CONFLICT` (field"),
    }


@pytest.fixture(scope="module")
def scratch_root(tmp_path_factory):
    return tmp_path_factory.mktemp("glink_mut")


def _run_copy(base: Path, files: dict[str, str]):
    for rel, content in files.items():
        dest = base / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(content, encoding="utf-8", newline="")
    env = {**os.environ, "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"}
    return subprocess.run([sys.executable, "-m", "pytest", "-q", "-x", "-p", "no:cacheprovider", "--rootdir", str(base),
                           str(base / "tools" / "tests" / "test_glink_contract.py")],
                          cwd=base, capture_output=True, text=True, env=env, timeout=300)


def _load_files():
    return {rel: (ROOT / rel).read_text(encoding="utf-8") for rel in COPY_FILES}


def test_contract_test_passes_on_an_unmutated_copy(scratch_root):
    result = _run_copy(scratch_root / "clean", _load_files())
    assert result.returncode == 0, result.stdout[-2000:]


@pytest.mark.parametrize("name", sorted(_mutations()))
def test_contract_test_fails_on_drift(scratch_root, name):
    files = _load_files()
    d = json.loads(files["tools/tests/glink_vectors.json"])
    before = dict(files)
    _mutations()[name](files, d)
    assert files != before
    result = _run_copy(scratch_root / name, files)
    assert result.returncode != 0, f"mutation {name} was not detected"
    assert "failed" in result.stdout.lower() or "error" in result.stdout.lower()

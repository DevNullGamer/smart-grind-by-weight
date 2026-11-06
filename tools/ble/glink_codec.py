"""GLink v1 reference codec in Python (docs/GLINK-PROTOCOL.md): fragment frame, strict JSON reader, request parser,
response / broadcast encoders and the write / start decisions.

A line-by-line mirror of src/bluetooth/glink_*.{h,cpp}; both run the same conformance vectors
(tools/tests/glink_vectors.json). Pure: no Bluetooth or I/O, the clock is injected (milliseconds). Numbers (dose,
grind size, weight, target) are integers in tenths, rounded on the decimal digits as written, so nothing depends on
binary floating point. A later step builds the BLE client on top of this module.
"""

from dataclasses import dataclass, field
from enum import IntEnum

# ---- constants (src/config/bluetooth.h) -------------------------------------------------------------------------

PROTO_VERSION = 1
FRAME_MAGIC = 0xA5
FRAME_VERSION = 0x01
FRAME_HEADER_BYTES = 8
DEFAULT_PACKET_BYTES = 20
MAX_PACKET_BYTES = 512
MAX_MESSAGE_BYTES = 4096
MAX_REQUEST_BYTES = 512
MAX_REPLY_BYTES = 1024
REASSEMBLY_TIMEOUT_MS = 2000
BAD_FRAME_LIMIT = 10
BAD_FRAME_WINDOW_MS = 10000
JSON_MAX_DEPTH = 4
JSON_KEY_MAX_LEN = 32
JSON_STRING_MAX_LEN = 32
JSON_NUMBER_MAX_LEN = 15
PROFILE_COUNT_MAX = 4
PROFILE_WRITABLE_INDEX = 3
DOSE_MIN_TENTHS = 50
DOSE_MAX_TENTHS = 600
GRIND_SIZE_MAX_TENTHS = 9999
GRIND_UNIT_MAX_LEN = 6
START_LEAD_IN_MS = 3000
WRITE_MIN_INTERVAL_MS = 1000
MAX_NODES = 96  # mirrors GLINK_JSON_MAX_NODES; the C++ reader runs out of nodes beyond it

_U32 = 0xFFFFFFFF


# ---- fragment frame (section 3) ---------------------------------------------------------------------------------

PARTIAL = "partial"
COMPLETE = "complete"
BAD_FRAGMENT = "bad_fragment"
PAYLOAD_TOO_LARGE = "payload_too_large"
DISCARDED = "discarded"


def header_parse(packet: bytes):
    """(sequence, total_length, offset), or None for a bad magic / version or a packet without payload."""
    if packet is None or len(packet) <= FRAME_HEADER_BYTES:
        return None
    if packet[0] != FRAME_MAGIC or packet[1] != FRAME_VERSION:
        return None
    return (packet[2] | packet[3] << 8, packet[4] | packet[5] << 8, packet[6] | packet[7] << 8)


def header_write(sequence: int, total_length: int, offset: int) -> bytes:
    return bytes([FRAME_MAGIC, FRAME_VERSION, sequence & 0xFF, (sequence >> 8) & 0xFF,
                  total_length & 0xFF, (total_length >> 8) & 0xFF, offset & 0xFF, (offset >> 8) & 0xFF])


def packet_size(packet_len: int) -> int:
    packet_len = min(packet_len, MAX_PACKET_BYTES)
    return packet_len if packet_len > FRAME_HEADER_BYTES else 0


def fragment_count(message_len: int, packet_len: int = DEFAULT_PACKET_BYTES) -> int:
    packet = packet_size(packet_len)
    if packet == 0 or message_len == 0 or message_len > MAX_MESSAGE_BYTES:
        return 0
    per = packet - FRAME_HEADER_BYTES
    return (message_len + per - 1) // per


def fragments(message: bytes, sequence: int, packet_len: int = DEFAULT_PACKET_BYTES):
    """All fragments (packets) of a message, in order. Empty list for an unusable message / packet size."""
    count = fragment_count(len(message), packet_len)
    per = packet_size(packet_len) - FRAME_HEADER_BYTES
    return [header_write(sequence, len(message), i * per) + message[i * per:(i + 1) * per] for i in range(count)]


class Reassembler:
    """Rules 1 to 5 of section 3.3. `capacity` is the largest message accepted (the device uses MAX_REQUEST_BYTES; a
    client reassembling TX uses MAX_MESSAGE_BYTES). feed() returns one of the result names above."""

    _IDLE, _IN_PROGRESS, _DISCARDING = range(3)

    def __init__(self, capacity: int = MAX_REQUEST_BYTES):
        self.capacity = min(capacity, MAX_MESSAGE_BYTES)
        self.message = b""
        self.reset()

    def reset(self):
        self._state = self._IDLE
        self._buf = bytearray()
        self._sequence = 0
        self._total = 0
        self._last_ms = 0
        self.message = b""

    def _bad(self):
        self._state = self._IDLE
        self._buf = bytearray()
        return BAD_FRAGMENT

    def feed(self, packet: bytes, now_ms: int) -> str:
        self.message = b""
        header = header_parse(packet)
        if header is None:
            return self._bad()
        sequence, total, offset = header
        payload = bytes(packet[FRAME_HEADER_BYTES:])
        n = len(payload)
        if total == 0 or total > MAX_MESSAGE_BYTES or offset >= total or offset + n > total:
            return self._bad()

        if offset == 0:  # starts a new message and drops an incomplete one
            self._buf = bytearray()
            self._sequence, self._total, self._last_ms = sequence, total, now_ms
            if total > self.capacity:
                self._state = self._DISCARDING
                return PAYLOAD_TOO_LARGE
            self._buf += payload
            if len(self._buf) == total:
                self._state = self._IDLE
                self.message = bytes(self._buf)
                self._buf = bytearray()
                return COMPLETE
            self._state = self._IN_PROGRESS
            return PARTIAL

        alive = self._state != self._IDLE and ((now_ms - self._last_ms) & _U32) <= REASSEMBLY_TIMEOUT_MS
        if not alive or sequence != self._sequence or total != self._total:
            return self._bad()
        if self._state == self._DISCARDING:
            self._last_ms = now_ms
            if offset + n == self._total:
                self._state = self._IDLE
            return DISCARDED
        if offset != len(self._buf):
            return self._bad()
        self._buf += payload
        self._last_ms = now_ms
        if len(self._buf) == self._total:
            self._state = self._IDLE
            self.message = bytes(self._buf)
            self._buf = bytearray()
            return COMPLETE
        return PARTIAL


class BadFrameWindow:
    """Rule 6: BAD_FRAME_LIMIT bad fragments less than BAD_FRAME_WINDOW_MS apart (first to last) disconnect."""

    def __init__(self):
        self.reset()

    def reset(self):
        self._times = []

    def record(self, now_ms: int) -> bool:
        self._times.append(now_ms)
        del self._times[:-BAD_FRAME_LIMIT]
        if len(self._times) < BAD_FRAME_LIMIT:
            return False
        return ((now_ms - self._times[0]) & _U32) < BAD_FRAME_WINDOW_MS


# ---- strict JSON reader (section 4.2) ---------------------------------------------------------------------------

class JString(bytes):
    """A string value (raw bytes, no escapes)."""


class JNumber:
    """A number token, kept as text so that rounding works on the decimal digits."""

    def __init__(self, token: bytes):
        self.token = token.decode("ascii")

    def tenths(self) -> int:
        """Rounded half away from zero to 0.1, as a scaled integer (18.05 -> 181, -0.04 -> 0)."""
        s = self.token
        neg = s.startswith("-")
        if neg:
            s = s[1:]
        whole, _, frac = s.partition(".")
        t = int(whole) * 10
        if frac:
            t += int(frac[0])
            if len(frac) > 1 and frac[1] >= "5":
                t += 1
        return -t if neg else t

    def integer(self):
        """The whole number (3, 3.0, -0), or None when the token has a non-zero fraction."""
        whole, _, frac = self.token.partition(".")
        if frac.strip("0"):
            return None
        return int(whole)


class JObject(list):
    """An object: a list of (key, value) in message order. Values: JObject, JString, JNumber, True, False, None."""

    def get(self, key, default=None):
        for k, v in self:
            if k == key:
                return v
        return default

    def has(self, key) -> bool:
        return any(k == key for k, _ in self)

    def keys(self):
        return [k for k, _ in self]


class _Reader:
    def __init__(self, data: bytes):
        self.t = data
        self.n = len(data)
        self.i = 0
        self.nodes = 0

    def ws(self):
        while self.i < self.n and self.t[self.i] in b" \t\r\n":
            self.i += 1

    def node(self):
        if self.nodes >= MAX_NODES:
            raise ValueError("too many members")
        self.nodes += 1

    def key(self, siblings):
        if self.i >= self.n or self.t[self.i] != 0x22:
            raise ValueError("key")
        self.i += 1
        start = self.i
        while self.i < self.n and self.t[self.i] != 0x22:
            c = self.t[self.i]
            if not (48 <= c <= 57 or 65 <= c <= 90 or 97 <= c <= 122 or c == 95):
                raise ValueError("key charset")
            self.i += 1
        if self.i >= self.n:
            raise ValueError("key end")
        key = self.t[start:self.i].decode("ascii")
        if not 1 <= len(key) <= JSON_KEY_MAX_LEN:
            raise ValueError("key length")
        self.i += 1
        if any(k == key for k, _ in siblings):
            raise ValueError("duplicate key")
        return key

    def string(self):
        self.i += 1
        start = self.i
        while self.i < self.n and self.t[self.i] != 0x22:
            c = self.t[self.i]
            if c < 0x20 or c == 0x5C:
                raise ValueError("string byte")
            self.i += 1
        if self.i >= self.n:
            raise ValueError("string end")
        if self.i - start > JSON_STRING_MAX_LEN:
            raise ValueError("string length")
        value = JString(self.t[start:self.i])
        self.i += 1
        return value

    def number(self):
        start = self.i
        t, n = self.t, self.n
        if self.i < n and t[self.i] == 0x2D:
            self.i += 1
        if self.i >= n or not 48 <= t[self.i] <= 57:
            raise ValueError("number")
        if t[self.i] == 0x30:
            self.i += 1
        else:
            while self.i < n and 48 <= t[self.i] <= 57:
                self.i += 1
        if self.i < n and t[self.i] == 0x2E:
            self.i += 1
            if self.i >= n or not 48 <= t[self.i] <= 57:
                raise ValueError("fraction")
            while self.i < n and 48 <= t[self.i] <= 57:
                self.i += 1
        if self.i - start > JSON_NUMBER_MAX_LEN:
            raise ValueError("number length")
        return JNumber(t[start:self.i])

    def literal(self, word: bytes, value):
        if self.t[self.i:self.i + len(word)] != word:
            raise ValueError("literal")
        self.i += len(word)
        return value

    def value(self, depth):
        if self.i >= self.n:
            raise ValueError("end")
        c = self.t[self.i]
        if c == 0x7B:
            return self.obj(depth + 1)
        if c == 0x22:
            return self.string()
        if c == 0x2D or 48 <= c <= 57:
            return self.number()
        if c == 0x74:
            return self.literal(b"true", True)
        if c == 0x66:
            return self.literal(b"false", False)
        if c == 0x6E:
            return self.literal(b"null", None)
        raise ValueError("value")

    def obj(self, depth):
        if depth > JSON_MAX_DEPTH:
            raise ValueError("depth")
        out = JObject()
        self.i += 1
        self.ws()
        if self.i < self.n and self.t[self.i] == 0x7D:
            self.i += 1
            return out
        while True:
            self.ws()
            self.node()
            key = self.key(out)
            self.ws()
            if self.i >= self.n or self.t[self.i] != 0x3A:
                raise ValueError("colon")
            self.i += 1
            self.ws()
            out.append((key, self.value(depth)))
            self.ws()
            if self.i >= self.n:
                raise ValueError("end")
            if self.t[self.i] == 0x2C:
                self.i += 1
                continue
            if self.t[self.i] == 0x7D:
                self.i += 1
                return out
            raise ValueError("separator")


def parse_json(data):
    """Strict parse of a request. Returns the root JObject, or None for any violation of the reader rules."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    if not data or len(data) > 0xFFFF:
        return None
    r = _Reader(bytes(data))
    try:
        r.ws()
        if r.i >= r.n or r.t[r.i] != 0x7B:
            return None
        r.node()  # the root counts as a node, like in the C++ table
        root = r.obj(1)
        r.ws()
        return root if r.i == r.n else None
    except ValueError:
        return None


# ---- vocabularies (section 6) -----------------------------------------------------------------------------------

ERRORS = ["INVALID_PARAM", "MISSING_FIELD", "OUT_OF_RANGE", "INVALID_STATE", "NOT_FOUND", "CONFLICT", "MALFORMED_JSON",
          "UNKNOWN_OBJECT", "UNKNOWN_OP", "PAYLOAD_TOO_LARGE", "BUSY", "UI_BUSY", "STORE_FAILED", "RATE_LIMITED",
          "PROFILE_READ_ONLY", "EDIT_IN_PROGRESS", "PROFILE_NOT_SET", "NOT_READY", "UPDATING", "MOTOR_DISABLED",
          "LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NO_DATA", "LOAD_CELL_RATE_INVALID", "COMMAND_IN_FLIGHT"]
DEV_STATES = ["IDLE", "STARTING", "GRINDING", "DONE", "SETTING", "UPDATING", "WARNING", "UNKNOWN"]
PHASES = ["INITIALIZING", "SETUP", "TARING", "TARE_CONFIRM", "PREDICTIVE", "PULSE_DECISION", "PULSE_EXECUTE",
          "PULSE_SETTLING", "FINAL_SETTLING", "TIME_GRINDING", "TIME_ADDITIONAL_PULSE", "PRIME", "PRIME_SETTLING",
          "PURGE_CONFIRM"]
RESULT_CODES = ["OK", "OVERSHOOT", "MAX_PULSES", "TIMEOUT", "ERR_STALLED", "ERR_NO_FLOW", "ERR_SENSOR", "ERR_TARE",
                "ERR_MOTOR", "ERR_NEG_WEIGHT", "STOPPED", "UPDATE"]
ORIGINS = ["BLE", "TOUCH", "WEB"]
MODES = ["WEIGHT", "TIME"]
PROFILE_NAMES = ["SINGLE", "DOUBLE", "CUSTOM", "PROFILE"]
OBJECTS = {"appHello": ["handshake"], "grindProfile": ["get", "select", "update"], "grindControl": ["start", "stop"],
           "grindResult": ["get"]}

# The short English texts of "message" (not a contract; the vectors pin them for the tests).
MSG = {
    "MALFORMED": "malformed json", "TOO_LARGE": "request larger than 512 bytes", "UNKNOWN_OBJECT": "unknown object",
    "UNKNOWN_TOP_LEVEL": "unknown top-level member", "REQUEST_MISSING": "request missing",
    "REQUEST_NOT_OBJECT": "request must be an object", "NO_OBJECT": "no object in request",
    "ONE_OBJECT": "exactly one object per request", "OBJECT_NOT_OBJECT": "object must be an object",
    "OP_MISSING": "op missing", "OP_NOT_STRING": "op must be a string", "OP_UNSUPPORTED": "op not supported",
    "UNKNOWN_FIELD": "unknown field", "SELECTOR_NOT_OBJECT": "selector must be an object",
    "SELECTOR_TYPE_MISSING": "selector.type missing", "SELECTOR_TYPE_UNSUPPORTED": "selector.type not supported",
    "SELECTOR_VALUE_MISSING": "selector.value missing", "SELECTOR_VALUE_NOT_ALLOWED": "selector.value not allowed",
    "SELECTOR_VALUE_NOT_INT": "selector.value must be an integer", "INDEX_MISSING": "index missing",
    "INDEX_NOT_INT": "index must be an integer", "INDEX_RANGE": "index out of range",
    "PROFILE_MISSING": "profile missing", "PROFILE_NOT_INT": "profile must be an integer",
    "PROFILE_RANGE": "profile out of range", "DOSE_NOT_NUMBER": "dose must be a number",
    "DOSE_RANGE": "dose out of range", "EXPECT_NOT_NUMBER": "expectDose must be a number",
    "EXPECT_RANGE": "expectDose out of range", "SIZE_NOT_NUMBER": "grindSize must be a number or null",
    "SIZE_RANGE": "grindSize out of range", "UNIT_NOT_STRING": "grindUnit must be a string or null",
    "UNIT_BAD_CHAR": "grindUnit has an invalid character", "UNIT_TOO_LONG": "grindUnit too long",
    "UNIT_NEEDS_SIZE": "grindUnit needs a grindSize", "NOTHING_TO_UPDATE": "nothing to update",
    "DOSE_REQUIRED": "dose required to create the profile", "READ_ONLY": "profile is read-only over BLE",
    "PROFILE_NOT_SET": "profile not set", "NO_RESULT": "no result since boot",
    "HANDSHAKE_REQUIRED": "handshake required", "BUSY": "previous request still pending",
    "UI_BUSY": "ui task did not answer in time", "STORE_FAILED": "could not store the value",
    "RATE_LIMITED": "stored changes at most once per second", "NOT_READY_READY": "not on the ready screen",
    "NOT_READY_READY_OR_MENU": "not on the ready or menu screen",
    "EDIT_IN_PROGRESS": "profile is being edited on the grinder", "IN_FLIGHT": "a start is in progress",
    "UPDATING": "update in progress", "MOTOR_DISABLED": "motor is disabled",
    "LOAD_CELL_NOT_CONNECTED": "load cell not connected", "LOAD_CELL_NO_DATA": "load cell sends no data",
    "LOAD_CELL_RATE_INVALID": "load cell sample rate invalid",
    "DOSE_OUT_OF_BLE_RANGE": "stored dose outside 5.0 to 60.0 g", "CONFLICT": "dose differs from expectDose",
}


@dataclass
class Failure:
    """One refusal: code, text, optional field path; `object` is the object name it is answered under (None = the
    pseudo-object "error")."""

    code: str
    message: str
    field: str = ""
    object: str = None


@dataclass
class Request:
    object: str = ""
    op: str = ""
    selector_index: bool = False
    selector_value: int = 0
    index: int = 0
    has_dose: bool = False
    dose_tenths: int = 0
    has_grind_size: bool = False
    grind_size_null: bool = False
    grind_size_tenths: int = 0
    has_grind_unit: bool = False  # grind_unit is "" for null / empty / spaces only (clears the unit)
    grind_unit: str = ""
    has_expect_dose: bool = False
    expect_dose_tenths: int = 0


class _Refused(Exception):
    def __init__(self, failure: Failure):
        self.failure = failure


def _refuse(code, key, field="", obj=None):
    raise _Refused(Failure(code, MSG[key], field, obj))


# ---- request parser ---------------------------------------------------------------------------------------------

def _read_op(obj, oid, allowed):
    if not obj.has("op"):
        _refuse("MISSING_FIELD", "OP_MISSING", "op", oid)
    op = obj.get("op")
    if not isinstance(op, JString):
        _refuse("INVALID_PARAM", "OP_NOT_STRING", "op", oid)
    for name in allowed:
        if op == name.encode():
            return name
    _refuse("UNKNOWN_OP", "OP_UNSUPPORTED", "op", oid)


def _check_keys(obj, allowed, prefix, oid):
    for key in obj.keys():
        if key not in allowed:
            _refuse("INVALID_PARAM", "UNKNOWN_FIELD", prefix + key, oid)


def _read_index(value, lo, hi, field, not_int, rng, oid):
    v = value.integer() if isinstance(value, JNumber) else None
    if v is None:
        _refuse("INVALID_PARAM", not_int, field, oid)
    if v < lo or v > hi:
        _refuse("OUT_OF_RANGE", rng, field, oid)
    return v


def _read_tenths(value, lo, hi, field, not_number, rng, oid):
    if not isinstance(value, JNumber):
        _refuse("INVALID_PARAM", not_number, field, oid)
    t = value.tenths()
    if t < lo or t > hi:
        _refuse("OUT_OF_RANGE", rng, field, oid)
    return t


_UNIT_CHARS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ./%-_")


def _read_unit(value, oid):
    if value is None:
        return ""
    if not isinstance(value, JString):
        _refuse("INVALID_PARAM", "UNIT_NOT_STRING", "grindUnit", oid)
    raw = bytes(value).strip(b" ")
    if any(c not in _UNIT_CHARS for c in raw):
        _refuse("INVALID_PARAM", "UNIT_BAD_CHAR", "grindUnit", oid)
    if len(raw) > GRIND_UNIT_MAX_LEN:
        _refuse("OUT_OF_RANGE", "UNIT_TOO_LONG", "grindUnit", oid)
    return raw.decode("ascii")


def _parse_get(obj, oid, req):
    _check_keys(obj, ("op", "selector"), "", oid)
    if not obj.has("selector"):
        return
    sel = obj.get("selector")
    if not isinstance(sel, JObject):
        _refuse("INVALID_PARAM", "SELECTOR_NOT_OBJECT", "selector", oid)
    _check_keys(sel, ("type", "value"), "selector.", oid)
    if not sel.has("type"):
        _refuse("MISSING_FIELD", "SELECTOR_TYPE_MISSING", "selector.type", oid)
    kind = sel.get("type")
    if isinstance(kind, JString) and kind == b"all":
        if sel.has("value"):
            _refuse("INVALID_PARAM", "SELECTOR_VALUE_NOT_ALLOWED", "selector.value", oid)
        return
    if not (isinstance(kind, JString) and kind == b"index"):
        _refuse("INVALID_PARAM", "SELECTOR_TYPE_UNSUPPORTED", "selector.type", oid)
    if not sel.has("value"):
        _refuse("MISSING_FIELD", "SELECTOR_VALUE_MISSING", "selector.value", oid)
    req.selector_index = True
    req.selector_value = _read_index(sel.get("value"), 0, PROFILE_COUNT_MAX - 1, "selector.value",
                                     "SELECTOR_VALUE_NOT_INT", "INDEX_RANGE", oid)


def _required_index(obj, key, missing, not_int, rng, oid):
    if not obj.has(key):
        _refuse("MISSING_FIELD", missing, key, oid)
    return _read_index(obj.get(key), 0, PROFILE_COUNT_MAX - 1, key, not_int, rng, oid)


def _parse_update(obj, oid, req):
    _check_keys(obj, ("op", "index", "dose", "grindSize", "grindUnit"), "", oid)
    req.index = _required_index(obj, "index", "INDEX_MISSING", "INDEX_NOT_INT", "INDEX_RANGE", oid)
    if req.index != PROFILE_WRITABLE_INDEX:
        _refuse("PROFILE_READ_ONLY", "READ_ONLY", "index", oid)
    if obj.has("dose"):
        req.has_dose = True
        req.dose_tenths = _read_tenths(obj.get("dose"), DOSE_MIN_TENTHS, DOSE_MAX_TENTHS, "dose", "DOSE_NOT_NUMBER",
                                       "DOSE_RANGE", oid)
    if obj.has("grindSize"):
        req.has_grind_size = True
        size = obj.get("grindSize")
        if size is None:
            req.grind_size_null = True
        else:
            req.grind_size_tenths = _read_tenths(size, 0, GRIND_SIZE_MAX_TENTHS, "grindSize", "SIZE_NOT_NUMBER",
                                                 "SIZE_RANGE", oid)
    if obj.has("grindUnit"):
        req.has_grind_unit = True
        req.grind_unit = _read_unit(obj.get("grindUnit"), oid)
    if not (req.has_dose or req.has_grind_size or req.has_grind_unit):
        _refuse("MISSING_FIELD", "NOTHING_TO_UPDATE", "", oid)


def _parse_start(obj, oid, req):
    _check_keys(obj, ("op", "profile", "expectDose"), "", oid)
    req.index = _required_index(obj, "profile", "PROFILE_MISSING", "PROFILE_NOT_INT", "PROFILE_RANGE", oid)
    if obj.has("expectDose"):
        req.has_expect_dose = True
        req.expect_dose_tenths = _read_tenths(obj.get("expectDose"), DOSE_MIN_TENTHS, DOSE_MAX_TENTHS, "expectDose",
                                              "EXPECT_NOT_NUMBER", "EXPECT_RANGE", oid)


def parse_request(data):
    """Parses one request message (4.1 steps 1 to 4 and 7). Returns (Request, None) or (None, Failure)."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    try:
        return _parse_request(bytes(data)), None
    except _Refused as e:
        return None, e.failure


def _parse_request(data: bytes) -> Request:
    if len(data) > MAX_REQUEST_BYTES:
        _refuse("PAYLOAD_TOO_LARGE", "TOO_LARGE")
    root = parse_json(data)
    if root is None:
        _refuse("MALFORMED_JSON", "MALFORMED")
    for key in root.keys():
        if key != "request":
            _refuse("INVALID_PARAM", "UNKNOWN_TOP_LEVEL", key)
    if not root.has("request"):
        _refuse("MISSING_FIELD", "REQUEST_MISSING", "request")
    request = root.get("request")
    if not isinstance(request, JObject):
        _refuse("INVALID_PARAM", "REQUEST_NOT_OBJECT", "request")
    if len(request) == 0:
        _refuse("MISSING_FIELD", "NO_OBJECT", "request")
    if len(request) > 1:
        _refuse("INVALID_PARAM", "ONE_OBJECT", "request")
    name, obj = request[0]
    if name not in OBJECTS:
        _refuse("UNKNOWN_OBJECT", "UNKNOWN_OBJECT")
    if not isinstance(obj, JObject):
        _refuse("INVALID_PARAM", "OBJECT_NOT_OBJECT", name, name)

    req = Request(object=name)
    if name == "grindControl" and isinstance(obj.get("op"), JString) and obj.get("op") == b"stop":
        req.op = "stop"  # executed whatever else the object contains (4.1 step 4)
        return req
    req.op = _read_op(obj, name, OBJECTS[name])
    if name == "grindProfile":
        if req.op == "get":
            _parse_get(obj, name, req)
        elif req.op == "select":
            _check_keys(obj, ("op", "index"), "", name)
            req.index = _required_index(obj, "index", "INDEX_MISSING", "INDEX_NOT_INT", "INDEX_RANGE", name)
        else:
            _parse_update(obj, name, req)
    elif name == "grindControl":
        _parse_start(obj, name, req)
    return req


# ---- encoders (compact JSON, section 4.3) -----------------------------------------------------------------------

def _q(text: str) -> str:
    out = ['"']
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ord(ch) < 0x20:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def fmt_tenths(v: int) -> str:
    """Scaled integer to the one-decimal wire form: 180 -> 18.0, -3 -> -0.3."""
    a = abs(v)
    return "%s%d.%d" % ("-" if v < 0 else "", a // 10, a % 10)


def _limit(text: str) -> str:
    """The encoders return "" (like the C++ ones return 0) when a message would exceed MAX_REPLY_BYTES."""
    return text if len(text.encode("utf-8")) <= MAX_REPLY_BYTES else ""


def _resp(obj, body):
    return '{"response":{%s:{"result":"success","data":%s}}}' % (_q(obj), body)


def encode_error(obj, code, message, field=""):
    """{"response":{"<object>":{"result":"fail","error":{...}}}}; obj None uses the pseudo-object "error"."""
    err = '"code":%s,"message":%s' % (_q(code), _q(message))
    if field:
        err += ',"field":%s' % _q(field)
    return '{"response":{%s:{"result":"fail","error":{%s}}}}' % (_q(obj or "error"), err)


def encode_failure(f: Failure) -> str:
    return encode_error(f.object, f.code, f.message, f.field)


@dataclass
class ProfileEntry:
    index: int = 0
    dose_tenths: int = 0
    has_grind_size: bool = False
    grind_size_tenths: int = 0
    grind_unit: str = ""  # "" = none (only written with a size)


def _entry_fields(e: ProfileEntry) -> str:
    size = fmt_tenths(e.grind_size_tenths) if e.has_grind_size else "null"
    unit = _q(e.grind_unit) if e.has_grind_size and e.grind_unit else "null"
    return '"index":%d,"name":%s,"dose":%s,"grindSize":%s,"grindUnit":%s,"writable":%s' % (
        e.index, _q(PROFILE_NAMES[e.index]), fmt_tenths(e.dose_tenths), size, unit,
        "true" if e.index == PROFILE_WRITABLE_INDEX else "false")


def encode_profile_list(entries) -> str:
    return _limit(_resp("grindProfile", "[" + ",".join("{%s}" % _entry_fields(e) for e in entries) + "]"))


def encode_profile_entry(e: ProfileEntry) -> str:
    return _limit(_resp("grindProfile", "{%s}" % _entry_fields(e)))


def encode_profile_update(e: ProfileEntry, changed: bool, stored: bool) -> str:
    return _limit(_resp("grindProfile", '{%s,"changed":%s,"stored":%s}' % (
        _entry_fields(e), "true" if changed else "false", "true" if stored else "false")))


def encode_select(select_profile: int) -> str:
    return _resp("grindProfile", '{"selectProfile":%d}' % select_profile)


def encode_start(lead_in_ms: int = START_LEAD_IN_MS) -> str:
    return _resp("grindControl", '{"leadInMs":%d}' % lead_in_ms)


def encode_stop() -> str:
    return _resp("grindControl", '{"accepted":true}')


@dataclass
class ResultInfo:
    seq: int = 0
    code: str = "OK"
    origin: str = "BLE"
    profile: int = 0
    mode: str = "WEIGHT"
    target_tenths: int = 0
    final_weight_tenths: int = 0
    weight_settled: bool = False
    pulses: int = 0
    duration_ms: int = 0
    motor_ms: int = 0


def _result_fields(r: ResultInfo) -> str:
    return ('"seq":%d,"code":%s,"origin":%s,"profile":%d,"mode":%s,"target":%s,"finalWeight":%s,"weightSettled":%s,'
            '"pulses":%d,"durationMs":%d,"motorMs":%d') % (
        r.seq, _q(r.code), _q(r.origin), r.profile, _q(r.mode), fmt_tenths(r.target_tenths),
        fmt_tenths(r.final_weight_tenths), "true" if r.weight_settled else "false", r.pulses, r.duration_ms, r.motor_ms)


def encode_result_get(r: ResultInfo) -> str:
    return _resp("grindResult", "{%s}" % _result_fields(r))


def encode_result_broadcast(r: ResultInfo) -> str:
    return _limit('{"broadcast":{"grindResult":{%s}}}' % _result_fields(r))


@dataclass
class BaseInfo:
    release_ver: int = 0
    sn_code: str = ""
    fw_version: str = ""
    board: str = ""


def encode_base_info(info: BaseInfo) -> str:
    return _limit(
        '{"broadcast":{"baseInfo":{"releaseVer":%d,"snCode":%s,"wifiName":null,"device":"SGBW","proto":%d,'
        '"fwVersion":%s,"board":%s,"doseMin":%s,"doseMax":%s,"maxRequestBytes":%d}}}' % (
            info.release_ver, _q(info.sn_code), PROTO_VERSION, _q(info.fw_version), _q(info.board),
            fmt_tenths(DOSE_MIN_TENTHS), fmt_tenths(DOSE_MAX_TENTHS), MAX_REQUEST_BYTES))


@dataclass
class PeriodInfo:
    dev_state: str = "UNKNOWN"
    weight_valid: bool = False
    weight_tenths: int = 0
    select_profile: int = 0
    phase: str = ""  # "" = absent
    has_target: bool = False  # target and mode go together
    target_tenths: int = 0
    mode: str = "WEIGHT"
    has_lead_in: bool = False
    lead_in_ms: int = 0
    blocked: str = ""  # "" = null
    result_seq: int = 0
    profile_rev: int = 0


def encode_period_info(p: PeriodInfo) -> str:
    parts = ['"devState":%s' % _q(p.dev_state), '"weight":%s' % (fmt_tenths(p.weight_tenths) if p.weight_valid else "null"),
             '"selectProfile":%d' % p.select_profile]
    if p.phase:
        parts.append('"phase":%s' % _q(p.phase))
    if p.has_target:
        parts += ['"target":%s' % fmt_tenths(p.target_tenths), '"mode":%s' % _q(p.mode)]
    if p.has_lead_in:
        parts.append('"leadInMs":%d' % p.lead_in_ms)
    parts += ['"blocked":%s' % (_q(p.blocked) if p.blocked else "null"), '"resultSeq":%d' % p.result_seq,
              '"profileRev":%d' % p.profile_rev]
    return _limit('{"broadcast":{"periodInfo":{%s}}}' % ",".join(parts))


# ---- decisions (sections 4.1, 6.1, 6.5, 7) ----------------------------------------------------------------------

class Screen(IntEnum):
    LEAD_IN = 0
    UPDATING = 1
    READY = 2
    MENU = 3
    EDIT = 4
    GRINDING = 5
    DONE = 6
    OTHER = 7


class OpClass(IntEnum):
    GET = 0
    UPDATE = 1
    SELECT = 2
    START = 3


class StartBlock(IntEnum):
    NONE = 0
    MOTOR_DISABLED = 1
    UPDATE_LOCK = 2
    LOAD_CELL_NOT_CONNECTED = 3
    LOAD_CELL_NO_DATA = 4
    LOAD_CELL_RATE_INVALID = 5


def _state_refusal(screen: Screen, op: OpClass):
    """The table of 7.1: None = accept (a READY start continues with the decide chain), else (code, message key)."""
    if screen == Screen.LEAD_IN:
        return None if op == OpClass.GET else ("COMMAND_IN_FLIGHT", "IN_FLIGHT")
    if screen == Screen.UPDATING:
        return ("UPDATING", "UPDATING")
    if screen == Screen.READY:
        return None
    if screen == Screen.MENU:
        return None if op in (OpClass.GET, OpClass.UPDATE) else ("NOT_READY", "NOT_READY_READY")
    if screen == Screen.EDIT:
        if op == OpClass.GET:
            return None
        if op == OpClass.UPDATE:
            return ("EDIT_IN_PROGRESS", "EDIT_IN_PROGRESS")
        return ("NOT_READY", "NOT_READY_READY")
    if op == OpClass.GET:
        return None
    return ("NOT_READY", "NOT_READY_READY_OR_MENU" if op == OpClass.UPDATE else "NOT_READY_READY")


def state_check(screen: Screen, op: OpClass, obj=None):
    """None = accept, else the Failure of 7.1."""
    r = _state_refusal(screen, op)
    return None if r is None else Failure(r[0], MSG[r[1]], "", obj)


def gate(handshaken: bool, request_pending: bool, request: Request):
    """4.1 steps 5 and 6. None = pass on, else a Failure (INVALID_STATE before the handshake, BUSY while pending)."""
    if request.object == "grindControl" and request.op == "stop":
        return None
    if not handshaken and request.object != "appHello":
        return Failure("INVALID_STATE", MSG["HANDSHAKE_REQUIRED"], "", request.object)
    if request_pending:
        return Failure("BUSY", MSG["BUSY"], "", request.object)
    return None


def write_interval_ok(have_last: bool, last_store_ms: int, now_ms: int) -> bool:
    return (not have_last) or ((now_ms - last_store_ms) & _U32) >= WRITE_MIN_INTERVAL_MS


@dataclass
class ProfileState:
    exists: bool = False  # profile 3 only
    dose_tenths: int = 0
    has_grind_size: bool = False
    grind_size_tenths: int = 0
    grind_unit: str = ""


@dataclass
class UpdatePlan:
    result: ProfileEntry = field(default_factory=ProfileEntry)
    changed: bool = False


def decide_update(screen: Screen, current: ProfileState, request: Request, interval_ok: bool):
    """Returns (UpdatePlan, None) or (None, Failure). State rules, create-needs-dose, unit-needs-size, RATE_LIMITED;
    STORE_FAILED is the caller's."""
    obj = "grindProfile"
    f = state_check(screen, OpClass.UPDATE, obj)
    if f:
        return None, f
    if not current.exists and not request.has_dose:
        return None, Failure("MISSING_FIELD", MSG["DOSE_REQUIRED"], "dose", obj)
    r = ProfileEntry(index=PROFILE_WRITABLE_INDEX)
    r.dose_tenths = request.dose_tenths if request.has_dose else current.dose_tenths
    r.has_grind_size = current.exists and current.has_grind_size
    r.grind_size_tenths = current.grind_size_tenths if r.has_grind_size else 0
    r.grind_unit = current.grind_unit[:GRIND_UNIT_MAX_LEN] if current.exists else ""
    if request.has_grind_size:
        r.has_grind_size = not request.grind_size_null
        r.grind_size_tenths = 0 if request.grind_size_null else request.grind_size_tenths
        if request.grind_size_null:
            r.grind_unit = ""
    if request.has_grind_unit:
        r.grind_unit = request.grind_unit[:GRIND_UNIT_MAX_LEN]
    if r.grind_unit and not r.has_grind_size:
        return None, Failure("INVALID_PARAM", MSG["UNIT_NEEDS_SIZE"], "grindUnit", obj)
    changed = (not current.exists or r.dose_tenths != current.dose_tenths or r.has_grind_size != current.has_grind_size
               or (r.has_grind_size and r.grind_size_tenths != current.grind_size_tenths)
               or r.grind_unit != current.grind_unit)
    if changed and not interval_ok:
        return None, Failure("RATE_LIMITED", MSG["RATE_LIMITED"], "", obj)
    return UpdatePlan(r, changed), None


def decide_select(screen: Screen, index: int, profile3_exists: bool, current_selection: int, interval_ok: bool):
    """Returns (will_store, None) or (False, Failure)."""
    obj = "grindProfile"
    f = state_check(screen, OpClass.SELECT, obj)
    if f:
        return False, f
    if index == PROFILE_WRITABLE_INDEX and not profile3_exists:
        return False, Failure("PROFILE_NOT_SET", MSG["PROFILE_NOT_SET"], "index", obj)
    if index == current_selection:
        return False, None
    if not interval_ok:
        return False, Failure("RATE_LIMITED", MSG["RATE_LIMITED"], "", obj)
    return True, None


_BLOCK_ERRORS = {
    StartBlock.MOTOR_DISABLED: ("MOTOR_DISABLED", "MOTOR_DISABLED"),
    StartBlock.UPDATE_LOCK: ("UPDATING", "UPDATING"),
    StartBlock.LOAD_CELL_NOT_CONNECTED: ("LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NOT_CONNECTED"),
    StartBlock.LOAD_CELL_NO_DATA: ("LOAD_CELL_NO_DATA", "LOAD_CELL_NO_DATA"),
    StartBlock.LOAD_CELL_RATE_INVALID: ("LOAD_CELL_RATE_INVALID", "LOAD_CELL_RATE_INVALID"),
}


def start_block_error(block: StartBlock):
    """(code, message) of a start block, or ("", "") for NONE."""
    r = _BLOCK_ERRORS.get(block)
    return (r[0], MSG[r[1]]) if r else ("", "")


@dataclass
class StartInputs:
    profile_exists: bool = True
    stored_dose_tenths: int = 0
    has_expect_dose: bool = False
    expect_dose_tenths: int = 0
    block: StartBlock = StartBlock.NONE


def decide_start(screen: Screen, inputs: StartInputs):
    """The whole order of 7.2. None = accepted, else a Failure."""
    obj = "grindControl"
    f = state_check(screen, OpClass.START, obj)
    if f:
        return f
    if not inputs.profile_exists:
        return Failure("PROFILE_NOT_SET", MSG["PROFILE_NOT_SET"], "profile", obj)
    if not DOSE_MIN_TENTHS <= inputs.stored_dose_tenths <= DOSE_MAX_TENTHS:
        return Failure("OUT_OF_RANGE", MSG["DOSE_OUT_OF_BLE_RANGE"], "dose", obj)
    if inputs.has_expect_dose and inputs.expect_dose_tenths != inputs.stored_dose_tenths:
        return Failure("CONFLICT", MSG["CONFLICT"], "expectDose", obj)
    code, message = start_block_error(inputs.block)
    if code:
        return Failure(code, message, "", obj)
    return None


def dev_state(screen: Screen, block: StartBlock) -> str:
    if screen == Screen.LEAD_IN:
        return "STARTING"
    if screen == Screen.UPDATING:
        return "UPDATING"
    if screen == Screen.READY:
        return "IDLE" if block == StartBlock.NONE else "WARNING"
    if screen in (Screen.MENU, Screen.EDIT, Screen.OTHER):
        return "SETTING"
    if screen == Screen.GRINDING:
        return "GRINDING"
    if screen == Screen.DONE:
        return "DONE"
    return "UNKNOWN"


def blocked_for(state: str, block: StartBlock) -> str:
    """The `blocked` value of periodInfo ("" = null)."""
    if state == "IDLE":
        return ""
    if state == "STARTING":
        return "COMMAND_IN_FLIGHT"
    if state == "UPDATING":
        return "UPDATING"
    if state == "WARNING":
        return start_block_error(block)[0]
    return "NOT_READY"


# ---- result mapping (6.3) ---------------------------------------------------------------------------------------

OUTCOMES = ["UNKNOWN", "SUCCESS", "OVERSHOOT", "MAX_PULSES", "TIMEOUT", "ERROR"]
ABORT_REASONS = ["NONE", "SENSOR", "TARE", "PRIME_FLOW", "PREDICTIVE_FLOW", "STALL", "MOTOR_FAULT", "NEG_WEIGHT", "UPDATE"]

_REASON_CODES = {"STALL": "ERR_STALLED", "PRIME_FLOW": "ERR_NO_FLOW", "PREDICTIVE_FLOW": "ERR_NO_FLOW",
                 "TARE": "ERR_TARE", "MOTOR_FAULT": "ERR_MOTOR", "NEG_WEIGHT": "ERR_NEG_WEIGHT", "UPDATE": "UPDATE"}


def result_code(outcome: str, reason: str, stopped: bool) -> str:
    """A stop explains the end only when the grind has no outcome of its own (UNKNOWN, or the TIMEOUT a stopped grind
    falls into). An ERROR without a known reason is ERR_SENSOR."""
    if stopped and outcome in ("UNKNOWN", "TIMEOUT"):
        return "STOPPED"
    if outcome == "SUCCESS":
        return "OK"
    if outcome in ("OVERSHOOT", "MAX_PULSES"):
        return outcome
    if outcome in ("TIMEOUT", "UNKNOWN"):
        return "TIMEOUT"
    return _REASON_CODES.get(reason, "ERR_SENSOR")


def result_weight_settled(code: str) -> bool:
    return code in ("OK", "OVERSHOOT", "MAX_PULSES")

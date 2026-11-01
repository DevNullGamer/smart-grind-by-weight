"""
Web panel contract in Python (docs/PORTAL-API.md): the pure rules the dev server answers with.

This is a port of the firmware's network/portal_api.cpp and portal_guard.cpp (strict flat-object scanner, field
table, normalisers, Host/Origin/Content-Type/custom-header checks, decisions). It has no I/O and no clock.
tools/tests/test_portal_dev_server.py runs it against tools/tests/portal_api_vectors.json, the same vectors the
firmware native tests use, so the two implementations cannot drift apart unnoticed.
"""

import json
import math
import struct

BODY_MAX = 512
KEY_MAX_LEN = 32
STRING_MAX_LEN = 24
HEADER_MAX_LEN = 128
NUMBER_TOKEN_MAX = 32
INPUT_LIMIT = 1.0e6  # largest magnitude a normaliser works with (portal_api.cpp clamp_input)
LEAD_IN_MS = 3000
CUSTOM_HEADER_VALUE = "1"
BRIGHTNESS_MIN = 15
BRIGHTNESS_MAX = 100
PURGE_MIN_G, PURGE_MAX_G = 0.1, 2.5
FRESHNESS_STEPS_H = (0.5, 1.0, 2.0, 3.0, 4.0, 8.0, 12.0, 24.0, 48.0)
PROFILE_NAMES = ("SINGLE", "DOUBLE", "CUSTOM")
COMMANDS = ("stop", "tare", "start", "motor_test")

# code -> (HTTP status, reason). Same table as kErrors in portal_api.cpp.
ERRORS = {
    "empty_body": (400, "The request body is empty."),
    "body_too_large": (413, "The request body is larger than the grinder accepts."),
    "malformed_json": (400, "The body is not a flat JSON object of simple values."),
    "nested_value": (400, "Nested objects and arrays are not accepted."),
    "duplicate_key": (400, "A key appears more than once."),
    "unknown_key": (400, "The body contains a key the grinder does not know."),
    "unexpected_field": (400, "A field is not allowed for this command."),
    "missing_field": (400, "A required field is missing."),
    "empty_patch": (400, "The body does not change anything."),
    "wrong_type": (400, "A value has the wrong JSON type."),
    "invalid_value": (400, "A value is not acceptable."),
    "out_of_range": (400, "A value is outside the allowed range."),
    "unknown_command": (400, "The command is not known."),
    "invalid_device_name": (400, "The device name must be 1-24 characters of a-z, 0-9 and hyphen, not starting or ending with a hyphen."),
    "web_interface_off": (403, "The web interface is switched off on the grinder."),
    "bad_host": (403, "The Host header is not a .local name or an IP address."),
    "bad_origin": (403, "The Origin header does not match the Host header."),
    "missing_custom_header": (403, "The X-Grinder: 1 header is required."),
    "unsupported_media_type": (415, "The body must be sent as application/json."),
    "command_in_flight": (409, "Another request is still being handled."),
    "busy": (409, "A grind, autotune or motor run is active."),
    "not_ready": (409, "The grinder screen is not in a state that allows this."),
    "update_in_progress": (409, "A firmware update is running."),
    "motor_disabled": (409, "The motor is disabled (board not confirmed)."),
    "motor_fault": (409, "The motor output fault is latched until reboot."),
    "load_cell_not_connected": (409, "The load cell is not connected."),
    "load_cell_no_data": (409, "The load cell delivers no data."),
    "load_cell_rate_invalid": (409, "The load cell sample rate is invalid."),
    "ui_busy": (503, "The grinder screen did not answer in time, try again."),
    "store_failed": (500, "The value could not be saved."),
}


class Refusal(Exception):
    """A refusal with one of the codes of ERRORS."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code

    @property
    def status(self):
        return ERRORS[self.code][0]

    @property
    def reason(self):
        return ERRORS[self.code][1]


# ---------------------------------------------------------------------------------------------- field table
def _f(key, type_, group, **extra):
    field = {"key": key, "type": type_, "group": group}
    field.update(extra)
    return field


# Same order, limits and defaults as GET /api/schema (kFields in portal_api.cpp).
FIELDS = [
    _f("ble_startup", "bool", "settings", default=True),
    _f("logging", "bool", "settings", default=False),
    _f("swipe", "bool", "settings", default=False),
    _f("auto_start", "bool", "settings", default=False),
    _f("auto_return", "bool", "settings", default=False),
    _f("brightness_normal", "int", "settings", unit="%", min=15, max=100, step=1, default=100),
    _f("brightness_screensaver", "int", "settings", unit="%", min=15, max=100, step=1, default=35),
    _f("purge_mode", "enum", "settings", choices=["keep", "remove"], default="remove"),
    _f("purge_amount_g", "number", "settings", unit="g", min=0.1, max=2.5, step=0.1, default=1.0),
    _f("freshness_hours", "number", "settings", unit="h", min=0.5, max=48.0, default=8.0),
    _f("grind_mode", "enum", "settings", choices=["weight", "time"], default="weight"),
    _f("single_weight_g", "number", "profile", unit="g", min=5.0, max=1000.0, default=9.0),
    _f("single_time_s", "number", "profile", unit="s", min=0.5, max=25.0, default=5.0),
    _f("double_weight_g", "number", "profile", unit="g", min=5.0, max=1000.0, default=18.0),
    _f("double_time_s", "number", "profile", unit="s", min=0.5, max=25.0, default=10.0),
    _f("custom_weight_g", "number", "profile", unit="g", min=5.0, max=1000.0, default=21.5),
    _f("custom_time_s", "number", "profile", unit="s", min=0.5, max=25.0, default=12.0),
    _f("device_name", "string", "device", min=1, max=24, default="grindbyweight"),
]
FIELD_BY_KEY = {f["key"]: f for f in FIELDS}
FIELD_INDEX = {f["key"]: i for i, f in enumerate(FIELDS)}


def schema():
    """The GET /api/schema document."""
    profiles = []
    for p, name in enumerate(PROFILE_NAMES):
        profiles.append({"index": p, "name": name, "weight_key": FIELDS[11 + 2 * p]["key"],
                         "time_key": FIELDS[12 + 2 * p]["key"]})
    return {"fields": [dict(f) for f in FIELDS], "profiles": profiles}


def default_values():
    values = {f["key"]: f["default"] for f in FIELDS}
    return values


# ---------------------------------------------------------------------------------------------- JSON writer
def format_number(value):
    """At most 3 decimals, trailing zeros trimmed, one decimal kept ("9.0", "21.5")."""
    text = f"{value:.3f}"
    if "." in text:
        text = text.rstrip("0")
        if text.endswith("."):
            text += "0"
    return text


def encode_json(obj):
    """Compact JSON the way the firmware writes it: floats with at most 3 decimals."""
    if obj is True:
        return "true"
    if obj is False:
        return "false"
    if obj is None:
        return "null"
    if isinstance(obj, float):
        return format_number(obj) if math.isfinite(obj) else "null"
    if isinstance(obj, int):
        return str(obj)
    if isinstance(obj, str):
        return json.dumps(obj)
    if isinstance(obj, (list, tuple)):
        return "[" + ",".join(encode_json(x) for x in obj) + "]"
    if isinstance(obj, dict):
        return "{" + ",".join(json.dumps(k) + ":" + encode_json(v) for k, v in obj.items()) + "}"
    raise TypeError(f"cannot encode {type(obj).__name__}")


def error_body(code):
    status, reason = ERRORS[code]
    return status, encode_json({"error": code, "reason": reason})


# ---------------------------------------------------------------------------------------------- body scanner
def _is_digit(c):
    return 48 <= c <= 57


def _number_token_valid(token):
    n = len(token)
    i = 0
    if i < n and token[i] == 45:
        i += 1
    if i >= n or not _is_digit(token[i]):
        return False
    if token[i] == 48:
        i += 1
    else:
        while i < n and _is_digit(token[i]):
            i += 1
    if i < n and token[i] == 46:
        i += 1
        if i >= n or not _is_digit(token[i]):
            return False
        while i < n and _is_digit(token[i]):
            i += 1
    if i < n and token[i] in (101, 69):
        i += 1
        if i < n and token[i] in (43, 45):
            i += 1
        if i >= n or not _is_digit(token[i]):
            return False
        while i < n and _is_digit(token[i]):
            i += 1
    return i == n


class _Scanner:
    """Strict flat-object scanner (portal_api.cpp scan_object). `on_key(key)` and `on_value(key, value)` raise
    Refusal; values are bool, None, str or float (numbers always float, inf when they overflow)."""

    WS = b" \t\r\n"

    def __init__(self, body):
        self.b = body
        self.i = 0

    def skip_ws(self):
        while self.i < len(self.b) and self.b[self.i] in self.WS:
            self.i += 1

    def string(self):
        b = self.b
        if self.i >= len(b) or b[self.i] != 34:
            raise Refusal("malformed_json")
        self.i += 1
        start = self.i
        while self.i < len(b) and b[self.i] != 34:
            if b[self.i] < 0x20 or b[self.i] > 0x7E or b[self.i] == 92:
                raise Refusal("malformed_json")
            self.i += 1
        if self.i >= len(b):
            raise Refusal("malformed_json")
        text = b[start:self.i].decode("ascii")
        self.i += 1
        return text

    def word(self, word, value):
        if self.b[self.i:self.i + len(word)] != word:
            raise Refusal("malformed_json")
        self.i += len(word)
        return value

    def value(self):
        b = self.b
        if self.i >= len(b):
            raise Refusal("malformed_json")
        c = b[self.i]
        if c in (123, 91):  # { [
            raise Refusal("nested_value")
        if c == 34:
            return self.string()
        if c == 116:
            return self.word(b"true", True)
        if c == 102:
            return self.word(b"false", False)
        if c == 110:
            return self.word(b"null", None)
        start = self.i
        while self.i < len(b) and b[self.i] in b"0123456789-+.eE":
            self.i += 1
        token = b[start:self.i]
        if not token or len(token) > NUMBER_TOKEN_MAX or not _number_token_valid(token):
            raise Refusal("malformed_json")
        try:
            return float(token)
        except OverflowError:  # pragma: no cover - float() returns inf instead
            return math.inf

    def scan(self, on_key, on_value):
        """Returns the number of members."""
        b = self.b
        if len(b) == 0:
            raise Refusal("empty_body")
        if len(b) > BODY_MAX:
            raise Refusal("body_too_large")
        seen = []
        self.skip_ws()
        if self.i >= len(b) or b[self.i] != 123:
            raise Refusal("malformed_json")
        self.i += 1
        self.skip_ws()
        if self.i < len(b) and b[self.i] == 125:
            self.i += 1
        else:
            while True:
                key = self.string()
                if len(key) == 0 or len(key) > KEY_MAX_LEN:
                    raise Refusal("malformed_json")
                if key in seen:
                    raise Refusal("duplicate_key")
                seen.append(key)
                self.skip_ws()
                if self.i >= len(b) or b[self.i] != 58:
                    raise Refusal("malformed_json")
                self.i += 1
                self.skip_ws()
                on_key(key)
                value = self.value()
                on_value(key, value)
                self.skip_ws()
                if self.i >= len(b):
                    raise Refusal("malformed_json")
                if b[self.i] == 44:
                    self.i += 1
                    self.skip_ws()
                    continue
                if b[self.i] == 125:
                    self.i += 1
                    break
                raise Refusal("malformed_json")
        self.skip_ws()
        if self.i < len(b):
            raise Refusal("malformed_json")
        return len(seen)


# ---------------------------------------------------------------------------------------------- normalisers
def f32(x):
    """Round to a C float (the firmware stores floats)."""
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def _clamp_input(x):
    return max(-INPUT_LIMIT, min(INPUT_LIMIT, x))


def _round_half_away(x):
    return math.floor(abs(x) + 0.5) * (1 if x >= 0 else -1)


def normalise_brightness(x):
    r = _round_half_away(_clamp_input(x))
    r = max(0, min(100, r))
    return float(max(BRIGHTNESS_MIN, min(BRIGHTNESS_MAX, r)))


def normalise_purge_amount(x):
    # Firmware: (int)(f32(x) * 10 + 0.5f), i.e. half rounds up in float32 arithmetic (not Python's half-even)
    x = max(PURGE_MIN_G, min(PURGE_MAX_G, f32(_clamp_input(x))))
    units = math.floor(f32(f32(x * 10.0) + 0.5))
    return f32(units / 10.0)


def normalise_freshness(x):
    best, best_diff = FRESHNESS_STEPS_H[0], abs(x - FRESHNESS_STEPS_H[0])
    for step in FRESHNESS_STEPS_H[1:]:
        diff = abs(x - step)
        if diff < best_diff:  # strict: a tie keeps the lower step
            best, best_diff = step, diff
    return best


def device_name_valid(name):
    if not (1 <= len(name) <= STRING_MAX_LEN):
        return False
    if name[0] == "-" or name[-1] == "-":
        return False
    return all(c in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name)


def stored_brightness_percent(percent):
    """What the menu shows for a stored brightness: the fraction is read back truncated, (int)(f * 100)."""
    fraction = f32(percent / 100.0)
    return int(f32(fraction * 100.0))


def _check_setting(field, value):
    """Type/range check of one PATCH member. Returns (applied value, adjusted)."""
    type_ = field["type"]
    if type_ == "bool":
        if not isinstance(value, bool):
            raise Refusal("wrong_type")
        return value, False
    if type_ == "enum":
        if not isinstance(value, str):
            raise Refusal("wrong_type")
        if len(value) > STRING_MAX_LEN or value not in field["choices"]:
            raise Refusal("invalid_value")
        return value, False
    if type_ == "string":
        if not isinstance(value, str):
            raise Refusal("wrong_type")
        if not device_name_valid(value):
            raise Refusal("invalid_device_name")
        return value, False
    # int / number
    if isinstance(value, bool) or not isinstance(value, float):
        raise Refusal("wrong_type")
    if not math.isfinite(value):
        raise Refusal("invalid_value")
    key = field["key"]
    if field["group"] == "profile":
        candidate = f32(_clamp_input(value))
        if not (field["min"] <= candidate <= field["max"]):
            raise Refusal("out_of_range")
        return candidate, False
    if key.startswith("brightness_"):
        result = normalise_brightness(value)
    elif key == "purge_amount_g":
        result = normalise_purge_amount(value)
    else:
        result = normalise_freshness(value)
    return result, abs(result - value) > 1.0e-4


def parse_patch(body):
    """Validates a PATCH body. Returns (applied {key: value}, adjusted [keys]); raises Refusal."""
    applied = {}
    adjusted = []
    current = {"field": None}

    def on_key(key):
        if key not in FIELD_BY_KEY:
            raise Refusal("unknown_key")
        current["field"] = FIELD_BY_KEY[key]

    def on_value(key, value):
        result, was_adjusted = _check_setting(current["field"], value)
        applied[key] = result
        if was_adjusted:
            adjusted.append(key)

    count = _Scanner(body).scan(on_key, on_value)
    if count == 0:
        raise Refusal("empty_patch")
    order = sorted(applied, key=lambda k: FIELD_INDEX[k])
    return {k: applied[k] for k in order}, sorted(adjusted, key=lambda k: FIELD_INDEX[k])


def parse_command(body):
    """Validates a command body. Returns (cmd, profile or None); raises Refusal."""
    state = {"cmd": None, "profile": None, "field": None}

    def on_key(key):
        if key not in ("cmd", "profile"):
            raise Refusal("unknown_key")
        state["field"] = key

    def on_value(key, value):
        if state["field"] == "cmd":
            if not isinstance(value, str):
                raise Refusal("wrong_type")
            if len(value) > STRING_MAX_LEN or value not in COMMANDS:
                raise Refusal("unknown_command")
            state["cmd"] = value
            return
        if isinstance(value, bool) or not isinstance(value, float):
            raise Refusal("wrong_type")
        if not math.isfinite(value) or value != math.floor(value):
            raise Refusal("invalid_value")
        if value < 0 or value >= len(PROFILE_NAMES):
            raise Refusal("out_of_range")
        state["profile"] = int(value)

    _Scanner(body).scan(on_key, on_value)
    if state["cmd"] is None:
        raise Refusal("missing_field")
    if state["cmd"] == "start":
        if state["profile"] is None:
            raise Refusal("missing_field")
    elif state["profile"] is not None:
        raise Refusal("unexpected_field")
    return state["cmd"], state["profile"]


# ---------------------------------------------------------------------------------------------- guards
def _is_hex(c):
    return c in "0123456789abcdefABCDEF"


def _port_valid(s):
    return 1 <= len(s) <= 5 and s.isascii() and s.isdigit() and 1 <= int(s) <= 65535


def _ipv4_valid(s):
    parts = s.split(".")
    if len(parts) != 4:
        return False
    for part in parts:
        if not (1 <= len(part) <= 3 and part.isascii() and part.isdigit()):
            return False
        if int(part) > 255 or (len(part) > 1 and part[0] == "0"):
            return False
    return True


def _ipv6_valid(s):
    n = len(s)
    if n < 2 or n > 45:
        return False
    groups = 0
    compressed = False
    i = 0
    if s[0] == ":":
        if s[1] != ":":
            return False
        compressed = True
        i = 2
        if i == n:
            return True
    while i < n:
        start = i
        dotted = False
        while i < n and s[i] != ":":
            dotted = dotted or s[i] == "."
            i += 1
        length = i - start
        if length == 0:
            return False
        if dotted:
            if i != n or not _ipv4_valid(s[start:i]):
                return False
            groups += 2
        else:
            if length > 4 or not all(_is_hex(c) for c in s[start:i]):
                return False
            groups += 1
        if i == n:
            break
        i += 1
        if i < n and s[i] == ":":
            if compressed:
                return False
            compressed = True
            i += 1
        elif i == n:
            return False
    return groups <= 7 if compressed else groups == 8


def _local_name_valid(s):
    if not s or len(s) > 253:
        return False
    labels = s.split(".")
    if len(labels) < 2 or labels[-1].lower() != "local":
        return False
    for label in labels:
        if not (1 <= len(label) <= 63) or label[0] == "-" or label[-1] == "-":
            return False
        if not all(c.lower() in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in label):
            return False
    return True


def host_valid(host):
    if not host or len(host) > HEADER_MAX_LEN or not host.isascii():
        return False
    if host[0] == "[":
        end = host.find("]")
        if end < 0 or not _ipv6_valid(host[1:end]):
            return False
        if end + 1 == len(host):
            return True
        return host[end + 1] == ":" and _port_valid(host[end + 2:])
    name = host
    colon = host.find(":")
    if colon >= 0:
        name = host[:colon]
        if not _port_valid(host[colon + 1:]):
            return False
    return _ipv4_valid(name) or _local_name_valid(name)


def check_host(host):
    if not host_valid(host):
        raise Refusal("bad_host")


def check_origin(origin, host):
    if origin is None:
        return
    if host is None or len(origin) != 7 + len(host) or origin[:7] != "http://":
        raise Refusal("bad_origin")
    if origin[7:].lower() != host.lower():
        raise Refusal("bad_origin")


def check_content_type(content_type):
    if content_type is None or len(content_type) > HEADER_MAX_LEN:
        raise Refusal("unsupported_media_type")
    media = content_type.split(";", 1)[0].strip(" \t")
    if media.lower() != "application/json":
        raise Refusal("unsupported_media_type")


def check_custom_header(value):
    if value != CUSTOM_HEADER_VALUE:
        raise Refusal("missing_custom_header")


def check_request(host, origin, content_type, custom, state_changing):
    """Host, Origin and (state-changing only) Content-Type and X-Grinder, in the order of section 2."""
    check_host(host)
    check_origin(origin, host)
    if state_changing:
        check_content_type(content_type)
        check_custom_header(custom)


# ---------------------------------------------------------------------------------------------- decisions
def block_to_error(block):
    """A GrindStartBlock / MotorTestBlock name (vector spelling) as an error code, or None."""
    if block in (None, "NONE"):
        return None
    if block == "START_FAILED":
        return "busy"
    return block.lower()


def decide_command(cmd, ui, busy, update_held, in_flight, start_block, motor_test_block):
    """portal_command_decide: the refusal code or None. `ui` is READY, MENU or OTHER."""
    if cmd == "stop":
        return None
    if in_flight:
        return "command_in_flight"
    if update_held:
        return "update_in_progress"
    if busy:
        return "busy"
    if cmd == "tare":
        return None if ui == "READY" else "not_ready"
    if cmd == "start":
        if ui != "READY":
            return "not_ready"
        return block_to_error(start_block)
    if cmd == "motor_test":
        if ui == "OTHER":
            return "not_ready"
        return block_to_error(motor_test_block)
    return None


def decide_write(ui, busy, update_held, in_flight):
    """portal_write_decide: the refusal code or None."""
    if in_flight:
        return "command_in_flight"
    if update_held:
        return "update_in_progress"
    if busy:
        return "busy"
    return "not_ready" if ui == "OTHER" else None

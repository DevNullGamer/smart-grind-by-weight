"""WiFi provisioning helpers shared by the Python tools (pure: no BLE, no I/O, no secrets in messages).

Protocol (firmware: wifi_parse_provisioning in src/network/wifi_credentials.cpp, BLE debug RX characteristic):
  set    [0x04][ssid_len][ssid][pass_len][pass][token16]   token = 16 lowercase hex ASCII characters
  clear  [0x05]
SSID is 1-32 bytes, the password is empty (open network) or 8-63 bytes, neither contains NUL; lengths count UTF-8 bytes.
The device answers with text lines on the debug TX characteristic (after command 0x01); classify_reply() maps them.

tools/web-flasher/wifi-setup.js is the JavaScript twin of this module. Both are checked against the shared vectors in
tools/tests/wifi_provision_vectors.json and (for the firmware parser) test_native/test_wifi_credentials.cpp.
"""

import importlib.util
import json
import re
import secrets
from pathlib import Path

CMD_DEBUG_ENABLE = 0x01
CMD_DEBUG_DISABLE = 0x02
CMD_WIFI_SET = 0x04
CMD_WIFI_CLEAR = 0x05

SSID_MAX_BYTES = 32
PASSWORD_MIN_BYTES = 8
PASSWORD_MAX_BYTES = 63
TOKEN_LENGTH = 16
MAX_FRAME_BYTES = 1 + 1 + SSID_MAX_BYTES + 1 + PASSWORD_MAX_BYTES + TOKEN_LENGTH  # 114

DEFAULT_HOST = "grindbyweight.local"
TOKEN_RE = re.compile(r"^[0-9a-f]{16}$")

# classify_reply() results
REPLY_STORED = "stored"
REPLY_CLEARED = "cleared"
REPLY_LOCKED = "locked"            # credentials exist and the WiFi menu page is not open
REPLY_REJECTED = "rejected"
REPLY_STORE_FAILED = "store_failed"
REPLY_UNKNOWN = "unknown"


class ProvisionError(ValueError):
    """Invalid input. The message never contains the password or the token."""


def generate_token():
    """A fresh API token: 16 lowercase hex characters from the OS random source."""
    return secrets.token_hex(8)


def is_valid_token(token):
    return isinstance(token, str) and TOKEN_RE.match(token) is not None


def mask_token(token):
    """Safe-to-print form of a token: first and last two characters."""
    if not isinstance(token, str) or not token:
        return "(none)"
    return token[:2] + "*" * (len(token) - 4) + token[-2:] if len(token) > 4 else "*" * len(token)


def _utf8(text, what):
    if not isinstance(text, str):
        raise ProvisionError("%s must be text" % what)
    try:
        data = text.encode("utf-8")
    except UnicodeEncodeError:
        raise ProvisionError("%s contains characters that cannot be encoded" % what) from None
    if b"\x00" in data:
        raise ProvisionError("%s must not contain NUL characters" % what)
    return data


def encode_ssid(ssid):
    data = _utf8(ssid, "SSID")
    if not 1 <= len(data) <= SSID_MAX_BYTES:
        raise ProvisionError("SSID must be 1-%d bytes (it is %d bytes in UTF-8)" % (SSID_MAX_BYTES, len(data)))
    return data


def encode_password(password):
    """Empty password = open network; otherwise 8-63 bytes."""
    data = _utf8(password, "Password")
    if data and not PASSWORD_MIN_BYTES <= len(data) <= PASSWORD_MAX_BYTES:
        raise ProvisionError("Password must be empty (open network) or %d-%d bytes (it is %d bytes in UTF-8)"
                             % (PASSWORD_MIN_BYTES, PASSWORD_MAX_BYTES, len(data)))
    return data


def validate_credentials(ssid, password):
    """Raises ProvisionError unless the SSID/password pair would be accepted by the firmware."""
    encode_ssid(ssid)
    encode_password(password)


def resolve_password(open_network, password_env, environ, prompt):
    """Password for wifi-set: none for an open network, else from the named environment variable, else prompt()."""
    if open_network:
        return ""
    if password_env:
        if password_env not in environ:
            raise ProvisionError("Environment variable %s is not set" % password_env)
        return environ[password_env]
    return prompt()


def build_set_frame(ssid, password, token):
    """The complete BLE write for command 0x04."""
    ssid_bytes = encode_ssid(ssid)
    pass_bytes = encode_password(password)
    if not is_valid_token(token):
        raise ProvisionError("Token must be 16 lowercase hex characters")
    return (bytes([CMD_WIFI_SET, len(ssid_bytes)]) + ssid_bytes
            + bytes([len(pass_bytes)]) + pass_bytes + token.encode("ascii"))


def build_clear_frame():
    return bytes([CMD_WIFI_CLEAR])


def classify_reply(text):
    """Maps the device's debug text (log lines of BluetoothManager::handle_wifi_command) to a REPLY_* constant.

    Order matters: the "already stored" refusal and "could not be stored" both contain the word stored.
    """
    if not text:
        return REPLY_UNKNOWN
    if "credentials already stored" in text or "Open Menu -> WiFi" in text:
        return REPLY_LOCKED
    if "could not be stored" in text or "clear failed" in text:
        return REPLY_STORE_FAILED
    if "credentials stored" in text:
        return REPLY_STORED
    if "credentials cleared" in text:
        return REPLY_CLEARED
    if "rejected" in text:
        return REPLY_REJECTED
    return REPLY_UNKNOWN


def parse_hardware_info(text):
    """Parses the BLE hardware-info JSON; returns {"connected": bool, "ip": str, "host": str} or None.

    None for anything that is not a complete JSON object (a read can be stale or cut at the ATT MTU).
    """
    try:
        data = json.loads(text)
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict) or "wifi" not in data:
        return None
    ip = data.get("ip") if isinstance(data.get("ip"), str) else ""
    host = data.get("host") if isinstance(data.get("host"), str) else ""
    return {"connected": data.get("wifi") is True and ip != "", "ip": ip, "host": host}


def config_values(token, host=DEFAULT_HOST):
    """What ~/.grinder-wifi.json (and the web flasher's downloaded file) holds for this device."""
    if not is_valid_token(token):
        raise ProvisionError("Token must be 16 lowercase hex characters")
    return {"host": host, "token": token}


def token_save_failure_lines(token, error):
    """Lines to print when the grinder confirmed the credentials but the token could not be written to disk.

    This is the one place the token is printed unmasked: it exists nowhere else, so it must not be lost.
    """
    return [
        "[ERROR] The credentials ARE stored on the grinder, but the update token could NOT be saved (%s)." % error,
        "[IMPORTANT] Write this update token down now, it is shown only once: %s" % token,
        "[INFO] Use it with: python3 tools/grinder.py wifi-upload --token <token> (or set GRINDER_WIFI_TOKEN), "
        "or run wifi-set again with Menu -> WiFi open on the grinder to generate a new one.",
    ]


def _load_wifi_tool():
    """tools/wifi/grinder-wifi.py (hyphenated name, so it cannot be imported normally)."""
    path = Path(__file__).resolve().parent / "wifi" / "grinder-wifi.py"
    spec = importlib.util.spec_from_file_location("grinder_wifi_config", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def save_device_config(token, config_path=None):
    """Stores host + token for `grinder.py wifi-upload` through the WiFi tool's own 0600 writer.

    The token is replaced. Other keys of an existing config file are kept, including a custom host; the host is
    only set (to the default name) when the file has none. Returns the path written; raises OSError if the file
    cannot be written.
    """
    wifi_tool = _load_wifi_tool()
    path = Path(config_path) if config_path else wifi_tool.CONFIG_PATH
    config = wifi_tool.load_config(path)
    values = config_values(token)
    if not config.get("host"):
        config.pop("host", None)
        config = {"host": values["host"], **config}  # host first, like the web flasher's file
    config["token"] = values["token"]
    wifi_tool.save_config(config, path)
    return path

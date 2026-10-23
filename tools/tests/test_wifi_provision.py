"""Tests for tools/wifi_provision.py (pure frame builder / validation / reply parsing)."""

import json
import re
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import wifi_provision as wp  # noqa: E402

VECTORS = json.loads((Path(__file__).resolve().parent / "wifi_provision_vectors.json").read_text(encoding="utf-8"))
TOKEN = "0123456789abcdef"


def reference_parse(payload):
    """Independent re-implementation of wifi_parse_provisioning (frame without the command byte)."""
    pos = 0
    ssid_len = payload[pos]
    pos += 1
    assert 1 <= ssid_len <= 32
    ssid = payload[pos:pos + ssid_len]
    assert len(ssid) == ssid_len
    pos += ssid_len
    pass_len = payload[pos]
    pos += 1
    assert pass_len == 0 or 8 <= pass_len <= 63
    password = payload[pos:pos + pass_len]
    assert len(password) == pass_len
    pos += pass_len
    token = payload[pos:]
    assert len(token) == 16
    assert b"\x00" not in ssid + password
    assert re.fullmatch(rb"[0-9a-f]{16}", token)
    return ssid.decode("utf-8"), password.decode("utf-8"), token.decode("ascii")


@pytest.mark.parametrize("vector", VECTORS["set_frames"], ids=lambda v: v["name"])
def test_frames_match_shared_vectors(vector):
    frame = wp.build_set_frame(vector["ssid"], vector["password"], vector["token"])
    assert frame.hex() == vector["frame_hex"]
    assert frame[0] == 0x04
    assert reference_parse(frame[1:]) == (vector["ssid"], vector["password"], vector["token"])


def test_clear_frame_matches_vector():
    assert wp.build_clear_frame().hex() == VECTORS["clear_frame_hex"]
    assert wp.build_clear_frame() == b"\x05"


def test_literal_basic_frame():
    frame = wp.build_set_frame("home", "secret123", TOKEN)
    assert frame == bytes([0x04, 4]) + b"home" + bytes([9]) + b"secret123" + TOKEN.encode()


def test_max_frame_is_114_bytes():
    frame = wp.build_set_frame("s" * 32, "p" * 63, TOKEN)
    assert len(frame) == wp.MAX_FRAME_BYTES == 114


def test_ssid_boundaries():
    for bad in ("", "a" * 33):
        with pytest.raises(wp.ProvisionError):
            wp.build_set_frame(bad, "secret123", TOKEN)
    assert wp.build_set_frame("a", "secret123", TOKEN)
    assert wp.build_set_frame("a" * 32, "secret123", TOKEN)


def test_ssid_counts_utf8_bytes_not_characters():
    # 16 two-byte characters = 32 bytes: fits; 17 = 34 bytes: too long although only 17 characters
    assert wp.build_set_frame("é" * 16, "secret123", TOKEN)
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("é" * 17, "secret123", TOKEN)


def test_password_rules():
    assert wp.build_set_frame("net", "", TOKEN)  # open network
    for bad in ("a", "1234567", "p" * 64):
        with pytest.raises(wp.ProvisionError):
            wp.build_set_frame("net", bad, TOKEN)
    assert wp.build_set_frame("net", "12345678", TOKEN)
    assert wp.build_set_frame("net", "p" * 63, TOKEN)


def test_password_counts_utf8_bytes():
    # 32 two-byte characters = 64 bytes: too long
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("net", "ä" * 32, TOKEN)
    assert wp.build_set_frame("net", "ä" * 31, TOKEN)  # 62 bytes


def test_nul_rejected():
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("ne\x00t", "secret123", TOKEN)
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("net", "secret\x00123", TOKEN)


def test_lone_surrogate_rejected_without_traceback():
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("net\ud800", "secret123", TOKEN)


def test_non_text_rejected():
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame(None, "secret123", TOKEN)
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("net", b"secret123", TOKEN)


@pytest.mark.parametrize("token", ["", "0123456789abcde", "0123456789abcdef0", "0123456789ABCDEF",
                                   "0123456789abcdeg", None, 12345])
def test_bad_tokens_rejected(token):
    with pytest.raises(wp.ProvisionError):
        wp.build_set_frame("net", "secret123", token)


def test_error_messages_do_not_leak_secrets():
    with pytest.raises(wp.ProvisionError) as info:
        wp.build_set_frame("net", "short", TOKEN)
    assert "short" not in str(info.value)
    with pytest.raises(wp.ProvisionError) as info:
        wp.build_set_frame("net", "secret123", "ABCDEF")
    assert "ABCDEF" not in str(info.value)


def test_generate_token_format_and_uniqueness():
    tokens = {wp.generate_token() for _ in range(50)}
    assert len(tokens) == 50
    assert all(wp.is_valid_token(t) for t in tokens)


@pytest.mark.parametrize("text, expected", [
    ("BLE_DEBUG: WiFi credentials stored, connecting\n", wp.REPLY_STORED),
    ("BLE_DEBUG: WiFi credentials cleared\n", wp.REPLY_CLEARED),
    ("BLE_DEBUG: WiFi command rejected, credentials already stored. Open Menu -> WiFi first.\n", wp.REPLY_LOCKED),
    ("BLE_DEBUG: WiFi set rejected, bad payload (3), expected [0x04, ssid_len, ssid, pass_len, pass, token16]\n",
     wp.REPLY_REJECTED),
    ("BLE_DEBUG: WiFi set rejected, invalid credentials\n", wp.REPLY_REJECTED),
    ("BLE_DEBUG: WiFi clear rejected, expected [0x05]\n", wp.REPLY_REJECTED),
    ("BLE_DEBUG: WiFi credentials could not be stored\n", wp.REPLY_STORE_FAILED),
    ("BLE_DEBUG: WiFi clear failed (storage error)\n", wp.REPLY_STORE_FAILED),
    ("BLE_DEBUG: Stream enabled\n", wp.REPLY_UNKNOWN),
    ("", wp.REPLY_UNKNOWN),
    (None, wp.REPLY_UNKNOWN),
])
def test_classify_reply(text, expected):
    assert wp.classify_reply(text) == expected


def test_parse_hardware_info():
    connected = '{"wifi_available":true,"wifi":true,"ip":"192.168.1.20","host":"grindbyweight.local"}'
    assert wp.parse_hardware_info(connected) == {"connected": True, "ip": "192.168.1.20",
                                                 "host": "grindbyweight.local"}
    waiting = '{"wifi":false,"ip":"","host":"grindbyweight.local"}'
    assert wp.parse_hardware_info(waiting)["connected"] is False
    assert wp.parse_hardware_info('{"wifi":true,"ip":""}')["connected"] is False


@pytest.mark.parametrize("text", ["", None, "not json", '{"wifi":tr', "[1,2]", '{"load_cell_active":true}'])
def test_parse_hardware_info_rejects_unusable(text):
    assert wp.parse_hardware_info(text) is None


def test_resolve_password_sources():
    def fail_prompt():
        raise AssertionError("must not prompt")

    assert wp.resolve_password(True, None, {}, fail_prompt) == ""
    assert wp.resolve_password(False, "WIFI_PW", {"WIFI_PW": "secret123"}, fail_prompt) == "secret123"
    assert wp.resolve_password(False, None, {}, lambda: "typed1234") == "typed1234"
    with pytest.raises(wp.ProvisionError):
        wp.resolve_password(False, "MISSING", {}, fail_prompt)


def test_validate_credentials_matches_builder():
    wp.validate_credentials("net", "")
    wp.validate_credentials("net", "12345678")
    with pytest.raises(wp.ProvisionError):
        wp.validate_credentials("", "12345678")
    with pytest.raises(wp.ProvisionError):
        wp.validate_credentials("net", "short")


def test_mask_token():
    masked = wp.mask_token(TOKEN)
    assert masked.startswith("01") and masked.endswith("ef") and "456789" not in masked
    assert wp.mask_token(None) == "(none)"


def test_grinder_py_registers_wifi_set_and_clear():
    import grinder
    parser = grinder.create_parser()
    args = parser.parse_args(["wifi-set", "My Net", "--password-env", "PW"])
    assert (args.command, args.ssid, args.password_env, args.open, args.device) == ("wifi-set", "My Net", "PW", False, "GrindByWeight")
    assert parser.parse_args(["wifi-set", "Open Net", "--open"]).open is True
    with pytest.raises(SystemExit):
        parser.parse_args(["wifi-set", "x", "--open", "--password-env", "PW"])
    assert parser.parse_args(["wifi-clear"]).command == "wifi-clear"


def test_save_device_config_keeps_other_keys(tmp_path):
    path = tmp_path / "cfg.json"
    path.write_text(json.dumps({"host": "10.0.0.5", "extra": 1}), encoding="utf-8")
    written = wp.save_device_config(TOKEN, path)
    assert written == path
    # A custom host is kept; only the token is replaced
    assert json.loads(path.read_text(encoding="utf-8")) == {"host": "10.0.0.5", "token": TOKEN, "extra": 1}


def test_save_device_config_sets_default_host_only_when_absent(tmp_path):
    path = tmp_path / "cfg.json"
    path.write_text(json.dumps({"token": "ffffffffffffffff", "extra": 1}), encoding="utf-8")
    wp.save_device_config(TOKEN, path)
    assert json.loads(path.read_text(encoding="utf-8")) == {"host": "grindbyweight.local", "token": TOKEN, "extra": 1}
    path.write_text(json.dumps({"host": ""}), encoding="utf-8")
    wp.save_device_config(TOKEN, path)
    assert json.loads(path.read_text(encoding="utf-8"))["host"] == "grindbyweight.local"


def test_save_device_config_raises_oserror_when_unwritable(tmp_path):
    with pytest.raises(OSError):
        wp.save_device_config(TOKEN, tmp_path)  # a directory cannot be written as a file


def test_token_save_failure_lines_show_token_once_and_explain():
    lines = wp.token_save_failure_lines(TOKEN, OSError("disk full"))
    text = " ".join(lines)
    assert "ARE stored on the grinder" in text and "NOT be saved" in text and "disk full" in text
    assert text.count(TOKEN) == 1
    assert "--token" in text and "GRINDER_WIFI_TOKEN" in text and "Menu -> WiFi" in text


def test_wifi_set_command_puts_ssid_after_terminator():
    import grinder
    tool = grinder.GrinderTool()
    parser = grinder.create_parser()
    args = parser.parse_args(["wifi-set", "--password-env", "PW", "--", "-weird name"])
    assert args.ssid == "-weird name"
    cmd = tool.wifi_set_command(args)
    assert cmd[-2:] == ["--", "-weird name"]
    assert cmd.index("wifi-set") < cmd.index("--password-env") < cmd.index("--")
    assert "--open" not in cmd
    open_args = parser.parse_args(["wifi-set", "--open", "--", "Cafe"])
    open_cmd = tool.wifi_set_command(open_args)
    assert open_cmd.index("--open") < open_cmd.index("--") and open_cmd[-1] == "Cafe"


def test_save_device_config_creates_file_and_rejects_bad_token(tmp_path):
    path = tmp_path / "new.json"
    wp.save_device_config(TOKEN, path)
    # Same bytes as buildConfigJson() in tools/web-flasher/wifi-setup.js (checked in wifi-setup.test.js)
    assert path.read_text(encoding="utf-8") == (
        '{\n  "host": "grindbyweight.local",\n  "token": "0123456789abcdef"\n}\n')
    assert json.loads(path.read_text(encoding="utf-8")) == {"host": "grindbyweight.local", "token": TOKEN}
    with pytest.raises(wp.ProvisionError):
        wp.save_device_config("bad", tmp_path / "other.json")
    assert not (tmp_path / "other.json").exists()


def test_config_values():
    assert wp.config_values(TOKEN) == {"host": "grindbyweight.local", "token": TOKEN}
    with pytest.raises(wp.ProvisionError):
        wp.config_values("nope")

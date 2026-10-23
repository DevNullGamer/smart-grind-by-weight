"""Tests for the pure helpers of tools/wifi/grinder-wifi.py (no network, no device)."""

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def _load_tool():
    path = TOOLS_DIR / "wifi" / "grinder-wifi.py"
    spec = importlib.util.spec_from_file_location("grinder_wifi_under_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


wifi = _load_tool()
ENV = wifi.FIRMWARE_ENV


def test_validate_token():
    assert wifi.validate_token("0123456789abcdef")
    assert not wifi.validate_token("0123456789ABCDEF")
    assert not wifi.validate_token("0123456789abcde")
    assert not wifi.validate_token("0123456789abcdef0")
    assert not wifi.validate_token("0123456789abcdeg")
    assert not wifi.validate_token("")
    assert not wifi.validate_token(None)


def test_mask_token_hides_the_middle():
    masked = wifi.mask_token("0123456789abcdef")
    assert masked.startswith("01") and masked.endswith("ef")
    assert "456789" not in masked
    assert wifi.mask_token(None) == "(none)"


@pytest.mark.parametrize("raw, expected", [
    ("192.168.1.145", "192.168.1.145"),
    ("grindbyweight.local", "grindbyweight.local"),
    ("  grindbyweight.local  ", "grindbyweight.local"),
    ("http://grindbyweight.local/", "grindbyweight.local"),
    ("HTTP://192.168.1.5:80/api/info", "192.168.1.5"),
    ("192.168.1.5:80", "192.168.1.5"),
    ("", None),
    (None, None),
    ("bad host", None),
    ("-bad", None),
    ("a;b", None),
])
def test_normalize_host(raw, expected):
    assert wifi.normalize_host(raw) == expected


def test_resolve_settings_precedence():
    config = {"host": "cfg.local", "token": "c" * 16}
    environ = {"GRINDER_WIFI_HOST": "env.local", "GRINDER_WIFI_TOKEN": "e" * 16}
    assert wifi.resolve_settings("arg.local", "a" * 16, environ, config) == ("arg.local", "a" * 16)
    assert wifi.resolve_settings(None, None, environ, config) == ("env.local", "e" * 16)
    assert wifi.resolve_settings(None, None, {}, config) == ("cfg.local", "c" * 16)
    assert wifi.resolve_settings(None, None, {}, {}) == (wifi.DEFAULT_HOST, None)


def test_config_roundtrip(tmp_path):
    path = tmp_path / "cfg.json"
    assert wifi.load_config(path) == {}
    wifi.save_config({"host": "x.local", "token": "0" * 16}, path)
    assert wifi.load_config(path) == {"host": "x.local", "token": "0" * 16}


def test_config_ignores_garbage(tmp_path):
    path = tmp_path / "cfg.json"
    path.write_text("not json", encoding="utf-8")
    assert wifi.load_config(path) == {}
    path.write_text("[1, 2]", encoding="utf-8")
    assert wifi.load_config(path) == {}


def test_config_path_is_in_the_home_directory_not_the_repo():
    assert wifi.CONFIG_PATH.parent == Path.home()
    assert TOOLS_DIR.parent not in wifi.CONFIG_PATH.parents


def test_sha256_file(tmp_path):
    data = b"firmware" * 1000
    path = tmp_path / "f.bin"
    path.write_bytes(data)
    assert wifi.sha256_file(path, chunk=777) == hashlib.sha256(data).hexdigest()


def _make_project(tmp_path, built=None, cached=()):
    if built is not None:
        build_dir = tmp_path / ".pio" / "build" / ENV
        build_dir.mkdir(parents=True)
        (build_dir / "firmware.bin").write_bytes(built)
    if cached:
        cache = tmp_path / "firmware_cache" / ENV
        cache.mkdir(parents=True)
        for number, data in cached:
            (cache / ("build_%d.bin" % number)).write_bytes(data)
    return tmp_path


def test_default_firmware_prefers_the_latest_pio_build(tmp_path):
    project = _make_project(tmp_path, built=b"new", cached=[(5, b"old")])
    assert wifi.find_default_firmware(project) == project / ".pio" / "build" / ENV / "firmware.bin"


def test_default_firmware_takes_the_newest_build_of_any_derived_env(tmp_path):
    import os
    old = tmp_path / ".pio" / "build" / ENV
    new = tmp_path / ".pio" / "build" / (ENV + "-wifi-dev")
    other = tmp_path / ".pio" / "build" / "some-other-board"
    for directory in (old, new, other):
        directory.mkdir(parents=True)
        (directory / "firmware.bin").write_bytes(b"x")
    os.utime(old / "firmware.bin", (1000, 1000))
    os.utime(new / "firmware.bin", (2000, 2000))
    os.utime(other / "firmware.bin", (3000, 3000))  # newest, but not an env of this project's board
    assert wifi.find_default_firmware(tmp_path) == new / "firmware.bin"


def test_default_firmware_falls_back_to_highest_cache_number(tmp_path):
    project = _make_project(tmp_path, cached=[(9, b"a"), (100, b"b"), (20, b"c")])
    assert wifi.find_default_firmware(project).name == "build_100.bin"


def test_default_firmware_none_when_nothing_exists(tmp_path):
    assert wifi.find_default_firmware(tmp_path) is None


def test_find_cache_build_matches_identical_content(tmp_path):
    project = _make_project(tmp_path, built=b"image-bytes", cached=[(7, b"other"), (8, b"image-bytes")])
    assert wifi.find_cache_build(project / ".pio" / "build" / ENV / "firmware.bin", project) == 8


def test_find_cache_build_none_without_match(tmp_path):
    project = _make_project(tmp_path, built=b"image-bytes", cached=[(7, b"other-bytes!")])
    assert wifi.find_cache_build(project / ".pio" / "build" / ENV / "firmware.bin", project) is None


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX permission bits")
def test_config_file_is_private(tmp_path):
    import stat
    path = tmp_path / "cfg.json"
    wifi.save_config({"token": "0" * 16}, path)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    path.chmod(0o644)
    wifi.save_config({"token": "1" * 16}, path)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_format_network_error_hints_at_a_closed_connection():
    text = wifi.format_network_error(ConnectionResetError("reset by peer"))
    assert "closed the connection" in text and "/api/info" in text and text.isascii()
    assert wifi.format_network_error(BrokenPipeError("pipe")).endswith("/api/info")
    assert wifi.format_network_error(OSError("timed out")) == "timed out"


def test_build_begin_body():
    sha = "a" * 64
    body = json.loads(wifi.build_begin_body(2191088, sha))
    assert body == {"size": 2191088, "sha256": sha}
    body = json.loads(wifi.build_begin_body(2191088, sha, build=207, version="1.2.3"))
    assert body == {"size": 2191088, "sha256": sha, "build": 207, "version": "1.2.3"}


def test_parse_json_body():
    assert wifi.parse_json_body(b'{"a":1}') == {"a": 1}
    assert wifi.parse_json_body(b"[1]") == {}
    assert wifi.parse_json_body(b"nonsense") == {}
    assert wifi.parse_json_body(b"\xff\xfe") == {}


def test_describe_error():
    assert wifi.describe_error(409, {"error": "busy", "reason": "Update refused: grinder busy"}) == \
        "HTTP 409: Update refused: grinder busy"
    assert "retry in 60 s" in wifi.describe_error(429, {"error": "locked", "retry_after_s": 60})
    assert wifi.describe_error(500, {}) == "HTTP 500: no details"


def test_format_progress_is_ascii():
    text = wifi.format_progress(1024 * 1024, 2 * 1024 * 1024, 4.0)
    assert text.isascii()
    assert "50.0%" in text and "256 KB/s" in text
    assert "100.0%" in wifi.format_progress(0, 0, 0)


def test_reboot_finished():
    assert not wifi.reboot_finished(None, 207, 206, True)
    assert not wifi.reboot_finished({"build": 206}, 207, 206, True)
    assert wifi.reboot_finished({"build": 207}, 207, 206, False)
    # Unknown expected build: the device must have been seen down, or report a different build
    assert not wifi.reboot_finished({"build": 206}, None, 206, False)
    assert wifi.reboot_finished({"build": 206}, None, 206, True)
    assert wifi.reboot_finished({"build": 210}, None, 206, False)
    assert not wifi.reboot_finished({"version": "x"}, None, 206, True)


def test_grinder_py_registers_wifi_commands():
    import grinder
    tool = grinder.GrinderTool()
    assert tool.wifi_tool == TOOLS_DIR / "wifi" / "grinder-wifi.py"
    assert tool.wifi_tool.is_file()
    # Launched with the current interpreter, never a venv path
    from unittest import mock
    with mock.patch.object(grinder.subprocess, "run") as run:
        run.return_value = mock.MagicMock(returncode=0)
        args = mock.MagicMock(host="h.local", token="0" * 16, expect_build=5, no_wait=True, firmware="x.bin")
        assert tool.cmd_wifi_upload(args) == 0
    cmd = run.call_args[0][0]
    assert cmd[0] == sys.executable
    assert cmd[1].endswith("grinder-wifi.py")
    assert cmd[2] == "upload"
    assert cmd[-1] == "x.bin"
    assert "--expect-build" in cmd and "--no-wait" in cmd

"""R4 additions to the WiFi tool: the expected build number reaches /api/update/begin (the device keeps it as the
expected-version marker), also for images archived by a derived env, and a rolled-back update is explained."""

import importlib.util
import json
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def _load_tool():
    spec = importlib.util.spec_from_file_location("grinder_wifi_rollback_under_test", TOOLS_DIR / "wifi" / "grinder-wifi.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


wifi = _load_tool()
ENV = wifi.FIRMWARE_ENV


def _cache(project, env, number, data):
    directory = project / "firmware_cache" / env
    directory.mkdir(parents=True, exist_ok=True)
    (directory / ("build_%d.bin" % number)).write_bytes(data)


def test_find_cache_build_looks_into_derived_env_directories(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"wifi-dev-image")
    _cache(tmp_path, ENV, 11, b"main-image")
    _cache(tmp_path, ENV + "-wifi-dev", 12, b"wifi-dev-image")
    _cache(tmp_path, ENV + "-boot-fail-test", 13, b"boot-fail-image")
    assert wifi.find_cache_build(image, tmp_path) == 12
    image.write_bytes(b"boot-fail-image")
    assert wifi.find_cache_build(image, tmp_path) == 13


def test_find_cache_build_prefers_the_main_env_on_identical_content(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"same")
    _cache(tmp_path, ENV + "-wifi-dev", 5, b"same")
    _cache(tmp_path, ENV, 9, b"same")
    assert wifi.find_cache_build(image, tmp_path) == 9


def test_find_cache_build_none_without_cache_directory(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"x")
    assert wifi.find_cache_build(image, tmp_path) is None


def test_upload_sends_the_build_of_a_derived_env_image(monkeypatch, tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"y" * 4096)
    _cache(tmp_path, ENV + "-wifi-dev", 321, b"y" * 4096)
    bodies = []

    real_find = wifi.find_cache_build
    monkeypatch.setattr(wifi, "find_cache_build", lambda path: real_find(path, tmp_path))
    monkeypatch.setattr(wifi, "get_info", lambda host, timeout=10: {"name": "n", "version": "1", "build": 5,
                                                                    "busy": False, "confirm": False})
    monkeypatch.setattr(wifi, "request", lambda host, method, path, token=None, body=None, headers=None, timeout=0:
                        (bodies.append(body) or (409, {"error": "busy", "reason": "x"})))
    monkeypatch.setattr(wifi, "load_config", lambda path=wifi.CONFIG_PATH: {})
    args = wifi.build_parser().parse_args(["upload", "--host", "h.local", "--token", "0" * 16, str(image)])
    wifi.cmd_upload(args)
    assert len(bodies) == 1
    sent = json.loads(bodies[0])
    assert sent["build"] == 321
    assert "version" not in sent  # a version string is the same for many builds and would hide a rollback


def test_begin_body_without_known_build_has_no_build_key():
    assert "build" not in json.loads(wifi.build_begin_body(2191088, "a" * 64, None))


def test_rollback_hint_when_the_old_build_is_back():
    text = wifi.rollback_hint({"build": 5}, old_build=5, expected_build=6)
    assert text and "rolled back" in text and text.isascii()


def test_rollback_hint_silent_otherwise():
    assert wifi.rollback_hint(None, 5, 6) is None
    assert wifi.rollback_hint({"version": "x"}, 5, 6) is None
    assert wifi.rollback_hint({"build": 7}, 5, 6) is None      # some other build: not a plain rollback
    assert wifi.rollback_hint({"build": 5}, 5, 5) is None      # same build pushed again


def test_confirm_note_mentions_rollback_window():
    known = wifi.confirm_note(207)
    assert "30 s" in known and "rolls back" in known and "cannot be detected" not in known
    assert "cannot be detected" in wifi.confirm_note(None)

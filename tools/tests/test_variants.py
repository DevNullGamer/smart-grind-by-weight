"""Tests for the single-firmware-env handling (deprecated --variant) in tools/grinder.py and tools/ble/grinder-ble.py.

No BLE hardware, no firmware builds: everything uses temp dirs and mocks.
"""

import importlib.util
import sys
from pathlib import Path
from types import ModuleType
from unittest import mock

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import grinder  # noqa: E402
from firmware_variants import FIRMWARE_ENV, LEGACY_V2_ENV  # noqa: E402

ENV = "waveshare-esp32s3-touch-amoled-164"
V2_ENV = "waveshare-esp32s3-touch-amoled-164-v2"


# --------------------------------------------------------------------------- helpers

def run(coro):
    """Drive a coroutine that never truly suspends (all awaited deps are mocks).

    Avoids asyncio.run(), which needs a socketpair on Windows and is blocked by pytest-socket setups.
    """
    try:
        coro.send(None)
    except StopIteration as stop:
        return stop.value
    coro.close()
    raise AssertionError("coroutine suspended unexpectedly")


def _load_ble_module():
    """Load ble/grinder-ble.py (hyphenated) with its venv bootstrap and bleak import neutralised."""
    bleak = ModuleType("bleak")
    bleak.BleakClient = mock.MagicMock()
    bleak.BleakScanner = mock.MagicMock()
    bleak.BleakError = Exception
    backends = ModuleType("bleak.backends")
    char = ModuleType("bleak.backends.characteristic")
    char.BleakGATTCharacteristic = object
    dev = ModuleType("bleak.backends.device")
    dev.BLEDevice = object
    fakes = {
        "bleak": bleak,
        "bleak.backends": backends,
        "bleak.backends.characteristic": char,
        "bleak.backends.device": dev,
    }
    path = TOOLS_DIR / "ble" / "grinder-ble.py"
    spec = importlib.util.spec_from_file_location("grinder_ble_under_test", path)
    module = importlib.util.module_from_spec(spec)
    ok = mock.MagicMock(returncode=0, stdout="", stderr="")
    # ensure_venv_requirements() checks venv dir / requirements file and runs pip: fake all of it
    with mock.patch.dict(sys.modules, fakes), \
            mock.patch("pathlib.Path.exists", return_value=True), \
            mock.patch("subprocess.run", return_value=ok), \
            mock.patch("builtins.print"):
        spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def ble_module():
    return _load_ble_module()


@pytest.fixture
def project(tmp_path, ble_module):
    """Fake project root laid out as <root>/tools/ble/grinder-ble.py; the ble module's __file__ is pointed at it."""
    (tmp_path / "tools" / "ble").mkdir(parents=True)
    with mock.patch.object(ble_module, "__file__", str(tmp_path / "tools" / "ble" / "grinder-ble.py")):
        yield tmp_path


@pytest.fixture
def ble_tool(project, ble_module):
    return ble_module.GrinderBLETool()


def _touch(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


# --------------------------------------------------------------------------- constants

def test_env_constants():
    assert FIRMWARE_ENV == ENV
    assert LEGACY_V2_ENV == V2_ENV


def test_platformio_has_single_production_env():
    ini = (TOOLS_DIR.parent / "platformio.ini").read_text(encoding="utf-8")
    assert f"[env:{ENV}]" in ini
    # The old per-board V2 production env must stay gone; debug envs may end in -v2 (e.g. rmt-selftest-v2).
    assert f"[env:{V2_ENV}]" not in ini
    assert "BOARD_DEFAULT_VARIANT" not in ini


# --------------------------------------------------------------------------- grinder.py argparse

@pytest.fixture
def parser():
    return grinder.create_parser()


@pytest.mark.parametrize("cmd", ["build", "upload", "build-upload"])
def test_variant_default_is_none(parser, cmd):
    assert parser.parse_args([cmd]).variant is None


@pytest.mark.parametrize("cmd", ["build", "upload", "build-upload"])
@pytest.mark.parametrize("v", ["v1", "v2"])
def test_deprecated_variant_still_accepted(parser, cmd, v):
    assert parser.parse_args([cmd, "--variant", v]).variant == v


def test_upload_firmware_path(parser):
    assert parser.parse_args(["upload", "some.bin"]).firmware == "some.bin"


@pytest.mark.parametrize("cmd", ["build", "upload", "build-upload"])
@pytest.mark.parametrize("bad", ["v3", "V1", "", "1"])
def test_invalid_variant_rejected(parser, cmd, bad, capsys):
    with pytest.raises(SystemExit) as exc:
        parser.parse_args([cmd, "--variant", bad])
    assert exc.value.code == 2
    assert "invalid choice" in capsys.readouterr().err


# --------------------------------------------------------------------------- grinder.py behaviour

@pytest.fixture
def tool(tmp_path):
    t = grinder.GrinderTool()
    t.project_dir = tmp_path
    t.check_venv = mock.Mock(return_value=True)
    t.run_async_command = mock.AsyncMock(return_value=0)
    return t


def _ns(**kw):
    base = dict(firmware=None, variant=None, device="GrindByWeight", force_full=False)
    base.update(kw)
    return grinder.argparse.Namespace(**base)


def test_firmware_path(tool, tmp_path):
    assert tool.firmware_path() == tmp_path / ".pio" / "build" / ENV / "firmware.bin"


@pytest.mark.parametrize("variant", [None, "v1", "v2"])
def test_cmd_build_always_uses_single_env(tool, variant, capsys):
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=0))
    assert tool.cmd_build(_ns(variant=variant)) == 0
    assert tool.run_command.call_args[0][0][-2:] == ["-e", ENV]
    assert ("deprecated" in capsys.readouterr().out) == (variant is not None)


def test_upload_uses_single_env_firmware(tool, tmp_path):
    fw = _touch(tmp_path / ".pio/build" / ENV / "firmware.bin")
    assert run(tool.cmd_upload(_ns())) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(fw) in cmd and "--variant" not in cmd


def test_upload_ignores_other_envs(tool, tmp_path):
    _touch(tmp_path / ".pio/build" / f"{ENV}-debug" / "firmware.bin")
    assert run(tool.cmd_upload(_ns())) == 1
    tool.run_async_command.assert_not_called()


def test_upload_deprecated_variant_warns_and_is_not_forwarded(tool, tmp_path, capsys):
    _touch(tmp_path / ".pio/build" / ENV / "firmware.bin")
    assert run(tool.cmd_upload(_ns(variant="v2"))) == 0
    assert "--variant" not in tool.run_async_command.call_args[0][0]
    assert "deprecated" in capsys.readouterr().out


def test_upload_explicit_path(tool, tmp_path):
    p = _touch(tmp_path / "x.bin")
    assert run(tool.cmd_upload(_ns(firmware=str(p)))) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(p) in cmd and "--variant" not in cmd


def test_build_upload_uploads_built_firmware(tool, tmp_path):
    built = _touch(tmp_path / ".pio/build" / ENV / "firmware.bin")
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=0))
    assert run(tool.cmd_build_upload(_ns(variant="v2"))) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(built) in cmd and "--variant" not in cmd


def test_build_upload_build_failure_skips_upload(tool):
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=2))
    assert run(tool.cmd_build_upload(_ns())) == 2
    tool.run_async_command.assert_not_called()


# --------------------------------------------------------------------------- BLE: find_firmware_file

def test_ble_find_firmware(ble_tool, project):
    fw = _touch(project / ".pio/build" / ENV / "firmware.bin")
    assert ble_tool.find_firmware_file() == str(fw)


def test_ble_find_firmware_ignores_other_envs(ble_tool, project):
    _touch(project / ".pio/build" / V2_ENV / "firmware.bin")
    assert ble_tool.find_firmware_file() is None


# --------------------------------------------------------------------------- BLE: find_cached_firmware

def test_cache_env_dir_hit(ble_tool, project):
    c = _touch(project / "firmware_cache" / ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", ENV) == c


def test_cache_legacy_flat_fallback(ble_tool, project):
    legacy = _touch(project / "firmware_cache" / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", ENV) == legacy


def test_cache_legacy_v2_dir_fallback(ble_tool, project):
    legacy = _touch(project / "firmware_cache" / V2_ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", ENV) == legacy


def test_cache_identical_candidates_not_ambiguous(ble_tool, project):
    c = _touch(project / "firmware_cache" / ENV / "build_042.bin", b"same")
    _touch(project / "firmware_cache" / V2_ENV / "build_042.bin", b"same")
    assert ble_tool.find_cached_firmware("42", ENV) == c


def test_cache_differing_candidates_are_ambiguous(ble_tool, project):
    _touch(project / "firmware_cache" / "build_042.bin", b"v1 image")
    _touch(project / "firmware_cache" / V2_ENV / "build_042.bin", b"v2 image")
    assert ble_tool.find_cached_firmware("42", ENV) is None


def test_cache_other_env_has_no_legacy_fallback(ble_tool, project):
    _touch(project / "firmware_cache" / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", f"{ENV}-debug") is None


def test_cache_build_number_zero_padding_and_miss(ble_tool, project):
    c = _touch(project / "firmware_cache" / ENV / "build_007.bin")
    assert ble_tool.find_cached_firmware("7", ENV) == c
    assert ble_tool.find_cached_firmware("8", ENV) is None


# --------------------------------------------------------------------------- BLE: env inference

@pytest.mark.parametrize("path,expected", [
    (f"/proj/.pio/build/{ENV}/firmware.bin", ENV),
    (f"C:\\proj\\.pio\\build\\{ENV}\\firmware.bin", ENV),
    ("/proj/out/firmware.bin", None),
    ("firmware.bin", None),
    ("/proj/.pio/build/firmware.bin", None),
    ("/proj/.pio/other/env/firmware.bin", None),
])
def test_env_from_firmware_path(ble_module, path, expected):
    if "\\" in path and sys.platform != "win32":
        pytest.skip("Windows path semantics")
    assert ble_module.GrinderBLETool._env_from_firmware_path(path) == expected


# --------------------------------------------------------------------------- BLE: delta decision in upload_firmware

def _prepare_upload(ble_tool, device_build="42"):
    ble_tool.get_device_build_number = mock.AsyncMock(return_value=device_build)
    ble_tool.find_cached_firmware = mock.Mock(return_value=None)
    ble_tool.generate_delta_patch = mock.Mock(return_value=b"patch")
    ble_tool.send_ota_update = mock.AsyncMock(return_value=True)
    ble_tool._get_firmware_build_number = mock.Mock(return_value="43")
    return ble_tool


def _fw(project, env=ENV):
    return _touch(project / ".pio/build" / env / "firmware.bin", b"\0" * 1000)


def test_upload_env_inferred_from_path_used_for_cache_lookup(ble_tool, project):
    _prepare_upload(ble_tool)
    run(ble_tool.upload_firmware(str(_fw(project, f"{ENV}-debug"))))
    ble_tool.find_cached_firmware.assert_called_once_with("42", f"{ENV}-debug")


def test_upload_unknown_path_uses_main_env_cache(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _touch(project / "custom" / "firmware.bin", b"\0" * 1000)
    run(ble_tool.upload_firmware(str(fw)))
    ble_tool.find_cached_firmware.assert_called_once_with("42", ENV)


def test_upload_no_cache_forces_full(ble_tool, project):
    _prepare_upload(ble_tool)
    run(ble_tool.upload_firmware(str(_fw(project))))
    assert ble_tool.update_method == "full"
    assert ble_tool.full_reason == "no cached firmware"


def test_upload_with_cache_uses_delta(ble_tool, project):
    _prepare_upload(ble_tool)
    base = _touch(project / "firmware_cache" / ENV / "build_042.bin")
    ble_tool.find_cached_firmware = mock.Mock(return_value=base)
    ble_tool.generate_delta_patch = mock.Mock(return_value=b"tiny")  # 4 bytes < 80% of 1000
    run(ble_tool.upload_firmware(str(_fw(project))))
    assert ble_tool.update_method == "delta"
    assert ble_tool.generate_delta_patch.call_args[0][0] == base


def test_upload_force_full_skips_cache(ble_tool, project):
    _prepare_upload(ble_tool)
    run(ble_tool.upload_firmware(str(_fw(project)), force_full=True))
    ble_tool.find_cached_firmware.assert_not_called()
    assert ble_tool.full_reason == "forced full update"

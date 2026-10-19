"""Tests for hardware-variant (v1/v2) handling in tools/grinder.py and tools/ble/grinder-ble.py.

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
from firmware_variants import DEFAULT_VARIANT, FIRMWARE_ENVS  # noqa: E402

V1_ENV = "waveshare-esp32s3-touch-amoled-164"
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


# --------------------------------------------------------------------------- mapping

def test_mapping_and_default():
    assert FIRMWARE_ENVS == {"v1": V1_ENV, "v2": V2_ENV}
    assert DEFAULT_VARIANT == "v1"


# --------------------------------------------------------------------------- grinder.py argparse

@pytest.fixture
def parser():
    return grinder.create_parser()


def test_build_default_variant(parser):
    assert parser.parse_args(["build"]).variant == "v1"


@pytest.mark.parametrize("v", ["v1", "v2"])
def test_build_variant(parser, v):
    assert parser.parse_args(["build", "--variant", v]).variant == v


def test_build_upload_default_and_explicit(parser):
    assert parser.parse_args(["build-upload"]).variant == "v1"
    ns = parser.parse_args(["build-upload", "--variant", "v2", "--force-full"])
    assert ns.variant == "v2" and ns.force_full is True


def test_upload_default_variant_is_none(parser):
    ns = parser.parse_args(["upload"])
    assert ns.variant is None and ns.firmware is None


def test_upload_variant_and_path(parser):
    assert parser.parse_args(["upload", "--variant", "v2"]).variant == "v2"
    ns = parser.parse_args(["upload", "some.bin", "--variant", "v1"])
    assert ns.firmware == "some.bin" and ns.variant == "v1"


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


@pytest.mark.parametrize("variant,env", [("v1", V1_ENV), ("v2", V2_ENV)])
def test_firmware_path_for_variant(tool, tmp_path, variant, env):
    assert tool.firmware_path_for_variant(variant) == tmp_path / ".pio" / "build" / env / "firmware.bin"


@pytest.mark.parametrize("variant,env", [("v1", V1_ENV), ("v2", V2_ENV), (None, V1_ENV)])
def test_cmd_build_uses_env(tool, variant, env):
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=0))
    assert tool.cmd_build(_ns(variant=variant)) == 0
    cmd = tool.run_command.call_args[0][0]
    assert cmd[-2:] == ["-e", env]


def test_upload_variant_uses_that_envs_firmware(tool, tmp_path):
    v1 = _touch(tmp_path / ".pio/build" / V1_ENV / "firmware.bin")
    v2 = _touch(tmp_path / ".pio/build" / V2_ENV / "firmware.bin")
    assert run(tool.cmd_upload(_ns(variant="v1"))) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(v1) in cmd and str(v2) not in cmd
    assert cmd[cmd.index("--variant") + 1] == "v1"

    run(tool.cmd_upload(_ns(variant="v2")))
    cmd = tool.run_async_command.call_args[0][0]
    assert str(v2) in cmd and str(v1) not in cmd
    assert cmd[cmd.index("--variant") + 1] == "v2"


def test_upload_variant_missing_firmware_errors_no_fallback(tool, tmp_path):
    # Only the other variant exists: must NOT fall back to it
    _touch(tmp_path / ".pio/build" / V1_ENV / "firmware.bin")
    assert run(tool.cmd_upload(_ns(variant="v2"))) == 1
    tool.run_async_command.assert_not_called()


def test_upload_without_variant_picks_newest_and_warns(tool, tmp_path, capsys):
    import os
    v1 = _touch(tmp_path / ".pio/build" / V1_ENV / "firmware.bin")
    v2 = _touch(tmp_path / ".pio/build" / V2_ENV / "firmware.bin")
    os.utime(v1, (1000, 1000))
    os.utime(v2, (2000, 2000))
    assert run(tool.cmd_upload(_ns())) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(v2) in cmd
    assert "--variant" not in cmd
    assert "not interchangeable" in capsys.readouterr().out


def test_upload_without_variant_no_firmware_errors(tool):
    assert run(tool.cmd_upload(_ns())) == 1


def test_upload_explicit_path_no_variant_no_warning(tool, tmp_path, capsys):
    p = _touch(tmp_path / "x.bin")
    assert run(tool.cmd_upload(_ns(firmware=str(p)))) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(p) in cmd and "--variant" not in cmd
    assert "not interchangeable" not in capsys.readouterr().out


@pytest.mark.parametrize("variant,env,other", [("v1", V1_ENV, V2_ENV), ("v2", V2_ENV, V1_ENV), (None, V1_ENV, V2_ENV)])
def test_build_upload_uploads_exactly_built_env(tool, tmp_path, variant, env, other):
    built = _touch(tmp_path / ".pio/build" / env / "firmware.bin")
    other_fw = _touch(tmp_path / ".pio/build" / other / "firmware.bin")
    import os
    os.utime(built, (1000, 1000))
    os.utime(other_fw, (9000, 9000))  # other env is newer: must still not be chosen
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=0))
    assert run(tool.cmd_build_upload(_ns(variant=variant))) == 0
    cmd = tool.run_async_command.call_args[0][0]
    assert str(built) in cmd and str(other_fw) not in cmd
    assert "--variant" in cmd


def test_build_upload_build_failure_skips_upload(tool):
    tool.run_command = mock.Mock(return_value=mock.Mock(returncode=2))
    assert run(tool.cmd_build_upload(_ns(variant="v2"))) == 2
    tool.run_async_command.assert_not_called()


# --------------------------------------------------------------------------- BLE: find_firmware_file

def test_ble_find_firmware_default_is_v1(ble_tool, project):
    v1 = _touch(project / ".pio/build" / V1_ENV / "firmware.bin")
    _touch(project / ".pio/build" / V2_ENV / "firmware.bin")
    assert ble_tool.find_firmware_file() == str(v1)
    assert ble_tool.find_firmware_file("v1") == str(v1)


def test_ble_find_firmware_v2(ble_tool, project):
    _touch(project / ".pio/build" / V1_ENV / "firmware.bin")
    v2 = _touch(project / ".pio/build" / V2_ENV / "firmware.bin")
    assert ble_tool.find_firmware_file("v2") == str(v2)


@pytest.mark.parametrize("have,want", [(V1_ENV, "v2"), (V2_ENV, "v1"), (V2_ENV, None)])
def test_ble_find_firmware_never_crosses_variants(ble_tool, project, have, want):
    _touch(project / ".pio/build" / have / "firmware.bin")
    assert ble_tool.find_firmware_file(want) is None


# --------------------------------------------------------------------------- BLE: find_cached_firmware

def test_cache_v1_env_dir_hit(ble_tool, project):
    c = _touch(project / "firmware_cache" / V1_ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V1_ENV) == c


def test_cache_v1_legacy_flat_fallback(ble_tool, project):
    legacy = _touch(project / "firmware_cache" / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V1_ENV) == legacy


def test_cache_v1_prefers_env_dir_over_legacy(ble_tool, project):
    _touch(project / "firmware_cache" / "build_042.bin")
    c = _touch(project / "firmware_cache" / V1_ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V1_ENV) == c


def test_cache_v2_no_legacy_fallback(ble_tool, project):
    _touch(project / "firmware_cache" / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V2_ENV) is None


def test_cache_v2_env_dir_hit(ble_tool, project):
    c = _touch(project / "firmware_cache" / V2_ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V2_ENV) == c


def test_cache_no_cross_variant_hits(ble_tool, project):
    _touch(project / "firmware_cache" / V1_ENV / "build_042.bin")
    assert ble_tool.find_cached_firmware("42", V2_ENV) is None
    other = project / "firmware_cache" / V2_ENV
    _touch(other / "build_007.bin")
    assert ble_tool.find_cached_firmware("7", V1_ENV) is None


def test_cache_build_number_zero_padding_and_miss(ble_tool, project):
    c = _touch(project / "firmware_cache" / V2_ENV / "build_007.bin")
    assert ble_tool.find_cached_firmware("7", V2_ENV) == c
    assert ble_tool.find_cached_firmware("8", V2_ENV) is None


# --------------------------------------------------------------------------- BLE: env inference

@pytest.mark.parametrize("path,expected", [
    (f"/proj/.pio/build/{V1_ENV}/firmware.bin", V1_ENV),
    (f"/proj/.pio/build/{V2_ENV}/firmware.bin", V2_ENV),
    (f"C:\\proj\\.pio\\build\\{V2_ENV}\\firmware.bin", V2_ENV),
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


def _fw(project, env=V2_ENV):
    return _touch(project / ".pio/build" / env / "firmware.bin", b"\0" * 1000)


def test_upload_env_inferred_from_path_used_for_cache_lookup(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _fw(project, V2_ENV)
    run(ble_tool.upload_firmware(str(fw)))
    ble_tool.find_cached_firmware.assert_called_once_with("42", V2_ENV)


def test_upload_variant_matching_path_looks_up_cache(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _fw(project, V2_ENV)
    run(ble_tool.upload_firmware(str(fw), variant="v2"))
    ble_tool.find_cached_firmware.assert_called_once_with("42", V2_ENV)


@pytest.mark.parametrize("path_env,variant", [(V1_ENV, "v2"), (V2_ENV, "v1")])
def test_upload_mismatch_aborts(ble_tool, project, path_env, variant, capsys):
    _prepare_upload(ble_tool)
    ble_tool.find_cached_firmware = mock.Mock(return_value=project / "would_be_delta_base.bin")
    fw = _fw(project, path_env)
    assert run(ble_tool.upload_firmware(str(fw), variant=variant)) is False
    ble_tool.find_cached_firmware.assert_not_called()
    ble_tool.send_ota_update.assert_not_called()
    ble_tool.generate_delta_patch.assert_not_called()
    assert "[ERROR]" in capsys.readouterr().out


def test_upload_unknown_env_forces_full(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _touch(project / "custom" / "firmware.bin", b"\0" * 1000)
    run(ble_tool.upload_firmware(str(fw)))
    ble_tool.find_cached_firmware.assert_not_called()
    assert ble_tool.update_method == "full"
    assert ble_tool.full_reason == "unknown hardware variant (use --variant)"


def test_upload_variant_given_for_unknown_path_uses_variant_env(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _touch(project / "custom" / "firmware.bin", b"\0" * 1000)
    run(ble_tool.upload_firmware(str(fw), variant="v2"))
    ble_tool.find_cached_firmware.assert_called_once_with("42", V2_ENV)


def test_upload_matching_variant_with_cache_uses_delta(ble_tool, project):
    _prepare_upload(ble_tool)
    base = _touch(project / "firmware_cache" / V2_ENV / "build_042.bin")
    ble_tool.find_cached_firmware = mock.Mock(return_value=base)
    ble_tool.generate_delta_patch = mock.Mock(return_value=b"tiny")  # 4 bytes < 80% of 1000
    fw = _fw(project, V2_ENV)
    run(ble_tool.upload_firmware(str(fw), variant="v2"))
    assert ble_tool.update_method == "delta"
    ble_tool.generate_delta_patch.assert_called_once()
    assert ble_tool.generate_delta_patch.call_args[0][0] == base


def test_upload_force_full_skips_cache(ble_tool, project):
    _prepare_upload(ble_tool)
    fw = _fw(project, V2_ENV)
    run(ble_tool.upload_firmware(str(fw), force_full=True, variant="v2"))
    ble_tool.find_cached_firmware.assert_not_called()
    assert ble_tool.full_reason == "forced full update"

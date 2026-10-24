"""Tests for steps 6.1/6.2: netstat parser, requirements stamp, release.py, grinder-ble exit codes.

No BLE, no PlatformIO, no network. Git work happens in temp repos.
"""

import importlib.util
import runpy
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import grinder  # noqa: E402
import release  # noqa: E402
from test_variants import _load_ble_module, run  # noqa: E402


def _load_webflasher():
    spec = importlib.util.spec_from_file_location("start_webflasher_under_test", TOOLS_DIR / "start-webflasher.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


webflasher = _load_webflasher()
parse = webflasher.parse_netstat_listening_pid

NETSTAT = """
Active Connections

  Proto  Local Address          Foreign Address        State           PID
  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       900
  TCP    0.0.0.0:13             0.0.0.0:0              LISTENING       13013
  TCP    0.0.0.0:8000           0.0.0.0:0              LISTENING       4242
  TCP    [::]:18000             [::]:0                 LISTENING       1800
  TCP    [::1]:9000             [::]:0                 LISTENING       9001
  TCP    127.0.0.1:8000         127.0.0.1:55000        ESTABLISHED     7777
  TCP    127.0.0.1:55001        127.0.0.1:7000         ESTABLISHED     8888
  TCP    0.0.0.0:7001           0.0.0.0:0              ABHOEREN        7001
"""


# --------------------------------------------------------------------------- netstat parser

def test_parser_ipv4_listening():
    assert parse(NETSTAT, 8000) == 4242


def test_parser_ipv6_listening():
    assert parse(NETSTAT, 18000) == 1800
    assert parse(NETSTAT, 9000) == 9001


def test_parser_port_13_not_135():
    assert parse(NETSTAT, 13) == 13013
    only135 = "  TCP    0.0.0.0:135   0.0.0.0:0   LISTENING   900\n"
    assert parse(only135, 13) is None


def test_parser_port_8000_not_18000():
    only18000 = "  TCP    [::]:18000   [::]:0   LISTENING   1800\n"
    assert parse(only18000, 8000) is None
    assert parse(NETSTAT, 18000) == 1800


def test_parser_established_ignored():
    est = "  TCP    127.0.0.1:8000   127.0.0.1:55000   ESTABLISHED   7777\n"
    assert parse(est, 8000) is None
    assert parse(est, 55000) is None  # foreign port is not a local listener


def test_parser_localized_state_word():
    assert parse(NETSTAT, 7001) == 7001
    de = "  TCP    0.0.0.0:8000   0.0.0.0:0   ABHOEREN   321\n"
    assert parse(de, 8000) == 321


def test_parser_empty_and_garbage():
    assert parse("", 8000) is None
    assert parse("garbage\nTCP 1 2\n  UDP *:8000 *:* 55\n", 8000) is None


def test_parser_non_numeric_pid_ignored():
    assert parse("  TCP    0.0.0.0:8000   0.0.0.0:0   LISTENING   abc\n", 8000) is None


# --------------------------------------------------------------------------- requirements stamp

@pytest.fixture
def reqs(tmp_path):
    r = tmp_path / "requirements.txt"
    r.write_bytes(b"bleak\ndetools\n")
    return r, tmp_path / ".requirements.sha256"


def test_no_stamp_not_synced(reqs):
    r, stamp = reqs
    assert grinder.venv_is_synced(r, stamp) is False


def test_stamp_written_then_synced(reqs):
    r, stamp = reqs
    grinder.write_requirements_stamp(r, stamp)
    assert stamp.exists()
    assert grinder.venv_is_synced(r, stamp) is True


def test_crlf_vs_lf_same_hash(tmp_path):
    a = tmp_path / "a.txt"
    b = tmp_path / "b.txt"
    a.write_bytes(b"bleak\ndetools\n")
    b.write_bytes(b"bleak\r\ndetools\r\n")
    assert grinder.compute_requirements_hash(a) == grinder.compute_requirements_hash(b)


def test_crlf_checkout_still_synced(reqs):
    r, stamp = reqs
    grinder.write_requirements_stamp(r, stamp)
    r.write_bytes(b"bleak\r\ndetools\r\n")
    assert grinder.venv_is_synced(r, stamp) is True


def test_changed_requirements_not_synced(reqs):
    r, stamp = reqs
    grinder.write_requirements_stamp(r, stamp)
    r.write_bytes(b"bleak\ndetools\nnewpkg\n")
    assert grinder.venv_is_synced(r, stamp) is False


def test_missing_requirements_counts_as_synced(tmp_path):
    assert grinder.venv_is_synced(tmp_path / "none.txt", tmp_path / "stamp") is True


def _tool(tmp_path):
    tool = grinder.GrinderTool()
    tool.venv_python = tmp_path / "python.exe"
    tool.venv_python.write_text("x")
    tool.requirements_txt = tmp_path / "requirements.txt"
    tool.requirements_txt.write_text("bleak\n")
    tool.requirements_stamp = tmp_path / ".requirements.sha256"
    tool.safe_print = mock.MagicMock()
    return tool


def test_check_venv_synced_does_not_install(tmp_path):
    tool = _tool(tmp_path)
    grinder.write_requirements_stamp(tool.requirements_txt, tool.requirements_stamp)
    with mock.patch.object(tool, "install_dependencies") as inst:
        assert tool.check_venv() is True
    inst.assert_not_called()


def test_check_venv_resync_success(tmp_path):
    tool = _tool(tmp_path)
    with mock.patch.object(tool, "install_dependencies", return_value=True) as inst:
        assert tool.check_venv() is True
    inst.assert_called_once()


def test_check_venv_resync_failure_warns_and_continues(tmp_path):
    tool = _tool(tmp_path)
    with mock.patch.object(tool, "install_dependencies", return_value=False), \
            mock.patch.object(tool, "print_warning") as warn:
        assert tool.check_venv() is True
    warn.assert_called_once()


def test_check_venv_missing_venv_installs(tmp_path):
    tool = _tool(tmp_path)
    tool.venv_python = tmp_path / "missing.exe"
    with mock.patch.object(tool, "install_dependencies", return_value=False) as inst:
        assert tool.check_venv() is False
    inst.assert_called_once()


# --------------------------------------------------------------------------- release.py

def test_latest_tag_from_describe():
    f = release.latest_tag_from_describe
    assert f(0, "v1.2.3\n") == "v1.2.3"
    assert f(128, "") is None
    assert f(0, "  \n") is None
    assert f(128, "v1.0.0") is None


def _git(cwd, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false",
                    "-c", "tag.gpgsign=false", *args], cwd=cwd, check=True, capture_output=True)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    _git(tmp_path, "init", "-q")
    (tmp_path / "f").write_text("1")
    _git(tmp_path, "add", "f")
    _git(tmp_path, "commit", "-q", "-m", "c1")
    monkeypatch.chdir(tmp_path)
    return tmp_path


def test_current_version_no_tags(repo, capsys):
    assert release.get_current_version() == "v0.0.0"
    out = capsys.readouterr().out
    assert "No tags found" in out
    assert "fatal" not in out.lower()


def test_current_version_with_tag(repo):
    _git(repo, "tag", "-a", "v1.2.3", "-m", "r")
    assert release.get_current_version() == "v1.2.3"


def test_current_version_latest_of_two(repo):
    _git(repo, "tag", "-a", "v1.0.0", "-m", "r")
    (repo / "f").write_text("2")
    _git(repo, "commit", "-qam", "c2")
    _git(repo, "tag", "-a", "v1.1.0", "-m", "r")
    assert release.get_current_version() == "v1.1.0"


def test_current_version_unreachable_tag_warns(repo, capsys):
    _git(repo, "checkout", "-q", "-b", "other")
    (repo / "f").write_text("2")
    _git(repo, "commit", "-qam", "c2")
    _git(repo, "tag", "-a", "v9.0.0", "-m", "r")
    _git(repo, "checkout", "-q", "--detach", "HEAD~1")
    assert release.get_current_version() == "v0.0.0"
    assert "WARNING" in capsys.readouterr().out


def test_release_source_is_ascii():
    text = (TOOLS_DIR / "release.py").read_text(encoding="utf-8")
    bad = sorted({c for c in text if ord(c) > 127})
    assert bad == [], f"non-ASCII characters in release.py: {bad}"


def _run_release_main(argv, monkeypatch, inputs=None):
    monkeypatch.setattr(sys, "argv", ["release.py", *argv])
    if inputs is not None:
        it = iter(inputs)

        def fake_input(*a, **k):
            v = next(it)
            if isinstance(v, BaseException):
                raise v
            return v
        monkeypatch.setattr("builtins.input", fake_input)
    with pytest.raises(SystemExit) as exc:
        runpy.run_path(str(TOOLS_DIR / "release.py"), run_name="__main__")
    return exc.value.code


def test_release_main_help_exit_0(monkeypatch):
    assert _run_release_main(["--help"], monkeypatch) == 0


def test_release_main_unknown_command_exit_1(monkeypatch):
    assert _run_release_main(["bogus"], monkeypatch) == 1


def test_release_main_keyboard_interrupt_exit_1(repo, monkeypatch):
    (repo / "dirty").write_text("x")  # dirty tree -> first prompt
    assert _run_release_main([], monkeypatch, inputs=[KeyboardInterrupt()]) == 1


def test_release_main_failure_exit_1(repo, monkeypatch):
    # clean tree, no commits since tag -> "continue anyway?" then invalid choice -> create_release False
    assert _run_release_main([], monkeypatch, inputs=["y", "x"]) == 1


def test_release_main_cancel_exit_1(repo, monkeypatch):
    # no tags -> recent commits shown, then invalid choice
    assert _run_release_main([], monkeypatch, inputs=["x"]) == 1


# --------------------------------------------------------------------------- grinder-ble.py

@pytest.fixture(scope="module")
def ble():
    return _load_ble_module()


class FakeTool:
    def __init__(self, **kw):
        self.connected = False
        self.cfg = {"connect": True, "upload": True, "export": True, "info": {"a": 1}, **kw}
        self.safe_print = mock.MagicMock()

    async def connect_to_device(self, name):
        self.connected = self.cfg["connect"]
        return self.cfg["connect"]

    async def disconnect(self):
        self.connected = False

    def find_firmware_file(self):
        return "fw.bin"

    async def upload_firmware(self, path, force_full=False):
        if isinstance(self.cfg["upload"], BaseException):
            raise self.cfg["upload"]
        return self.cfg["upload"]

    async def export_data(self, db=None):
        return self.cfg["export"]

    async def get_system_info(self):
        return self.cfg["info"]

    def print_system_info(self, info):
        pass


def _main(ble, monkeypatch, argv, **cfg):
    monkeypatch.setattr(sys, "argv", ["grinder-ble.py", *argv])
    with mock.patch.object(ble, "GrinderBLETool", lambda: FakeTool(**cfg)):
        return run(ble.main())


def test_ble_main_upload_success(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["upload"], upload=True) == 0


def test_ble_main_upload_failure(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["upload"], upload=False) == 1


def test_ble_main_export_success(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["export"], export=True) == 0


def test_ble_main_export_failure(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["export"], export=False) == 1


def test_ble_main_info_failure(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["info"], info=None) == 1


def test_ble_main_connect_failure(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["export"], connect=False) == 1


def test_ble_main_keyboard_interrupt(ble, monkeypatch):
    assert _main(ble, monkeypatch, ["upload"], upload=KeyboardInterrupt()) == 1


def test_ensure_venv_requirements_uses_sys_executable(ble, tmp_path):
    (tmp_path / "tools" / "venv").mkdir(parents=True)
    (tmp_path / "tools" / "requirements.txt").write_text("bleak\n")
    ok = mock.MagicMock(returncode=0, stdout="", stderr="")
    with mock.patch.object(ble, "__file__", str(tmp_path / "tools" / "ble" / "grinder-ble.py")), \
            mock.patch("subprocess.run", return_value=ok) as sr, mock.patch("builtins.print"):
        assert ble.ensure_venv_requirements() is True
    cmd = sr.call_args[0][0]
    assert cmd[:3] == [sys.executable, "-m", "pip"]
    assert "install" in cmd and "-r" in cmd


def test_generate_delta_patch_command(ble, tmp_path):
    old = tmp_path / "old.bin"
    old.write_bytes(b"old")
    tool = ble.GrinderBLETool()
    seen = {}

    def fake_run(cmd, **kw):
        seen["cmd"] = cmd
        Path(cmd[-1]).write_bytes(b"PATCH")
        return mock.MagicMock(returncode=0, stdout="", stderr="")

    with mock.patch("subprocess.run", side_effect=fake_run):
        assert tool.generate_delta_patch(old, b"new") == b"PATCH"
    cmd = seen["cmd"]
    assert cmd[:3] == [sys.executable, "-m", "detools"]
    assert cmd[3] == "create_patch"
    assert str(old) in cmd


def test_generate_delta_patch_failure_returns_none(ble, tmp_path):
    old = tmp_path / "old.bin"
    old.write_bytes(b"old")
    tool = ble.GrinderBLETool()
    bad = mock.MagicMock(returncode=1, stdout="", stderr="boom")
    with mock.patch("subprocess.run", return_value=bad), mock.patch("builtins.print"):
        assert tool.generate_delta_patch(old, b"new") is None

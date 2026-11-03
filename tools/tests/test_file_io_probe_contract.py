"""Contract guard for the periodic filesystem health check (chg017).

Pure text checks, no build and no hardware. The 30 s check must stay read-only: the old write/delete probe
("/test_access") wore the flash for no benefit.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TASK = (ROOT / "src/tasks/file_io_task.cpp").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(rf"FileIOTask::{name}\(\)\s*\{{.*?\n\}}\n", TASK, re.S)
    assert match, f"FileIOTask::{name} not found"
    return "\n".join(line.split("//")[0] for line in match.group(0).splitlines())


def test_probe_is_read_only():
    body = function_body("validate_filesystem_access")
    assert not re.search(r'open\([^)]*,\s*"[wa]\+?b?"', body), "probe opens a file for writing"
    for token in ("FILE_WRITE", "FILE_APPEND", ".remove(", ".mkdir(", ".rename(", ".println(", ".print(", ".write(",
                  "test_access"):
        assert token not in body, f"probe uses {token}"


def test_probe_opens_the_root_directory():
    body = function_body("validate_filesystem_access")
    assert 'LittleFS.open("/")' in body
    assert "isDirectory()" in body
    assert ".close()" in body


def test_negative_check_catches_a_write_probe():
    old = ('bool FileIOTask::validate_filesystem_access() {\n'
           '    File f = LittleFS.open("/test_access", "w");\n'
           '    f.println("test");\n    LittleFS.remove("/test_access");\n    return true;\n}\n')
    assert re.search(r'open\([^)]*,\s*"[wa]\+?b?"', old)
    assert ".remove(" in old


def test_health_check_cadence_is_30_seconds():
    assert re.search(r"cycle_start_time - last_filesystem_check_time >= 30000\)", TASK)


def test_init_removes_a_stray_probe_file_only_when_mounted():
    body = function_body("init")
    guard = body.index("if (filesystem_available)")
    exists = body.index('LittleFS.exists("/test_access")')
    remove = body.index('LittleFS.remove("/test_access")')
    assert guard < exists < remove
    assert body.count('"/test_access"') == 2

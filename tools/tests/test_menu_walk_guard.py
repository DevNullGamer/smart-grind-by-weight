"""Guards for the debug-only menu walk harness (DEBUG_MENU_WALK, chg006 item 23).

The flag must only ever be set by the ...-menu-walk env, so the main/release image can never contain the harness.
Pure text checks on platformio.ini, the sources and the release workflow: no build, no hardware.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MAIN_ENV = "waveshare-esp32s3-touch-amoled-164"
WALK_ENV = f"{MAIN_ENV}-menu-walk"
FLAG = "DEBUG_MENU_WALK"


def sections_with_flag(ini_text: str) -> list[str]:
    """Names of the [section]s of a platformio.ini text that mention the DEBUG_MENU_WALK flag."""
    found = []
    current = None
    for line in ini_text.splitlines():
        header = re.match(r"\s*\[([^\]]+)\]", line)
        if header:
            current = header.group(1)
            continue
        if current and re.search(rf"-D\s*{FLAG}\b", line):
            found.append(current)
    return found


def test_flag_is_only_in_the_menu_walk_env():
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    assert sections_with_flag(ini) == [f"env:{WALK_ENV}"]


def test_guard_detects_the_flag_in_the_main_env():
    # Negative check on a temp copy: adding the flag to the main env section must be caught
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    broken = ini.replace(f"[env:{MAIN_ENV}]\n", f"[env:{MAIN_ENV}]\nbuild_flags = -D{FLAG}=1\n", 1)
    assert broken != ini
    assert f"env:{MAIN_ENV}" in sections_with_flag(broken)


def test_walk_env_extends_the_main_env_and_sets_only_the_flag():
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    body = ini.split(f"[env:{WALK_ENV}]", 1)[1].split("\n[", 1)[0]
    assert f"extends = env:{MAIN_ENV}" in body
    assert f"-D{FLAG}=1" in body


def test_flag_is_defined_off_by_default_with_an_error_guard():
    debug_h = (ROOT / "src" / "config" / "debug.h").read_text(encoding="utf-8")
    assert re.search(rf"#ifndef {FLAG}\s+#define {FLAG} 0", debug_h)
    assert re.search(rf"#if {FLAG} &&.*\s+#error", debug_h)


def test_harness_sources_are_compiled_out_unless_the_flag_is_set():
    cpp = (ROOT / "src" / "ui" / "menu_walk_debug.cpp").read_text(encoding="utf-8")
    assert f"#if {FLAG}" in cpp
    assert cpp.rstrip().endswith(f"#endif  // {FLAG}")
    ui_manager = (ROOT / "src" / "ui" / "ui_manager.cpp").read_text(encoding="utf-8")
    assert re.search(rf"#if {FLAG}\s+menu_walk_start\(", ui_manager)


def test_flag_is_not_used_outside_the_harness_files():
    allowed = {
        "src/config/debug.h",
        "src/ui/menu_walk_debug.h",
        "src/ui/menu_walk_debug.cpp",
        "src/ui/ui_manager.cpp",
    }
    offenders = []
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".h", ".cpp", ".hpp", ".c"}:
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel not in allowed and FLAG in path.read_text(encoding="utf-8", errors="ignore"):
            offenders.append(rel)
    assert offenders == []


def test_release_workflow_does_not_build_the_walk_env():
    workflows = ROOT / ".github" / "workflows"
    if not workflows.exists():
        return
    for wf in workflows.glob("*.yml"):
        text = wf.read_text(encoding="utf-8", errors="ignore")
        assert "menu-walk" not in text, wf.name
        assert FLAG not in text, wf.name

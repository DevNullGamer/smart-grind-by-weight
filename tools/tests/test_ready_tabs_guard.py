"""READY tab layout guard (chg020 step 4): the menu tab index is a named value, never a literal.

Pure text checks of the UI sources. The PROFILE page exists only once GaggiMate has set a dose, so the MENU tab sits at
index 3 or 4 (src/ui/ready_tabs.h); a literal 3 would silently break one of the two layouts.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
UI = ROOT / "src" / "ui"

LITERAL_TAB_USES = [
    re.compile(r"set_current_tab\(\s*\d"),
    re.compile(r"\bcurrent_tab\s*(?:==|!=|<=|>=|<|>)\s*[1-9]"),  # 0 is the first profile page, not a menu index
    re.compile(r"\b(?:tab|tab_id)\s*(?:==|!=|<=|>=|<|>)\s*[1-9]"),
    re.compile(r"\b(?:set_active_tab|lv_tabview_set_active)\([^;]*,\s*[1-9]\d*\s*[,)]"),
]


def offenders(files):
    found = []
    for rel, text in files:
        for number, line in enumerate(text.splitlines(), 1):
            code = line.split("//", 1)[0]
            if any(p.search(code) for p in LITERAL_TAB_USES):
                found.append(f"{rel}:{number}")
    return found


def ui_sources():
    for path in sorted(UI.rglob("*")):
        if path.suffix in {".h", ".cpp"}:
            yield path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8", errors="ignore")


def test_no_literal_menu_tab_index_in_the_ui_sources():
    assert offenders(ui_sources()) == []


def test_guard_detects_planted_literals():
    planted = [("a.cpp", "ui_manager_->set_current_tab(3);"), ("b.cpp", "if (current_tab == 3) {"),
               ("c.cpp", "if (ui_manager_->profile_controller && tab < 3) {"),
               ("d.cpp", "if (current_tab >= ui_manager_->menu_tab_index()) {"),
               ("e.cpp", "lv_tabview_set_active(tabview, 4, LV_ANIM_OFF);")]
    assert offenders(planted) == ["a.cpp:1", "b.cpp:1", "c.cpp:1", "e.cpp:1"]


def test_menu_tab_constants_follow_the_profile_count():
    header = (UI / "ready_tabs.h").read_text(encoding="utf-8")
    assert re.search(r"READY_MENU_TAB_INDEX\s*=\s*USER_PROFILE_COUNT;", header)
    assert re.search(r"READY_MENU_TAB_INDEX_NO_DOSE\s*=\s*USER_BASE_PROFILE_COUNT;", header)


def test_the_menu_tab_is_asked_from_the_screen():
    manager = (UI / "ui_manager.h").read_text(encoding="utf-8")
    assert "int menu_tab_index() const { return ready_screen.menu_tab_index(); }" in manager
    controllers = "".join(text for rel, text in ui_sources() if "controllers/" in rel)
    assert controllers.count("menu_tab_index()") >= 8

"""Single-source guard for the persisted UI settings keys (chg006 item 23).

The namespaces/keys of the settings table live only in src/system/ui_settings.h (ui_settings.cpp holds the
accessors). Hard guard, pure text checks (no build, no hardware):
  1. the quoted key/namespace literals appear only in the allowed files;
  2. no file under src/ui/ uses Preferences except grinding_screen.* and the debug harness;
  3. bluetooth/manager.cpp and logging/grind_logging.cpp contain none of the literals.
The shared "grinder" namespace is not guarded: hardware_manager, board_detect and ota_marker use it legitimately.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"

LITERALS = [
    "bluetooth", "startup", "logging", "enabled", "swipe", "autogrind", "auto_start", "auto_return",
    "brightness", "normal", "screensaver", "grind_mode", "grinder_mode", "grinder_amt_g", "freshness_hrs",
    "screen", "dim_min", "off_min",
]
LITERAL_RE = re.compile(r'"(' + "|".join(LITERALS) + r')"')
PREFERENCES_RE = re.compile(r"\bPreferences\b")

# Files that may legitimately mention the raw strings
LITERAL_ALLOWED = {
    "src/system/ui_settings.h",
    "src/system/ui_settings.cpp",
    "src/ui/menu_walk_debug.cpp",  # debug harness oracle, compiled out of main
    "src/controllers/grind_controller.h",  # PREF_KEY_* (A4)
    "src/controllers/profile_controller.cpp",  # "grind_mode" (A4)
    "src/network/portal_api.cpp",  # public web API key names (same words, not NVS keys; no Preferences access)
}
PREFERENCES_ALLOWED_PREFIXES = ("src/ui/screens/grinding_screen.", "src/ui/menu_walk_debug.")
NO_LITERAL_FILES = ["src/bluetooth/manager.cpp", "src/logging/grind_logging.cpp"]


def source_files(base: Path):
    for path in sorted(base.rglob("*")):
        if path.suffix in {".h", ".cpp", ".hpp", ".c"}:
            yield path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8", errors="ignore")


def literal_offenders(files) -> list[str]:
    found = []
    for rel, text in files:
        if rel in LITERAL_ALLOWED:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if LITERAL_RE.search(line):
                found.append(f"{rel}:{number}")
    return found


def ui_preferences_offenders(files) -> list[str]:
    found = []
    for rel, text in files:
        if not rel.startswith("src/ui/") or rel.startswith(PREFERENCES_ALLOWED_PREFIXES):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if PREFERENCES_RE.search(line):
                found.append(f"{rel}:{number}")
    return found


def test_settings_literals_only_in_allowed_files():
    assert literal_offenders(source_files(SRC)) == []


def test_ui_code_does_not_use_preferences():
    assert ui_preferences_offenders(source_files(SRC / "ui")) == []


def test_bluetooth_and_logging_do_not_contain_the_literals():
    for rel in NO_LITERAL_FILES:
        text = (ROOT / rel).read_text(encoding="utf-8")
        assert not LITERAL_RE.search(text), rel


def test_guard_detects_a_planted_raw_use():
    planted = [("src/ui/screens/menu_screen.cpp", 'Preferences p; p.begin("swipe", true);')]
    assert literal_offenders(planted) == ["src/ui/screens/menu_screen.cpp:1"]
    assert ui_preferences_offenders(planted) == ["src/ui/screens/menu_screen.cpp:1"]
    assert literal_offenders([("src/ui/ui_manager.cpp", 'prefs.begin("grinder", false);')]) == []


def test_facade_keys_are_defined_once_in_ui_settings_header():
    header = (SRC / "system" / "ui_settings.h").read_text(encoding="utf-8")
    for ns in ["bluetooth", "logging", "swipe", "autogrind", "brightness", "screen"]:
        assert len(re.findall(rf'= "{ns}";', header)) == 1, ns


def test_grind_controller_pref_keys_match_the_facade():
    header = (SRC / "system" / "ui_settings.h").read_text(encoding="utf-8")
    controller = (SRC / "controllers" / "grind_controller.h").read_text(encoding="utf-8")
    for const, facade in [
        ("PREF_KEY_GRINDER_MODE", "kPurgeMode"),
        ("PREF_KEY_GRINDER_AMOUNT_G", "kPurgeAmountG"),
        ("PREF_KEY_GRIND_FRESHNESS_HOURS", "kFreshnessHours"),
    ]:
        theirs = re.search(rf'{const} = "([^"]+)"', controller).group(1)
        ours = re.search(rf'{facade} = "([^"]+)"', header).group(1)
        assert theirs == ours, const

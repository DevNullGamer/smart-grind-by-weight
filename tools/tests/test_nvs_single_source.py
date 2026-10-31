"""Single-source guard for the persisted UI settings keys (chg006 item 23).

The namespaces/keys of the settings table live only in src/system/ui_settings.h. While call sites are being
migrated (23.9-23.11) this test REPORTS the remaining raw uses as warnings; from 23.11 set STRICT = True so any
raw use outside the allowed files fails. Pure text checks: no build, no hardware.
"""

import re
import warnings
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
STRICT = False  # 23.11 flips this to True

NAMESPACES = ["bluetooth", "logging", "swipe", "autogrind", "brightness"]
# Files that may legitimately mention the raw strings
ALLOWED = {
    "src/system/ui_settings.h",
    "src/ui/menu_walk_debug.cpp",  # debug harness table, compiled out of main
}
# prefs.begin("<ns>", ...) is the raw-access pattern that the facade replaces
BEGIN_RE = re.compile(r'\.begin\(\s*"(' + "|".join(NAMESPACES) + r')"')


def raw_namespace_uses() -> list[str]:
    found = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in {".h", ".cpp", ".hpp", ".c"}:
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel in ALLOWED:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="ignore").splitlines(), 1):
            if BEGIN_RE.search(line):
                found.append(f"{rel}:{number}")
    return found


def test_raw_namespace_uses_are_reported():
    offenders = raw_namespace_uses()
    if offenders:
        warnings.warn(f"{len(offenders)} raw settings namespace uses remain: {offenders}")
    if STRICT:
        assert offenders == []


def test_guard_detects_a_raw_use():
    assert BEGIN_RE.search('    prefs.begin("swipe", true);')
    assert not BEGIN_RE.search('    prefs.begin("grinder", false);')


def test_facade_keys_are_defined_once_in_ui_settings_header():
    header = (SRC / "system" / "ui_settings.h").read_text(encoding="utf-8")
    for ns in NAMESPACES:
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

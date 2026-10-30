"""NVS keys are limited to 15 characters: a longer key makes put*() fail (KEY_TOO_LONG) and the value never persists.

Pure text check of the firmware sources: PREF_KEY_* constants, string-literal keys passed to Preferences put*/get*/remove,
and the key table of the menu-walk harness.
"""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent.parent / "src"
NVS_KEY_MAX = 15

CONSTANT = re.compile(r'PREF_KEY_\w+\s*=\s*"([^"]*)"')
LITERAL_CALL = re.compile(r'(?:->|\.)\s*(?:put|get)(?:Float|Int|UInt|Bool|String|ULong|Long|UChar|Short|Double)\(\s*"([^"]*)"')
REMOVE_CALL = re.compile(r'(?:->|\.)\s*remove\(\s*"([^"]*)"')
WALK_TABLE = re.compile(r'\{"\w+",\s*"[^"]*",\s*"([^"]*)",\s*Kind::')


def collect_keys() -> list[tuple[str, str]]:
    found = []
    for path in SRC.rglob("*"):
        if path.suffix not in (".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in (CONSTANT, LITERAL_CALL, REMOVE_CALL, WALK_TABLE):
            for key in pattern.findall(text):
                found.append((path.name, key))
    return found


def test_scan_finds_the_known_keys():
    keys = {key for _, key in collect_keys()}
    assert {"grinder_mode", "grinder_amt_g", "freshness_hrs", "motor_lat_ms"} <= keys


def test_all_preference_keys_fit_the_nvs_limit():
    too_long = [(f, k, len(k)) for f, k in collect_keys() if len(k) > NVS_KEY_MAX]
    assert not too_long, f"NVS keys longer than {NVS_KEY_MAX} chars: {too_long}"

"""Text guard (chg026 step 3): the rpm store, profile controller and NVS are reached only on the UI task.
Nothing in src/bluetooth/ may call into grind_rpm, ProfileController, UiSettings or Preferences for rpm."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BT = ROOT / "src/bluetooth"
FORBIDDEN = re.compile(
    r"grind_rpm|GrindRpm|ProfileController|profile_controller|UiSettings|ui_settings|Preferences|"
    r"nvs_|kGrindRpm|put_int|get_int"
)


def _code(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_bluetooth_sources_do_not_reach_rpm_storage():
    files = sorted(BT.glob("*.cpp")) + sorted(BT.glob("*.h"))
    assert files
    for path in files:
        code = _code(path.read_text(encoding="utf-8"))
        hits = FORBIDDEN.findall(code)
        # lines mentioning rpm must not carry any forbidden call either
        for line in code.splitlines():
            if re.search(r"rpm", line, re.I):
                assert not FORBIDDEN.search(line), f"{path.name}: {line.strip()}"
        if path.name in ("manager.cpp", "manager.h") or path.name.startswith("glink_"):
            assert not [h for h in hits if h in ("grind_rpm", "GrindRpm", "kGrindRpm")], f"{path.name}: {hits}"

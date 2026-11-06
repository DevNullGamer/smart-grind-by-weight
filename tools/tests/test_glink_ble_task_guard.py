"""Text guard (chg020 step 6a): GLink code on the BLE side never touches LVGL, UI, controllers or Preferences,
and grindControl / grindResult stay refused (UI_BUSY) until step 6b enables them."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BT = ROOT / "src/bluetooth"
GLINK_FILES = ["glink_gate", "glink_mailbox", "glink_screen", "glink_snapshot"]
FORBIDDEN = re.compile(
    r"lvgl|\blv_\w+|\.\./ui/|ui_manager|UIManager|ui_settings|UiSettings|Preferences|"
    r"ProfileController|profile_controller|GrindController|grind_controller|portal_ui_controller|glink_controller"
)


def _code(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_new_glink_files_have_no_ui_or_storage_access():
    for name in GLINK_FILES:
        for ext in ("h", "cpp"):
            code = _code((BT / f"{name}.{ext}").read_text(encoding="utf-8"))
            hits = FORBIDDEN.findall(code)
            assert not hits, f"{name}.{ext}: {hits}"


def test_manager_glink_code_has_no_ui_or_storage_access():
    code = _code((BT / "manager.cpp").read_text(encoding="utf-8"))
    for pat in (r"lvgl", r"\blv_\w+", r"\.\./ui/", r"ui_manager", r"UIManager", r"ProfileController",
                r"UiSettings", r"glink_controller"):
        assert not re.search(pat, code), pat
    for line in code.splitlines():
        if re.search(r"glink|Glink|GLink", line):
            assert not re.search(r"Preferences|grind_controller|GrindController", line), line


def test_grind_control_and_result_still_answer_ui_busy():
    code = (BT / "manager.cpp").read_text(encoding="utf-8")
    m = re.search(r"\} else \{\s*//[^\n]*6b[^\n]*\n(?:\s*//[^\n]*\n)*\s*n = glink_encode_error\(([^;]*)\);", code)
    assert m, "final else branch (grindControl/grindResult) not found"
    assert "GlinkError::UI_BUSY" in m.group(1)
    # nothing else dispatches grindControl / grindResult yet
    assert "GlinkObject::GRIND_CONTROL" not in code
    assert "GlinkObject::GRIND_RESULT" not in code

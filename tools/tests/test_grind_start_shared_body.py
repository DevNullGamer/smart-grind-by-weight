"""Pin that every grind start (START button, auto-start, web remote start, GLink) runs one shared body.

Host only: reads src/ui/controllers/grinding_controller.cpp as text because the UI code cannot be built on the host.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "ui" / "controllers" / "grinding_controller.cpp"


def _body(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start)]


def test_begin_grind_keeps_the_start_log_lines_and_profile_id():
    text = SRC.read_text(encoding="utf-8")
    body = _body(text, "GrindStartBlock GrindingUIController::begin_grind(")
    assert "[%lums GRIND_START] About to call start_grind()" in body
    assert "[%lums GRIND_START] start_grind() returned" in body
    assert "set_grind_profile_id(profile_id)" in body
    assert body.index("About to call") < body.index("grind_controller->start_grind(") < body.index("start_grind() returned")


def test_start_log_lines_exist_only_in_the_shared_body():
    text = SRC.read_text(encoding="utf-8")
    assert text.count("GRIND_START] About to call start_grind()") == 1
    assert text.count("GRIND_START] start_grind() returned") == 1
    assert text.count("grind_controller->start_grind(") == 1


def test_profile_and_glink_starts_use_begin_grind():
    text = SRC.read_text(encoding="utf-8")
    profile = _body(text, "GrindStartBlock GrindingUIController::start_profile_grind(")
    glink = _body(text, "GrindStartBlock GrindingUIController::start_glink_grind(")
    assert "begin_grind(static_cast<uint8_t>(profile), target_weight, target_time_ms, ui_manager_->current_mode)" in profile
    assert "begin_grind(" in glink and "GrindMode::WEIGHT" in glink


def test_button_and_web_start_go_through_start_profile_grind():
    assert "start_profile_grind(ui_manager_->profile_controller->get_current_profile())" in SRC.read_text(encoding="utf-8")
    portal = (ROOT / "src" / "ui" / "controllers" / "portal_ui_controller.cpp").read_text(encoding="utf-8")
    assert "start_profile_grind(lead_in_profile_)" in portal

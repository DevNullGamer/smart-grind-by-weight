"""Pin that Start on Cup is gated on a running Motor Test.

Host only: reads src/ui/ui_manager.cpp as text because the UI code cannot be built on the host.
See docs/DESIGN-NOTES.md (Web panel, known gaps).
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _function_body(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start)]


def test_auto_start_checks_motor_test_gate():
    text = (ROOT / "src" / "ui" / "ui_manager.cpp").read_text(encoding="utf-8")
    body = _function_body(text, "void UIManager::update_auto_actions()")
    assert "is_motor_test_active()" in body
    assert re.search(r"auto_start_enabled\s*&&\s*auto_start_allowed\(", body)

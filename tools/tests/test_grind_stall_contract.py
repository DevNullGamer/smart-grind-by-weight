"""Source-contract guard for the mid-grind stall abort wiring (chg019 step 2).

Pure text checks, no build and no hardware. The stall check must run only in weight mode and only in
the PREDICTIVE case of check_safety(), and the tracker must be re-baselined on PREDICTIVE entry.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CPP = (ROOT / "src/controllers/grind_controller.cpp").read_text(encoding="utf-8")
HDR = (ROOT / "src/controllers/grind_controller.h").read_text(encoding="utf-8")
CTRL = (ROOT / "src/config/grind_control.h").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(rf"^[\w:<>&\* ]+ GrindController::{name}\(.*?\n\}}\n", CPP, re.S | re.M)
    assert match, f"GrindController::{name} not found"
    return match.group(0)


def strip_comments(text: str) -> str:
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def predictive_case(body: str) -> str:
    start = body.index("case GrindPhase::PREDICTIVE: {")
    end = body.index("default:", start)
    return body[start:end]


def test_stall_update_only_in_predictive_case():
    body = strip_comments(function_body("check_safety"))
    assert body.count("grind_stall_update(") == 1
    case = predictive_case(body)
    assert "grind_stall_update(" in case
    assert body.replace(case, "").count("grind_stall_update") == 0


def test_check_safety_is_weight_mode_only():
    body = strip_comments(function_body("check_safety"))
    guard = body.index("mode != GrindMode::WEIGHT")
    assert guard < body.index("grind_stall_update(")
    assert "return false" in body[guard:guard + 120]


def test_stall_uses_constants_and_reason_bit():
    case = predictive_case(strip_comments(function_body("check_safety")))
    assert "GRIND_STALL_MIN_GAIN_G" in case and "GRIND_STALL_WINDOW_MS" in case
    assert "SAFETY_REASON_STALL" in case
    assert "grinder->is_grinding()" in case


def test_switch_phase_resets_tracker_on_predictive_entry():
    body = strip_comments(function_body("switch_phase"))
    match = re.search(r"if \(new_phase == GrindPhase::PREDICTIVE[^\n]*\{(.*?)\n    \}\n", body, re.S)
    assert match, "PREDICTIVE entry block not found"
    assert "grind_stall_reset(stall_tracker_" in match.group(1)
    assert body.count("grind_stall_reset(") == 1


def test_error_text_fits_12_chars():
    m = re.search(r'SAFETY_REASON_STALL,\s*"([^"]+)"', CPP)
    assert m, "stall error text not found"
    assert m.group(1) == "Err: stalled"
    assert len(m.group(1)) <= 12


def test_reason_bits_are_distinct_single_bits():
    bits = dict(re.findall(r"SAFETY_REASON_(\w+) = 1 << (\d+);", HDR))
    assert "STALL" in bits
    values = list(bits.values())
    assert len(values) == len(set(values)), f"duplicate reason bits: {bits}"
    assert all(int(v) < 8 for v in values)


def test_stall_constants_and_kill_switch_comment():
    assert re.search(r"#define GRIND_STALL_WINDOW_MS\s+5000\b", CTRL)
    assert re.search(r"#define GRIND_STALL_MIN_GAIN_G\s+0\.3f", CTRL)
    line = next(ln for ln in CTRL.splitlines() if "#define GRIND_SAFETY_ABORTS_ENABLED" in ln)
    assert "Err: stalled" in line


def test_tracker_member_declared():
    assert re.search(r"GrindStallTracker\s+stall_tracker_", HDR)
    assert '#include "grind_safety.h"' in HDR

"""Text guards (chg020 step 6b): the GLink start/stop/result wiring.

The firmware code cannot be built on the host, so the rules that keep the motor path safe are pinned as text:
nothing starts a grind except through the shared start body behind the decision chain, a stop is never queued and
never stops on a link drop, the two remote-start lead-ins exclude each other, the user's grind mode is put back, and
every result path publishes through the seam (including the 60 s TIMEOUT branch).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


def _read(rel):
    return (SRC / rel).read_text(encoding="utf-8")


def _body(text, signature):
    start = text.index(signature)
    end = text.index("\n}", start)
    return text[start:end]


def _code(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# ---- result seam (known gap closed: the TIMEOUT branch) ----
def test_timeout_branch_publishes_its_result_as_not_stopped():
    text = _read("controllers/grind_controller.cpp")
    branch = text[text.index("} else if (new_phase == GrindPhase::TIMEOUT) {"):]
    branch = branch[: branch.index("} else if (new_phase == GrindPhase::IDLE)")]
    assert "publish_result(last_session_result_, abort_reason_, event_data.error_weight, false);" in branch
    assert "true" not in re.findall(r"publish_result\([^;]*\)", branch)[0].split(",")[-1]


def test_every_publish_result_call_is_accounted_for():
    text = _read("controllers/grind_controller.cpp")
    calls = re.findall(r"publish_result\(([^;]*?)\);", _code(text), flags=re.S)
    calls = [c for c in calls if not c.lstrip().startswith("GrindSessionResult outcome")]
    flat = [re.sub(r"\s+", " ", c) for c in calls]
    # stop (stopped = true), normal completion and the TIMEOUT/abort branch (stopped = false): exactly these three
    assert len(flat) == 3, flat
    assert sum(c.rstrip().endswith(", true") for c in flat) == 1
    assert sum(c.rstrip().endswith(", false") for c in flat) == 2


# ---- stop ----
def test_stop_runs_on_the_bluetooth_task_and_is_never_queued():
    code = _code(_read("bluetooth/manager.cpp"))
    m = re.search(r"request\.object == GlinkObject::GRIND_CONTROL && request\.op == GlinkOp::STOP\) \{(.*?)\n    \} else", code, flags=re.S)
    assert m, "stop branch not found"
    assert "portal_stop_now();" in m.group(1)
    assert "glink_encode_stop(" in m.group(1)
    assert "submit(" not in m.group(1)
    # it sits before the update refusal, so a stop works during an update, and after the handshake gate
    assert code.index("GlinkOp::STOP) {") < code.index("UpdateGuard::is_held() || ota_handler.is_ota_active()")
    assert code.index("glink_gate(glink_session.handshaken()") < code.index("GlinkOp::STOP) {")


def test_a_link_drop_does_not_stop_a_grind():
    code = _code(_read("bluetooth/manager.cpp"))
    for name in ("onDisconnect", "glink_service_tick"):
        body = _body(code, "void BluetoothManager::" + name)
        assert "portal_stop_now" not in body, name
        assert "request_stop" not in body, name
        assert "stop_grind" not in body, name
    assert code.count("portal_stop_now()") == 1  # only the explicit stop request


def test_glink_controller_never_stops_the_motor_itself():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    assert "portal_stop_now" not in code and "stop_grind" not in code and "request_stop" not in code


# ---- start path ----
def test_start_goes_through_decision_chain_and_shared_body_only():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    assert code.count("start_glink_grind(") == 1
    assert "->start_grind(" not in code and "begin_grind(" not in code
    body = _body(code, "void GlinkUIController::start_after_lead_in()")
    assert body.index("glink_decide_start(") < body.index("start_glink_grind(")
    # the request itself only begins the lead-in; the first decision is glink_decide_start
    apply = _body(code, "GlinkMailboxResult GlinkUIController::apply_start(")
    assert "glink_decide_start(" in apply and "start_glink_grind" not in apply and "start_grind" not in apply


def test_lead_in_is_decided_again_every_frame_and_cancels_on_touch_stop_and_link_off():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    body = _body(code, "void GlinkUIController::run_lead_in()")
    for needle in ("get_ms_since_last_touch()", "portal_stop_pending(", "link_mode() != GlinkLinkMode::OFF",
                   "glink_decide_start(", "classify_screen(true)", "glink_lead_in_step("):
        assert needle in body, needle
    assert "show_with_stop(" in _body(code, "GlinkMailboxResult GlinkUIController::apply_start(")


def test_lead_in_runs_before_the_link_check_so_a_drop_does_not_cancel_it():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    body = _body(code, "void GlinkUIController::update()")
    assert body.index("run_lead_in();") < body.index("if (!linked) return;")
    assert body.index("watch_result();") < body.index("if (!linked) return;")


def test_refusal_of_start_glink_grind_does_not_map_to_the_motor_dialog():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    body = _body(code, "void GlinkUIController::start_after_lead_in()")
    assert "if (grind->start_block() == block) ui_manager_->show_start_blocked_notice(block);" in body
    assert "glink_refusal_shows_dialog(" in body


# ---- the two lead-ins exclude each other ----
def test_web_and_ble_lead_ins_count_each_other_as_in_flight():
    portal = _code(_read("ui/controllers/portal_ui_controller.cpp"))
    gather = _body(portal, "PortalDecisionInput PortalUIController::gather_decision() const")
    assert "glink_controller_->lead_in_active()" in gather
    glink = _code(_read("ui/controllers/glink_controller.cpp"))
    classify = _body(glink, "GlinkScreen GlinkUIController::classify_screen(")
    assert "portal->lead_in_active()" in classify and "lead_in_.active" in classify


# ---- grind mode ----
def test_forced_weight_mode_is_armed_by_a_glink_start_and_restored_on_ready():
    text = _read("ui/controllers/grinding_controller.cpp")
    glink = _body(text, "GrindStartBlock GrindingUIController::start_glink_grind(")
    assert "if (block == GrindStartBlock::NONE) forced_mode_.arm();" in glink
    ready = _body(text, "void GrindingUIController::enter_ready_state()")
    assert ready.index("forced_mode_.on_ready(") < ready.index("if (!grind_button_)")
    assert "ui_manager_->current_mode = restored_mode;" in ready
    # the START button and the web start never arm it
    assert "forced_mode_.arm()" not in _body(text, "GrindStartBlock GrindingUIController::start_profile_grind(")
    assert text.count("forced_mode_.arm()") == 1


# ---- result ----
def test_result_watcher_keys_on_result_count_and_checks_it_around_the_read():
    code = _code(_read("ui/controllers/glink_controller.cpp"))
    body = _body(code, "void GlinkUIController::watch_result()")
    assert body.index("result_count()") < body.index("read_last_result(") < body.rindex("result_count()")
    assert "record.seq = count;" in body
    assert "glink_result_from_record(" in body


def test_result_broadcast_goes_to_a_handshaken_client_only():
    code = _code(_read("bluetooth/manager.cpp"))
    assert "glink_session.handshaken() && glink_send_result_if_due()" in code
    assert code.count("glink_result_feed.on_handshake();") == 1
    assert code.count("glink_result_feed.reset();") == 1

"""GLink v1 client (tools/ble/glink_client.py) against the simulated grinder (tools/ble/glink_sim.py).

No Bluetooth, no event loop and no real time: the clock is a FakeClock, and every wait of the client polls it, so
timeouts and the 3 s lead-in run instantly. The simulator also answers every request/response pair of the shared
conformance vectors (tools/tests/glink_vectors.json), which ties it to the contract the C++ firmware is tested with.
"""

import json
import subprocess
import sys
from argparse import Namespace
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ble"))
sys.path.insert(0, str(ROOT / "tools"))

import glink_client as gc  # noqa: E402
import glink_codec as codec  # noqa: E402
import glink_sim  # noqa: E402
from glink_codec import Screen, StartBlock  # noqa: E402

V = json.loads((Path(__file__).resolve().parent / "glink_vectors.json").read_text(encoding="utf-8"))


def run(coro):
    """Drive a coroutine that never suspends (FakeClock never does); avoids asyncio.run on locked-down hosts."""
    try:
        coro.send(None)
    except StopIteration as stop:
        return stop.value
    coro.close()
    raise AssertionError("coroutine suspended unexpectedly")


class RecordingTransport:
    """SimTransport wrapper that records RX writes and TX notifications."""

    def __init__(self, inner):
        self.inner = inner
        self.writes = []
        self.notifications = []

    async def connect(self, on_notify, on_disconnect):
        def tap(packet):
            self.notifications.append(bytes(packet))
            on_notify(packet)

        return await self.inner.connect(tap, on_disconnect)

    async def write(self, packet):
        self.writes.append(bytes(packet))
        await self.inner.write(packet)

    async def disconnect(self):
        await self.inner.disconnect()


class DeadTransport:
    """Accepts everything, answers nothing."""

    def __init__(self, mtu=23):
        self.mtu = mtu
        self.writes = []

    async def connect(self, on_notify, on_disconnect):
        return self.mtu

    async def write(self, packet):
        self.writes.append(bytes(packet))

    async def disconnect(self):
        pass


class World:
    def __init__(self, mtu=23, handshake=True):
        self.clock = gc.FakeClock(5000)
        self.sim = glink_sim.GlinkSim(self.clock)
        self.transport = RecordingTransport(glink_sim.SimTransport(self.sim, mtu))
        self.client = gc.GlinkClient(self.transport, self.clock)
        run(self.client.connect(handshake=handshake))

    def do(self, coro):
        return run(coro)

    def wait(self, ms):
        self.clock.advance(ms)


@pytest.fixture
def w():
    return World()


def refused(coro):
    with pytest.raises(gc.GlinkRefused) as e:
        run(coro)
    return e.value


# ---- module hygiene and packet size ------------------------------------------------------------------------------

def test_no_bluetooth_import_at_module_import_time():
    code = ("import sys; sys.path.insert(0, %r); import glink_client, glink_sim; "
            "assert 'bleak' not in sys.modules, 'bleak imported'" % str(ROOT / "tools" / "ble"))
    r = subprocess.run([sys.executable, "-I", "-c", code], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("mtu,expected", [(None, 20), (0, 20), (23, 20), (24, 21), (185, 182), (517, 512), (1000, 512)])
def test_packet_size_from_mtu(mtu, expected):
    assert gc.packet_bytes_for_mtu(mtu) == expected


def test_default_mtu_sends_20_byte_packets_that_equal_the_vector():
    w = World(mtu=23)
    handshake = next(f for f in V["frame_cases"] if f["name"] == "handshake_20")
    assert [p.hex() for p in w.transport.writes] == handshake["packets"]
    assert w.client.packet_bytes == 20
    assert all(len(p) <= 20 for p in w.transport.notifications)


def test_large_mtu_sends_larger_packets():
    w = World(mtu=185)
    assert w.client.packet_bytes == 182
    assert len(w.transport.writes) == 1 and len(w.transport.writes[0]) == 51  # the 43-byte handshake in one packet
    w.transport.notifications.clear()
    w.do(w.client.profile_get())
    assert 1 <= len(w.transport.notifications) <= 3
    assert max(len(p) for p in w.transport.notifications) > 20


def test_default_mtu_response_takes_many_fragments():
    w = World(mtu=23)
    w.do(w.client.profile_update(dose=18.0, grind_size=14.5, grind_unit="um"))
    w.transport.notifications.clear()
    w.do(w.client.profile_get())
    assert len(w.transport.notifications) >= 30  # section 8: about 34 notifications for four profiles


def test_forced_packet_size_wins():
    clock = gc.FakeClock(0)
    sim = glink_sim.GlinkSim(clock)
    transport = RecordingTransport(glink_sim.SimTransport(sim, 185))
    client = gc.GlinkClient(transport, clock, packet_bytes=20)
    run(client.connect())
    assert client.packet_bytes == 20 and max(len(p) for p in transport.writes) == 20


# ---- handshake, gate, timeouts -----------------------------------------------------------------------------------

def test_handshake_delivers_base_info_and_period_info(w):
    info = w.client.base_info
    assert info["device"] == "SGBW" and info["proto"] == 1 and info["snCode"] == "7c2a3f10b4d8"
    assert info["maxRequestBytes"] == 512 and info["wifiName"] is None
    assert w.client.period["devState"] == "IDLE" and w.client.period["blocked"] is None
    assert w.client.profiles_dirty  # the first periodInfo of a connection counts as a change


def test_request_before_handshake_is_invalid_state():
    w = World(handshake=False)
    e = refused(w.client.profile_get())
    assert e.code == "INVALID_STATE" and e.object == "grindProfile"


def test_stop_works_before_handshake():
    w = World(handshake=False)
    assert w.do(w.client.stop()) is True


def test_no_handshake_within_10_s_disconnects():
    w = World(handshake=False)
    w.wait(9990)
    assert w.client.connected
    w.wait(30)
    assert not w.client.connected and w.sim.session is None
    assert refused_or_disconnected(w.client.profile_get())


def refused_or_disconnected(coro):
    with pytest.raises(gc.GlinkError):
        run(coro)
    return True


def test_handshaken_client_may_stay_silent():
    w = World()
    w.wait(60000)
    assert w.client.connected


def test_handshake_timeout_when_the_grinder_is_silent():
    clock = gc.FakeClock(0)
    client = gc.GlinkClient(DeadTransport(), clock)
    with pytest.raises(gc.GlinkTimeout):
        run(client.connect())
    assert 5000 <= clock.now_ms() <= 5010


def test_response_timeout_is_5_s_on_the_injected_clock():
    clock = gc.FakeClock(0)
    client = gc.GlinkClient(DeadTransport(), clock)
    run(client.connect(handshake=False))
    with pytest.raises(gc.GlinkTimeout):
        run(client.profile_get())
    assert 5000 <= clock.now_ms() <= 5010
    assert client._pending is None  # a new request is possible after a timeout


def test_response_for_another_object_is_ignored():
    clock = gc.FakeClock(0)
    client = gc.GlinkClient(DeadTransport(), clock)
    run(client.connect(handshake=False))
    wrong = ('{"response":{"grindResult":{"result":"success","data":{}}}}').encode()
    for packet in codec.fragments(wrong, 9, 182):
        client._on_notify(packet)
    with pytest.raises(gc.GlinkTimeout):
        run(client.profile_get())


def test_one_request_at_a_time(w):
    w.client._pending = "grindProfile"
    with pytest.raises(gc.GlinkError, match="already pending"):
        w.do(w.client.profile_get())


def test_oversized_request_is_refused_locally(w):
    with pytest.raises(ValueError):
        w.do(w.client.request("grindProfile", {"op": "get", "x": "y" * 600}))


def test_busy_while_a_request_is_pending_on_the_grinder(w):
    w.sim.ui_delay_ms = 500
    w.do(w.client._send({"request": {"grindProfile": {"op": "get"}}}))
    w.do(w.client._send({"request": {"grindProfile": {"op": "get"}}}))
    reply = w.do(w.client._wait(lambda: w.client._take_response("grindProfile"), 1000, "reply"))
    assert reply["error"]["code"] == "BUSY"
    reply = w.do(w.client._wait(lambda: w.client._take_response("grindProfile"), 1000, "reply"))
    assert reply["result"] == "success"


def test_ui_busy_after_1000_ms_and_the_request_is_not_applied(w):
    w.sim.ui_delay_ms = 1500
    t0 = w.clock.now_ms()
    e = refused(w.client.profile_update(dose=18.0))
    assert e.code == "UI_BUSY" and 1000 <= w.clock.now_ms() - t0 <= 1020
    w.wait(3000)
    assert not w.sim.profile3.exists and w.sim.profile_rev == 0


def test_a_slow_but_in_time_ui_answer_is_applied(w):
    w.sim.ui_delay_ms = 400
    assert w.do(w.client.profile_update(dose=18.0))["changed"] is True


# ---- bad frames --------------------------------------------------------------------------------------------------

BAD = bytes([0xA4, 1, 1, 0, 43, 0, 0, 0]) + b"xxxx"


def test_nine_bad_fragments_do_not_disconnect_and_the_link_still_works(w):
    for _ in range(9):
        w.do(w.client.send_packet(BAD))
    assert w.client.connected
    assert w.do(w.client.profile_get())


def test_ten_bad_fragments_within_10_s_disconnect(w):
    for _ in range(10):
        if w.client.connected:
            w.do(w.client.send_packet(BAD))
        w.wait(500)
    assert not w.client.connected


def test_the_tenth_bad_fragment_outside_the_window_does_not(w):
    for _ in range(9):
        w.do(w.client.send_packet(BAD))
        w.wait(1000)
    w.wait(1000)
    w.do(w.client.send_packet(BAD))
    assert w.client.connected


def test_oversize_message_gets_payload_too_large(w):
    frames = next(p for p in V["pairs"] if p["name"] == "envelope_payload_too_large")["request_frames"]
    for f in frames:
        w.do(w.client.send_packet(bytes.fromhex(f)))
    reply = w.do(w.client._wait(lambda: w.client._take_response("grindProfile"), 1000, "reply"))
    assert reply["error"]["code"] == "PAYLOAD_TOO_LARGE"


def test_garbage_json_from_the_grinder_is_counted_not_fatal(w):
    for packet in codec.fragments(b"{not json", 3, 20):
        w.client._on_notify(packet)
    assert w.client.bad_fragments == 1 and w.client.connected


# ---- grindProfile ------------------------------------------------------------------------------------------------

def test_get_all_and_one(w):
    entries = w.do(w.client.profile_get())
    assert [e["name"] for e in entries] == ["SINGLE", "DOUBLE", "CUSTOM"]
    assert not w.client.profiles_dirty
    assert w.do(w.client.profile_get(2))["dose"] == 21.5
    assert refused(w.client.profile_get(3)).code == "NOT_FOUND"
    assert refused(w.client.profile_get(4)).code == "OUT_OF_RANGE"


def test_update_creates_profile_3_and_get_returns_it(w):
    data = w.do(w.client.profile_update(dose=18.0, grind_size=14.5, grind_unit="um"))
    assert data["changed"] and data["stored"] and data["writable"] and data["grindUnit"] == "um"
    assert w.do(w.client.profile_get(3))["grindSize"] == 14.5
    assert len(w.do(w.client.profile_get())) == 4
    w.wait(100)
    assert w.client.period["profileRev"] == 1


def test_identical_update_changes_nothing_and_is_not_rate_limited(w):
    w.do(w.client.profile_update(dose=18.0))
    data = w.do(w.client.profile_update(dose=18.04))  # rounds to 18.0
    assert data["changed"] is False and data["stored"] is False


def test_clear_grind_size_clears_the_unit_too(w):
    w.do(w.client.profile_update(dose=18.0, grind_size=14.5, grind_unit="um"))
    w.wait(1000)
    data = w.do(w.client.profile_update(clear_grind_size=True))
    assert data["grindSize"] is None and data["grindUnit"] is None


def test_profile_rev_change_marks_the_profiles_stale(w):
    w.do(w.client.profile_get())
    assert not w.client.profiles_dirty
    w.sim.set_profile3(200, 100, "um")  # an edit on the grinder
    w.wait(100)
    assert w.client.profiles_dirty


UPDATE_REFUSALS = [
    # (name, setup(sim), kwargs of profile_update or a raw body, code, field)
    ("read_only", None, {"raw": {"op": "update", "index": 1, "dose": 18.0}}, "PROFILE_READ_ONLY", "index"),
    ("dose_too_high", None, {"raw": {"op": "update", "index": 3, "dose": 61.0}}, "OUT_OF_RANGE", "dose"),
    ("dose_too_low", None, {"raw": {"op": "update", "index": 3, "dose": 4.94}}, "OUT_OF_RANGE", "dose"),
    ("dose_wrong_type", None, {"raw": {"op": "update", "index": 3, "dose": "18"}}, "INVALID_PARAM", "dose"),
    ("nothing_to_update", None, {"raw": {"op": "update", "index": 3}}, "MISSING_FIELD", ""),
    ("create_needs_dose", None, {"grind_size": 14.5}, "MISSING_FIELD", "dose"),
    ("size_out_of_range", None, {"raw": {"op": "update", "index": 3, "dose": 18.0, "grindSize": 1000.0}},
     "OUT_OF_RANGE", "grindSize"),
    ("unit_too_long", None, {"dose": 18.0, "grind_size": 1.0, "grind_unit": "abcdefg"}, "OUT_OF_RANGE", "grindUnit"),
    ("unit_bad_char", None, {"dose": 18.0, "grind_size": 1.0, "grind_unit": "a;b"}, "INVALID_PARAM", "grindUnit"),
    ("unit_needs_size", None, {"dose": 18.0, "grind_unit": "um"}, "INVALID_PARAM", "grindUnit"),
    ("unknown_key", None, {"raw": {"op": "update", "index": 3, "dose": 18.0, "colour": "red"}}, "INVALID_PARAM", "colour"),
    ("edit_in_progress", lambda s: s.set_screen(Screen.EDIT), {"dose": 18.0}, "EDIT_IN_PROGRESS", ""),
    ("grinding", lambda s: s.touch_start(1), {"dose": 18.0}, "NOT_READY", ""),
    ("done_screen", lambda s: s.set_screen(Screen.DONE), {"dose": 18.0}, "NOT_READY", ""),
    ("other_screen", lambda s: s.set_screen(Screen.OTHER), {"dose": 18.0}, "NOT_READY", ""),
    ("lead_in", lambda s: s.begin_lead_in(1), {"dose": 18.0}, "COMMAND_IN_FLIGHT", ""),
    ("updating", lambda s: s.set_screen(Screen.UPDATING), {"dose": 18.0}, "UPDATING", ""),
    ("rate_limited", lambda s: setattr(s, "last_store_ms", s.clock.now_ms() - 300), {"dose": 18.0}, "RATE_LIMITED", ""),
    ("store_failed", lambda s: setattr(s, "fail_store", True), {"dose": 18.0}, "STORE_FAILED", ""),
]


@pytest.mark.parametrize("name,setup,kwargs,code,field", UPDATE_REFUSALS, ids=[r[0] for r in UPDATE_REFUSALS])
def test_update_refusals_change_nothing(w, name, setup, kwargs, code, field):
    if setup:
        setup(w.sim)
    rev = w.sim.profile_rev
    if "raw" in kwargs:
        e = refused(w.client.call("grindProfile", kwargs["raw"]))
    else:
        e = refused(w.client.profile_update(**kwargs))
    assert (e.code, e.field) == (code, field)
    assert not w.sim.profile3.exists and w.sim.profile_rev == rev


def test_update_is_possible_in_the_menu_and_after_the_rate_limit(w):
    w.sim.set_screen(Screen.MENU)
    w.do(w.client.profile_update(dose=18.0))
    assert refused(w.client.profile_update(dose=19.0)).code == "RATE_LIMITED"
    w.wait(1000)
    assert w.do(w.client.profile_update(dose=19.0))["changed"]


def test_select(w):
    assert w.do(w.client.profile_select(2)) == 2
    w.wait(100)
    assert w.client.period["selectProfile"] == 2
    assert refused(w.client.profile_select(3)).code == "PROFILE_NOT_SET"
    assert refused(w.client.profile_select(4)).code == "OUT_OF_RANGE"
    assert w.do(w.client.profile_select(2)) == 2  # already selected: success, never rate limited
    w.wait(1000)
    w.sim.set_screen(Screen.MENU)
    assert refused(w.client.profile_select(0)).code == "NOT_READY"


def test_select_rate_limited_then_allowed(w):
    w.do(w.client.profile_select(0))
    assert refused(w.client.profile_select(2)).code == "RATE_LIMITED"
    w.wait(1000)
    assert w.do(w.client.profile_select(2)) == 2


# ---- grindControl: start, stop, lead-in --------------------------------------------------------------------------

def ready_with_profile_3(w):
    w.sim.set_profile3(180, 145, "um")
    w.wait(20)


def test_start_runs_a_3_s_lead_in_then_grinds_and_broadcasts_the_result(w):
    ready_with_profile_3(w)
    assert w.do(w.client.start(3, expect_dose=18.0)) == 3000
    w.wait(20)
    p = w.client.period
    assert p["devState"] == "STARTING" and p["blocked"] == "COMMAND_IN_FLIGHT"
    assert p["target"] == 18.0 and p["mode"] == "WEIGHT" and 0 < p["leadInMs"] <= 3000
    assert w.sim.selected == 1  # start selects nothing
    w.wait(2950)  # 2970 ms after the start: still in the lead-in
    assert w.sim.screen == Screen.LEAD_IN
    w.wait(40)
    assert w.sim.screen == Screen.GRINDING
    w.wait(300)
    p = w.client.period
    assert p["devState"] == "GRINDING" and p["phase"] in codec.PHASES and "leadInMs" not in p
    result = w.do(w.client.wait_result(10000))
    assert (result["seq"], result["code"], result["origin"], result["profile"]) == (1, "OK", "BLE", 3)
    assert result["finalWeight"] == 18.0 and result["weightSettled"] is True
    w.wait(300)
    assert w.client.period["devState"] == "DONE" and w.client.period["resultSeq"] == 1
    w.sim.touch()  # the user takes the cup
    w.wait(20)
    assert w.client.period["devState"] == "IDLE"


def test_start_and_confirm_reports_started(w):
    out = w.do(w.client.start_and_confirm(1))
    assert out.started and out.lead_in_ms == 3000 and out.period["devState"] == "GRINDING"


def test_a_touch_cancels_the_lead_in_without_a_result(w):
    w.do(w.client.start(1))
    w.wait(1500)
    w.sim.touch()
    w.wait(4000)
    assert w.client.period["devState"] == "IDLE" and w.client.results == [] and w.sim.result is None
    assert w.sim.session is not None


def test_start_and_confirm_reports_a_cancelled_lead_in(w):
    mark = w.client.period_count
    w.do(w.client.start(1))
    w.wait(1000)
    w.sim.touch()
    out = w.do(w.client._wait(lambda: w.client.period if w.client.period_count > mark and
                              w.client.period["devState"] != "STARTING" else None, 5000, "end"))
    assert out["devState"] == "IDLE"


def test_stop_cancels_the_lead_in(w):
    w.do(w.client.start(1))
    w.wait(500)
    assert w.do(w.client.stop()) is True
    w.wait(20)
    assert w.client.period["devState"] == "IDLE" and w.sim.result is None


def test_stop_during_a_grind_ends_with_stopped(w):
    out = w.do(w.client.start_and_confirm(1))
    assert out.started
    w.wait(1000)
    assert w.do(w.client.stop()) is True
    result = w.do(w.client.wait_result(2000, after_seq=0))  # already in, the stop reply waited
    assert result["code"] == "STOPPED" and result["weightSettled"] is False and 0 < result["finalWeight"] < 18.0


def test_stop_with_nothing_running_is_accepted(w):
    assert w.do(w.client.stop()) is True


def test_stop_ignores_extra_keys_and_works_while_updating(w):
    w.sim.set_screen(Screen.UPDATING)
    reply = w.do(w.client.request("grindControl", {"op": "stop", "reason": "x"}, track=False))
    assert reply["data"] == {"accepted": True}


def test_lead_in_blocks_start_update_select_but_not_get(w):
    ready_with_profile_3(w)
    w.do(w.client.start(1))
    assert refused(w.client.start(1)).code == "COMMAND_IN_FLIGHT"
    assert refused(w.client.profile_update(dose=19.0)).code == "COMMAND_IN_FLIGHT"
    assert refused(w.client.profile_select(2)).code == "COMMAND_IN_FLIGHT"
    assert len(w.do(w.client.profile_get())) == 4


START_REFUSALS = [
    ("profile_not_set", None, {"profile": 3}, "PROFILE_NOT_SET", "profile"),
    ("conflict", lambda s: s.set_profile3(180), {"profile": 3, "expect_dose": 17.0}, "CONFLICT", "expectDose"),
    ("dose_above_ble_range", lambda s: s.doses.__setitem__(2, 1000), {"profile": 2}, "OUT_OF_RANGE", "dose"),
    ("not_ready_menu", lambda s: s.set_screen(Screen.MENU), {"profile": 1}, "NOT_READY", ""),
    ("not_ready_grinding", lambda s: s.touch_start(1), {"profile": 1}, "NOT_READY", ""),
    ("updating", lambda s: s.set_screen(Screen.UPDATING), {"profile": 1}, "UPDATING", ""),
    ("motor_disabled", lambda s: s.set_block(StartBlock.MOTOR_DISABLED), {"profile": 1}, "MOTOR_DISABLED", ""),
    ("update_lock", lambda s: s.set_block(StartBlock.UPDATE_LOCK), {"profile": 1}, "UPDATING", ""),
    ("lc_not_connected", lambda s: s.set_block(StartBlock.LOAD_CELL_NOT_CONNECTED), {"profile": 1},
     "LOAD_CELL_NOT_CONNECTED", ""),
    ("lc_no_data", lambda s: s.set_block(StartBlock.LOAD_CELL_NO_DATA), {"profile": 1}, "LOAD_CELL_NO_DATA", ""),
    ("lc_rate", lambda s: s.set_block(StartBlock.LOAD_CELL_RATE_INVALID), {"profile": 1}, "LOAD_CELL_RATE_INVALID", ""),
    ("profile_out_of_range", None, {"profile": 4}, "OUT_OF_RANGE", "profile"),
]


@pytest.mark.parametrize("name,setup,kwargs,code,field", START_REFUSALS, ids=[r[0] for r in START_REFUSALS])
def test_start_refusals(w, name, setup, kwargs, code, field):
    if setup:
        setup(w.sim)
    e = refused(w.client.start(**kwargs))
    assert (e.code, e.field) == (code, field)
    assert w.sim.screen != Screen.LEAD_IN


def test_a_block_that_appears_during_the_lead_in_opens_the_dialog(w):
    w.do(w.client.start(1))
    w.wait(500)
    w.sim.set_block(StartBlock.LOAD_CELL_NO_DATA)
    w.wait(2600)
    p = w.client.period
    assert p["devState"] == "SETTING" and p["blocked"] == "NOT_READY" and w.sim.result is None
    w.sim.touch()
    w.wait(20)
    assert w.client.period["devState"] == "WARNING" and w.client.period["blocked"] == "LOAD_CELL_NO_DATA"


def test_a_state_change_during_the_lead_in_drops_the_start_silently(w):
    w.do(w.client.start(1))
    w.wait(500)
    w.sim.set_screen(Screen.UPDATING)
    w.wait(3000)
    assert w.client.period["devState"] == "UPDATING" and w.sim.result is None


def test_start_and_confirm_times_out_after_lead_in_plus_2_s(w, monkeypatch):
    monkeypatch.setattr(w.sim, "_end_lead_in", lambda: None)  # a grinder that never leaves the lead-in
    t0 = w.clock.now_ms()
    out = w.do(w.client.start_and_confirm(1))
    assert not out.started and 5000 <= w.clock.now_ms() - t0 <= 5030


def test_a_grind_by_touch_is_broadcast_to_the_connected_client(w):
    w.sim.touch_start(2)
    result = w.do(w.client.wait_result(20000))
    assert result["origin"] == "TOUCH" and result["profile"] == 2 and result["target"] == 21.5


@pytest.mark.parametrize("code", ["TIMEOUT", "ERR_STALLED", "ERR_NO_FLOW", "ERR_SENSOR", "OVERSHOOT", "MAX_PULSES"])
def test_result_codes_are_passed_through(w, code):
    w.sim.grind_outcome = code
    w.sim.grind_final_offset_tenths = 3
    w.sim.touch_start(1)
    result = w.do(w.client.wait_result(20000))
    assert result["code"] == code and result["weightSettled"] == (code in ("OVERSHOOT", "MAX_PULSES"))


# ---- link drop, result catch-up ----------------------------------------------------------------------------------

def test_result_get_before_any_result_is_not_found(w):
    assert refused(w.client.result_get()).code == "NOT_FOUND"


def test_link_drop_during_a_grind_and_result_get_after_reconnect(w):
    out = w.do(w.client.start_and_confirm(1))
    assert out.started
    w.sim.drop_link()
    assert not w.client.connected
    with pytest.raises(gc.GlinkDisconnected):
        w.do(w.client.profile_get())
    w.wait(10000)  # the grind continues without the client and ends
    assert w.sim.result is not None and w.client.results == []
    w.do(w.client.connect())
    assert w.client.period["resultSeq"] == 1 and w.client.last_result_seq == 0
    result = w.do(w.client.fetch_missed_result())
    assert result["seq"] == 1 and result["code"] == "OK" and result["finalWeight"] == 18.0
    assert w.do(w.client.fetch_missed_result()) is None  # nothing newer
    assert w.client.profiles_dirty  # first periodInfo of the new connection


def test_a_link_drop_while_waiting_raises(w):
    w.do(w.client.start(1))
    w.sim.drop_link()
    with pytest.raises(gc.GlinkDisconnected):
        w.do(w.client.wait_result(5000))


def test_the_grinder_refuses_connections_during_an_update():
    w = World()
    w.do(w.client.disconnect())
    w.sim.set_screen(Screen.UPDATING)
    with pytest.raises(ConnectionRefusedError):
        w.do(w.client.connect())


def test_cadence_heartbeat_and_grinding_rate():
    w = World(mtu=23)
    n = w.client.period_count
    w.wait(6100)
    assert w.client.period_count - n == 3  # every 2 s while nothing changes
    w.sim.touch_start(1)
    w.wait(100)
    n = w.client.period_count
    w.wait(2000)
    assert 1 <= w.client.period_count - n <= 3  # at most 1 per second at a small MTU
    fast = World(mtu=185)
    fast.sim.touch_start(1)
    fast.wait(100)
    n = fast.client.period_count
    fast.wait(2000)
    assert 6 <= fast.client.period_count - n <= 9  # 4 per second


# ---- conformance vectors through the simulator -------------------------------------------------------------------

class Probe:
    """Raw client for the vectors: collects complete messages the grinder sends."""

    def __init__(self, sim):
        self.messages = []
        self._reasm = codec.Reassembler(codec.MAX_MESSAGE_BYTES)
        self.session = sim.accept(self._on_notify, None, 23)

    def _on_notify(self, packet):
        if self._reasm.feed(packet, 0) == codec.COMPLETE:
            self.messages.append(self._reasm.message.decode("utf-8"))

    def send(self, text):
        for packet in codec.fragments(text.encode("utf-8"), 1, 20):
            self.session.receive(packet)


def apply_state(sim, state, request_result):
    """Maps the free-text `state` of a vector pair to simulator settings."""
    if "profile 3 = 18.0 g, no grind size" in state:
        sim.set_profile3(180)
    elif "profile 3 = 18.0 g 14.5 um" in state or "profile 3 set" in state:
        sim.set_profile3(180, 145, "um")
    if state == "handshaken, MENU":
        sim.set_profile3(180, 145, "um")  # the update vector in the menu edits an existing profile 3
    if "profile 2 = 100.0 g" in state:
        sim.doses[2] = 1000
    if "selected 1" in state:
        sim.selected = 1
    if "a stored change 300 ms ago" in state:
        sim.last_store_ms = sim.clock.now_ms() - 300
    if "flash write fails" in state:
        sim.fail_store = True
    blocks = {"motor disabled": StartBlock.MOTOR_DISABLED, "load cell not connected": StartBlock.LOAD_CELL_NOT_CONNECTED,
              "load cell sends no data": StartBlock.LOAD_CELL_NO_DATA,
              "load cell sample rate invalid": StartBlock.LOAD_CELL_RATE_INVALID}
    for text, block in blocks.items():
        if text in state:
            sim.set_block(block)
    screens = {"MENU": Screen.MENU, "EDIT screen": Screen.EDIT, "calibration screen": Screen.OTHER,
               "grind-complete screen": Screen.DONE, "update in progress": Screen.UPDATING}
    for text, screen in screens.items():
        if text in state:
            sim.set_screen(screen)
    if "GRINDING" in state:
        sim.touch_start(1)
    if "lead-in running" in state:
        sim.begin_lead_in(1)
    if "a grind finished" in state:
        sim.set_last_result(request_result)
    if "UI task does not answer" in state:
        sim.ui_delay_ms = 1500


RESULT_VECTOR = codec.ResultInfo(5, "OK", "BLE", 3, "WEIGHT", 180, 181, True, 2, 14230, 11800)
PAIRS = V["pairs"]


@pytest.mark.parametrize("pair", PAIRS, ids=[p["name"] for p in PAIRS])
def test_vector_pair_through_the_simulator(pair):
    clock = gc.FakeClock(5000)
    sim = glink_sim.GlinkSim(clock)
    probe = Probe(sim)
    state = pair["state"]
    if "no appHello" not in state and state not in ("connected", "connected, TX subscribed"):
        probe.session.handshaken = True
    apply_state(sim, state, RESULT_VECTOR)
    probe.messages.clear()  # broadcasts caused by the set-up
    if "still being processed" in state:
        sim.ui_delay_ms = 500
        probe.send('{"request":{"grindProfile":{"op":"get"}}}')
    if pair.get("request_frames"):
        for frame in pair["request_frames"]:
            probe.session.receive(bytes.fromhex(frame))
    else:
        probe.send(pair["request"])
    clock.advance(1100 if "UI task" in state else 20)
    responses = [m for m in probe.messages if m.startswith('{"response"')]
    if pair["response"] is None:
        assert responses == []
        assert probe.messages == pair["broadcasts"]
    else:
        assert responses[-1 if "still being processed" in state else 0] == pair["response"] or (
            pair["response"] in responses)


def test_period_and_result_vectors_parse_in_the_client(w):
    for case in V["period_cases"]:
        w.client._on_message(case["message"].encode())
        assert w.client.period == json.loads(case["message"])["broadcast"]["periodInfo"]
    for case in V["result_broadcasts"]:
        w.client._on_message(case["message"].encode())
        assert w.client.results[-1] == json.loads(case["message"])["broadcast"]["grindResult"]


def test_unknown_states_and_fields_are_tolerated(w):
    w.client._on_message(b'{"broadcast":{"periodInfo":{"devState":"HIGHSPEEDCLEAN","weight":null,"selectProfile":1,'
                         b'"blocked":"SOMETHING_NEW","resultSeq":0,"profileRev":0,"future":1}}}')
    assert w.client.period["devState"] == "HIGHSPEEDCLEAN" and w.client.connected


# ---- command line ------------------------------------------------------------------------------------------------

def cli(*argv, clock=None):
    out = []
    ns = Namespace(command=argv[0], device="GrindByWeight", address=None, sim=True, select=None, dose=None,
                   grind_size=None, grind_unit=None, clear_grind_size=False, profile=0, expect_dose=None,
                   no_wait=False, timeout=60)
    for a in argv[1:]:
        k, _, v = a.partition("=")
        setattr(ns, k, json.loads(v))
    clock = clock or gc.FakeClock(0)
    code = run(gc.run_command(ns, out.append, clock=clock))
    return code, "\n".join(out)


def test_cli_info_profile_start_stop_against_the_simulator():
    code, out = cli("glink-info")
    assert code == 0 and "7c2a3f10b4d8" in out and "DOUBLE" in out
    code, out = cli("glink-profile", "dose=18.0", "grind_size=14.5", 'grind_unit="um"')
    assert code == 0 and "changed" in out and "14.5 um" in out
    code, out = cli("glink-profile", "select=2")
    assert code == 0 and "selected profile 2" in out
    code, out = cli("glink-start", "profile=1")
    assert code == 0 and "grind started" in out and "result #1: OK" in out
    code, out = cli("glink-stop")
    assert code == 0 and "stop accepted" in out


def test_cli_reports_refusals_with_exit_code_1():
    w_clock = gc.FakeClock(0)
    code, out = cli("glink-profile", "select=2", "dose=18.0", clock=w_clock)  # not RATE_LIMITED
    assert code == 0 and "RATE_LIMITED" not in out
    code, out = cli("glink-profile", "dose=99.0")
    assert code == 1 and "dose" in out
    code, out = cli("glink-start", "profile=3", "expect_dose=18.0")
    assert code == 1 and "PROFILE_NOT_SET" in out


def test_cli_select_then_update_waits_one_second():
    clock = gc.FakeClock(0)
    out = []
    ns = Namespace(command="glink-profile", device="x", address=None, sim=True, select=2, dose=18.0, grind_size=None,
                   grind_unit=None, clear_grind_size=False)
    sim_holder = {}
    real = gc.build_transport

    def spy(args, c):
        t = real(args, c)
        sim_holder["sim"] = t.sim
        return t

    with mock.patch.object(gc, "build_transport", spy):
        assert run(gc.run_command(ns, out.append, clock=clock)) == 0
    assert sim_holder["sim"].profile3.exists and "RATE_LIMITED" not in "\n".join(out)


def test_cli_start_without_expect_dose_warns():
    code, out = cli("glink-start", "profile=1", "no_wait=true")
    assert code == 0 and "[WARNING]" in out and "--expect-dose" in out
    code, out = cli("glink-start", "profile=1", "no_wait=true", "expect_dose=18.0")
    assert "[WARNING]" not in out


def test_cli_link_loss_while_waiting_says_the_grind_keeps_running(monkeypatch):
    clock = gc.FakeClock(0)
    out = []
    ns = Namespace(command="glink-start", device="x", address=None, sim=True, profile=1, expect_dose=18.0,
                   no_wait=False, timeout=60)
    real = gc.build_transport

    def spy(args, c):
        t = real(args, c)
        c.add_listener(lambda: t.sim.drop_link() if t.sim.screen == Screen.GRINDING else None)
        return t

    with mock.patch.object(gc, "build_transport", spy):
        code = run(gc.run_command(ns, out.append, clock=clock))
    text = "\n".join(out)
    assert code == 1 and "keeps running" in text and "glink-stop" in text


# ---- review round 1 -----------------------------------------------------------------------------------------------

def test_an_idle_period_before_the_start_response_does_not_decide(w):
    real = w.sim._start

    def start_with_stale_idle(s, req):
        real(s, req)  # the success response, then STARTING
        # a periodInfo that predates the start but is delivered after the response and is the latest state seen:
        # a mark taken before the response would let it decide "not started"
        s.send(codec.encode_period_info(codec.PeriodInfo(dev_state="IDLE")))

    w.sim._start = start_with_stale_idle
    out = w.do(w.client.start_and_confirm(1))
    assert out.started and out.period["devState"] == "GRINDING"


def test_the_firmware_order_parse_before_gate_before_the_handshake():
    w = World(handshake=False)
    e = refused(w.client.call("grindProfile", {"op": "update", "index": 3, "dose": 61.0}))
    assert e.code == "OUT_OF_RANGE"  # doc 4.1 would say INVALID_STATE (gate, step 5, before fields, step 7)
    e = refused(w.client.call("grindProfile", {"op": "get"}))
    assert e.code == "INVALID_STATE"


def test_busy_comes_after_field_checks(w):
    w.sim.ui_delay_ms = 500
    w.do(w.client._send({"request": {"grindProfile": {"op": "get"}}}))
    w.do(w.client._send({"request": {"grindProfile": {"op": "update", "index": 3, "dose": 61.0}}}))
    reply = w.do(w.client._wait(lambda: w.client._take_response("grindProfile"), 1000, "reply"))
    assert reply["error"]["code"] == "OUT_OF_RANGE"


def test_stop_keeps_the_pending_responses_and_checks_the_reply_shape(w):
    w.client._responses.append(("grindProfile", {"result": "success", "data": []}))
    w.do(w.client.stop())
    assert list(w.client._responses) == [("grindProfile", {"result": "success", "data": []})]
    # a start reply is not a stop reply: stop ignores it and waits for its own
    w.client._responses.clear()
    w.client._responses.append(("grindControl", {"result": "success", "data": {"leadInMs": 3000}}))
    w.do(w.client.stop())
    assert [r for r in w.client._responses if r[1].get("data") == {"leadInMs": 3000}]


def test_numbers_are_validated_and_sent_in_one_decimal_form(w):
    for bad in (float("nan"), float("inf"), 4.94, 60.05, 1e22, True, "18"):
        with pytest.raises(gc.GlinkUsageError):
            w.do(w.client.profile_update(dose=bad))
    with pytest.raises(gc.GlinkUsageError):
        w.do(w.client.profile_update(dose=18.0, grind_size=1000.0))
    with pytest.raises(gc.GlinkUsageError):
        w.do(w.client.start(1, expect_dose=61.0))
    w.do(w.client.profile_update(dose=18.04, grind_size=14.55, grind_unit="um"))
    assert w.sim.profile3.dose_tenths == 180 and w.sim.profile3.grind_size_tenths == 146
    text = gc._dumps({"request": {"x": {"a": gc.dose_num(5), "b": gc.size_num(-0.04), "c": gc.dose_num(1.5e1)}}})
    assert text == '{"request":{"x":{"a":5.0,"b":0.0,"c":15.0}}}'


def test_start_timeout_message_says_it_may_have_been_taken():
    clock = gc.FakeClock(0)
    client = gc.GlinkClient(DeadTransport(), clock)
    run(client.connect(handshake=False))
    with pytest.raises(gc.GlinkTimeout, match="may have been taken"):
        run(client.start(1))


def test_oserror_from_the_transport_is_reported_by_the_cli():
    class Refusing(DeadTransport):
        async def connect(self, on_notify, on_disconnect):
            raise OSError("adapter off")

    out = []
    ns = Namespace(command="glink-info", device="x", address=None, sim=False)
    with mock.patch.object(gc, "build_transport", lambda args, c: Refusing()):
        assert run(gc.run_command(ns, out.append, clock=gc.FakeClock(0))) == 1
    assert "adapter off" in out[0]


def test_cli_without_a_grinder_reports_a_timeout():
    clock = gc.FakeClock(0)
    out = []
    ns = Namespace(command="glink-info", device="x", address=None, sim=False)
    with mock.patch.object(gc, "build_transport", lambda args, c: DeadTransport()):
        code = run(gc.run_command(ns, out.append, clock=clock))
    assert code == 1 and "[ERROR]" in out[0]


def test_grinder_py_forwards_the_glink_options():
    import grinder  # noqa: PLC0415

    args = grinder.create_parser().parse_args(
        ["glink-start", "3", "--expect-dose", "18.0", "--no-wait", "--sim", "--timeout", "30"])
    assert (args.command, args.profile, args.expect_dose, args.no_wait, args.sim) == ("glink-start", 3, 18.0, True, True)
    for name in ("glink-info", "glink-profile", "glink-stop"):
        assert grinder.create_parser().parse_args([name]).command == name
    tool = grinder.GrinderTool()
    captured = []

    async def fake_run(cmd, cwd=None):
        captured.append(cmd)
        return 0

    with mock.patch.object(tool, "check_venv", lambda: True), mock.patch.object(tool, "run_async_command", fake_run):
        assert run(tool.cmd_glink(args)) == 0
    cmd = captured[0]
    assert cmd[2:5] == ["glink-start", "3", "--expect-dose"] and "--sim" in cmd and "--no-wait" in cmd

"""Simulated GLink v1 grinder (docs/GLINK-PROTOCOL.md) for tests and adapter-less development.

Implements every object, operation, state and error of the contract on top of glink_codec (frame, reassembler,
request parser and the write / start decisions), plus the lead-in, a simulated grind with the result broadcast, the
handshake timeout, the bad-fragment window and the periodInfo cadence. Time comes from an injected clock
(`now_ms()`, `add_listener()`): the simulator advances whenever the clock does, nothing runs on its own.

Defaults follow tools/tests/glink_vectors.json: build 640, fw 1.4.1, board V2, profiles 9.0 / 18.0 / 21.5 g,
profile 3 not set, profile 1 selected, READY screen, motor and scale ready.
"""

from dataclasses import dataclass

import glink_codec as codec
from glink_codec import Screen, StartBlock

HANDSHAKE_TIMEOUT_MS = 10000
UI_REPLY_TIMEOUT_MS = 1000
PERIOD_HEARTBEAT_MS = 2000
PERIOD_GRINDING_FAST_MS = 250
PERIOD_GRINDING_SLOW_MS = 1000
FAST_MTU_MIN = 100

# (phase, duration in ms) of a simulated weight grind; the weight ramps up to the target by the end of PULSE_EXECUTE
GRIND_PHASES = [("TARING", 300), ("PREDICTIVE", 3000), ("PULSE_DECISION", 100), ("PULSE_EXECUTE", 200),
                ("PULSE_SETTLING", 300), ("FINAL_SETTLING", 500)]
RAMP_END_MS = 300 + 3000 + 100 + 200
GRIND_TOTAL_MS = sum(d for _, d in GRIND_PHASES)


@dataclass
class _Grind:
    profile: int
    origin: str
    target_tenths: int
    start_ms: int


@dataclass
class _Pending:
    due_ms: int
    deadline_ms: int
    request: codec.Request


class SimSession:
    """The one BLE connection of the simulated grinder."""

    def __init__(self, sim, on_notify, on_close, mtu):
        self.sim = sim
        self.on_notify = on_notify
        self.on_close = on_close
        self.mtu = mtu
        self.open = True
        self.handshaken = False
        self.connected_ms = sim.clock.now_ms()
        self.pending = None
        self.reassembler = codec.Reassembler(codec.MAX_REQUEST_BYTES)
        self.bad_frames = codec.BadFrameWindow()
        self.tx_seq = 0

    @property
    def packet_bytes(self):
        return codec.DEFAULT_PACKET_BYTES if not self.mtu or self.mtu <= 23 else min(self.mtu - 3, codec.MAX_PACKET_BYTES)

    def send(self, text: str):
        """One message to the client, fragmented to the packet size of the MTU."""
        if not self.open or not text:
            return
        self.tx_seq = self.tx_seq % 0xFFFF + 1
        for packet in codec.fragments(text.encode("utf-8"), self.tx_seq, self.packet_bytes):
            self.on_notify(packet)

    def receive(self, packet: bytes):
        """One RX write (one fragment)."""
        if not self.open:
            raise ConnectionError("link closed")
        now = self.sim.clock.now_ms()
        outcome = self.reassembler.feed(packet, now)
        if outcome == codec.BAD_FRAGMENT:
            if self.bad_frames.record(now):
                self.close()
        elif outcome == codec.PAYLOAD_TOO_LARGE:
            self.send(codec.encode_error(None, "PAYLOAD_TOO_LARGE", codec.MSG["TOO_LARGE"]))
        elif outcome == codec.COMPLETE:
            self.sim._handle_message(self, self.reassembler.message)

    def close(self):
        """7.4: every disconnect clears the per-connection state."""
        if not self.open:
            return
        self.open = False
        self.handshaken = False
        self.pending = None
        self.reassembler.reset()
        self.bad_frames.reset()
        if self.sim.session is self:
            self.sim.session = None
        if self.on_close:
            self.on_close()


class GlinkSim:
    def __init__(self, clock):
        self.clock = clock
        self.info = codec.BaseInfo(640, "7c2a3f10b4d8", "1.4.1", "V2")
        self.screen = Screen.READY
        self.block = StartBlock.NONE
        self.doses = [90, 180, 215]  # tenths of profiles 0 to 2
        self.profile3 = codec.ProfileState()
        self.selected = 1
        self.profile_rev = 0
        self.last_store_ms = None
        self.fail_store = False  # the next stored change fails with STORE_FAILED
        self.ui_delay_ms = 0  # the UI task answers this late (more than 1000: UI_BUSY, request not applied)
        self.result = None
        self.weight_tenths = 0
        self.weight_valid = True
        self.grind_outcome = "OK"  # result code a simulated grind ends with
        self.grind_final_offset_tenths = 0
        self.session = None
        self._grind = None
        self._lead_in_end = 0
        self._lead_in_profile = 0
        self._lead_in_origin = "BLE"
        self._last_sig = None
        self._last_period_ms = 0
        clock.add_listener(self.tick)

    # ---- test / bench controls (what a person or the grinder itself does) ----

    def accept(self, on_notify, on_close=None, mtu=23) -> SimSession:
        """A client connects. Refused while an update runs or another client is connected."""
        if self.screen == Screen.UPDATING:
            raise ConnectionRefusedError("update in progress")
        if self.session is not None:
            raise ConnectionRefusedError("one connection at a time")
        self.session = SimSession(self, on_notify, on_close, mtu)
        return self.session

    def drop_link(self):
        if self.session:
            self.session.close()

    def set_screen(self, screen: Screen):
        """The grinder moves to another screen; a pending lead-in is dropped silently (5.5)."""
        if self.screen == Screen.LEAD_IN and screen != Screen.LEAD_IN:
            self._lead_in_end = 0
        self.screen = screen
        self._publish()

    def set_block(self, block: StartBlock):
        self.block = block
        self._publish()

    def set_profile3(self, dose_tenths, size_tenths=None, unit=""):
        self.profile3 = codec.ProfileState(True, dose_tenths, size_tenths is not None, size_tenths or 0, unit)
        self.profile_rev += 1
        self._publish()

    def set_last_result(self, result: codec.ResultInfo):
        self.result = result

    def touch(self):
        """A touch on the grinder: cancels the lead-in, stops a grind, acknowledges DONE and dialogs."""
        if self.screen == Screen.LEAD_IN:
            self.screen = Screen.READY
        elif self.screen == Screen.GRINDING:
            self._finish_grind("STOPPED")
        elif self.screen in (Screen.DONE, Screen.OTHER):
            self.screen = Screen.READY
            self.weight_tenths = 0
        self._publish()

    def begin_lead_in(self, profile: int, origin: str = "BLE"):
        self.screen = Screen.LEAD_IN
        self._lead_in_end = self.clock.now_ms() + codec.START_LEAD_IN_MS
        self._lead_in_profile = profile
        self._lead_in_origin = origin
        self._publish()

    def touch_start(self, profile: int):
        """A grind started on the touchscreen (origin TOUCH), without lead-in."""
        self._begin_grind(profile, "TOUCH")
        self._publish()

    def stored_dose(self, profile: int) -> int:
        return self.profile3.dose_tenths if profile == codec.PROFILE_WRITABLE_INDEX else self.doses[profile]

    # ---- time ----

    def tick(self):
        now = self.clock.now_ms()
        s = self.session
        if s and not s.handshaken and now - s.connected_ms > HANDSHAKE_TIMEOUT_MS:
            s.close()
        s = self.session
        if s and s.pending and now >= min(s.pending.due_ms, s.pending.deadline_ms):
            self._resolve_pending(s, now)
        if self.screen == Screen.LEAD_IN and now >= self._lead_in_end:
            self._end_lead_in()
        if self.screen == Screen.GRINDING:
            self._advance_grind(now)
        self._publish()

    def _end_lead_in(self):
        """5.5: the decision is made again at the end of the lead-in."""
        profile = self._lead_in_profile
        self.screen = Screen.READY
        if self.block != StartBlock.NONE:
            self.screen = Screen.OTHER  # the grinder's confirmation dialog (SETTING, blocked NOT_READY)
        elif profile == codec.PROFILE_WRITABLE_INDEX and not self.profile3.exists:
            pass  # dropped silently
        else:
            self._begin_grind(profile, self._lead_in_origin)

    def _begin_grind(self, profile, origin):
        self.screen = Screen.GRINDING
        self._grind = _Grind(profile, origin, self.stored_dose(profile), self.clock.now_ms())
        self.weight_tenths = 0

    def _phase(self, elapsed):
        t = 0
        for name, duration in GRIND_PHASES:
            t += duration
            if elapsed < t:
                return name
        return GRIND_PHASES[-1][0]

    def _advance_grind(self, now):
        g = self._grind
        elapsed = now - g.start_ms
        self.weight_tenths = round(g.target_tenths * min(1.0, elapsed / RAMP_END_MS))
        if elapsed >= GRIND_TOTAL_MS:
            self._finish_grind(self.grind_outcome)

    def _finish_grind(self, code):
        g = self._grind
        elapsed = self.clock.now_ms() - g.start_ms
        if code == "STOPPED":
            final = self.weight_tenths
        else:
            final = g.target_tenths + self.grind_final_offset_tenths
            self.weight_tenths = final
        seq = (self.result.seq if self.result else 0) + 1
        self.result = codec.ResultInfo(seq, code, g.origin, g.profile, "WEIGHT", g.target_tenths, final,
                                       codec.result_weight_settled(code), 1 if code != "STOPPED" else 0, elapsed,
                                       max(0, elapsed - 800))
        self._grind = None
        self.screen = Screen.DONE
        s = self.session
        if s and s.handshaken:
            s.send(codec.encode_result_broadcast(self.result))

    # ---- periodInfo ----

    def _period(self) -> codec.PeriodInfo:
        now = self.clock.now_ms()
        state = codec.dev_state(self.screen, self.block)
        selected = self.selected
        if selected == codec.PROFILE_WRITABLE_INDEX and not self.profile3.exists:
            selected = 0
        p = codec.PeriodInfo(dev_state=state, weight_valid=self.weight_valid, weight_tenths=self.weight_tenths,
                             select_profile=selected, blocked=codec.blocked_for(state, self.block),
                             result_seq=self.result.seq if self.result else 0, profile_rev=self.profile_rev)
        if state == "GRINDING":
            p.phase = self._phase(now - self._grind.start_ms)
            p.has_target, p.target_tenths = True, self._grind.target_tenths
        elif state == "STARTING":
            p.has_target, p.target_tenths = True, self.stored_dose(self._lead_in_profile)
            p.has_lead_in, p.lead_in_ms = True, max(0, self._lead_in_end - now)
        return p

    def _publish(self, force=False):
        """Section 8: on change (at most every 250 ms / 1 s while GRINDING), while GRINDING at that cadence, else
        every 2 s."""
        s = self.session
        if not s or not s.handshaken:
            return
        now = self.clock.now_ms()
        p = self._period()
        sig = (p.dev_state, p.phase, p.select_profile, p.blocked, p.result_seq, p.profile_rev)
        grinding = p.dev_state == "GRINDING"
        interval = PERIOD_GRINDING_FAST_MS if s.mtu >= FAST_MTU_MIN else PERIOD_GRINDING_SLOW_MS
        since = now - self._last_period_ms
        if force:
            send = True
        elif sig != self._last_sig:
            send = not (grinding and self._last_sig and self._last_sig[0] == p.dev_state and since < interval)
        elif grinding:
            send = since >= interval
        else:
            send = since >= PERIOD_HEARTBEAT_MS
        if send:
            self._last_sig = sig
            self._last_period_ms = now
            s.send(codec.encode_period_info(p))

    # ---- requests ----

    def _handle_message(self, s: SimSession, data: bytes):
        req, fail = codec.parse_request(data)
        if fail is not None:
            # 4.1 steps 1 to 5: parse with all field checks first, then the gate (6) and the pending check (7).
            s.send(codec.encode_failure(fail))
            return
        if req.object == "grindControl" and req.op == "stop":
            self._do_stop(s)
            self._publish()
            return
        gate_fail = codec.gate(s.handshaken, s.pending is not None, req)
        if gate_fail:
            s.send(codec.encode_failure(gate_fail))
            return
        if self.ui_delay_ms > 0 and req.object != "appHello":
            now = self.clock.now_ms()
            s.pending = _Pending(now + self.ui_delay_ms, now + UI_REPLY_TIMEOUT_MS, req)
            return
        self._execute(s, req)
        self._publish()

    def _resolve_pending(self, s: SimSession, now: int):
        pending = s.pending
        s.pending = None
        if pending.due_ms <= pending.deadline_ms:
            self._execute(s, pending.request)
        else:  # the BLE task answers UI_BUSY and the UI task must not apply the request afterwards
            s.send(codec.encode_error(pending.request.object, "UI_BUSY", codec.MSG["UI_BUSY"]))

    def _fail(self, s, code, key, field="", obj=None):
        s.send(codec.encode_error(obj, code, codec.MSG[key], field))

    def _execute(self, s: SimSession, req: codec.Request):
        if req.object == "appHello":
            s.handshaken = True
            s.send(codec.encode_base_info(self.info))
            self._publish(force=True)
        elif req.object == "grindProfile":
            {"get": self._profile_get, "select": self._profile_select, "update": self._profile_update}[req.op](s, req)
        elif req.object == "grindControl":
            self._start(s, req)
        else:
            self._result_get(s)

    def _entries(self):
        out = [codec.ProfileEntry(i, self.doses[i]) for i in range(3)]
        if self.profile3.exists:
            out.append(self._entry3())
        return out

    def _entry3(self):
        p = self.profile3
        return codec.ProfileEntry(3, p.dose_tenths, p.has_grind_size, p.grind_size_tenths, p.grind_unit)

    def _profile_get(self, s, req):
        f = codec.state_check(self.screen, codec.OpClass.GET, "grindProfile")
        if f:
            s.send(codec.encode_failure(f))
        elif not req.selector_index:
            s.send(codec.encode_profile_list(self._entries()))
        elif req.selector_value < 3:
            s.send(codec.encode_profile_entry(codec.ProfileEntry(req.selector_value, self.doses[req.selector_value])))
        elif self.profile3.exists:
            s.send(codec.encode_profile_entry(self._entry3()))
        else:
            self._fail(s, "NOT_FOUND", "PROFILE_NOT_SET", "selector.value", "grindProfile")

    def _interval_ok(self):
        return codec.write_interval_ok(self.last_store_ms is not None, self.last_store_ms or 0, self.clock.now_ms())

    def _profile_select(self, s, req):
        will_store, f = codec.decide_select(self.screen, req.index, self.profile3.exists, self.selected,
                                            self._interval_ok())
        if f:
            s.send(codec.encode_failure(f))
            return
        if will_store:
            if self.fail_store:
                self.fail_store = False
                self._fail(s, "STORE_FAILED", "STORE_FAILED", obj="grindProfile")
                return
            self.selected = req.index
            self.last_store_ms = self.clock.now_ms()
        s.send(codec.encode_select(self.selected))

    def _profile_update(self, s, req):
        plan, f = codec.decide_update(self.screen, self.profile3, req, self._interval_ok())
        if f:
            s.send(codec.encode_failure(f))
            return
        if plan.changed:
            if self.fail_store:
                self.fail_store = False
                self._fail(s, "STORE_FAILED", "STORE_FAILED", obj="grindProfile")
                return
            r = plan.result
            self.profile3 = codec.ProfileState(True, r.dose_tenths, r.has_grind_size, r.grind_size_tenths, r.grind_unit)
            self.profile_rev += 1
            self.last_store_ms = self.clock.now_ms()
        s.send(codec.encode_profile_update(plan.result, plan.changed, plan.changed))

    def _start(self, s, req):
        inputs = codec.StartInputs(req.index != 3 or self.profile3.exists, self.stored_dose(req.index),
                                   req.has_expect_dose, req.expect_dose_tenths, self.block)
        f = codec.decide_start(self.screen, inputs)
        if f:
            s.send(codec.encode_failure(f))
            return
        s.send(codec.encode_start())  # the response first, then the STARTING periodInfo
        self.begin_lead_in(req.index)

    def _do_stop(self, s):
        """Cancels a lead-in, stops a grind (result STOPPED); always answers accepted."""
        s.send(codec.encode_stop())
        if self.screen == Screen.LEAD_IN:
            self.screen = Screen.READY
        elif self.screen == Screen.GRINDING:
            self._finish_grind("STOPPED")

    def _result_get(self, s):
        f = codec.state_check(self.screen, codec.OpClass.GET, "grindResult")
        if f:
            s.send(codec.encode_failure(f))
        elif self.result is None:
            self._fail(s, "NOT_FOUND", "NO_RESULT", obj="grindResult")
        else:
            s.send(codec.encode_result_get(self.result))


class SimTransport:
    """Client-side transport that connects to a GlinkSim in the same process."""

    def __init__(self, sim: GlinkSim, mtu: int = 23):
        self.sim = sim
        self.mtu = mtu
        self.session = None

    async def connect(self, on_notify, on_disconnect):
        self.session = self.sim.accept(on_notify, on_disconnect, self.mtu)
        return self.mtu

    async def write(self, packet: bytes):
        self.session.receive(packet)

    async def disconnect(self):
        if self.session:
            self.session.close()

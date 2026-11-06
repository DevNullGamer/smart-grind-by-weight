"""GLink v1 client (docs/GLINK-PROTOCOL.md): handshake, one request at a time, response matching, timeouts, start / stop
with the lead-in decision, result catch-up after a link drop.

Built on glink_codec (frame, reassembler). Nothing here imports Bluetooth at module level: the client talks to a
`Transport`, and `BleakTransport` imports bleak only inside connect(). `glink_sim.SimTransport` is the in-process
transport used for tests and for `--sim`. The clock is injected (`RealClock`, or `FakeClock` in tests): every wait
polls the clock, so timeouts run without real time.
"""

import asyncio
import json
import math
import time
from decimal import ROUND_HALF_UP, Decimal
from collections import deque

import glink_codec as codec

SERVICE_UUID = "53474257-0001-4632-901b-4752494e4421"
RX_UUID = "53474257-0002-4632-901b-4752494e4421"
TX_UUID = "53474257-0003-4632-901b-4752494e4421"
DEVICE_NAME = "GrindByWeight"

RESPONSE_TIMEOUT_MS = 5000  # 7.5: wait up to 5 s for a response
PERIOD_TIMEOUT_MS = 5000  # 2: no periodInfo within 5 s means the channel is not ready
START_OUTCOME_EXTRA_MS = 2000  # 5.5: leadInMs plus 2 s
POLL_MS = 10


# ---- errors ------------------------------------------------------------------------------------------------------

class GlinkError(Exception):
    """Base class of everything the client raises."""


class GlinkTimeout(GlinkError):
    pass


class GlinkDisconnected(GlinkError):
    pass


class GlinkProtocolError(GlinkError):
    pass


class GlinkConnectionError(GlinkError):
    """The Bluetooth layer failed (no adapter, device gone, connect or write error)."""


class GlinkUsageError(GlinkError, ValueError):
    """A value the client refuses to send (out of range, not a finite number)."""


class Num:
    """A number sent in a fixed one-decimal form (4.3): no exponent, never NaN or infinity."""

    def __init__(self, value, lo: str, hi: str, name: str):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise GlinkUsageError("%s must be a finite number" % name)
        d = Decimal(repr(value)).quantize(Decimal("0.1"), rounding=ROUND_HALF_UP)  # half away from zero (4.4)
        if d == 0:
            d = Decimal("0.0")
        if d < Decimal(lo) or d > Decimal(hi):
            raise GlinkUsageError("%s must be %s to %s" % (name, lo, hi))
        self.token = format(d, "f")


def dose_num(value, name="dose"):
    return Num(value, "5.0", "60.0", name)


def size_num(value):
    return Num(value, "0.0", "999.9", "grindSize")


class GlinkRefused(GlinkError):
    """The grinder answered `result: fail`. `code` is the error code of 6.4."""

    def __init__(self, code, message="", field="", obj=""):
        super().__init__("%s: %s%s" % (code, message, " (field %s)" % field if field else ""))
        self.code = code
        self.message = message
        self.field = field
        self.object = obj


# ---- clocks ------------------------------------------------------------------------------------------------------

class RealClock:
    """Wall clock; sleep_ms really sleeps. Listeners (the simulator's tick) run after each sleep."""

    def __init__(self):
        self._listeners = []

    def now_ms(self) -> int:
        return int(time.monotonic() * 1000)

    def add_listener(self, fn):
        self._listeners.append(fn)

    async def sleep_ms(self, ms: int):
        await asyncio.sleep(ms / 1000.0)
        for fn in list(self._listeners):
            fn()


class FakeClock:
    """Deterministic clock: sleep_ms advances time at once, in steps of at most 10 ms, running the listeners at
    every step, so a simulated grinder sees exact timing."""

    def __init__(self, start_ms: int = 0):
        self._now = start_ms
        self._listeners = []

    def now_ms(self) -> int:
        return self._now

    def add_listener(self, fn):
        self._listeners.append(fn)

    def advance(self, ms: int):
        while ms > 0:
            step = min(ms, 10)
            self._now += step
            ms -= step
            for fn in list(self._listeners):
                fn()

    async def sleep_ms(self, ms: int):
        self.advance(ms)  # never suspends, so a test can drive the client without an event loop


# ---- transports --------------------------------------------------------------------------------------------------

def packet_bytes_for_mtu(mtu) -> int:
    """3.2: min(ATT MTU - 3, 512); 20 bytes when the MTU is unknown or at the default of 23."""
    if not mtu or mtu <= 23:
        return codec.DEFAULT_PACKET_BYTES
    return min(mtu - 3, codec.MAX_PACKET_BYTES)


class BleakTransport:
    """The real grinder over bleak. bleak is imported inside connect() only."""

    def __init__(self, name: str = DEVICE_NAME, address: str = None, scan_timeout: float = 20.0):
        self.name = name
        self.address = address
        self.scan_timeout = scan_timeout
        self._client = None

    async def connect(self, on_notify, on_disconnect):
        from bleak import BleakClient, BleakError, BleakScanner  # noqa: PLC0415 (keeps bleak out of module import)

        try:
            device = self.address
            if device is None:
                device = await BleakScanner.find_device_by_name(self.name, timeout=self.scan_timeout)
                if device is None:
                    raise GlinkConnectionError("device %r not found" % self.name)
            self._client = BleakClient(device, disconnected_callback=lambda _c: on_disconnect())
            await self._client.connect()
        except (BleakError, OSError, asyncio.TimeoutError) as e:
            raise GlinkConnectionError("Bluetooth connect failed: %s" % (e or type(e).__name__)) from e
        try:
            rx = self._client.services.get_characteristic(RX_UUID)
            tx = self._client.services.get_characteristic(TX_UUID)
            if rx is None or tx is None or "notify" not in tx.properties or not (
                    "write" in rx.properties or "write-without-response" in rx.properties):
                raise GlinkProtocolError("GLink service not found on the device (firmware without GaggiMate link?)")
            await self._client.start_notify(TX_UUID, lambda _c, data: on_notify(bytes(data)))
            return getattr(self._client, "mtu_size", None)
        except BaseException as e:  # any failure after connect: do not leave the single connection occupied
            try:
                await self._client.disconnect()
            except Exception:  # noqa: BLE001 (best effort)
                pass
            if isinstance(e, (BleakError, OSError, asyncio.TimeoutError)):
                raise GlinkConnectionError("Bluetooth setup failed: %s" % (e or type(e).__name__)) from e
            raise

    async def write(self, packet: bytes):
        from bleak import BleakError  # noqa: PLC0415

        try:
            await self._client.write_gatt_char(RX_UUID, packet, response=True)
        except (BleakError, OSError, asyncio.TimeoutError) as e:
            raise GlinkConnectionError("Bluetooth write failed: %s" % (e or type(e).__name__)) from e

    async def disconnect(self):
        if self._client is not None and self._client.is_connected:
            await self._client.disconnect()


# ---- client ------------------------------------------------------------------------------------------------------

class StartOutcome:
    """Result of start_and_confirm: `started` per 5.5 (first periodInfo that is not STARTING says GRINDING)."""

    def __init__(self, started: bool, lead_in_ms: int, period: dict):
        self.started = started
        self.lead_in_ms = lead_in_ms
        self.period = period


class GlinkClient:
    def __init__(self, transport, clock=None, packet_bytes: int = None):
        self.transport = transport
        self.clock = clock or RealClock()
        self._forced_packet_bytes = packet_bytes
        self.packet_bytes = packet_bytes or codec.DEFAULT_PACKET_BYTES
        self.connected = False
        self.base_info = None
        self.period = None
        self.period_count = 0
        self.profiles_dirty = True  # 11.7: the first periodInfo of a connection counts as a change
        self.last_result_seq = 0
        self.results = []  # grindResult broadcasts (and fetched results), in arrival order
        self.bad_fragments = 0
        self.broadcasts = deque(maxlen=50)
        self._reasm = codec.Reassembler(codec.MAX_MESSAGE_BYTES)
        self._responses = deque()
        self._pending = None
        self._hello_error = None
        self._tx_seq = 0
        self._profile_rev = None

    # -- connection --

    async def connect(self, handshake: bool = True):
        self._reset_connection_state()
        mtu = await self.transport.connect(self._on_notify, self._on_disconnect)
        self.connected = True
        self.packet_bytes = self._forced_packet_bytes or packet_bytes_for_mtu(mtu)
        if handshake:
            await self.handshake()

    async def handshake(self):
        """appHello, then baseInfo and periodInfo (the first periodInfo within 5 s means the channel is ready)."""
        self.base_info = None
        self._hello_error = None
        self._responses.clear()
        mark = self.period_count
        await self._send({"request": {"appHello": {"op": "handshake"}}})
        # a failure of appHello arrives as a response
        await self._wait(lambda: self._failed_hello() or (self.base_info is not None and self.period_count > mark),
                         PERIOD_TIMEOUT_MS, "baseInfo and periodInfo after appHello")
        if self._hello_error:
            raise self._hello_error
        if self.base_info.get("device") != "SGBW" or self.base_info.get("proto") != codec.PROTO_VERSION:
            raise GlinkProtocolError("not a GLink v1 grinder: %r" % (self.base_info,))

    async def disconnect(self):
        await self.transport.disconnect()
        self.connected = False

    def _reset_connection_state(self):
        self._reasm.reset()
        self._responses.clear()
        self._pending = None
        self._hello_error = None
        self.base_info = None
        self.period = None
        self._profile_rev = None
        self.profiles_dirty = True

    def _on_disconnect(self):
        self.connected = False

    # -- receive --

    def _on_notify(self, packet: bytes):
        outcome = self._reasm.feed(packet, self.clock.now_ms())
        if outcome in (codec.BAD_FRAGMENT, codec.PAYLOAD_TOO_LARGE):
            self.bad_fragments += 1
        elif outcome == codec.COMPLETE:
            self._on_message(self._reasm.message)

    def _on_message(self, data: bytes):
        try:
            message = json.loads(data.decode("utf-8"))
        except ValueError:
            self.bad_fragments += 1
            return
        if not isinstance(message, dict) or len(message) != 1:
            return
        kind, body = next(iter(message.items()))
        if not isinstance(body, dict):
            return
        if kind == "response":
            self._responses.extend(body.items())
        elif kind == "broadcast":
            for name, payload in body.items():
                self._on_broadcast(name, payload)

    def _on_broadcast(self, name, payload):
        if not isinstance(payload, dict):
            return
        self.broadcasts.append((name, payload))
        if name == "baseInfo":
            self.base_info = payload
        elif name == "periodInfo":
            self.period = payload
            self.period_count += 1
            rev = payload.get("profileRev")
            if rev != self._profile_rev:
                self.profiles_dirty = True
            self._profile_rev = rev
        elif name == "grindResult":
            self._note_result(payload)

    def _note_result(self, result):
        seq = result.get("seq", 0)
        if seq > self.last_result_seq:
            self.last_result_seq = seq
        self.results.append(result)

    # -- send / wait --

    async def _send(self, message: dict):
        data = _dumps(message).encode("utf-8")
        if len(data) > codec.MAX_REQUEST_BYTES:
            raise ValueError("request is %d bytes, the limit is %d" % (len(data), codec.MAX_REQUEST_BYTES))
        self._tx_seq = self._tx_seq % 0xFFFF + 1
        for packet in codec.fragments(data, self._tx_seq, self.packet_bytes):
            await self.send_packet(packet)

    async def send_packet(self, packet: bytes):
        """One raw RX write (also used by tests to inject bad fragments)."""
        if not self.connected:
            raise GlinkDisconnected("not connected")
        await self.transport.write(packet)

    async def _wait(self, predicate, timeout_ms: int, what: str):
        deadline = self.clock.now_ms() + timeout_ms
        while True:
            value = predicate()
            if value:
                return value
            if not self.connected:
                raise GlinkDisconnected("link lost while waiting for " + what)
            if self.clock.now_ms() >= deadline:
                raise GlinkTimeout("timeout waiting for " + what)
            await self.clock.sleep_ms(POLL_MS)

    def _failed_hello(self):
        # an appHello failure is a response; there is no success response
        while self._responses:
            name, body = self._responses.popleft()
            if name in ("appHello", "error"):
                self._hello_error = _refusal(name, body)
                return True
        return False

    def _take_response(self, obj, accept=None):
        # 7.5: ignore a response whose object name does not match the pending request ("error" matches any)
        kept = deque()
        found = None
        while self._responses:
            name, body = self._responses.popleft()
            if found is None and (name == obj or name == "error") and (accept is None or accept(body)):
                found = body
            else:
                kept.append((name, body))  # not ours (another request's reply, e.g. while a stop is sent)
        self._responses = kept
        return found

    async def request(self, obj: str, body: dict, timeout_ms: int = RESPONSE_TIMEOUT_MS, track: bool = True,
                      accept=None) -> dict:
        """One request; returns the response member (`{"result": ..., ...}`) without interpreting it. An untracked
        request (stop) keeps the responses of a pending tracked one; `accept` picks its reply among the same object."""
        if track:
            if self._pending is not None:
                raise GlinkError("a request is already pending (%s)" % self._pending)
            self._pending = obj
        try:
            if track:
                self._responses.clear()
            await self._send({"request": {obj: body}})
            return await self._wait(lambda: self._take_response(obj, accept), timeout_ms, "the %s response" % obj)
        finally:
            if track:
                self._pending = None

    async def call(self, obj: str, body: dict, timeout_ms: int = RESPONSE_TIMEOUT_MS):
        """request(), raising GlinkRefused on `result: fail`; returns `data` on success."""
        reply = await self.request(obj, body, timeout_ms)
        if reply.get("result") != "success":
            raise _refusal(obj, reply)
        return reply.get("data")

    # -- grindProfile --

    async def profile_get(self, index: int = None):
        """All profiles (a list) or one entry."""
        body = {"op": "get"}
        if index is not None:
            body["selector"] = {"type": "index", "value": index}
        data = await self.call("grindProfile", body)
        if index is None:
            self.profiles_dirty = False
        return data

    async def profile_select(self, index: int) -> int:
        return (await self.call("grindProfile", {"op": "select", "index": index}))["selectProfile"]

    async def profile_update(self, dose=None, grind_size=None, grind_unit=None, index: int = 3,
                             clear_grind_size: bool = False, clear_grind_unit: bool = False):
        """Update profile 3. Absent fields stay unchanged; `clear_*` sends null."""
        body = {"op": "update", "index": index}
        if dose is not None:
            body["dose"] = dose_num(dose)
        if clear_grind_size:
            body["grindSize"] = None
        elif grind_size is not None:
            body["grindSize"] = size_num(grind_size)
        if clear_grind_unit:
            body["grindUnit"] = None
        elif grind_unit is not None:
            body["grindUnit"] = grind_unit
        return await self.call("grindProfile", body)

    # -- grindControl (provisional, keep replaceable: 5.5) --

    async def start(self, profile: int, expect_dose=None) -> int:
        """Sends start; returns leadInMs. Use start_and_confirm() to learn whether the grind began."""
        body = {"op": "start", "profile": profile}
        if expect_dose is not None:
            body["expectDose"] = dose_num(expect_dose, "expectDose")
        try:
            return (await self.call("grindControl", body))["leadInMs"]
        except GlinkTimeout as e:
            raise GlinkTimeout("no answer to start: it may have been taken. Check the state (glink-info) or "
                               "stop it (glink-stop). " + str(e)) from e

    async def start_and_confirm(self, profile: int, expect_dose=None) -> StartOutcome:
        """5.5: after the success response the first periodInfo whose devState is not STARTING decides. None within
        leadInMs plus 2 s: treated as not started (`period` is then the latest state seen)."""
        lead_in = await self.start(profile, expect_dose)
        mark = self.period_count  # only a periodInfo after the success response counts, not an older IDLE one
        timeout = lead_in + START_OUTCOME_EXTRA_MS
        try:
            period = await self._wait(
                lambda: self.period if self.period_count > mark and self.period.get("devState") != "STARTING" else None,
                timeout, "the end of the lead-in")
        except GlinkTimeout:
            return StartOutcome(False, lead_in, self.period)
        return StartOutcome(period.get("devState") == "GRINDING", lead_in, period)

    async def stop(self) -> bool:
        """Always accepted, also before the handshake and while another request is pending."""
        def is_stop_reply(body):
            data = body.get("data")
            return body.get("result") != "success" or (isinstance(data, dict) and "accepted" in data)

        reply = await self.request("grindControl", {"op": "stop"}, track=False, accept=is_stop_reply)
        if reply.get("result") != "success":
            raise _refusal("grindControl", reply)
        return bool(reply["data"]["accepted"])

    # -- grindResult --

    async def result_get(self):
        data = await self.call("grindResult", {"op": "get"})
        self._note_result(data)
        return data

    async def wait_result(self, timeout_ms: int, after_seq: int = None):
        """Next grindResult broadcast newer than `after_seq` (default: the last one seen)."""
        after = self.last_result_seq if after_seq is None else after_seq
        return await self._wait(lambda: next((r for r in self.results if r.get("seq", 0) > after), None),
                                timeout_ms, "a grindResult")

    async def fetch_missed_result(self):
        """After a (re)connect: when periodInfo.resultSeq is newer than the last seq seen, fetch it with get."""
        if self.period is None or self.period.get("resultSeq", 0) <= self.last_result_seq:
            return None
        return await self.result_get()


def _dumps(message: dict) -> str:
    """Compact JSON; `Num` values are written as their fixed one-decimal token."""
    tokens = []

    def walk(v):
        if isinstance(v, Num):
            tokens.append(v.token)
            return "\x00num%d\x00" % (len(tokens) - 1)
        if isinstance(v, dict):
            return {k: walk(x) for k, x in v.items()}
        return v

    text = json.dumps(walk(message), separators=(",", ":"))
    for i, token in enumerate(tokens):
        text = text.replace('"\\u0000num%d\\u0000"' % i, token)
    return text


def _refusal(obj, reply) -> GlinkRefused:
    err = reply.get("error") or {}
    return GlinkRefused(err.get("code", "UNKNOWN"), err.get("message", ""), err.get("field", ""), obj)


# ---- command line (grinder-ble.py glink-* commands) --------------------------------------------------------------

def _fmt_entry(e) -> str:
    size = "-" if e.get("grindSize") is None else "%s %s" % (e["grindSize"], e.get("grindUnit") or "")
    return "  %d %-8s dose %5.1f g  grind size %s%s" % (e["index"], e["name"], e["dose"], size,
                                                         "  (writable)" if e.get("writable") else "")


def _print_state(out, client):
    p = client.period or {}
    out("state: %s  weight: %s g  profile: %s  blocked: %s  results: %s" % (
        p.get("devState"), p.get("weight"), p.get("selectProfile"), p.get("blocked"), p.get("resultSeq")))


def _print_result(out, r):
    out("result #%s: %s  profile %s  target %s  final %s g  pulses %s  %s ms  (%s)" % (
        r["seq"], r["code"], r["profile"], r["target"], r["finalWeight"], r["pulses"], r["durationMs"], r["origin"]))


def build_transport(args, clock):
    """BleakTransport, or with --sim the in-process simulator (no adapter needed)."""
    if getattr(args, "sim", False):
        import glink_sim  # noqa: PLC0415
        return glink_sim.SimTransport(glink_sim.GlinkSim(clock))
    return BleakTransport(args.device, getattr(args, "address", None))


async def run_command(args, out=print, transport=None, clock=None) -> int:
    """glink-info, glink-profile, glink-start, glink-stop. Returns the process exit code."""
    clock = clock or RealClock()
    transport = transport or build_transport(args, clock)
    client = GlinkClient(transport, clock)
    try:
        if args.command == "glink-stop":
            await client.connect(handshake=False)  # stop works before the handshake
            out("stop accepted" if await client.stop() else "stop refused")
            return 0
        await client.connect()
        info = client.base_info
        out("grinder %s, build %s, fw %s, board %s, GLink proto %s" % (
            info.get("snCode"), info.get("releaseVer"), info.get("fwVersion"), info.get("board"), info.get("proto")))
        _print_state(out, client)
        if args.command == "glink-info":
            for e in await client.profile_get():
                out(_fmt_entry(e))
            if client.period.get("resultSeq", 0) > 0:
                _print_result(out, await client.result_get())
        elif args.command == "glink-profile":
            updating = (args.dose is not None or args.grind_size is not None or args.grind_unit is not None
                        or args.clear_grind_size)
            if args.select is not None:
                out("selected profile %d" % await client.profile_select(args.select))
                if updating:
                    await clock.sleep_ms(codec.WRITE_MIN_INTERVAL_MS)  # 7.3: one stored change per second
            if updating:
                data = await client.profile_update(args.dose, args.grind_size, args.grind_unit,
                                                   clear_grind_size=args.clear_grind_size)
                out("updated:" + (" changed" if data["changed"] else " unchanged"))
                out(_fmt_entry(data))
            else:
                for e in await client.profile_get():
                    out(_fmt_entry(e))
        elif args.command == "glink-start":
            if args.expect_dose is None:
                out("[WARNING] no --expect-dose: the dose stored on the grinder starts without a check. "
                    "Anyone in range can start a grind (docs/GLINK-PROTOCOL.md section 1.1).")
            keeps_running = ("The grind keeps running on the grinder (docs/GLINK-PROTOCOL.md 7.4). "
                             "Stop it with: grinder.py glink-stop (or touch the grinder).")
            try:
                outcome = await client.start_and_confirm(args.profile, args.expect_dose)
                if not outcome.started:
                    out("grind did not start: state %s, blocked %s" % (outcome.period.get("devState"),
                                                                        outcome.period.get("blocked")))
                    return 1
                out("grind started (lead-in %d ms)" % outcome.lead_in_ms)
                if not args.no_wait:
                    _print_result(out, await client.wait_result(args.timeout * 1000))
            except (KeyboardInterrupt, asyncio.CancelledError, GlinkDisconnected, GlinkTimeout):
                out("[WARNING] " + keeps_running)
                raise
        return 0
    except GlinkError as e:
        out("[ERROR] %s" % e)
        return 1
    except (OSError, asyncio.TimeoutError) as e:
        out("[ERROR] Bluetooth failed: %s" % (e or type(e).__name__))
        return 1
    finally:
        if client.connected:
            await client.disconnect()

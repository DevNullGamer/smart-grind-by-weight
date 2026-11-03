#!/usr/bin/env python3
"""
Local development server for the web panel page (web/portal/index.html). Needs no hardware.

It speaks the contract of docs/PORTAL-API.md with a simulated grinder behind it: the same Host/Origin/
Content-Type/X-Grinder guards, body rules, clamping, refusals and decisions as the firmware (portal_model.py is
tested against the vectors the firmware tests use). Dev-only extras, not part of the contract: GET/POST
/dev/state to flip the simulated switches (see README.md). Standard library only.

  python3 tools/portal/dev_server.py [--host 127.0.0.1] [--port 8080] [--web-off] [--ui menu] ...

Open it at http://127.0.0.1:8080 (the Host guard refuses "localhost", like the grinder refuses a DNS name).
"""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_page  # noqa: E402
import portal_model as model  # noqa: E402

BUILD_NUMBER = 1
UI_STATES = ("ready", "menu", "edit", "calibration", "confirm", "purge_confirm", "autotuning", "ota_update")
LEAD_IN_S = model.LEAD_IN_MS / 1000.0
GRIND_RATE_G_PER_S = 3.0
COMPLETE_SHOW_S = 3.0
MOTOR_TEST_S = 1.5
BLOCK_CODES = ("motor_disabled", "update_in_progress", "load_cell_not_connected", "load_cell_no_data",
               "load_cell_rate_invalid", "busy", "motor_fault")
DEV_BOOL_KEYS = ("web_interface", "update_held", "in_flight", "drop_start", "ui_busy", "store_failed")
DEV_STATE_KEYS = DEV_BOOL_KEYS + ("ui_state", "start_block", "motor_test_block", "latency_ms", "weight")
LATENCY_MAX_MS = 30000
WEIGHT_MAX_G = 1000.0
PAGE_HEADERS = {
    "Cache-Control": "no-cache",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "Content-Security-Policy": "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; "
                               "connect-src 'self'; frame-ancestors 'none'",
}


class SimGrinder:
    """The grinder behind the server: settings, a screen state and a time-driven grind. Thread-safe."""

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.lock = threading.RLock()
        self.boot = clock()
        self.web_interface = True
        self.ui_state = "ready"
        self.update_held = False
        self.in_flight = False        # forces "another request is in flight" (dev switch)
        self.start_block = None       # error code the grinder-side start checks would give
        self.motor_test_block = None
        self.drop_start = False       # stays on: every lead-in is dropped when it ends (another screen took over)
        self.ui_busy = False          # settings reads and writes answer 503 ui_busy
        self.store_failed = False     # a settings write that changes a value answers 500 store_failed
        self.latency_ms = 0           # every reply but /dev/state waits this long (a slow grinder)
        self.weight_hold = None       # a fixed weight shown instead of the simulated one (None = not held)
        self.stored = {k: v for k, v in model.default_values().items()}
        self.stored["brightness_normal"] = float(self.stored["brightness_normal"])
        self.stored["brightness_screensaver"] = float(self.stored["brightness_screensaver"])
        self.current_profile = 1
        self.weight = 0.0
        self.run = None               # dict: kind, started, ... while something is running
        self.complete_until = None

    # --- time-driven part
    def _tick(self):
        now = self.clock()
        run = self.run
        if run is not None:
            elapsed = now - run["started"]
            if self._in_lead_in():
                self._poll_lead_in(run, elapsed)
                run = self.run
            if run is not None and run["live"]:
                elapsed = now - run["started"]
                if run["kind"] == "start":
                    self.weight = min(run["target_g"], elapsed * run["rate"])
                    if elapsed >= run["duration"]:
                        self.run = None
                        self.ui_state = "grind_complete"
                        self.complete_until = now + COMPLETE_SHOW_S
                elif elapsed >= MOTOR_TEST_S:
                    self.run = None
        if self.complete_until is not None and now >= self.complete_until:
            self.complete_until = None
            self.ui_state = "ready"

    def _poll_lead_in(self, run, elapsed):
        """Like the firmware's lead-in poll (contract section 6): the start is decided again on every look while it
        waits, and dropped at once when the screen, an update or a start block says no (the screen stays as it is).
        drop_start is the dev switch for "another screen took over at the very end", so it acts when the lead-in
        ends. A start that still passes then runs the grind."""
        refused = model.decide_command(
            "start", self._ui_mode(), False, self.update_held, False, self.start_block or "NONE", "NONE")
        if refused or (elapsed >= LEAD_IN_S and self.drop_start):
            self.run = None
            return
        if elapsed < LEAD_IN_S:
            return
        run["live"] = True
        self.ui_state = "grinding"  # during the lead-in the status still reads ready (contract section 7)
        run["started"] += LEAD_IN_S  # the grind itself starts when the lead-in ends

    def _ui_mode(self):
        if self.ui_state == "ready":
            return "READY"
        return "MENU" if self.ui_state == "menu" else "OTHER"

    def _busy(self):
        return self.run is not None and self.run["live"]

    def _in_lead_in(self):
        return self.run is not None and self.run["kind"] == "start" and not self.run["live"]

    def _in_flight(self):
        # The lead-in counts as in flight (contract section 6)
        return self.in_flight or self._in_lead_in()

    def _effective_blocks(self):
        """The block codes the status reports. While an update holds the grinder the firmware reports its lock-out,
        after the checks that come first (grind_start_check.cpp, motor_test_check.cpp)."""
        start = self.start_block
        if self.update_held and start != "motor_disabled":
            start = "update_in_progress"
        motor = self.motor_test_block
        if self.update_held and motor not in ("motor_fault", "motor_disabled"):
            motor = "update_in_progress"
        return start, motor

    def _phase(self):
        if self.run is None or not self.run["live"] or self.run["kind"] != "start":
            return "completed" if self.ui_state == "grind_complete" else "idle"
        return "predictive" if self.run["mode"] == "weight" else "time_grinding"

    # --- reads
    def values(self):
        with self.lock:
            out = {}
            for f in model.FIELDS:
                v = self.stored[f["key"]]
                if f["type"] == "int":
                    v = model.stored_brightness_percent(v)
                out[f["key"]] = v
            return out

    def settings_doc(self):
        with self.lock:
            self._tick()
            return {"values": self.values(), "current_profile": self.current_profile}

    def status_doc(self):
        with self.lock:
            self._tick()
            start, motor = self._effective_blocks()
            weight = self.weight if self.weight_hold is None else self.weight_hold
            return {
                "ui_state": self.ui_state,
                "grind_phase": self._phase(),
                "busy": self._busy(),
                "weight_g": round(weight, 1),
                "motor_available": self.start_block != "motor_disabled",
                "start_block": start,
                "motor_test_block": motor,
                "uptime_s": int(self.clock() - self.boot),
                "build": BUILD_NUMBER,
                "web_interface": True,
            }

    # --- writes
    def read_settings_refusal(self):
        """The refusal a settings read gets (503 while the UI task is simulated as stuck), or None."""
        return "ui_busy" if self.ui_busy else None

    def patch(self, applied, adjusted):
        """Apply a validated patch (the decision was already made). Returns the reply document or raises Refusal.
        Like the firmware, a value equal to the stored one is not written; with store_failed the first value that
        would be written cannot be saved, so nothing is stored."""
        with self.lock:
            self._tick()
            if self.store_failed and any(self.stored[k] != v for k, v in applied.items()):
                raise model.Refusal("store_failed")
            name_before = self.stored["device_name"]
            for key, value in applied.items():
                self.stored[key] = value
            doc = {"ok": True, "values": self.values(), "current_profile": self.current_profile, "adjusted": adjusted}
            if "device_name" in applied and applied["device_name"] != name_before:
                doc["address"] = applied["device_name"] + ".local"
            return doc

    def decide_write(self):
        with self.lock:
            self._tick()
            if self.ui_busy:
                return "ui_busy"
            return model.decide_write(self._ui_mode(), self._busy(), self.update_held, self._in_flight())

    def command(self, cmd, profile):
        """Decide and run a command. Returns (HTTP status, reply document) or raises Refusal."""
        with self.lock:
            self._tick()
            error = model.decide_command(cmd, self._ui_mode(), self._busy(), self.update_held, self._in_flight(),
                                         self.start_block or "NONE", self.motor_test_block or "NONE")
            if error:
                raise model.Refusal(error)
            now = self.clock()
            if cmd == "stop":
                self.run = None
                self.complete_until = None
                if self.ui_state in ("grind_complete", "grinding"):
                    self.ui_state = "ready"
                return 200, {"ok": True, "cmd": "stop"}
            if cmd == "tare":
                self.weight = 0.0
                if self.weight_hold is not None:
                    self.weight_hold = 0.0  # the tare zeroes the reading the hold stands in for
                return 200, {"ok": True, "cmd": "tare"}
            if cmd == "motor_test":
                self.run = {"kind": "motor_test", "started": now, "live": True}
                return 202, {"ok": True, "cmd": "motor_test"}
            self.current_profile = profile
            mode = self.stored["grind_mode"]
            names = ("single", "double", "custom")
            if mode == "weight":
                target = self.stored[f"{names[profile]}_weight_g"]
                duration = max(2.0, target / GRIND_RATE_G_PER_S)
            else:
                duration = self.stored[f"{names[profile]}_time_s"]
                target = duration * GRIND_RATE_G_PER_S
            self.weight = 0.0
            self.run = {"kind": "start", "started": now, "live": False, "mode": mode, "target_g": target,
                        "duration": duration, "rate": target / duration}
            return 202, {"ok": True, "cmd": "start", "profile": profile, "lead_in_ms": model.LEAD_IN_MS}

    # --- dev controls
    def dev_state(self):
        with self.lock:
            self._tick()
            return {"web_interface": self.web_interface, "ui_state": self.ui_state, "update_held": self.update_held,
                    "in_flight": self.in_flight, "start_block": self.start_block,
                    "motor_test_block": self.motor_test_block, "drop_start": self.drop_start,
                    "ui_busy": self.ui_busy, "store_failed": self.store_failed, "latency_ms": self.latency_ms,
                    "weight": self.weight_hold}

    def set_dev_state(self, changes):
        """Applies the members of a /dev/state body. Returns an error text or None."""
        with self.lock:
            for key, value in changes.items():
                if key not in DEV_STATE_KEYS:
                    return f"unknown dev setting {key}"
                if key in DEV_BOOL_KEYS:
                    if not isinstance(value, bool):
                        return f"{key} must be true or false"
                elif key == "ui_state":
                    if value not in UI_STATES:
                        return "ui_state must be one of " + ", ".join(UI_STATES)
                elif key in ("start_block", "motor_test_block"):
                    if value is not None and value not in BLOCK_CODES:
                        return f"{key} must be null or one of " + ", ".join(BLOCK_CODES)
                elif key == "latency_ms":
                    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= LATENCY_MAX_MS:
                        return f"latency_ms must be a whole number of milliseconds from 0 to {LATENCY_MAX_MS}"
                elif key == "weight":
                    if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                                              or abs(value) > WEIGHT_MAX_G):
                        return f"weight must be null or a number from -{WEIGHT_MAX_G:g} to {WEIGHT_MAX_G:g} g"
            for key, value in changes.items():
                if key == "weight":
                    value = None if value is None else float(value)
                    key = "weight_hold"
                setattr(self, key, value)
            # A screen change ends a grind or a motor run. A lead-in is kept: its next look decides it.
            if "ui_state" in changes and not self._in_lead_in():
                self.run = None
                self.complete_until = None
            return None


def make_handler(sim, page_text):
    page_bytes = page_text.encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        server_version = "grinder-dev-server"
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # one short line per request on stderr
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

        # --- plumbing
        def _send(self, status, body, content_type="application/json", extra=None):
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            for name, value in (extra or {}).items():
                self.send_header(name, value)
            self.send_header("Connection", "close")
            try:
                self.end_headers()
                self.wfile.write(data)
            except ConnectionError:
                pass  # the page gave up while the reply was delayed (latency_ms): nothing to tell
            self.close_connection = True

        def _delay(self, path):
            """Dev switch latency_ms: every reply but /dev/state waits, so the page's timeout can be reproduced."""
            if path != "/dev/state" and sim.latency_ms:
                time.sleep(sim.latency_ms / 1000.0)

        def _api(self, status, doc):
            self._send(status, model.encode_json(doc), extra={"Cache-Control": "no-store"})

        def _refuse(self, code):
            status, body = model.error_body(code)
            self._send(status, body, extra={"Cache-Control": "no-store"})

        def _read_body(self):
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            length = max(0, length)
            return self.rfile.read(min(length, model.BODY_MAX + 1)) if length else b""

        def _admit(self, state_changing):
            """Route switch, then the browser guards. Returns True when the request may proceed."""
            if not sim.web_interface:
                self._refuse("web_interface_off")
                return False
            try:
                model.check_request(self.headers.get("Host"), self.headers.get("Origin"),
                                    self.headers.get("Content-Type"), self.headers.get("X-Grinder"), state_changing)
            except model.Refusal as r:
                self._refuse(r.code)
                return False
            return True

        def _path(self):
            return self.path.split("?", 1)[0]

        # --- verbs
        def do_GET(self):
            path = self._path()
            self._delay(path)
            if path == "/api/info":
                self._api(200, {"name": sim.stored["device_name"], "version": "dev-server", "build": BUILD_NUMBER,
                                "board": "dev", "busy": sim.status_doc()["busy"], "confirm": True})
            elif path in ("/", "/index.html"):
                if self._admit(False):
                    self._send(200, page_bytes, "text/html; charset=utf-8", PAGE_HEADERS)
            elif path in ("/api/status", "/api/settings", "/api/schema"):
                if not self._admit(False):
                    return
                if path == "/api/status":
                    self._api(200, sim.status_doc())
                elif path == "/api/settings":
                    refusal = sim.read_settings_refusal()
                    if refusal:
                        self._refuse(refusal)
                    else:
                        self._api(200, sim.settings_doc())
                else:
                    self._api(200, model.schema())
            elif path == "/dev/state":
                self._api(200, sim.dev_state())
            elif path == "/api/command":
                self._send(405, "", "text/plain", {"Allow": "POST"})
            else:
                self._send(404, "Not found", "text/plain")

        def do_PATCH(self):
            self._delay(self._path())
            if self._path() != "/api/settings":
                self._send(404 if self._path() not in ("/api/status", "/api/schema", "/api/command", "/api/info", "/")
                           else 405, "", "text/plain")
                return
            if not self._admit(True):
                return
            body = self._read_body()
            try:
                applied, adjusted = model.parse_patch(body)
                error = sim.decide_write()
                if error:
                    raise model.Refusal(error)
                doc = sim.patch(applied, adjusted)
            except model.Refusal as r:
                self._refuse(r.code)
                return
            self._api(200, doc)

        def do_POST(self):
            path = self._path()
            self._delay(path)
            if path == "/dev/state":
                try:  # same rule as the API, so a foreign web page cannot flip the switches
                    model.check_content_type(self.headers.get("Content-Type"))
                    model.check_custom_header(self.headers.get("X-Grinder"))
                except model.Refusal as r:
                    self._refuse(r.code)
                    return
                try:
                    changes = json.loads(self._read_body() or b"{}")
                    problem = sim.set_dev_state(changes) if isinstance(changes, dict) else "body must be an object"
                except ValueError:
                    problem = "body is not JSON"
                if problem:
                    self._api(400, {"error": "dev_state", "reason": problem})
                else:
                    self._api(200, sim.dev_state())
                return
            if path != "/api/command":
                self._send(405 if path in ("/api/status", "/api/settings", "/api/schema", "/api/info", "/")
                           else 404, "", "text/plain")
                return
            if not self._admit(True):
                return
            try:
                cmd, profile = model.parse_command(self._read_body())
                status, doc = sim.command(cmd, profile)
            except model.Refusal as r:
                self._refuse(r.code)
                return
            self._api(status, doc)

    return Handler


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="127.0.0.1", help="address to listen on (default 127.0.0.1; 0.0.0.0 for the LAN)")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--web-off", action="store_true", help="start with the Web interface switch off (403 web_interface_off)")
    p.add_argument("--ui", choices=UI_STATES, default="ready", help="simulated screen state (default ready)")
    p.add_argument("--update-held", action="store_true", help="simulate a running firmware update")
    p.add_argument("--start-block", choices=BLOCK_CODES, help="simulate a grinder-side refusal of start")
    p.add_argument("--motor-test-block", choices=BLOCK_CODES, help="simulate a grinder-side refusal of the motor test")
    p.add_argument("--drop-start", action="store_true",
                   help="every remote start is dropped when its lead-in ends (another screen took over)")
    return p


def make_simulator(args):
    sim = SimGrinder()
    sim.web_interface = not args.web_off
    sim.ui_state = args.ui
    sim.update_held = args.update_held
    sim.start_block = args.start_block
    sim.motor_test_block = args.motor_test_block
    sim.drop_start = args.drop_start
    return sim


def main(argv=None):
    args = build_parser().parse_args(argv)
    sim = make_simulator(args)
    try:
        page = build_page.render_page()  # always the current sources, even if index.html was not rebuilt
    except (build_page.PageBuildError, OSError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    server = ThreadingHTTPServer((args.host, args.port), make_handler(sim, page))
    shown = args.host if args.host not in ("0.0.0.0", "") else "127.0.0.1"
    print(f"Dev server on http://{shown}:{server.server_address[1]}/  (Ctrl+C stops). Use the IP address, not 'localhost'.")
    if args.host in ("0.0.0.0", ""):
        print("WARNING: listening on every interface; anyone on the network can drive the simulated grinder.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Screenshot harness for the web panel page (dev tool, not run by CI; chg021).

Starts tools/portal/dev_server.py on a free port, sets a simulated state through /dev/state, and takes one headless
Chrome or Edge screenshot per width x colour scheme x state. Standard library only; needs Chrome or Edge installed.

  python3 tools/portal/snap.py --step V3 [--widths 360 768 1280] [--states ready_idle grinding] [--schemes light dark]
  python3 tools/portal/snap.py --list

Output: <out>/<step>/<state>_<width>_<scheme>.png, default out
~/.claude/workspace/chgreq/smart-grind-by-weight/chg021/shots (outside the repo). The page opens on
its default tab for the width (a screenshot cannot click), so the Settings and Insights tabs show at 1280 px only
(there Settings replaces Grind) or by hand. Reduced motion is forced with --reduced-motion.
"""

import argparse
import json
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = Path.home() / ".claude" / "workspace" / "chgreq" / "smart-grind-by-weight" / "chg021" / "shots"
WIDTHS = (360, 768, 1280)
HEIGHT = 1400
SCHEMES = ("light", "dark")

# state name -> (dev_server flags, /dev/state bodies posted in order, then the optional command, seconds to wait)
STATES = {
    "ready_idle": ([], [], None, 0),
    "grinding": ([], [], {"cmd": "start", "profile": 1}, 5),  # lead-in is 3 s
    "lead_in": ([], [], {"cmd": "start", "profile": 1}, 1),
    "complete": ([], [], {"cmd": "start", "profile": 1}, 12),
    "offline": (None, [], None, 0),  # server not started: the page shows its offline state
    "web_off": (["--web-off"], [], None, 0),
    "busy": ([], [], {"cmd": "motor_test"}, 0.5),
    "not_ready": ([], [{"ui_state": "edit"}], None, 0),
    "update": (["--update-held"], [], None, 0),
    "motor_fault": ([], [{"motor_test_block": "motor_fault"}], None, 0),
    "warnings": ([], [{"warnings": ["motor_fault", "load_cell_not_calibrated"]}], None, 0),
    "settings_default": ([], [], None, 0),
    "insights_empty": ([], [{"stats_reset": True, "history_clear": True}], None, 0),
    "insights_full": ([], [{"history_fill": 10}], None, 0),
}


def find_browser():
    """Chrome or Edge in the usual Windows, macOS and Linux places; None when neither exists."""
    candidates = (
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
    )
    return next((c for c in candidates if Path(c).exists()), None)


def post(port, path, body):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", "X-Grinder": "1"}, method="POST")
    urllib.request.urlopen(req, timeout=5).read()


def wait_for(port):
    for _ in range(50):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/api/info", timeout=1).read()
            return True
        except Exception:
            time.sleep(0.1)
    return False


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def shoot(browser, url, width, scheme, reduced_motion, target):
    with tempfile.TemporaryDirectory() as profile:
        cmd = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--user-data-dir={profile}",
               f"--window-size={width},{HEIGHT}", "--virtual-time-budget=4000", f"--screenshot={target}"]
        if scheme == "dark":
            cmd.append("--blink-settings=preferredColorScheme=1")
        if reduced_motion:
            cmd.append("--force-prefers-reduced-motion")
        try:
            subprocess.run(cmd + [url], capture_output=True, timeout=60)  # run() kills the browser on timeout
        except subprocess.TimeoutExpired:
            print(f"WARNING: the browser timed out for {target}", file=sys.stderr)


def run_state(name, args, browser, out_dir):
    flags, bodies, command, wait_s = STATES[name]
    port = free_port()
    server = None
    if flags is not None:
        server = subprocess.Popen([sys.executable, str(HERE / "dev_server.py"), "--port", str(port), *flags],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for(port):
            server.kill()
            raise RuntimeError("the dev server did not start")
    try:
        if server:
            for body in bodies:
                post(port, "/dev/state", body)
            if command:
                post(port, "/api/command", command)
                time.sleep(wait_s)
        for width in args.widths:
            for scheme in args.schemes:
                target = out_dir / f"{name}_{width}_{scheme}.png"
                shoot(browser, f"http://127.0.0.1:{port}/", width, scheme, args.reduced_motion, target)
                print(("ok      " if target.exists() else "MISSING ") + str(target))
    finally:
        if server:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--step", default="adhoc", help="output sub-folder, e.g. V3")
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    p.add_argument("--widths", type=int, nargs="+", default=list(WIDTHS))
    p.add_argument("--schemes", nargs="+", choices=SCHEMES, default=list(SCHEMES))
    p.add_argument("--states", nargs="+", choices=sorted(STATES), default=sorted(STATES))
    p.add_argument("--reduced-motion", action="store_true")
    p.add_argument("--browser", help="path to chrome or msedge (default: look in the usual places)")
    p.add_argument("--list", action="store_true", help="list the states and exit")
    args = p.parse_args(argv)
    if args.list:
        print("\n".join(sorted(STATES)))
        return 0
    browser = args.browser or find_browser()
    if not browser:
        print("ERROR: no Chrome or Edge found; pass --browser", file=sys.stderr)
        return 1
    out_dir = args.out / args.step
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in args.states:
        run_state(name, args, browser, out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())

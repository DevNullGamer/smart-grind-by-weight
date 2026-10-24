#!/usr/bin/env python3
"""WiFi push update tool for the Smart Grind By Weight scale (stdlib only, no venv needed).

Commands:
  info      show what the device reports on GET /api/info (no token needed)
  upload    push a full firmware image: POST /api/update/begin, PUT /api/update/data, then wait for the new
            firmware to answer on /api/info
  config    store host and token in a per-user config file (never in the repo)

Host and token come from --host/--token, then GRINDER_WIFI_HOST/GRINDER_WIFI_TOKEN, then the config file
(~/.grinder-wifi.json). The host is an IP address or a name such as grindbyweight.local.
"""

import argparse
import hashlib
import http.client
import json
import os
import re
import socket
import sys
import time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent
PROJECT_DIR = TOOLS_DIR.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from firmware_variants import FIRMWARE_ENV  # noqa: E402

DEFAULT_HOST = "grindbyweight.local"
CONFIG_PATH = Path.home() / ".grinder-wifi.json"
ENV_HOST = "GRINDER_WIFI_HOST"
ENV_TOKEN = "GRINDER_WIFI_TOKEN"
CHUNK_BYTES = 16 * 1024
CONNECT_TIMEOUT_S = 10
RESPONSE_TIMEOUT_S = 90        # finish() hashes and validates the whole image before it answers
PROMPT_TIMEOUT_S = 30          # how long the grinder waits for the user to tap INSTALL
PROMPT_WAIT_S = 45             # begin may block that long (plus margin) while the user decides
REBOOT_WAIT_S = 120
TOKEN_RE = re.compile(r"^[0-9a-f]{16}$")
HOST_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$")

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2


# --------------------------------------------------------------------------- pure helpers

def validate_token(token):
    """The device token is 16 lowercase hex characters."""
    return bool(token) and TOKEN_RE.match(token) is not None


def mask_token(token):
    if not token:
        return "(none)"
    return token[:2] + "*" * (len(token) - 4) + token[-2:] if len(token) > 4 else "*" * len(token)


def normalize_host(host):
    """Strips a scheme/port/path someone pasted; returns None if what is left is not a plausible host."""
    if not host:
        return None
    host = host.strip()
    host = re.sub(r"^https?://", "", host, flags=re.IGNORECASE)
    host = host.split("/", 1)[0]
    if host.count(":") == 1:  # host:port
        host = host.split(":", 1)[0]
    if not HOST_RE.match(host):
        return None
    return host


def load_config(path=CONFIG_PATH):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(values, path=CONFIG_PATH):
    """Writes the config file; restricts it to the user where the OS supports that."""
    path = Path(path)
    # Created with 0600 from the start (no window where the token is readable); on Windows the mode is ignored
    descriptor = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        json.dump(values, handle, indent=2)
        handle.write("\n")
    try:
        os.chmod(path, 0o600)  # also tightens a file that already existed with wider permissions
    except OSError:
        pass


def resolve_settings(host_arg, token_arg, environ, config):
    """Precedence: argument, environment, config file; host falls back to the default name."""
    host = host_arg or environ.get(ENV_HOST) or config.get("host") or DEFAULT_HOST
    token = token_arg or environ.get(ENV_TOKEN) or config.get("token")
    return host, token


def sha256_file(path, chunk=1024 * 1024):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(chunk)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _cache_build_number(path):
    match = re.match(r"^build_(\d+)\.bin$", Path(path).name)
    return int(match.group(1)) if match else None


# Envs whose image is a normal grinder firmware that WiFi push works with: the main env, its debug build and the
# WiFi dev build. The mock, selftest, spike and stress envs are measurement builds and must never be pushed by default.
DEFAULT_ENV_SUFFIXES = ("", "-debug", "-wifi-dev")


def find_default_firmware(project_dir=PROJECT_DIR, env=FIRMWARE_ENV):
    """The newest (by modification time) PlatformIO firmware.bin of the main env, its debug env or the wifi-dev env,
    else the highest-numbered image in firmware_cache of the main env."""
    build_root = Path(project_dir) / ".pio" / "build"
    built = []
    if build_root.is_dir():
        for suffix in DEFAULT_ENV_SUFFIXES:
            candidate = build_root / (env + suffix) / "firmware.bin"
            if candidate.is_file():
                built.append(candidate)
    if built:
        return max(built, key=lambda p: p.stat().st_mtime)
    cache = Path(project_dir) / "firmware_cache" / env
    candidates = [p for p in cache.glob("build_*.bin") if _cache_build_number(p) is not None] if cache.is_dir() else []
    if not candidates:
        return None
    return max(candidates, key=_cache_build_number)


def _cache_dirs(project_dir, env):
    """Every per-env directory of firmware_cache, the main env first (derived envs such as -wifi-dev or
    -boot-fail-test archive their images in their own directory)."""
    root = Path(project_dir) / "firmware_cache"
    if not root.is_dir():
        return []
    return sorted((d for d in root.iterdir() if d.is_dir()), key=lambda d: (d.name != env, d.name))


def find_cache_build(firmware_path, project_dir=PROJECT_DIR, env=FIRMWARE_ENV):
    """Build number of a firmware_cache image (of any env) that is byte-identical to firmware_path, else None.
    The build number is what the device compares after the restart; without it a rolled-back update is not reported."""
    size = Path(firmware_path).stat().st_size
    sha = None
    for cache in _cache_dirs(project_dir, env):
        for candidate in sorted(cache.glob("build_*.bin"), key=lambda p: _cache_build_number(p) or 0, reverse=True):
            number = _cache_build_number(candidate)
            if number is None or candidate.stat().st_size != size:
                continue
            if sha is None:
                sha = sha256_file(firmware_path)
            if sha256_file(candidate) == sha:
                return number
    return None


def rollback_hint(info, old_build, expected_build):
    """Text for 'the new firmware did not show up': a device that answers with the old build was most likely rolled
    back by the boot-health check (or never restarted); None when there is nothing to say."""
    if not info or "build" not in info:
        return None
    if info["build"] == old_build and expected_build != old_build:
        return ("The device runs the previous build %s again. The new image probably crashed or reset within "
                "30 s of its first start and was rolled back automatically." % info["build"])
    return None


def build_begin_body(size, sha256_hex, build=None, version=None):
    body = {"size": int(size), "sha256": sha256_hex}
    if build is not None:
        body["build"] = int(build)
    if version:
        body["version"] = str(version)
    return json.dumps(body, separators=(",", ":"))


def parse_json_body(raw):
    """Decodes a response body; returns {} for anything that is not a JSON object."""
    try:
        data = json.loads(raw.decode("utf-8") if isinstance(raw, (bytes, bytearray)) else raw)
        return data if isinstance(data, dict) else {}
    except (ValueError, UnicodeDecodeError):
        return {}


def describe_error(status, body):
    """One-line text for a failed request, from the device's {"error","reason"} reply."""
    reason = body.get("reason") or body.get("error") or "no details"
    text = "HTTP %d: %s" % (status, reason)
    if "retry_after_s" in body:
        text += " (retry in %s s)" % body["retry_after_s"]
    return text


def explain_begin_refusal(status, body):
    """Plain-language text for a refused begin request (the on-device Accept prompt and its neighbours)."""
    error = body.get("error")
    reason = body.get("reason") or "no details"
    if status == 403 and error == "declined":
        return "The update was declined on the grinder."
    if status == 408 and error == "no_response":
        return ("Nobody confirmed the update on the grinder within %d s. Run the command again and tap INSTALL "
                "on the grinder screen." % PROMPT_TIMEOUT_S)
    if status == 409 and error == "busy":
        return "The grinder cannot update right now: %s." % reason.rstrip(".")
    if status == 429 and error == "declined_recently":
        return "The update was declined a moment ago. Try again in %s s." % body.get("retry_after_s", "a few")
    return describe_error(status, body)


def format_network_error(error):
    """Readable text for a failed request; a reset usually means the device refused and closed the connection."""
    if isinstance(error, (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
        return ("%s - the device closed the connection; see its serial log or query /api/info" % error)
    return str(error)


def format_progress(sent, total, elapsed_s):
    percent = 100.0 * sent / total if total else 100.0
    rate = (sent / 1024.0) / elapsed_s if elapsed_s > 0 else 0.0
    return "%5.1f%%  %d / %d KB  %.0f KB/s" % (percent, sent // 1024, total // 1024, rate)


def confirm_note(expected_build):
    """Reminder printed once the new image answers: it is only confirmed after a stable run."""
    note = "Note: the new image is confirmed after ~30 s of stable running; a crash before that rolls back to the previous build"
    if expected_build is None:
        return note + " (no build number known, so a rollback cannot be detected by this tool)."
    return note + "."


def reboot_finished(info, expected_build, old_build, seen_down):
    """Has the device come back running the new firmware?"""
    if not info or "build" not in info:
        return False
    if expected_build is not None:
        return info["build"] == expected_build
    return seen_down or info["build"] != old_build


# --------------------------------------------------------------------------- HTTP

def request(host, method, path, token=None, body=None, headers=None, timeout=CONNECT_TIMEOUT_S):
    """One short request; returns (status, parsed_json_dict)."""
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        hdrs = dict(headers or {})
        if token:
            hdrs["Authorization"] = "Bearer " + token
        if body is not None:
            hdrs["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=hdrs)
        response = conn.getresponse()
        return response.status, parse_json_body(response.read())
    finally:
        conn.close()


def get_info(host, timeout=CONNECT_TIMEOUT_S):
    status, body = request(host, "GET", "/api/info", timeout=timeout)
    return body if status == 200 else None


def push_image(host, token, firmware_path, session, size):
    """PUT the raw image with progress; returns (status, parsed_json_dict)."""
    conn = http.client.HTTPConnection(host, 80, timeout=RESPONSE_TIMEOUT_S)
    try:
        conn.putrequest("PUT", "/api/update/data")
        conn.putheader("Authorization", "Bearer " + token)
        conn.putheader("X-Session", session)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(size))
        conn.endheaders()
        sent = 0
        started = time.time()
        last_print = 0.0
        interactive = sys.stdout.isatty()
        next_plain_percent = 10
        try:
            with open(firmware_path, "rb") as handle:
                while True:
                    block = handle.read(CHUNK_BYTES)
                    if not block:
                        break
                    conn.send(block)
                    sent += len(block)
                    now = time.time()
                    if interactive and now - last_print > 0.25:
                        sys.stdout.write("\r" + format_progress(sent, size, now - started))
                        sys.stdout.flush()
                        last_print = now
                    elif not interactive and sent * 100 // size >= next_plain_percent:
                        print(format_progress(sent, size, now - started))
                        next_plain_percent += 10
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            # The device refused mid-way and closed the socket: its reply explains why
            pass
        if interactive:
            sys.stdout.write("\r" + format_progress(sent, size, time.time() - started) + "\n")
        response = conn.getresponse()
        return response.status, parse_json_body(response.read())
    finally:
        conn.close()


# --------------------------------------------------------------------------- commands

def cmd_info(args):
    host, _ = resolve_settings(args.host, args.token, os.environ, load_config())
    host = normalize_host(host)
    if not host:
        print("ERROR: invalid host")
        return EXIT_USAGE
    try:
        info = get_info(host)
    except (OSError, http.client.HTTPException) as error:
        print("ERROR: cannot reach %s: %s" % (host, error))
        return EXIT_FAIL
    if info is None:
        print("ERROR: %s did not answer /api/info properly" % host)
        return EXIT_FAIL
    print("Host:     %s" % host)
    for key in ("name", "version", "build", "board", "busy", "confirm"):
        print("%-9s %s" % (key.capitalize() + ":", info.get(key, "?")))
    return EXIT_OK


def cmd_config(args):
    config = load_config()
    if args.show:
        print("Config file: %s" % CONFIG_PATH)
        print("Host:  %s" % config.get("host", "(none)"))
        print("Token: %s" % mask_token(config.get("token")))
        return EXIT_OK
    if args.host is None and args.token is None:
        print("ERROR: give --host and/or --token (or --show)")
        return EXIT_USAGE
    if args.host is not None:
        host = normalize_host(args.host)
        if not host:
            print("ERROR: invalid host")
            return EXIT_USAGE
        config["host"] = host
    if args.token is not None:
        if not validate_token(args.token):
            print("ERROR: token must be 16 lowercase hex characters")
            return EXIT_USAGE
        config["token"] = args.token
    save_config(config)
    print("Saved to %s" % CONFIG_PATH)
    return EXIT_OK


def cmd_upload(args):
    host, token = resolve_settings(args.host, args.token, os.environ, load_config())
    host = normalize_host(host)
    if not host:
        print("ERROR: invalid host")
        return EXIT_USAGE
    if not validate_token(token):
        print("ERROR: token missing or malformed (16 lowercase hex characters). Use --token, %s or 'config'." % ENV_TOKEN)
        return EXIT_USAGE

    firmware = Path(args.firmware) if args.firmware else find_default_firmware()
    if not firmware or not firmware.is_file():
        print("ERROR: no firmware image found (build first, or pass the .bin path)")
        return EXIT_USAGE
    size = firmware.stat().st_size
    sha = sha256_file(firmware)
    expected_build = args.expect_build if args.expect_build is not None else find_cache_build(firmware)
    print("Firmware: %s (%d bytes)" % (firmware, size))
    print("SHA-256:  %s" % sha)
    print("Build number: %s" % (expected_build if expected_build is not None else
                                "unknown (not found in firmware_cache, pass --expect-build to enable the rollback check)"))

    try:
        old_info = get_info(host)
    except (OSError, http.client.HTTPException) as error:
        print("ERROR: cannot reach %s: %s" % (host, error))
        return EXIT_FAIL
    if old_info is None:
        print("ERROR: %s did not answer /api/info properly" % host)
        return EXIT_FAIL
    old_build = old_info.get("build")
    print("Device: %s version %s build %s, busy=%s" % (
        old_info.get("name", "?"), old_info.get("version", "?"), old_build, old_info.get("busy", "?")))
    if old_info.get("confirm") is True:
        print("Confirm the update on the grinder screen (%d s)..." % PROMPT_TIMEOUT_S)

    try:
        status, body = request(host, "POST", "/api/update/begin", token=token,
                               body=build_begin_body(size, sha, expected_build), timeout=PROMPT_WAIT_S)
    except (OSError, http.client.HTTPException, ValueError) as error:
        print("ERROR: begin failed: %s" % format_network_error(error))
        return EXIT_FAIL
    if status != 200 or "session" not in body:
        print("ERROR: " + explain_begin_refusal(status, body))
        return EXIT_FAIL
    session = body["session"]
    print("Session accepted, sending image...")

    try:
        status, body = push_image(host, token, firmware, session, size)
    except (OSError, http.client.HTTPException, ValueError) as error:
        print("ERROR: upload failed: %s" % format_network_error(error))
        return EXIT_FAIL
    if status != 200 or body.get("status") != "installed":
        print("ERROR: upload failed - " + describe_error(status, body))
        return EXIT_FAIL
    print("Image installed, device is restarting.")
    if args.no_wait:
        return EXIT_OK

    print("Waiting for the device to come back (up to %d s)..." % REBOOT_WAIT_S)
    deadline = time.time() + REBOOT_WAIT_S
    seen_down = False
    time.sleep(2)
    while time.time() < deadline:
        try:
            info = get_info(host, timeout=3)
        except (OSError, http.client.HTTPException):
            info = None
            seen_down = True
        if reboot_finished(info, expected_build, old_build, seen_down):
            print("Device is back: version %s build %s" % (info.get("version", "?"), info.get("build")))
            print(confirm_note(expected_build))
            return EXIT_OK
        time.sleep(2)
    print("ERROR: the device did not come back with the new firmware within %d s" % REBOOT_WAIT_S)
    try:
        hint = rollback_hint(get_info(host, timeout=3), old_build, expected_build)
    except (OSError, http.client.HTTPException):
        hint = None
    if hint:
        print(hint)
    return EXIT_FAIL


def build_parser():
    parser = argparse.ArgumentParser(description="WiFi push update tool for the Smart Grind By Weight scale")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_conn_args(p):
        p.add_argument("--host", help="IP address or name (default: %s, %s or the config file)" % (DEFAULT_HOST, ENV_HOST))
        p.add_argument("--token", help="API token, 16 hex characters (or %s / config file)" % ENV_TOKEN)

    info = sub.add_parser("info", help="Show device info from /api/info")
    add_conn_args(info)
    upload = sub.add_parser("upload", help="Push a full firmware image over WiFi")
    add_conn_args(upload)
    upload.add_argument("firmware", nargs="?", help="Path to firmware .bin (default: latest build of the main env)")
    upload.add_argument("--expect-build", type=int, help="Build number the device must report after the update")
    upload.add_argument("--no-wait", action="store_true", help="Do not wait for the device to restart")
    config = sub.add_parser("config", help="Store host/token in the per-user config file")
    config.add_argument("--host")
    config.add_argument("--token")
    config.add_argument("--show", action="store_true", help="Show the stored settings (token masked)")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "info":
            return cmd_info(args)
        if args.command == "upload":
            return cmd_upload(args)
        return cmd_config(args)
    except KeyboardInterrupt:
        print("Interrupted")
        return EXIT_FAIL
    except socket.gaierror as error:
        print("ERROR: cannot resolve host: %s" % error)
        return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())

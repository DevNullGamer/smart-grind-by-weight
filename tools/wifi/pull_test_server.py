#!/usr/bin/env python3
"""Local test server for the grinder's HTTPS pull update (Check / Install), plain HTTP, stdlib only.

Serves what GitHub Pages will serve once a release exists:
    <base>/update-stable.json   and   <base>/update-beta.json   (tools/update_index.py document format)
    <base>/firmware/<tag>/smart-grind-by-weight-<tag>.bin         (the image you point it at)
The "url" in the documents starts with --base-url, which must equal what the firmware was built with
(SPIKE_PULL_BASE_URL in include/spike_secrets.h, or -DDEBUG_PULL_BASE_URL, wifi-dev pull envs only).

Fault injection (--fault) for the device-side failure tests:
    none        serve everything correctly
    wrong-sha   the index promises a different SHA-256 than the image has    -> "Checksum mismatch"
    truncate    correct Content-Length, half of the body, clean close         -> "Connection lost"
    kill        half of the body, then the connection is reset and this server exits -> "Download stalled"
    stall       half of the body, then silence for --stall-seconds             -> "Download stalled" 
    404         the image URL answers 404                                      -> "Not found"
    oversize    Content-Length is 4 KB larger than the index promised          -> "Reply too large"
    older       the index offers version 0.0.1                                 -> up to date
    not-image   the index describes a valid-looking blob that is not firmware  -> "Not a firmware image"
    no-release  both channels report that nothing is published                 -> up to date, no release

Prerelease versions (1.4.1-rc.3) are only offered on the beta channel; update-stable.json then reports
"none", like the real generator does. Use the device's beta channel (DEBUG_PULL_AUTORUN_BETA=1, the default)
for release-candidate test images.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import socket
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAULTS = ("none", "wrong-sha", "truncate", "kill", "stall", "404", "oversize", "older", "not-image", "no-release")
OLDER_VERSION = "0.0.1"
OVERSIZE_EXTRA = 4096
NOT_IMAGE_SIZE = 256 * 1024
APP_DESC_OFFSET = 32  # 24 B image header + 8 B segment header
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + 16  # magic, secure_version, 2 reserved words, then version[32]
APP_DESC_VERSION_LEN = 32


def load_update_index():
    """tools/update_index.py (the release index generator) as a module, so documents are built by its code."""
    spec = importlib.util.spec_from_file_location("update_index", os.path.join(TOOLS_DIR, "update_index.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_image_version(image):
    """Version string from the esp_app_desc_t of an ESP32 app image, or None when it is not one."""
    end = APP_DESC_VERSION_OFFSET + APP_DESC_VERSION_LEN
    if len(image) < end or struct.unpack_from("<I", image, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        return None
    raw = image[APP_DESC_VERSION_OFFSET:end].split(b"\0", 1)[0]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return None


def normalize_base_url(base_url):
    return base_url if base_url.endswith("/") else base_url + "/"


def flip_first_hex_digit(sha256):
    return ("0" if sha256[0] != "0" else "1") + sha256[1:]


class Plan:
    """Everything the server answers: documents per channel and how the image URL behaves."""

    def __init__(self, documents, image_path, image, declared_length, action):
        self.documents = documents  # {"stable": dict, "beta": dict}
        self.image_path = image_path  # "/firmware/v1.5.0/smart-grind-by-weight-v1.5.0.bin"
        self.image = image  # bytes behind image_path
        self.declared_length = declared_length  # Content-Length sent for the image
        self.action = action  # normal, 404, truncate, kill, stall

    def index_body(self, channel):
        """Same bytes as update_index.write_channel_files writes."""
        return (json.dumps(self.documents[channel], separators=(",", ":")) + "\n").encode("utf-8")


def build_plan(image, base_url, version, fault="none", published="2026-01-01"):
    """Documents and image behaviour for one image. Raises ValueError for unusable input."""
    if fault not in FAULTS:
        raise ValueError(f"unknown fault {fault!r}")
    ui = load_update_index()
    base_url = normalize_base_url(base_url)
    served = image
    if fault == "not-image":
        served = (b"<html><body>not firmware</body></html>\n" * (NOT_IMAGE_SIZE // 39 + 1))[:NOT_IMAGE_SIZE]
    if fault == "older":
        version = OLDER_VERSION
    parsed = ui.semver_parse(version)
    if parsed is None:
        raise ValueError(f"{version!r} is not a SemVer version")
    if len(served) < ui.MIN_IMAGE_SIZE:
        raise ValueError(f"image is {len(served)} bytes, below the {ui.MIN_IMAGE_SIZE} byte minimum the device accepts")

    tag = "v" + version
    image_name = ui.IMAGE_NAME.format(tag=tag)
    document = {
        "version": version,
        "tag": tag,
        "prerelease": bool(parsed[3]),
        "published": published,
        "size": len(served),
        "sha256": hashlib.sha256(served).hexdigest(),
        "url": f"{base_url}firmware/{tag}/{image_name}",
    }
    if fault == "wrong-sha":
        document["sha256"] = flip_first_hex_digit(document["sha256"])
    problem = ui.limit_problem(document)
    if problem:
        raise ValueError(f"the index document would be refused by the device: {problem}")

    entry = {"version": parsed, "tag": tag, "document": document}
    releases = [] if fault == "no-release" else [entry]
    documents = {
        channel: ui.channel_document(channel, ui.pick_channel(releases, channel)) for channel in ui.CHANNEL_FILES
    }
    declared = len(served) + OVERSIZE_EXTRA if fault == "oversize" else len(served)
    action = {"404": "404", "truncate": "truncate", "kill": "kill", "stall": "stall"}.get(fault, "normal")
    return Plan(documents, f"/firmware/{tag}/{image_name}", served, declared, action)


def guess_lan_ip():
    """The address other machines reach this PC on (no packet is sent)."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))  # TEST-NET-1: only selects the outgoing interface
        return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        probe.close()


def resolve_listen_host(host):
    """The address to bind: an explicit --host as given, otherwise the LAN address (127.0.0.1 if unknown)."""
    return host or guess_lan_ip()


def make_handler(plan, stall_seconds, on_kill):
    ui = load_update_index()
    channel_by_path = {"/" + name: channel for channel, name in ui.CHANNEL_FILES.items()}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "PullTestServer"

        def log_message(self, fmt, *args):
            print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {fmt % args}", flush=True)

        def _send_headers(self, status, length, content_type):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(length))
            self.send_header("Connection", "close")
            self.end_headers()

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in channel_by_path:
                body = plan.index_body(channel_by_path[path])
                self._send_headers(200, len(body), "application/json")
                self.wfile.write(body)
            elif path == plan.image_path and plan.action != "404":
                self._send_image()
            else:
                body = b"not found\n"
                self._send_headers(404, len(body), "text/plain")
                self.wfile.write(body)
            self.close_connection = True

        def _send_image(self):
            image = plan.image
            self._send_headers(200, plan.declared_length, "application/octet-stream")
            if plan.action in ("truncate", "kill", "stall"):
                half = len(image) // 2
                self.wfile.write(image[:half])
                self.wfile.flush()
                if plan.action == "stall":
                    print(f"fault stall: silent for {stall_seconds} s after {half} bytes", flush=True)
                    time.sleep(stall_seconds)
                elif plan.action == "kill":
                    print(f"fault kill: reset after {half} bytes, server exits", flush=True)
                    # SO_LINGER with a zero timeout makes close() send a reset instead of a clean FIN
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    self.connection.close()
                    on_kill()
                else:
                    print(f"fault truncate: closed after {half} of {len(image)} bytes", flush=True)
                return
            self.wfile.write(image)  # oversize: fewer bytes than declared, the device already refused at the headers

    return Handler


def default_on_kill():
    sys.stdout.flush()
    os._exit(1)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="Local server for the grinder's pull update tests (plain HTTP).",
                                     epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", required=True, help="firmware image to serve (a built firmware.bin)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", help="address to listen on; default: this PC's LAN address (never 0.0.0.0 unless given)")
    parser.add_argument("--base-url", help="public base URL written into the documents, default http://<LAN IP>:<port>/")
    parser.add_argument("--version", help="version to offer; default: the version inside the image")
    parser.add_argument("--fault", choices=FAULTS, default="none")
    parser.add_argument("--stall-seconds", type=float, default=60.0)
    parser.add_argument("--print-index", action="store_true", help="print both documents and exit")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        with open(args.image, "rb") as handle:
            image = handle.read()
    except OSError as error:
        print(f"cannot read {args.image}: {error}", file=sys.stderr)
        return 2
    version = args.version or read_image_version(image)
    if not version:
        print("no version found in the image, pass --version", file=sys.stderr)
        return 2
    host = resolve_listen_host(args.host)
    base_url = normalize_base_url(args.base_url or f"http://{host}:{args.port}/")
    try:
        plan = build_plan(image, base_url, version, args.fault)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.print_index:
        for channel in plan.documents:
            print(plan.index_body(channel).decode("utf-8"), end="")
        return 0

    server = ThreadingHTTPServer((host, args.port), make_handler(plan, args.stall_seconds, default_on_kill))
    print(f"serving {len(plan.image)} bytes of version {plan.documents['beta'].get('version', '(none)')} "
          f"fault={args.fault}")
    print(f'device base URL: {base_url}   (include/spike_secrets.h: #define SPIKE_PULL_BASE_URL "{base_url}")')
    print("channels: stable " + ("offers it" if "version" in plan.documents["stable"] else "reports none") +
          ", beta " + ("offers it" if "version" in plan.documents["beta"] else "reports none"))
    if host == "0.0.0.0":
        print("WARNING: listening on all interfaces (--host 0.0.0.0)")
    print(f"listening on {host}:{args.port}")
    print("Ctrl+C to stop", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopped")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

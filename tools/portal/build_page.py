#!/usr/bin/env python3
"""
Builds web/portal/index.html (the page the firmware embeds) from its two sources:

  web/portal/page.src.html     the page, with the marker  /*@@PORTAL_CLIENT@@*/
  web/portal/portal-client.js  the API client (also tested by node, tools/tests/portal-client.test.js)

The client is inlined so the page is a single file within the 64 KB gzip budget. index.html is committed because
tools/build-scripts/embed_portal.py embeds it as it is; tools/tests/test_portal_dev_server.py fails when it is
out of date. Usage:  python3 tools/portal/build_page.py [--check]
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PORTAL_DIR = ROOT / "web" / "portal"
MARKER = "/*@@PORTAL_CLIENT@@*/"


class PageBuildError(Exception):
    """The page cannot be built; the message says why."""


def render_page(portal_dir=PORTAL_DIR):
    """The page text with the client inlined."""
    template = (portal_dir / "page.src.html").read_text(encoding="utf-8").replace("\r\n", "\n")
    client = (portal_dir / "portal-client.js").read_text(encoding="utf-8").replace("\r\n", "\n")
    if template.count(MARKER) != 1:
        raise PageBuildError(f"page.src.html must contain the marker {MARKER} exactly once")
    if "</script" in client.lower():
        raise PageBuildError("portal-client.js must not contain '</script' (it is inlined in a script element)")
    return template.replace(MARKER, client.rstrip("\n"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="only report whether index.html is up to date")
    args = parser.parse_args(argv)
    try:
        text = render_page()
    except (PageBuildError, OSError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    target = PORTAL_DIR / "index.html"
    current = target.read_bytes().replace(b"\r\n", b"\n") if target.exists() else b""  # CRLF is not "stale"
    data = text.encode("utf-8")
    if args.check:
        print("index.html is up to date" if current == data else "index.html is OUT OF DATE, run build_page.py")
        return 0 if current == data else 1
    if current != data:
        target.write_bytes(data)
        print(f"Wrote {target} ({len(data)} bytes)")
    else:
        print("index.html already up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())

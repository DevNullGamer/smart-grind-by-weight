#!/usr/bin/env python3
"""
Builds web/portal/index.html (the page the firmware embeds) from its sources in web/portal/:

  page.src.html     the page shell, with one marker per part (below)
  portal.css        design tokens and layout        marker  /*@@PORTAL_CSS@@*/     (inside <style>)
  portal-client.js  the API client (node-tested)    marker  /*@@PORTAL_CLIENT@@*/  (inside <script>)
  portal-view.js    pure view helpers (node-tested) marker  /*@@PORTAL_VIEW@@*/    (inside <script>)
  portal-app.js     DOM glue only                   marker  /*@@PORTAL_APP@@*/     (inside <script>)

The parts are inlined so the page is a single file within the 64 KB gzip budget. index.html is committed because
tools/build-scripts/embed_portal.py embeds it as it is; tools/tests/test_portal_dev_server.py fails when it is
out of date. Usage:  python3 tools/portal/build_page.py [--check] [--no-strip]

The inlined parts are stripped of comments and indentation (strip_js / strip_css); the sources keep them. The rules
are deliberately conservative so a string or regex is never touched: JS loses only lines that start with //, CSS only
/* */ blocks outside quotes (never the /*@@...@@*/ markers); both lose leading/trailing blanks and empty lines. --no-strip
builds the unstripped page for debugging (it is not what is committed).
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PORTAL_DIR = ROOT / "web" / "portal"
MARKER = "/*@@PORTAL_CLIENT@@*/"  # kept for the original single-marker callers and tests

# (marker, source file, closing tag that must not appear inside the part)
PARTS = (
    ("/*@@PORTAL_CSS@@*/", "portal.css", "</style"),
    (MARKER, "portal-client.js", "</script"),
    ("/*@@PORTAL_VIEW@@*/", "portal-view.js", "</script"),
    ("/*@@PORTAL_APP@@*/", "portal-app.js", "</script"),
)


class PageBuildError(Exception):
    """The page cannot be built; the message says why."""


def strip_js(text, name="script"):
    """Drop full-line // comments, indentation and empty lines. Refuses a multi-line template literal (a backtick
    left unbalanced on a line), because stripping would change its text."""
    out = []
    for line in text.split("\n"):
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        if line.count("`") % 2:
            raise PageBuildError(f"{name}: a multi-line template literal cannot be stripped safely: {line[:60]}")
        out.append(line)
    return "\n".join(out)


def strip_css(text, name="style"):
    """Drop /* */ comment blocks outside quoted strings (keeping /*@@...@@*/ markers), then indentation and empty lines."""
    kept = []
    i = 0
    quote = ""
    while i < len(text):
        c = text[i]
        if quote:
            kept.append(c)
            if c == "\\" and i + 1 < len(text):
                kept.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
            kept.append(c)
        elif text.startswith("/*", i) and not text.startswith("/*@@", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise PageBuildError(f"{name}: unterminated comment")
            kept.append(" ")  # a comment between two tokens still separates them
            i = end + 1
        else:
            kept.append(c)
        i += 1
    lines = (line.strip() for line in "".join(kept).split("\n"))
    return "\n".join(line for line in lines if line)


def render_page(portal_dir=PORTAL_DIR, strip=True):
    """The page text with every part inlined (comments and indentation of the parts stripped unless strip=False)."""
    page = (portal_dir / "page.src.html").read_text(encoding="utf-8").replace("\r\n", "\n")
    for marker in (m for m, _f, _c in PARTS):
        if page.count(marker) != 1:
            raise PageBuildError(f"page.src.html must contain the marker {marker} exactly once")
    parts = {}
    for marker, name, closing in PARTS:
        text = (portal_dir / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        if closing in text.lower():
            raise PageBuildError(f"{name} must not contain '{closing}' (it is inlined in a {closing[2:]} element)")
        if strip:
            text = strip_css(text, name) if name.endswith(".css") else strip_js(text, name)
        parts[marker] = text.rstrip("\n")
    # Replace after validating everything, in one pass over the template, so a part that happens to contain a marker
    # text cannot be expanded again.
    out = []
    rest = page
    while rest:
        hits = [(rest.find(m), m) for m in parts if m in rest]
        if not hits:
            out.append(rest)
            break
        pos, marker = min(hits)
        out.append(rest[:pos])
        out.append(parts[marker])
        rest = rest[pos + len(marker):]
    return "".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="only report whether index.html is up to date")
    parser.add_argument("--no-strip", action="store_true", help="keep comments and indentation (debugging only)")
    args = parser.parse_args(argv)
    try:
        text = render_page(strip=not args.no_strip)
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

"""Static checks of the web panel stylesheet (web/portal/portal.css, chg016 step 4).

The contrast test parses the colour tokens of both schemes and computes the WCAG 2 contrast ratio for every
text/background and component/background pair the page uses. Layout cannot be judged here (that is a browser check).
"""

import re
from pathlib import Path

import pytest

CSS_PATH = Path(__file__).resolve().parent.parent.parent / "web" / "portal" / "portal.css"
CSS = CSS_PATH.read_text(encoding="utf-8")

SURFACES = ("bg", "surface", "raised")
# text on every surface (4.5:1). The coloured tokens are used as text (tab, badge, messages, STOP label).
TEXT_TOKENS = ("text", "muted", "primary", "accent", "success", "warning", "error")
# control borders, icons, focus ring and switch parts against every surface (3:1)
COMPONENT_TOKENS = ("border", "primary", "accent", "warning", "error", "focus", "muted")
# (foreground, background) pairs drawn on a filled control (4.5:1 text)
FILLED_TEXT = (("on-primary", "primary"), ("on-error", "error"))


def parse_tokens(block):
    return {m.group(1): m.group(2).lower() for m in re.finditer(r"--([a-z-]+)\s*:\s*(#[0-9a-fA-F]{6})\s*;", block)}


def scheme_tokens(css):
    """Return {'light': {...}, 'dark': {...}} from the first :root block and the prefers-color-scheme: dark block."""
    root = re.search(r":root\s*\{(.*?)\n\}", css, re.S)
    dark = re.search(r"@media\s*\(prefers-color-scheme:\s*dark\)\s*\{\s*:root\s*\{(.*?)\n\s*\}\s*\}", css, re.S)
    assert root and dark, "light :root block and the dark prefers-color-scheme block must both exist"
    light_tokens = parse_tokens(root.group(1))
    dark_tokens = dict(light_tokens)
    dark_tokens.update(parse_tokens(dark.group(1)))
    return {"light": light_tokens, "dark": dark_tokens}


def luminance(hex_colour):
    channels = [int(hex_colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    lin = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def contrast_failures(tokens):
    """List of 'name ratio need' strings for every pair below its minimum."""
    failures = []

    def check(fg, bg, need, what):
        ratio = contrast(tokens[fg], tokens[bg])
        if ratio < need:
            failures.append(f"{what} {fg} on {bg}: {ratio:.2f}:1 (needs {need}:1)")

    for bg in SURFACES:
        for fg in TEXT_TOKENS:
            check(fg, bg, 4.5, "text")
        for fg in COMPONENT_TOKENS:
            check(fg, bg, 3.0, "component")
    for fg, bg in FILLED_TEXT:
        check(fg, bg, 4.5, "text")
    # a filled control (primary/error) must stand out from the page behind it, and the checked switch thumb from it
    for fill in ("primary", "error"):
        for bg in SURFACES:
            check(fill, bg, 3.0, "filled control")
    return failures


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_contrast_of_every_token_pair(scheme):
    tokens = scheme_tokens(CSS)[scheme]
    needed = set(SURFACES) | set(TEXT_TOKENS) | set(COMPONENT_TOKENS) | {t for pair in FILLED_TEXT for t in pair}
    assert needed <= set(tokens), f"missing tokens in {scheme}: {sorted(needed - set(tokens))}"
    failures = contrast_failures(tokens)
    assert not failures, f"{scheme} scheme contrast failures:\n" + "\n".join(failures)


def test_contrast_checker_fails_a_low_contrast_fixture():
    tokens = dict(scheme_tokens(CSS)["light"])
    tokens["muted"] = "#b0b0b8"  # light grey text on a light page
    tokens["error"] = "#ff9090"
    failures = contrast_failures(tokens)
    assert any("muted on bg" in f for f in failures)
    assert any("error on surface" in f for f in failures)
    # known values: black on white 21:1, same colour 1:1
    assert round(contrast("#000000", "#ffffff"), 2) == 21.0
    assert round(contrast("#777777", "#777777"), 2) == 1.0


def test_stylesheet_features_present():
    assert re.search(r"@media\s*\(prefers-color-scheme:\s*dark\)", CSS)
    assert re.search(r"@media\s*\(prefers-reduced-motion:", CSS)
    assert ":focus-visible" in CSS
    assert "env(safe-area-inset-bottom" in CSS
    assert re.search(r":focus-visible\s*\{[^}]*outline:\s*3px", CSS)


def test_no_imports_or_external_urls():
    code = re.sub(r"/\*.*?\*/", "", CSS, flags=re.S)  # the header comment names the rules
    assert "@import" not in code
    assert not re.search(r"url\(", code), "no url() at all: the CSP allows no images or fonts"
    assert "http://" not in code and "https://" not in code


def test_buttons_and_controls_are_at_least_44_px():
    def min_height(selector):
        m = re.search(re.escape(selector) + r"\s*\{[^}]*?min-height:\s*(\d+)px", CSS)
        return int(m.group(1)) if m else 0

    for selector in (".btn", "input[type=\"text\"], input[type=\"number\"], select", ".seg-opts span",
                     "input[type=\"range\"]", ".switch-row"):
        assert min_height(selector) >= 44, selector
    assert re.search(r"--dock-h:\s*56px", CSS)  # STOP dock on phones
    assert re.search(r"--tabbar-h:\s*56px", CSS)

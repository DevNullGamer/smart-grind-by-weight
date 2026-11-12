"""Static checks of the web panel stylesheet (web/portal/portal.css, chg016 step 4).

The contrast test parses the colour tokens of both schemes and computes the WCAG 2 contrast ratio for every
text/background and component/background pair the page uses. Layout cannot be judged here (that is a browser check).
"""

import re
from pathlib import Path

import pytest

CSS_PATH = Path(__file__).resolve().parent.parent.parent / "web" / "portal" / "portal.css"
CSS = CSS_PATH.read_text(encoding="utf-8")

SURFACES = ("bg", "surface", "raised", "tint-info", "tint-warning", "tint-error", "tint-success")
# text on every surface, tint fills included (4.5:1). The coloured tokens are used as text (tab, badge, messages, STOP label).
TEXT_TOKENS = ("text", "muted", "primary", "accent", "success", "warning", "error")
# control borders, icons, focus ring and switch parts against every surface (3:1)
COMPONENT_TOKENS = ("border", "primary", "accent", "warning", "error", "focus", "muted")
# (foreground, background) pairs drawn on a filled control (4.5:1 text)
FILLED_TEXT = (("on-primary", "primary"), ("on-error", "error"))


def parse_tokens(block):
    """Hex colour tokens (#rgb or #rrggbb) as {name: '#rrggbb'}; other formats are not parsed."""
    tokens = {}
    for m in re.finditer(r"--([a-z-]+)\s*:\s*#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\s*;", block):
        digits = m.group(2).lower()
        if len(digits) == 3:
            digits = "".join(c * 2 for c in digits)
        tokens[m.group(1)] = "#" + digits
    return tokens


def colour_declarations(block):
    """Names of the tokens declared as colours (hex, rgb(), hsl() ...), so an unparsed format is noticed."""
    names = []
    for m in re.finditer(r"--([a-z-]+)\s*:\s*([^;]+);", block):
        value = m.group(2).strip().lower()
        if value.startswith(("#", "rgb", "hsl", "hwb", "lab", "lch", "oklab", "oklch", "color(")) or value in ("white", "black"):
            names.append(m.group(1))
    return names


def assert_all_parsed(block, where):
    unparsed = sorted(set(colour_declarations(block)) - set(parse_tokens(block)))
    assert not unparsed, f"{where}: colour tokens in an unsupported format (use #rgb or #rrggbb): {unparsed}"


def scheme_tokens(css):
    """Return {'light': {...}, 'dark': {...}} from the first :root block and the prefers-color-scheme: dark block."""
    root = re.search(r":root\s*\{(.*?)\n\}", css, re.S)
    dark = re.search(r"@media\s*\(prefers-color-scheme:\s*dark\)\s*\{\s*:root\s*\{(.*?)\n\s*\}\s*\}", css, re.S)
    assert root and dark, "light :root block and the dark prefers-color-scheme block must both exist"
    assert_all_parsed(root.group(1), "light block")
    assert_all_parsed(dark.group(1), "dark block")
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


def rule_outline_colour(selector, css=CSS):
    """Token name of 'outline-color: var(--x)' in the rule whose selector list is exactly `selector`, else None."""
    m = re.search(r"(?:^|[}/])\s*" + re.escape(selector) + r"\s*\{([^}]*)\}", css)
    if not m:
        return None
    c = re.search(r"outline-color:\s*var\(--([a-z-]+)\)", m.group(1))
    return c.group(1) if c else None


# (selector of the focus ring on a filled control, the fill it is drawn on). The default --focus ring is only
# guaranteed against the surfaces, so a ring inside a solid fill must use the colour that is legible on that fill.
FILLED_FOCUS_RULES = (
    (".seg-opts input:checked:focus-visible + span", "primary"),
    (".header .stop.is-busy:focus-visible", "error"),
)


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_focus_ring_on_filled_controls_is_visible(scheme):
    tokens = scheme_tokens(CSS)[scheme]
    for selector, fill in FILLED_FOCUS_RULES:
        colour = rule_outline_colour(selector)
        assert colour, f"no outline-color rule for {selector}: the default focus ring is invisible on the {fill} fill"
        assert colour != "focus", selector
        ratio = contrast(tokens[colour], tokens[fill])
        assert ratio >= 3.0, f"{selector}: {colour} ring on {fill} fill {ratio:.2f}:1 (needs 3:1)"
    # why the rules exist: the default ring colour would not pass on the fills
    assert any(contrast(tokens["focus"], tokens[fill]) < 3.0 for _, fill in FILLED_FOCUS_RULES)


def test_focus_ring_rule_check_fails_without_the_filled_rule():
    stale = CSS.replace(".seg-opts input:checked:focus-visible + span", ".seg-opts input:checked:hover + span")
    assert rule_outline_colour(".seg-opts input:checked:focus-visible + span", stale) is None
    assert rule_outline_colour(".seg-opts input:checked:focus-visible + span") == "on-primary"


def test_token_parser_handles_short_hex_and_flags_unsupported_formats():
    assert parse_tokens("--a: #fff; --b: #1A2b3C;") == {"a": "#ffffff", "b": "#1a2b3c"}
    with pytest.raises(AssertionError):
        assert_all_parsed("--a: #ffffff; --b: rgb(1, 2, 3); --c: hsl(0 0% 50%);", "fixture")
    assert_all_parsed("--a: #fff; --shadow: none; --gutter: 16px;", "fixture")


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


def test_stepper_input_keeps_a_readable_minimum_width():
    """A narrow settings column once shrank the number inputs to 41 px (viewport 900): keep a floor, never 0."""
    rules = re.findall(r"\.stepper input\s*\{([^}]*)\}", CSS)
    assert rules, ".stepper input rule missing"
    widths = [int(m.group(1)) for r in rules for m in re.finditer(r"(?<![-\w])min-width:\s*(\d+)px", r)]
    assert widths and widths[-1] >= 56, f"stepper input min-width must be at least 56px, got {widths}"
    assert not re.search(r"\.stepper input[^{]*\{[^}]*min-width:\s*0\b", CSS)

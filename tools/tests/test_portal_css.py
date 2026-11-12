"""Static checks of the web panel stylesheet (web/portal/portal.css, chg016 step 4).

The contrast test parses the colour tokens of both schemes and computes the WCAG 2 contrast ratio for every
text/background and component/background pair the page uses. Layout cannot be judged here (that is a browser check).
"""

import re
import sys
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
    assert re.search(r"--dock-h:\s*52px", CSS)  # STOP dock on phones (chg021 V3: slimmer, 52 px, still above 44)
    assert re.search(r"--tabbar-h:\s*56px", CSS)


def test_stepper_input_keeps_a_readable_minimum_width():
    """A narrow settings column once shrank the number inputs to 41 px (viewport 900): keep a floor, never 0."""
    rules = re.findall(r"\.stepper input\s*\{([^}]*)\}", CSS)
    assert rules, ".stepper input rule missing"
    widths = [int(m.group(1)) for r in rules for m in re.finditer(r"(?<![-\w])min-width:\s*(\d+)px", r)]
    assert widths and widths[-1] >= 56, f"stepper input min-width must be at least 56px, got {widths}"
    assert not re.search(r"\.stepper input[^{]*\{[^}]*min-width:\s*0\b", CSS)


def _z(selector, css=CSS):
    return [int(m.group(1)) for m in re.finditer(re.escape(selector) + r"\s*\{[^}]*?z-index:\s*(\d+)", css)]


def test_sidebar_never_paints_over_stop():
    """STOP is a child of the sticky .header (a stacking context), so the desktop sidebar must stay below the header's level."""
    header = _z(".header")
    side = _z(".side")
    assert header and side, "z-index of .header and .side must be declared"
    assert max(side) < min(header), f"sidebar z-index {side} must be below the header's {header}, or it covers STOP"


def _media_block(query, css=CSS):
    """Body of the first '@media (<query>) { ... }' block (the text between its braces)."""
    start = re.search(r"@media\s*\(" + re.escape(query) + r"\)\s*\{", css)
    assert start, f"no @media ({query}) block"
    depth, i = 1, start.end()
    while depth and i < len(css):
        depth += {"{": 1, "}": -1}.get(css[i], 0)
        i += 1
    return css[start.end():i - 1]


def _rules(selector, css):
    """Declarations of every rule whose selector is exactly `selector`."""
    return re.findall(r"(?:^|[}/;])\s*" + re.escape(selector) + r"\s*\{([^}]*)\}", css)


def _strip_cannot_shrink(block):
    return any(re.search(r"flex:\s*none|flex-shrink:\s*0|flex:\s*\d+\s+0\b", r) for r in _rules(".strip", block))


def test_status_bar_state_gives_way_before_stop():
    """A long state word once pushed STOP off the right edge at 600 to 700 px (V3 audit): from 600 px the state and
    weight strip must be able to shrink, the state word is one line with an ellipsis, and STOP never shrinks."""
    for query in ("min-width: 600px", "min-width: 900px", "min-width: 1024px"):
        assert not _strip_cannot_shrink(_media_block(query, CSS)), f"@media ({query}): .strip must not be flex: none"
    code = re.sub(r"/\*.*?\*/", "", CSS, flags=re.S)
    state = " ".join(_rules(".strip .state", code))
    for needed in (r"min-width:\s*0\b", r"white-space:\s*nowrap", r"overflow:\s*hidden", r"text-overflow:\s*ellipsis"):
        assert re.search(needed, state), f".strip .state lacks {needed}"
    assert not re.search(r"flex:\s*none|flex-shrink:\s*0", state)
    assert re.search(r"flex:\s*none", " ".join(_rules(".header .stop", _media_block("min-width: 600px", CSS))))
    # 600 to 899 px the status bar is a grid, not a flex row, so the .strip flex checks above say nothing there:
    # the column holding the name and the state must be able to shrink to 0, or a long word widens it past STOP.
    tracks = _status_bar_grid_first_tracks(_media_block("min-width: 600px", CSS))
    assert tracks, "@media (min-width: 600px): .header must declare its grid-template-columns"
    for track in tracks:
        assert _track_can_shrink_to_zero(track), f"@media (min-width: 600px): .header first grid track is '{track}', needs minmax(0, ...)"
    # the same column under 600 px (the base rule)
    for track in _status_bar_grid_first_tracks(code):
        assert _track_can_shrink_to_zero(track), f".header first grid track is '{track}', needs minmax(0, ...)"


def test_strip_shrink_check_fails_on_the_old_rule():
    assert _strip_cannot_shrink("  .header h1 { flex: 1 1 auto; }\n  .strip { order: 2; flex: none; }\n")
    assert not _strip_cannot_shrink("  .strip { order: 2; flex: 1 1 0; min-width: 10rem; }\n")


def _first_grid_track(declarations):
    """First track of 'grid-template-columns' in a declaration block ('minmax(0, 1fr)' is one track), else None."""
    m = re.search(r"grid-template-columns:\s*([^;}]+)", declarations)
    if not m:
        return None
    value, depth = m.group(1).strip(), 0
    for i, c in enumerate(value):
        depth += {"(": 1, ")": -1}.get(c, 0)
        if c.isspace() and depth == 0:
            return value[:i]
    return value


def _status_bar_grid_first_tracks(block):
    return [t for t in (_first_grid_track(r) for r in _rules(".header", block)) if t]


def _track_can_shrink_to_zero(track):
    return bool(re.match(r"minmax\(\s*0(?:px)?\s*,", track))


def test_first_grid_track_parser():
    assert _first_grid_track("display: grid; grid-template-columns: minmax(0, 1fr) auto auto; gap: 2px") == "minmax(0, 1fr)"
    assert _first_grid_track("grid-template-columns: 1fr auto") == "1fr"
    assert _first_grid_track("grid-template-columns: auto") == "auto"
    assert _first_grid_track("display: flex") is None
    assert _track_can_shrink_to_zero("minmax(0, 1fr)") and _track_can_shrink_to_zero("minmax( 0px , 20rem)")
    for bad in ("1fr", "auto", "minmax(10rem, 1fr)", "minmax(min-content, 1fr)", "max-content", "minmax(0.5rem, 1fr)"):
        assert not _track_can_shrink_to_zero(bad), bad


TABLET_HEADER_GRID = "grid-template-columns: minmax(0, 1fr) auto auto;"


@pytest.mark.parametrize("broken", ["grid-template-columns: 1fr auto auto;", "grid-template-columns: auto auto auto;",
                                    "grid-template-columns: minmax(min-content, 1fr) auto auto;", ""])
def test_status_bar_guard_fails_when_the_tablet_grid_column_cannot_shrink(monkeypatch, broken):
    """Mutation check of the guard above on the real stylesheet: the 600 to 899 px block alone, the rest untouched."""
    block = _media_block("min-width: 600px")
    assert block.count(TABLET_HEADER_GRID) == 1, "the mutation fixture no longer matches portal.css: update TABLET_HEADER_GRID"
    mutated = CSS.replace(block, block.replace(TABLET_HEADER_GRID, broken))
    assert mutated != CSS
    test_status_bar_state_gives_way_before_stop()  # the committed stylesheet passes
    monkeypatch.setattr(sys.modules[__name__], "CSS", mutated)
    with pytest.raises(AssertionError, match=r"min-width: 600px.*(first grid track|must declare)"):
        test_status_bar_state_gives_way_before_stop()


def savebar_fill_percent(css):
    """Opacity (%) of the surface colour in the glass save bar's colour-mix()."""
    m = re.search(r"\.savebar\s*\{[^}]*background:\s*color-mix\(in srgb,\s*var\(--surface\)\s*(\d+(?:\.\d+)?)%\s*,\s*transparent\)", css)
    assert m, "the .savebar colour-mix(var(--surface) N%, transparent) rule was not found"
    return float(m.group(1))


def _blend(fg, bg, alpha):
    ch = [round(alpha * int(fg[i:i + 2], 16) + (1 - alpha) * int(bg[i:i + 2], 16)) for i in (1, 3, 5)]
    return "#" + "".join(f"{c:02x}" for c in ch)


def savebar_failures(css):
    """Text on the translucent save bar, over the worst backdrop (black or white, whichever is nearer the text colour)."""
    alpha = savebar_fill_percent(css) / 100
    out = []
    for scheme, tokens in scheme_tokens(css).items():
        for backdrop in ("#000000", "#ffffff"):
            fill = _blend(tokens["surface"], backdrop, alpha)
            for fg in ("text", "muted"):
                ratio = contrast(tokens[fg], fill)
                if ratio < 4.5:
                    out.append(f"{scheme} {fg} on save bar over {backdrop}: {ratio:.2f}:1")
    return out


def test_savebar_glass_keeps_text_and_muted_legible_over_any_backdrop():
    assert not savebar_failures(CSS), "\n".join(savebar_failures(CSS))


def test_savebar_contrast_check_fails_at_84_percent():
    mutated = re.sub(r"(\.savebar\s*\{[^}]*color-mix\(in srgb,\s*var\(--surface\)\s*)\d+(%)", r"\g<1>84\2", CSS)
    assert savebar_fill_percent(mutated) == 84
    assert savebar_failures(mutated), "84% fill should drop below 4.5:1 for some scheme/backdrop"


# (scheme, a --muted value with too little contrast on that scheme's page) for the mutation check below
LOWERED_MUTED = {"light": "#8a97ab", "dark": "#4f5a6b"}


def _with_muted(css, scheme, value):
    """The stylesheet with the --muted declaration of one scheme replaced (light is the first, dark the second)."""
    found = list(re.finditer(r"--muted:\s*#[0-9a-fA-F]{3,6}\s*;", css))
    assert len(found) == 2, "expected one --muted per scheme"
    m = found[0 if scheme == "light" else 1]
    return css[:m.start()] + f"--muted: {value};" + css[m.end():]


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_lowering_muted_in_the_stylesheet_fails_the_contrast_test(monkeypatch, scheme):
    """chg021 V1 mutation check: the real contrast test, run on the stylesheet with a weaker --muted, must fail for
    that scheme and name the pair; the other scheme has its own value and still passes."""
    other = "dark" if scheme == "light" else "light"
    mutated = _with_muted(CSS, scheme, LOWERED_MUTED[scheme])
    assert scheme_tokens(mutated)[scheme]["muted"] == LOWERED_MUTED[scheme]
    assert scheme_tokens(mutated)[other]["muted"] == scheme_tokens(CSS)[other]["muted"]
    monkeypatch.setattr(sys.modules[__name__], "CSS", mutated)
    with pytest.raises(AssertionError, match=r"text muted on (bg|surface|raised)"):
        test_contrast_of_every_token_pair(scheme)
    test_contrast_of_every_token_pair(other)


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_muted_keeps_a_margin_over_the_text_minimum_on_every_surface(scheme):
    """--muted is the weakest text token: it must reach 4.5:1 on the page, the cards, the raised tiles and every tint."""
    tokens = scheme_tokens(CSS)[scheme]
    for bg in SURFACES:
        assert contrast(tokens["muted"], tokens[bg]) >= 4.5, f"{scheme}: muted on {bg}"


# ---- chg021 V7: motion lives in one prefers-reduced-motion: no-preference block ----

MOTION_QUERY = "(prefers-reduced-motion: no-preference)"
MOTION_PROPERTY = re.compile(r"^(animation|transition)(-[a-z-]+)?$")


def _strip_comments_and_strings(css):
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    return re.sub(r"\"(?:[^\"\\]|\\.)*\"|'(?:[^'\\]|\\.)*'", '""', css)


def motion_findings(css):
    """Walk the stylesheet by brace depth. Returns (outside, inside): lists of 'where: what' strings for every
    animation*/transition* declaration, @keyframes and scroll-behavior: smooth, split by whether an enclosing
    @media (prefers-reduced-motion: no-preference) block exists."""
    outside, inside = [], []
    stack = []
    segment = ""
    for ch in _strip_comments_and_strings(css):
        if ch == "{":
            stack.append(" ".join(segment.split()))
            segment = ""
        elif ch in ";}":
            text = " ".join(segment.split())
            segment = ""
            if text and ":" in text and stack:
                prop, _, value = text.partition(":")
                prop, value = prop.strip().lower(), value.strip().lower()
                if MOTION_PROPERTY.match(prop) or (prop == "scroll-behavior" and value == "smooth"):
                    in_motion = any(h.startswith("@media") and MOTION_QUERY in h for h in stack)
                    (inside if in_motion else outside).append(f"{stack[-1]}: {prop}: {value}")
            if ch == "}" and stack:
                stack.pop()
        else:
            segment += ch
    return outside, inside


def keyframes_outside_motion(css):
    """Names of @keyframes whose enclosing blocks do not include the no-preference media query."""
    bad, stack, segment = [], [], ""
    for ch in _strip_comments_and_strings(css):
        if ch == "{":
            header = " ".join(segment.split())
            if re.match(r"@(-webkit-)?keyframes\b", header) and not any(
                    h.startswith("@media") and MOTION_QUERY in h for h in stack):
                bad.append(header)
            stack.append(header)
            segment = ""
        elif ch == "}":
            if stack:
                stack.pop()
            segment = ""
        elif ch == ";":
            segment = ""
        else:
            segment += ch
    return bad


def test_every_animation_transition_and_keyframes_sits_in_the_no_preference_block():
    outside, inside = motion_findings(CSS)
    assert not outside, "motion outside @media (prefers-reduced-motion: no-preference):\n" + "\n".join(outside)
    assert not keyframes_outside_motion(CSS)
    assert any("animation:" in s for s in inside) and any("transition:" in s for s in inside), \
        "the parser found no motion at all: it is broken or the motion block was removed"
    assert len(re.findall(r"@keyframes\b", CSS)) >= 3


def test_motion_check_fails_when_a_transition_or_keyframes_leaves_the_block():
    moved = CSS.replace(".ring-arc { transition: stroke-dashoffset .25s linear; }", "", 1)
    moved += "\n.ring-arc { transition: stroke-dashoffset .25s linear; }\n"
    assert moved != CSS
    outside, _ = motion_findings(moved)
    assert outside and "transition" in outside[0]
    no_block = CSS.replace("@keyframes shimmer { to { background-position: 0 0; } }", "", 1)
    no_block += "\n@keyframes shimmer { to { background-position: 0 0; } }\n"
    assert keyframes_outside_motion(no_block) == ["@keyframes shimmer"]
    # an animation in a different media block (min-width) is outside too
    other = CSS + "\n@media (min-width: 600px) { .x { animation: a 1s; } }\n"
    assert motion_findings(other)[0]


def test_no_smooth_scrolling_outside_the_no_preference_block():
    outside, _ = motion_findings(CSS)
    assert not [s for s in outside if "scroll-behavior" in s]
    assert motion_findings(CSS + "\nhtml { scroll-behavior: smooth; }\n")[0]


def _rule_declarations(css, selector):
    """Declarations of every top-level (depth 0) rule whose selector list is exactly `selector`."""
    stripped = _strip_comments_and_strings(css)
    out = []
    for m in re.finditer(r"(?:^|(?<=\}))\s*([^{}@]+?)\s*\{([^{}]*)\}", stripped):
        if " ".join(m.group(1).split()) == selector:
            out.append(m.group(2))
    return out


def test_stop_sending_cue_is_static_outside_motion_and_changes_no_layout():
    before_motion = CSS.split("@media (prefers-reduced-motion: no-preference)")[0]
    bodies = _rule_declarations(before_motion, ".stop.is-sending")
    assert len(bodies) == 1, "the static .stop.is-sending rule must exist at top level (outside the motion block)"
    body = bodies[0]
    assert re.search(r"box-shadow:\s*inset\b", body), "the ring must be an inset box-shadow"
    assert not re.search(r"animation|transition", body)
    props = set(re.findall(r"([a-z-]+)\s*:", body))
    layout = {"width", "height", "padding", "border", "display", "margin", "min-width", "min-height", "border-width",
              "font-size", "font-weight", "letter-spacing", "gap", "position"}
    assert not [p for p in props if p in layout or p.startswith(("padding", "border", "margin"))], props
    # the animated variant in the motion block also only touches box-shadow
    assert re.search(r"@keyframes stop-ring\s*\{\s*50%\s*\{\s*box-shadow:[^;}]*;?\s*\}\s*\}", CSS)


def _mix_srgb(a, b, pct_a):
    """color-mix(in srgb, a pct_a%, b) as #rrggbb."""
    return _blend(a, b, pct_a / 100)


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_skeleton_shimmer_band_differs_visibly_from_the_skeleton_fill(scheme):
    """The band is color-mix(--border N%, --raised) over --raised. Threshold: WCAG contrast of band vs fill >= 1.15:1
    (visible but calm: a 1.0x ratio would be invisible, the 3:1 of a component border would be harsh)."""
    m = re.search(r"linear-gradient\(100deg,\s*var\(--raised\)\s*30%,\s*color-mix\(in srgb,\s*var\(--border\)\s*(\d+)%,\s*var\(--raised\)\)\s*50%", CSS)
    assert m, "the shimmer gradient was not found"
    tokens = scheme_tokens(CSS)[scheme]
    band = _mix_srgb(tokens["border"], tokens["raised"], int(m.group(1)))
    ratio = contrast(band, tokens["raised"])
    assert ratio >= 1.15, f"{scheme}: shimmer band {band} vs fill {tokens['raised']} is {ratio:.3f}:1"
    assert ratio <= 3.0, f"{scheme}: shimmer band is harsh ({ratio:.2f}:1)"

"""Web panel dev server and its Python model of the contract (tools/portal/, chg014 step 5).

The model (portal_model.py) runs against the vectors the firmware native tests use
(tools/tests/portal_api_vectors.json); the server is exercised over real HTTP on a free port with a fake clock.
Also keeps the committed web/portal/index.html in line with its sources.
"""

import http.client
import importlib.util
import json
import re
import sys
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
PORTAL_TOOLS = ROOT / "tools" / "portal"
VECTORS = json.loads((ROOT / "tools" / "tests" / "portal_api_vectors.json").read_text(encoding="utf-8"))


def load(name):
    sys.path.insert(0, str(PORTAL_TOOLS))
    try:
        spec = importlib.util.spec_from_file_location(name + "_under_test", PORTAL_TOOLS / (name + ".py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.remove(str(PORTAL_TOOLS))


model = load("portal_model")
build_page = load("build_page")
dev = load("dev_server")


def body_bytes(text):
    return text.encode("utf-8")


# ------------------------------------------------------------------------------------------ vectors vs model
def test_error_table_matches_vectors_and_firmware():
    expected = {e["error"]: e["status"] for e in VECTORS["errors"]}
    assert {c: s for c, (s, _r) in model.ERRORS.items()} == expected
    cpp = (ROOT / "src" / "network" / "portal_api.cpp").read_text(encoding="utf-8")
    firmware = {m.group(2): (int(m.group(1)), m.group(3))
                for m in re.finditer(r'\{(\d{3}), "([a-z_]+)", "([^"]*)"\}', cpp)}
    for code, (status, reason) in model.ERRORS.items():
        assert firmware[code] == (status, reason), code


def test_schema_matches_vectors():
    doc = model.schema()
    assert doc["fields"] == VECTORS["fields"]
    assert [p["name"] for p in doc["profiles"]] == VECTORS["profile_names"]
    assert doc["profiles"][1] == {"index": 1, "name": "DOUBLE", "weight_key": "double_weight_g",
                                  "time_key": "double_time_s"}


@pytest.mark.parametrize("case", VECTORS["patch_cases"], ids=lambda c: c["name"])
def test_patch_vectors(case):
    body = body_bytes(case["body"])
    if case["error"]:
        with pytest.raises(model.Refusal) as e:
            model.parse_patch(body)
        assert e.value.code == case["error"]
        return
    applied, adjusted = model.parse_patch(body)
    assert adjusted == case["adjusted"]
    assert set(applied) == set(case["values"])
    for key, value in case["values"].items():
        if isinstance(value, float):
            assert applied[key] == pytest.approx(value, abs=1e-4), key
        else:
            assert applied[key] == value, key


@pytest.mark.parametrize("case", VECTORS["command_cases"], ids=lambda c: c["name"])
def test_command_vectors(case):
    body = body_bytes(case["body"])
    if case["error"]:
        with pytest.raises(model.Refusal) as e:
            model.parse_command(body)
        assert e.value.code == case["error"]
        return
    assert model.parse_command(body) == (case["cmd"], case["profile"])


def test_host_origin_content_type_custom_header_vectors():
    for c in VECTORS["host_cases"]:
        assert model.host_valid(c["host"]) is c["ok"], c["host"]
    for c in VECTORS["origin_cases"]:
        try:
            model.check_origin(c["origin"], c["host"])
            ok = True
        except model.Refusal:
            ok = False
        assert ok is c["ok"], c
    for c in VECTORS["content_type_cases"]:
        try:
            model.check_content_type(c["content_type"])
            ok = True
        except model.Refusal:
            ok = False
        assert ok is c["ok"], c
    for c in VECTORS["custom_header_cases"]:
        try:
            model.check_custom_header(c["value"])
            ok = True
        except model.Refusal:
            ok = False
        assert ok is c["ok"], c


def test_decision_and_write_vectors():
    for c in VECTORS["decision_cases"]:
        got = model.decide_command(c["cmd"], c["ui"], c["busy"], c["update_held"], c["in_flight"],
                                   c["start_block"], c["motor_test_block"])
        assert got == c["error"], c["name"]
    for c in VECTORS["write_cases"]:
        got = model.decide_write(c["ui"], c["busy"], c.get("update_held", False), c["in_flight"])
        assert got == c["error"], c["name"]


def test_status_vectors():
    assert len(VECTORS["status_cases"]) >= 9
    for c in VECTORS["status_cases"]:
        i = c["input"]
        doc = model.status_document(i["ui_state"], i["grind_phase"], i["busy"], i["weight_g"], i["motor_available"],
                                    i["start_block"], i["motor_test_block"], i["uptime_s"], i["build"],
                                    i["web_interface"], i["warnings"])
        assert model.encode_json(doc) == c["json"], c["name"]


def test_warning_ordering_ignores_duplicates_and_unknown_codes():
    assert model.ordered_warnings([]) == []
    assert model.ordered_warnings(["load_cell_not_calibrated", "motor_fault", "motor_fault", "bogus"]) == \
        ["motor_fault", "load_cell_not_calibrated"]


def test_number_formatting_rule():
    assert model.encode_json({"a": 9.0, "b": 21.5, "c": 0.1, "d": 0.12345, "e": 3, "f": True, "g": None}) == \
        '{"a":9.0,"b":21.5,"c":0.1,"d":0.123,"e":3,"f":true,"g":null}'


# ------------------------------------------------------------------------------------------ the page
def test_committed_page_is_built_from_its_sources():
    committed = (ROOT / "web" / "portal" / "index.html").read_bytes().replace(b"\r\n", b"\n")
    assert committed == build_page.render_page().encode("utf-8"), "run python3 tools/portal/build_page.py"


def test_page_content_rules():
    page = (ROOT / "web" / "portal" / "index.html").read_text(encoding="utf-8")
    assert "<title>Grinder panel</title>" in page and "REFERENCE PAGE" not in page
    assert "prefers-color-scheme" in page
    assert "PortalClient" in page and "PortalView" in page
    assert not re.search(r"/\*@@PORTAL_[A-Z]+@@\*/", page), "an unreplaced marker is left in index.html"
    for forbidden in ("localStorage", "sessionStorage", "Authorization", "Bearer", "console.", "innerHTML",
                      "http://", "https://", "<script src", "<link "):
        # CSP default-src 'self': nothing external; the page keeps no state and shows no raw markup
        assert forbidden not in page.replace("http://' + ", "").replace("'http://'", ""), forbidden


def test_page_fits_the_embed_budget():
    import gzip
    # Hard budget is 64 KB (embed_portal.py). chg016 aimed for 32 KB, then raised the target to 40 KB (39.3 KB with the
    # Insights tiles and the history list). chg021 V0 strips comments and indentation at build time (about 30 KB) and
    # moves the cap to 48 KB to leave room for the redesign (plan: 46 KB after the V steps). Any further raise needs its
    # own reason; trim duplication in the sources first.
    assert len(gzip.compress((ROOT / "web" / "portal" / "index.html").read_bytes(), 9)) < 48 * 1024


def test_strip_js_drops_only_full_line_comments_and_indentation():
    src = "  // a comment\n\n  var a = 'x';  // trailing stays\n    var b = '//x';\n  var c = \"/* y */\";\n"
    assert build_page.strip_js(src) == "var a = 'x';  // trailing stays\nvar b = '//x';\nvar c = \"/* y */\";"


def test_strip_js_keeps_a_regex_and_a_url_like_string_untouched():
    src = "var r = /a\\/\\/b/;\nvar u = 'x//y';\n"
    assert build_page.strip_js(src) == "var r = /a\\/\\/b/;\nvar u = 'x//y';"


# chg021 V0: comment-like text inside strings (and a regex) that the strip must leave exactly as written
STRIP_FIXTURE_LINES = (
    "var a = 'see // here';",
    'var b = "a /* b */ c";',
    "var c = '//x';",
    'var d = "//" + \'/* not a comment */\';',
    "var e = 'x'; var f = '// two on a line'; // trailing comment is kept too",
    "'//starts the line inside a string';",
    '"/* starts the line */";',
    "var g = /\\/\\/.*$/.test('a//b');",
    "var h = `// in a one-line template /* x */`;",
)


def test_strip_js_leaves_comment_markers_inside_strings_untouched():
    src = "\n".join(STRIP_FIXTURE_LINES) + "\n"
    assert build_page.strip_js(src).split("\n") == list(STRIP_FIXTURE_LINES)
    # indented and between real comments: only the indentation and the full-line comments go
    padded = "// head\n" + "".join(f"    {line}\n  // between\n\n" for line in STRIP_FIXTURE_LINES)
    assert build_page.strip_js(padded).split("\n") == list(STRIP_FIXTURE_LINES)


def test_strip_css_leaves_comment_markers_inside_strings_untouched():
    lines = ('.a::before { content: "/* x */"; }', ".b::after { content: '//x'; }",
             '.c::after { content: "it\\"s /* still */ a string"; }', ".d { background: none; }")
    assert build_page.strip_css("\n".join(lines) + "\n").split("\n") == list(lines)


def test_built_page_carries_the_string_fixture_untouched(tmp_path):
    make_sources(tmp_path)
    (tmp_path / "portal-view.js").write_text("// header comment\n" + "".join(f"  {line}\n" for line in STRIP_FIXTURE_LINES),
                                             encoding="utf-8")
    (tmp_path / "portal.css").write_text("/* note */\n.a::before { content: \"/* x */ //y\"; }\n", encoding="utf-8")
    page = build_page.render_page(tmp_path)
    assert "\n".join(STRIP_FIXTURE_LINES) in page
    assert '.a::before { content: "/* x */ //y"; }' in page
    assert "header comment" not in page and "/* note */" not in page


def test_strip_js_refuses_a_multi_line_template_literal():
    with pytest.raises(build_page.PageBuildError, match="template literal"):
        build_page.strip_js("var t = `one\ntwo`;\n")


def test_strip_css_drops_comment_blocks_but_not_markers_or_strings():
    src = ("/* head\n   more */\n.a { color: red; } /* tail */\n  .b::after { content: '/* keep */'; }\n"
           "x/**/y\n/*@@PORTAL_CSS@@*/\n")
    out = build_page.strip_css(src)
    assert out == ".a { color: red; }\n.b::after { content: '/* keep */'; }\nx y\n/*@@PORTAL_CSS@@*/"


def test_strip_css_refuses_an_unterminated_comment():
    with pytest.raises(build_page.PageBuildError, match="unterminated"):
        build_page.strip_css("a { } /* open\n")


def test_render_page_strips_parts_unless_asked_not_to(tmp_path):
    make_sources(tmp_path)
    (tmp_path / "portal-app.js").write_text("// note\n  var app = '//x';\n", encoding="utf-8")
    assert "// note" not in build_page.render_page(tmp_path)
    assert "\nvar app = '//x';" in build_page.render_page(tmp_path)
    assert "// note" in build_page.render_page(tmp_path, strip=False)


def test_sources_have_no_multi_line_template_literals():
    for _m, name, _c in build_page.PARTS:
        if name.endswith(".js"):  # strip_js raises when it finds one
            build_page.strip_js((ROOT / "web" / "portal" / name).read_text(encoding="utf-8"), name)


def _node():
    import shutil
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    return node


def test_every_inlined_script_of_the_built_page_passes_node_check(tmp_path):
    import subprocess
    page = (ROOT / "web" / "portal" / "index.html").read_text(encoding="utf-8")
    scripts = re.findall(r"<script>(.*?)</script>", page, re.S)
    assert len(scripts) == 3
    for i, body in enumerate(scripts):
        path = tmp_path / f"inline{i}.js"
        path.write_text(body, encoding="utf-8")
        done = subprocess.run([_node(), "--check", str(path)], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr


def test_node_tests_pass_against_the_stripped_parts(tmp_path):
    """The behaviour tests re-run on the stripped modules in a scratch tree, so a bad strip cannot ship. One test that
    reads the source text for its indentation is skipped there."""
    import shutil
    import subprocess
    (tmp_path / "tools" / "tests").mkdir(parents=True)
    (tmp_path / "tools" / "portal").mkdir()
    (tmp_path / "web" / "portal").mkdir(parents=True)
    for f in ("portal-client.test.js", "portal-view.test.js", "portal_api_vectors.json"):
        shutil.copy(ROOT / "tools" / "tests" / f, tmp_path / "tools" / "tests" / f)
    shutil.copy(PORTAL_TOOLS / "portal_model.py", tmp_path / "tools" / "portal" / "portal_model.py")
    for f in ("page.src.html", "index.html"):
        shutil.copy(ROOT / "web" / "portal" / f, tmp_path / "web" / "portal" / f)
    for _m, name, _c in build_page.PARTS:
        text = (ROOT / "web" / "portal" / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        stripped = build_page.strip_css(text, name) if name.endswith(".css") else build_page.strip_js(text, name)
        (tmp_path / "web" / "portal" / name).write_text(stripped, encoding="utf-8")
    done = subprocess.run([_node(), "--test", "--test-skip-pattern=page source wires the stale-reply guard",
                           "tools/tests/portal-client.test.js", "tools/tests/portal-view.test.js"],
                          cwd=tmp_path, capture_output=True, text=True)
    assert done.returncode == 0, done.stdout[-3000:] + done.stderr[-1000:]


def make_sources(directory):
    """A minimal valid source set for build_page.render_page."""
    files = {
        "page.src.html": "<style>\n/*@@PORTAL_CSS@@*/\n</style>\n<script>\n/*@@PORTAL_CLIENT@@*/\n</script>\n"
                         "<script>\n/*@@PORTAL_VIEW@@*/\n</script>\n<script>\n/*@@PORTAL_APP@@*/\n</script>\n",
        "portal.css": "a { color: red; }\n",
        "portal-client.js": "var client;\n",
        "portal-view.js": "var view;\n",
        "portal-app.js": "var app;\n",
    }
    for name, text in files.items():
        (directory / name).write_bytes(text.encode("utf-8"))  # bytes: no platform newline translation


def test_build_page_inlines_every_part(tmp_path):
    make_sources(tmp_path)
    page = build_page.render_page(tmp_path)
    for part in ("a { color: red; }", "var client;", "var view;", "var app;"):
        assert part in page
    assert "@@PORTAL" not in page


def test_build_page_crlf_sources_build_the_same_bytes(tmp_path):
    lf = tmp_path / "lf"
    crlf = tmp_path / "crlf"
    lf.mkdir()
    crlf.mkdir()
    make_sources(lf)
    make_sources(crlf)
    for path in crlf.iterdir():
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
    assert build_page.render_page(lf) == build_page.render_page(crlf)


def test_build_page_keeps_marker_text_inside_a_part_verbatim(tmp_path):
    make_sources(tmp_path)
    (tmp_path / "portal-app.js").write_text("var m = '/*@@PORTAL_VIEW@@*/';\n", encoding="utf-8")
    page = build_page.render_page(tmp_path)
    assert "var m = '/*@@PORTAL_VIEW@@*/';" in page  # not expanded a second time
    assert page.count("var view;") == 1


def test_stop_is_the_first_button_after_the_skip_link_and_never_disabled():
    page = (ROOT / "web" / "portal" / "index.html").read_text(encoding="utf-8")
    body = page[page.index("<body>"):]
    assert body.index('class="skip-link"') < body.index("<button")
    first = re.search(r"<button\b[^>]*>", body).group(0)
    assert 'id="stop"' in first and "disabled" not in first
    for tag in re.findall(r"<button\b[^>]*\bid=\"stop(?:-pane)?\"[^>]*>", body):
        assert "disabled" not in tag, tag


def _page_body():
    page = (ROOT / "web" / "portal" / "index.html").read_text(encoding="utf-8")
    return page[page.index("<body>"):page.index("<script>")]


def test_focus_order_in_the_markup_is_skip_link_stop_navigation_content():
    """chg021 V3: the tab order follows the DOM (skip link, STOP, navigation, content) and nothing reorders it."""
    body = _page_body()
    order = [body.index('class="skip-link"'), body.index('id="stop"'), body.index("<nav"), body.index("<main")]
    assert order == sorted(order), order
    # nothing focusable between the skip link and STOP, or between STOP and the navigation
    focusable = r"<(?:a|button|input|select|textarea)\b|tabindex="
    assert not re.search(focusable, body[body.index("</a>", order[0]):body.index("<button", order[0])])
    assert not re.search(focusable, body[body.index("</button>", order[1]):order[2]])
    # a positive tabindex would jump the queue ahead of STOP
    assert not re.search(r"tabindex=\"[1-9]", body)
    for name in ("portal-app.js", "portal-view.js"):
        assert not re.search(r"tabIndex\s*=\s*[1-9]", (ROOT / "web" / "portal" / name).read_text(encoding="utf-8")), name


def test_navigation_has_four_sections_and_marks_the_current_one():
    """chg021 V3: Grind, Settings, Insights, Device in one <nav>; the shown section carries aria-current."""
    body = _page_body()
    nav = body[body.index("<nav"):body.index("</nav>")]
    assert re.search(r"<nav\b[^>]*aria-label=", nav)
    tabs = re.findall(r"<button\b[^>]*>", nav)
    assert [re.search(r'data-tab="([a-z]+)"', t).group(1) for t in tabs] == ["grind", "settings", "insights", "device"]
    assert [re.search(r'id="([a-z-]+)"', t).group(1) for t in tabs] == ["tab-grind", "tab-settings", "tab-insights", "tab-device"]
    current = [t for t in tabs if "aria-current" in t]
    assert len(current) == 1 and 'id="tab-grind"' in current[0] and 'aria-current="true"' in current[0]
    for tab in tabs:
        assert "disabled" not in tab and 'type="button"' in tab
    # each section has its panel; only the first is shown before the script runs
    for name in ("grind", "settings", "insights", "device"):
        panel = re.search(r'<section\b[^>]*id="panel-' + name + r'"[^>]*>', body)
        assert panel, name
        assert (" hidden" in panel.group(0)) is (name != "grind"), name
    app = (ROOT / "web" / "portal" / "portal-app.js").read_text(encoding="utf-8")
    assert "setAttribute('aria-current', 'true')" in app and "removeAttribute('aria-current')" in app
    assert "aria-selected" not in body and "aria-selected" not in app  # one way of marking the section, not two


def test_device_section_placeholder_has_the_about_rows():
    body = _page_body()
    device = body[body.index('id="panel-device"'):]
    for element_id in ("about-name", "about-address", "about-firmware"):
        assert f'id="{element_id}"' in device, element_id


@pytest.mark.parametrize("marker", [m for m, _f, _c in build_page.PARTS])
def test_build_page_rejects_missing_and_duplicated_marker(tmp_path, marker):
    make_sources(tmp_path)
    shell = (tmp_path / "page.src.html").read_text(encoding="utf-8")
    (tmp_path / "page.src.html").write_text(shell.replace(marker, ""), encoding="utf-8")
    with pytest.raises(build_page.PageBuildError, match="exactly once"):
        build_page.render_page(tmp_path)
    (tmp_path / "page.src.html").write_text(shell + marker, encoding="utf-8")
    with pytest.raises(build_page.PageBuildError, match="exactly once"):
        build_page.render_page(tmp_path)


@pytest.mark.parametrize("name,closing", [(f, c) for _m, f, c in build_page.PARTS])
def test_build_page_rejects_closing_tag_inside_a_part(tmp_path, name, closing):
    make_sources(tmp_path)
    (tmp_path / name).write_text(f"var s = '{closing.upper()}>';", encoding="utf-8")
    with pytest.raises(build_page.PageBuildError, match=name):
        build_page.render_page(tmp_path)


# ------------------------------------------------------------------------------------------ server over HTTP
class FakeClock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


@pytest.fixture
def server():
    clock = FakeClock()
    sim = dev.SimGrinder(clock=clock)
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), dev.make_handler(sim, "<html>page</html>"))
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    httpd.sim, httpd.clock = sim, clock
    yield httpd
    httpd.shutdown()
    httpd.server_close()
    thread.join(timeout=5)


def call(httpd, method, path, body=None, headers=None, host=None, json_headers=True):
    port = httpd.server_address[1]
    h = {}
    if body is not None and json_headers:
        h["Content-Type"] = "application/json"
        h["X-Grinder"] = "1"
    h.update(headers or {})
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
    conn.putheader("Host", host if host is not None else f"127.0.0.1:{port}")
    for k, v in h.items():
        conn.putheader(k, v)
    data = body.encode("utf-8") if isinstance(body, str) else body
    if data is not None:
        conn.putheader("Content-Length", str(len(data)))
    conn.endheaders(data)
    res = conn.getresponse()
    raw = res.read()
    conn.close()
    try:
        doc = json.loads(raw)
    except ValueError:
        doc = None
    return res.status, doc, res


def test_info_is_open_even_when_web_is_off(server):
    server.sim.web_interface = False
    status, doc, _ = call(server, "GET", "/api/info", host="evil.com")
    assert status == 200 and doc["name"] == "grindbyweight"


@pytest.mark.parametrize("path", ["/", "/api/status", "/api/settings", "/api/schema", "/api/stats"])
def test_web_off_refuses_page_and_reads(server, path):
    server.sim.web_interface = False
    status, doc, _ = call(server, "GET", path)
    assert status == 403 and doc["error"] == "web_interface_off"
    assert doc["reason"]


def test_web_off_refuses_writes_and_stop(server):
    server.sim.web_interface = False
    assert call(server, "PATCH", "/api/settings", '{"logging":true}')[1]["error"] == "web_interface_off"
    assert call(server, "POST", "/api/command", '{"cmd":"stop"}')[1]["error"] == "web_interface_off"


def test_page_headers(server):
    status, _, res = call(server, "GET", "/")
    assert status == 200
    assert res.getheader("Cache-Control") == "no-cache"
    assert res.getheader("X-Content-Type-Options") == "nosniff"
    assert "frame-ancestors 'none'" in res.getheader("Content-Security-Policy")
    assert call(server, "GET", "/index.html")[0] == 200


def test_host_guard_applies_to_reads_and_origin_guard(server):
    assert call(server, "GET", "/api/status", host="localhost:80")[1]["error"] == "bad_host"
    assert call(server, "GET", "/api/status", headers={"Origin": "http://evil.com"})[1]["error"] == "bad_origin"
    port = server.server_address[1]
    assert call(server, "GET", "/api/status", headers={"Origin": f"http://127.0.0.1:{port}"})[0] == 200


def test_state_changing_guards_in_order(server):
    body = '{"logging":true}'
    s, d, _ = call(server, "PATCH", "/api/settings", body, json_headers=False)
    assert (s, d["error"]) == (415, "unsupported_media_type")
    s, d, _ = call(server, "PATCH", "/api/settings", body, headers={"Content-Type": "application/json"},
                   json_headers=False)
    assert (s, d["error"]) == (403, "missing_custom_header")
    s, d, _ = call(server, "PATCH", "/api/settings", body, host="evil.com")
    assert (s, d["error"]) == (403, "bad_host")


def test_schema_and_defaults_served(server):
    assert call(server, "GET", "/api/schema")[1]["fields"] == VECTORS["fields"]
    doc = call(server, "GET", "/api/settings")[1]
    assert doc["current_profile"] == 1
    assert doc["values"]["brightness_screensaver"] == 35
    assert {f["key"]: f["default"] for f in VECTORS["fields"]} == doc["values"]


def test_patch_roundtrip_clamps_and_reports_adjusted(server):
    s, d, res = call(server, "PATCH", "/api/settings", '{"brightness_normal":5,"swipe":true,"freshness_hours":11}')
    assert s == 200 and d["ok"] is True
    assert d["adjusted"] == ["brightness_normal", "freshness_hours"]
    assert d["values"]["brightness_normal"] == 15 and d["values"]["freshness_hours"] == 12.0
    assert res.getheader("Cache-Control") == "no-store"
    assert call(server, "GET", "/api/settings")[1]["values"] == d["values"]


def test_screen_timeouts_round_trip_through_the_server(server):
    values = call(server, "GET", "/api/settings")[1]["values"]
    assert values["screen_dim_min"] == 5 and values["screen_off_min"] == 60
    assert list(values)[11:13] == ["screen_dim_min", "screen_off_min"]
    s, d, _ = call(server, "PATCH", "/api/settings", '{"screen_dim_min":10,"screen_off_min":240}')
    assert s == 200 and d["adjusted"] == []
    assert d["values"]["screen_dim_min"] == 10 and d["values"]["screen_off_min"] == 240
    assert call(server, "GET", "/api/settings")[1]["values"] == d["values"]
    # one key leaves the other, Never reads back as 0, snapping is reported
    d = call(server, "PATCH", "/api/settings", '{"screen_off_min":0}')[1]
    assert d["values"]["screen_off_min"] == 0 and d["values"]["screen_dim_min"] == 10 and d["adjusted"] == []
    d = call(server, "PATCH", "/api/settings", '{"screen_dim_min":0}')[1]
    assert d["values"]["screen_dim_min"] == 0 and d["values"]["screen_off_min"] == 0
    d = call(server, "PATCH", "/api/settings", '{"screen_dim_min":7,"screen_off_min":100000}')[1]
    assert d["values"]["screen_dim_min"] == 5 and d["values"]["screen_off_min"] == 480
    assert d["adjusted"] == ["screen_dim_min", "screen_off_min"]
    d = call(server, "PATCH", "/api/settings", '{"screen_dim_min":-3,"screen_off_min":20}')[1]
    assert d["values"]["screen_dim_min"] == 0 and d["values"]["screen_off_min"] == 10
    # stored as the minutes they report (not run through the brightness conversion)
    assert call(server, "GET", "/api/settings")[1]["values"]["screen_off_min"] == 10


def test_screen_timeouts_do_not_disturb_profile_keys_or_the_device_name(server):
    s, d, _ = call(server, "PATCH", "/api/settings",
                   '{"screen_off_min":30,"single_weight_g":10,"custom_time_s":4,"device_name":"kitchen"}')
    assert s == 200
    values = d["values"]
    assert values["single_weight_g"] == 10 and values["custom_time_s"] == 4 and values["device_name"] == "kitchen"
    assert values["screen_off_min"] == 30 and values["screen_dim_min"] == 5
    assert values["double_weight_g"] == 18.0 and values["brightness_screensaver"] == 35
    # the schema's profile entries still name the profile keys
    profiles = call(server, "GET", "/api/schema")[1]["profiles"]
    assert profiles[0]["weight_key"] == "single_weight_g" and profiles[2]["time_key"] == "custom_time_s"


@pytest.mark.parametrize("body,error", [
    ('{"screen_dim_min":"5"}', "wrong_type"), ('{"screen_off_min":true}', "wrong_type"),
    ('{"screen_off_min":null}', "wrong_type"), ('{"screen_dim_min":[5]}', "nested_value"),
    ('{"screen_dim_min":1e999}', "invalid_value"), ('{"screen_on_min":5}', "unknown_key"),
    ('{"screen_dim_min":10,"screen_off_min":"x"}', "wrong_type")])
def test_screen_timeout_refusals_apply_nothing(server, body, error):
    s, d, _ = call(server, "PATCH", "/api/settings", body)
    assert s == 400 and d["error"] == error
    values = call(server, "GET", "/api/settings")[1]["values"]
    assert values["screen_dim_min"] == 5 and values["screen_off_min"] == 60


def test_screen_timeout_writes_are_refused_like_other_settings(server):
    body = '{"screen_dim_min":10}'
    server.sim.update_held = True
    assert call(server, "PATCH", "/api/settings", body)[1]["error"] == "update_in_progress"
    server.sim.update_held = False
    server.sim.ui_busy = True
    s, d, _ = call(server, "PATCH", "/api/settings", body)
    assert (s, d["error"]) == (503, "ui_busy")
    server.sim.ui_busy = False
    server.sim.ui_state = "edit"
    s, d, _ = call(server, "PATCH", "/api/settings", body)
    assert (s, d["error"]) == (409, "not_ready")
    server.sim.ui_state = "menu"
    server.sim.in_flight = True
    assert call(server, "PATCH", "/api/settings", body)[1]["error"] == "command_in_flight"
    server.sim.in_flight = False
    server.sim.store_failed = True
    assert call(server, "PATCH", "/api/settings", body)[1]["error"] == "store_failed"
    assert call(server, "PATCH", "/api/settings", '{"screen_dim_min":5}')[0] == 200  # unchanged: not written
    server.sim.store_failed = False
    assert call(server, "GET", "/api/settings")[1]["values"]["screen_dim_min"] == 5  # nothing got through
    assert call(server, "PATCH", "/api/settings", body)[0] == 200


def test_patch_refusal_applies_nothing(server):
    s, d, _ = call(server, "PATCH", "/api/settings", '{"logging":true,"single_weight_g":4}')
    assert (s, d["error"]) == (400, "out_of_range")
    assert call(server, "GET", "/api/settings")[1]["values"]["logging"] is False
    assert call(server, "PATCH", "/api/settings", '{"current_profile":2}')[1]["error"] == "unknown_key"
    assert call(server, "PATCH", "/api/settings", "{}")[1]["error"] == "empty_patch"
    assert call(server, "PATCH", "/api/settings", "")[1]["error"] == "empty_body"
    s, d, _ = call(server, "PATCH", "/api/settings", '{"a":"' + "x" * 600 + '"}')
    assert (s, d["error"]) == (413, "body_too_large")


def test_device_name_reply_has_address_only_on_change(server):
    d = call(server, "PATCH", "/api/settings", '{"device_name":"kitchen"}')[1]
    assert d["address"] == "kitchen.local"
    d = call(server, "PATCH", "/api/settings", '{"device_name":"kitchen"}')[1]
    assert "address" not in d
    assert call(server, "PATCH", "/api/settings", '{"device_name":"Kitchen"}')[1]["error"] == "invalid_device_name"
    assert call(server, "GET", "/api/info")[1]["name"] == "kitchen"


@pytest.mark.parametrize("ui,expected", [("menu", 200), ("edit", 409)])
def test_write_decision_by_screen(server, ui, expected):
    server.sim.ui_state = ui
    s, d, _ = call(server, "PATCH", "/api/settings", '{"logging":true}')
    assert s == expected
    if expected == 409:
        assert d["error"] == "not_ready"


def test_update_held_refuses_writes_and_commands_but_not_stop(server):
    server.sim.update_held = True
    assert call(server, "PATCH", "/api/settings", '{"logging":true}')[1]["error"] == "update_in_progress"
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "update_in_progress"
    assert call(server, "POST", "/api/command", '{"cmd":"stop"}')[0] == 200


def test_start_lead_in_grind_and_stop(server):
    s, d, _ = call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    assert (s, d) == (202, {"ok": True, "cmd": "start", "profile": 0, "lead_in_ms": 3000})
    # during the lead-in everything but stop is refused as in flight
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "command_in_flight"
    assert call(server, "PATCH", "/api/settings", '{"logging":true}')[1]["error"] == "command_in_flight"
    assert call(server, "GET", "/api/status")[1]["busy"] is False
    server.clock.now += 3.5
    status = call(server, "GET", "/api/status")[1]
    assert status["busy"] is True and status["ui_state"] == "grinding" and status["grind_phase"] == "predictive"
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "busy"
    server.clock.now += 20
    status = call(server, "GET", "/api/status")[1]
    assert status["ui_state"] == "grind_complete" and status["weight_g"] == 9.0
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "not_ready"
    server.clock.now += 4
    assert call(server, "GET", "/api/status")[1]["ui_state"] == "ready"


def test_stop_cancels_lead_in(server):
    call(server, "POST", "/api/command", '{"cmd":"start","profile":2}')
    assert call(server, "POST", "/api/command", '{"cmd":"stop"}')[0] == 200
    server.clock.now += 10
    status = call(server, "GET", "/api/status")[1]
    assert status["busy"] is False and status["ui_state"] == "ready"


def test_time_mode_start_uses_time_target(server):
    call(server, "PATCH", "/api/settings", '{"grind_mode":"time","single_time_s":2}')
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    server.clock.now += 3.0 + 1.0
    assert call(server, "GET", "/api/status")[1]["grind_phase"] == "time_grinding"
    server.clock.now += 1.5
    assert call(server, "GET", "/api/status")[1]["ui_state"] == "grind_complete"


def test_tare_motor_test_and_blocks(server):
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[0] == 200
    s, d, _ = call(server, "POST", "/api/command", '{"cmd":"motor_test"}')
    assert (s, d) == (202, {"ok": True, "cmd": "motor_test"})
    assert call(server, "GET", "/api/status")[1]["busy"] is True
    server.clock.now += 2
    assert call(server, "GET", "/api/status")[1]["busy"] is False
    server.sim.start_block = "load_cell_no_data"
    assert call(server, "POST", "/api/command", '{"cmd":"start","profile":1}')[1]["error"] == "load_cell_no_data"
    assert call(server, "GET", "/api/status")[1]["start_block"] == "load_cell_no_data"
    server.sim.motor_test_block = "motor_fault"
    assert call(server, "POST", "/api/command", '{"cmd":"motor_test"}')[1]["error"] == "motor_fault"


def test_command_body_refusals(server):
    assert call(server, "POST", "/api/command", '{"cmd":"start"}')[1]["error"] == "missing_field"
    assert call(server, "POST", "/api/command", '{"cmd":"stop","profile":1}')[1]["error"] == "unexpected_field"
    assert call(server, "POST", "/api/command", '{"cmd":"dance"}')[1]["error"] == "unknown_command"
    assert call(server, "POST", "/api/command", '{"cmd":"start","profile":3}')[1]["error"] == "out_of_range"


def test_status_shape(server):
    doc = call(server, "GET", "/api/status")[1]
    assert set(doc) == {"ui_state", "grind_phase", "busy", "weight_g", "motor_available", "start_block",
                        "motor_test_block", "uptime_s", "build", "web_interface", "warnings"}
    assert doc["web_interface"] is True and doc["start_block"] is None


def test_status_warnings_member_is_last_and_follows_dev_state(server):
    raw = call(server, "GET", "/api/status")[1]
    assert raw["warnings"] == [] and list(raw)[-2:] == ["web_interface", "warnings"]
    s, d, _ = call(server, "POST", "/dev/state", '{"warnings":["load_cell_not_calibrated","motor_fault","motor_fault"]}')
    assert s == 200 and d["warnings"] == ["motor_fault", "load_cell_not_calibrated"]
    assert call(server, "GET", "/api/status")[1]["warnings"] == ["motor_fault", "load_cell_not_calibrated"]
    for bad in ('{"warnings":"motor_fault"}', '{"warnings":["bogus"]}', '{"warnings":[1]}', '{"warnings":null}'):
        assert call(server, "POST", "/dev/state", bad)[0] == 400
    assert call(server, "POST", "/dev/state", '{"warnings":[]}')[1]["warnings"] == []


def test_unknown_paths_and_methods(server):
    assert call(server, "GET", "/nope")[0] == 404
    assert call(server, "GET", "/api/command")[0] == 405
    assert call(server, "POST", "/api/status", "{}")[0] == 405


def test_dev_state_endpoint(server):
    s, d, _ = call(server, "POST", "/dev/state", '{"web_interface":false,"ui_state":"menu"}')
    assert s == 200 and d["web_interface"] is False and d["ui_state"] == "menu"
    assert call(server, "GET", "/api/status")[1]["error"] == "web_interface_off"
    assert call(server, "POST", "/dev/state", '{"ui_state":"bogus"}')[0] == 400
    assert call(server, "POST", "/dev/state", '{"nope":1}')[0] == 400
    s, d, _ = call(server, "POST", "/dev/state", '{"web_interface":true}', json_headers=False)
    assert (s, d["error"]) == (415, "unsupported_media_type")
    s, d, _ = call(server, "POST", "/dev/state", '{"web_interface":true}', headers={"Content-Type": "application/json"},
                   json_headers=False)
    assert (s, d["error"]) == (403, "missing_custom_header")


def test_cli_arguments():
    args = dev.build_parser().parse_args(["--web-off", "--ui", "menu", "--start-block", "motor_disabled"])
    assert args.web_off and args.ui == "menu" and args.start_block == "motor_disabled"
    assert args.host == "127.0.0.1"


def test_purge_amount_half_rounds_up_like_the_firmware():
    # Python's round() is half-even (0.25 -> 0.2); the firmware's (int)(x * 10 + 0.5f) rounds half up
    for given, expected in ((0.25, 0.3), (0.35, 0.4), (0.45, 0.5), (1.25, 1.3), (2.45, 2.5), (0.15, 0.2), (1.34, 1.3)):
        assert model.normalise_purge_amount(given) == pytest.approx(expected, abs=1e-6), given
    for text, expected in (("0.25", 0.3), ("0.45", 0.5), ("1.25", 1.3)):
        applied, adjusted = model.parse_patch(body_bytes('{"purge_amount_g":' + text + '}'))
        assert applied["purge_amount_g"] == pytest.approx(expected, abs=1e-6) and adjusted == ["purge_amount_g"]


# ------------------------------------------------------------------------------------------ live view (chg016 step 2)
def test_app_only_uses_element_ids_the_page_has():
    page = (ROOT / "web" / "portal" / "page.src.html").read_text(encoding="utf-8")
    app = (ROOT / "web" / "portal" / "portal-app.js").read_text(encoding="utf-8")
    ids = set(re.findall(r"\$\('([a-z0-9-]+)'\)", app))
    ids |= {f"{prefix}{i}" for prefix in ("start-", "target-", "badge-", "reason-start-") for i in range(3)} \
        if "'start-' + i" in app else set()
    ids |= {f"{prefix}{tab}" for prefix in ("panel-", "tab-") for tab in ("grind", "settings", "insights", "device")}
    ids = {i for i in ids if not i.endswith("-")}
    missing = sorted(i for i in ids if f'id="{i}"' not in page)
    assert not missing, f"portal-app.js uses ids that page.src.html lacks: {missing}"


def test_stop_is_never_disabled_by_the_page_code():
    for name in ("portal-app.js", "portal-view.js"):
        text = (ROOT / "web" / "portal" / name).read_text(encoding="utf-8")
        assert not re.search(r"\$\('stop(-pane)?'\)\.disabled", text), name
    app = (ROOT / "web" / "portal" / "portal-app.js").read_text(encoding="utf-8")
    # STOP is wired straight to the client, with no confirm() and no enabled-check in front of it
    assert "confirm(" not in app and "prompt(" not in app
    assert "$('stop').addEventListener('click', stop)" in app


def test_every_start_tare_and_motor_test_button_has_visible_reason_text():
    page = (ROOT / "web" / "portal" / "page.src.html").read_text(encoding="utf-8")
    for button, reason in [("start-0", "reason-start-0"), ("start-1", "reason-start-1"), ("start-2", "reason-start-2"),
                           ("tare", "reason-tare"), ("motor-test", "reason-motor-test")]:
        tag = re.search(rf'<button id="{button}"[^>]*>', page).group(0)
        assert f'aria-describedby="{reason}"' in tag
        assert f'id="{reason}"' in page
    # the Start reason is shown once, visibly, under the Profiles heading (the per-button ones are screen-reader only)
    visible = re.search(r'<[a-z]+ id="reason-start"[^>]*>', page)
    assert visible, "page lacks #reason-start"
    assert not re.search(r'sr-only|hidden', visible.group(0)), visible.group(0)
    assert page.index("<h2>Profiles</h2>") < visible.start() < page.index('id="profile-0"')


def test_round_start_buttons_are_named_and_described_by_existing_ids():
    page = (ROOT / "web" / "portal" / "page.src.html").read_text(encoding="utf-8")
    for i, name in enumerate(("Single", "Double", "Custom")):
        tag = re.search(rf'<button id="start-{i}"[^>]*>', page).group(0)
        assert f'aria-label="Start {name}"' in tag and 'class="btn primary round"' in tag
        described = re.search(r'aria-describedby="([^"]+)"', tag).group(1)
        for ident in described.split():
            assert page.count(f'id="{ident}"') == 1, ident


def test_weight_is_not_inside_a_live_region():
    page = (ROOT / "web" / "portal" / "page.src.html").read_text(encoding="utf-8")
    for ident in ("readout-weight", "strip-weight"):
        pos = page.index(f'id="{ident}"')
        # walk the open tags before the weight: none that is still open may carry a live role
        open_stack = []
        for m in re.finditer(r"<(/?)([a-z0-9]+)([^>]*)>", page[:pos]):
            closing, tag, attrs = m.groups()
            if tag in ("meta", "link", "br", "img", "input", "use", "circle", "path", "rect", "source"):
                continue
            if attrs.endswith("/"):
                continue
            if closing:
                if open_stack and open_stack[-1][0] == tag:
                    open_stack.pop()
            else:
                open_stack.append((tag, attrs))
        for tag, attrs in open_stack:
            assert "aria-live" not in attrs and 'role="status"' not in attrs and 'role="alert"' not in attrs, (ident, tag, attrs)


def test_real_page_is_served_and_drives_a_remote_start(server):
    """Smoke: the page the dev server serves, then the calls the page makes for Start, in order."""
    page = build_page.render_page()
    server.RequestHandlerClass = dev.make_handler(server.sim, page)
    status, _, res = call(server, "GET", "/")
    assert status == 200
    assert call(server, "GET", "/api/status")[1]["start_block"] is None
    status, doc, _ = call(server, "POST", "/api/command", '{"cmd":"start","profile":1}')
    assert status == 202 and doc["lead_in_ms"] == 3000
    # during the lead-in the status still reads ready and not busy (contract section 7)
    st = call(server, "GET", "/api/status")[1]
    assert st["ui_state"] == "ready" and st["busy"] is False
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "command_in_flight"
    server.clock.now += 3.5
    st = call(server, "GET", "/api/status")[1]
    assert st["busy"] is True and st["ui_state"] == "grinding"
    assert call(server, "POST", "/api/command", '{"cmd":"stop"}')[0] == 200
    assert call(server, "GET", "/api/status")[1]["busy"] is False


# ------------------------------------------------------------------------------------------ chg016 step 5: dev switches
UPDATE_CASES = [c for c in VECTORS["decision_cases"]
                if c["error"] == "update_in_progress" and c["cmd"] in ("start", "motor_test")]


def _vector_block(name):
    return None if name in ("NONE", "START_FAILED") else name.lower()


@pytest.mark.parametrize("case", UPDATE_CASES, ids=lambda c: c["name"])
def test_update_status_blocks_match_the_update_refusal_vectors(case):
    sim = dev.SimGrinder(clock=FakeClock())
    sim.update_held = case["update_held"]
    sim.start_block = _vector_block(case["start_block"])
    sim.motor_test_block = _vector_block(case["motor_test_block"])
    status = sim.status_doc()
    key = "start_block" if case["cmd"] == "start" else "motor_test_block"
    assert status[key] == "update_in_progress"


def test_update_held_status_keeps_screen_and_shape(server):
    server.sim.update_held = True
    doc = call(server, "GET", "/api/status")[1]
    assert doc["start_block"] == "update_in_progress" and doc["motor_test_block"] == "update_in_progress"
    assert doc["ui_state"] == "ready" and doc["motor_available"] is True and doc["busy"] is False
    assert set(doc) == {"ui_state", "grind_phase", "busy", "weight_g", "motor_available", "start_block",
                        "motor_test_block", "uptime_s", "build", "web_interface", "warnings"}
    server.sim.update_held = False
    assert call(server, "GET", "/api/status")[1]["start_block"] is None


def test_update_lock_out_follows_the_firmware_block_order(server):
    server.sim.update_held = True
    server.sim.start_block = "load_cell_no_data"
    server.sim.motor_test_block = "busy"
    doc = call(server, "GET", "/api/status")[1]
    assert doc["start_block"] == "update_in_progress" and doc["motor_test_block"] == "update_in_progress"
    # a disabled motor and a latched fault come before the lock-out in the firmware
    server.sim.start_block = "motor_disabled"
    server.sim.motor_test_block = "motor_fault"
    doc = call(server, "GET", "/api/status")[1]
    assert doc["start_block"] == "motor_disabled" and doc["motor_test_block"] == "motor_fault"
    assert doc["motor_available"] is False


def test_update_held_refuses_start_with_the_status_reason(server):
    server.sim.update_held = True
    assert call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')[1]["error"] == "update_in_progress"
    assert call(server, "POST", "/api/command", '{"cmd":"motor_test"}')[1]["error"] == "update_in_progress"


def test_drop_start_switch_drops_the_lead_in(server):
    server.sim.drop_start = True
    assert call(server, "POST", "/api/command", '{"cmd":"start","profile":1}')[0] == 202
    server.clock.now += 3.5
    st = call(server, "GET", "/api/status")[1]
    assert st["ui_state"] == "ready" and st["busy"] is False and st["grind_phase"] == "idle"
    assert st["weight_g"] == 0.0
    # nothing is left in flight: a tare goes through at once
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[0] == 200


@pytest.mark.parametrize("change,ui_after", [({"ui_state": "edit"}, "edit"),
                                             ({"update_held": True}, "ready"),
                                             ({"start_block": "load_cell_no_data"}, "ready")])
def test_lead_in_is_dropped_when_something_took_over(server, change, ui_after):
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    assert call(server, "POST", "/dev/state", json.dumps(change))[0] == 200
    server.clock.now += 3.5
    st = call(server, "GET", "/api/status")[1]
    assert st["busy"] is False and st["ui_state"] == ui_after and st["grind_phase"] == "idle"


def test_lead_in_is_dropped_at_once_when_the_screen_changes(server):
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "command_in_flight"
    call(server, "POST", "/dev/state", '{"ui_state":"edit"}')
    # no time has passed: the lead-in is gone and nothing is in flight any more
    st = call(server, "GET", "/api/status")[1]
    assert st["ui_state"] == "edit" and st["busy"] is False
    server.sim.ui_state = "ready"
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[0] == 200
    server.clock.now += 3.5
    assert call(server, "GET", "/api/status")[1]["busy"] is False


def test_drop_start_switch_waits_for_the_end_of_the_lead_in(server):
    server.sim.drop_start = True
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    server.clock.now += 1.0
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "command_in_flight"
    server.clock.now += 2.5
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[0] == 200


def test_in_flight_switch_refuses_commands_and_writes(server):
    server.sim.in_flight = True
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[1]["error"] == "command_in_flight"
    assert call(server, "PATCH", "/api/settings", '{"logging":true}')[1]["error"] == "command_in_flight"
    assert call(server, "POST", "/api/command", '{"cmd":"stop"}')[0] == 200


def test_ui_busy_switch_refuses_settings_reads_and_writes(server):
    server.sim.ui_busy = True
    assert call(server, "GET", "/api/settings")[1]["error"] == "ui_busy"
    s, d, _ = call(server, "PATCH", "/api/settings", '{"logging":true}')
    assert (s, d["error"]) == (503, "ui_busy")
    assert call(server, "GET", "/api/status")[0] == 200
    server.sim.ui_busy = False
    assert call(server, "GET", "/api/settings")[0] == 200


def test_store_failed_switch_fails_a_changing_write_and_stores_nothing(server):
    server.sim.store_failed = True
    s, d, _ = call(server, "PATCH", "/api/settings", '{"logging":true,"swipe":true}')
    assert (s, d["error"]) == (500, "store_failed") and "values" not in d
    values = call(server, "GET", "/api/settings")[1]["values"]
    assert values["logging"] is False and values["swipe"] is False
    # a write that changes nothing is not written, so it still succeeds
    assert call(server, "PATCH", "/api/settings", '{"logging":false}')[0] == 200


def test_latency_switch_delays_replies_but_not_dev_state(server):
    server.sim.latency_ms = 800
    t0 = time.monotonic()
    assert call(server, "GET", "/api/status")[0] == 200
    assert time.monotonic() - t0 >= 0.75
    t0 = time.monotonic()
    assert call(server, "GET", "/dev/state")[0] == 200
    assert time.monotonic() - t0 < 0.6
    assert call(server, "POST", "/dev/state", '{"latency_ms":0}')[0] == 200
    assert server.sim.latency_ms == 0


def test_weight_hold_shows_a_fixed_reading_until_released(server):
    d = call(server, "POST", "/dev/state", '{"weight":18.04}')[1]
    assert d["weight"] == 18.04
    assert call(server, "GET", "/api/status")[1]["weight_g"] == 18.0
    assert call(server, "POST", "/api/command", '{"cmd":"tare"}')[0] == 200
    assert call(server, "GET", "/api/status")[1]["weight_g"] == 0.0
    assert call(server, "POST", "/dev/state", '{"weight":null}')[1]["weight"] is None


def test_dev_state_lists_every_key(server):
    doc = call(server, "GET", "/dev/state")[1]
    assert set(doc) | set(dev.DEV_ACTION_KEYS) == set(dev.DEV_STATE_KEYS)
    assert doc["latency_ms"] == 0 and doc["weight"] is None and doc["drop_start"] is False


@pytest.mark.parametrize("body", ['{"latency_ms":-1}', '{"latency_ms":30001}', '{"latency_ms":1.5}',
                                  '{"latency_ms":true}', '{"weight":"12"}', '{"weight":1001}', '{"weight":true}',
                                  '{"drop_start":"yes"}', '{"ui_busy":1}', '{"store_failed":null}'])
def test_dev_state_rejects_wrong_values(server, body):
    s, d, _ = call(server, "POST", "/dev/state", body)
    assert (s, d["error"]) == (400, "dev_state")


def test_flags_build_the_simulator():
    args = dev.build_parser().parse_args(["--update-held", "--drop-start", "--web-off", "--ui", "edit",
                                          "--start-block", "load_cell_no_data", "--motor-test-block", "motor_fault"])
    sim = dev.make_simulator(args)
    assert sim.update_held and sim.drop_start and not sim.web_interface and sim.ui_state == "edit"
    assert sim.start_block == "load_cell_no_data" and sim.motor_test_block == "motor_fault"
    assert dev.make_simulator(dev.build_parser().parse_args([])).drop_start is False


def test_legacy_status_flag_leaves_out_the_warnings_member():
    sim = dev.make_simulator(dev.build_parser().parse_args(["--legacy-status"]))
    assert sim.legacy_status is True
    assert dev.make_simulator(dev.build_parser().parse_args([])).legacy_status is False
    sim.set_dev_state({"warnings": ["motor_fault"]})
    doc = sim.status_doc()
    assert "warnings" not in doc
    assert list(doc) == ["ui_state", "grind_phase", "busy", "weight_g", "motor_available", "start_block",
                         "motor_test_block", "uptime_s", "build", "web_interface"]
    assert sim.dev_state()["warnings"] == ["motor_fault"]  # the dev key keeps its value


def _readme_text():
    return (PORTAL_TOOLS / "README.md").read_text(encoding="utf-8")


def test_readme_documents_every_flag():
    readme = _readme_text()
    flags = [o for action in dev.build_parser()._actions for o in action.option_strings
             if o.startswith("--") and o != "--help"]
    assert flags
    for flag in flags:
        # A row of the Flags table, not a mention elsewhere in the README
        assert re.search(rf"^\| `{re.escape(flag)}[ `]", readme, re.M), (
            f"README.md has no Flags table row for {flag}")


def test_readme_documents_every_dev_state_key():
    readme = _readme_text()
    keys = set(dev.SimGrinder().dev_state())  # the keys the server really reports, not the constant
    assert keys | set(dev.DEV_ACTION_KEYS) == set(dev.DEV_STATE_KEYS)
    for key in set(dev.DEV_STATE_KEYS):
        assert f"| `{key}` |" in readme, f"README.md has no table row for the /dev/state key {key}"


def test_readme_has_a_row_for_every_page_state():
    readme = _readme_text().lower()
    for state in ("offline", "web interface off", "busy", "not ready", "update running", "motor fault",
                  "ui_busy", "command_in_flight", "store_failed", "invalid_device_name", "out_of_range",
                  "empty data"):
        assert state in readme, f"README.md lacks a reproduction for {state}"


def test_strip_is_marked_stale_with_the_readout():
    src = (ROOT / "web" / "portal" / "page.src.html").read_text(encoding="utf-8")
    app = (ROOT / "web" / "portal" / "portal-app.js").read_text(encoding="utf-8")
    css = (ROOT / "web" / "portal" / "portal.css").read_text(encoding="utf-8")
    assert 'id="strip"' in src
    assert "setClass($('strip'), 'is-stale', readout.stale);" in app
    assert ".strip.is-stale .weight" in css


# ------------------------------------------------------------------------------------------ lifetime statistics
def test_stats_vectors():
    assert len(VECTORS["stats_cases"]) >= 10
    for c in VECTORS["stats_cases"]:
        assert model.encode_json(model.stats_document(c["input"])) == c["json"], c["name"]


def test_stats_document_orders_members_and_bounds_fractions():
    doc = model.stats_document({"powered_minutes": 5, "total_grinds": 2.0, "avg_pulses": float("nan"),
                                "total_weight_kg": "inf", "avg_abs_error_g": -3, "unknown_member": 9})
    assert list(doc) == list(model.STATS_KEYS) and "unknown_member" not in doc
    assert doc["total_grinds"] == 2 and isinstance(doc["total_grinds"], int)
    assert doc["avg_pulses"] == 0.0 and doc["total_weight_kg"] == 0.0 and doc["avg_abs_error_g"] == 0.0
    assert doc["single"] == 0 and doc["powered_minutes"] == 5
    assert model.stats_document({"total_weight_kg": 5e12})["total_weight_kg"] == 1e9


def test_stats_route_shape_and_headers(server):
    status, doc, res = call(server, "GET", "/api/stats")
    assert status == 200 and list(doc) == list(model.STATS_KEYS)
    assert doc["total_grinds"] == 412 and doc["total_weight_kg"] == 8.43
    assert res.getheader("Cache-Control") == "no-store"
    assert call(server, "GET", "/dev/state")[1]["stats"] == doc


def test_stats_route_is_gated_like_the_other_reads(server):
    assert call(server, "GET", "/api/stats", host="evil.com")[1]["error"] == "bad_host"
    assert call(server, "GET", "/api/stats", headers={"Origin": "http://evil.com"})[1]["error"] == "bad_origin"
    assert call(server, "POST", "/api/stats", "{}")[0] == 405
    assert call(server, "PATCH", "/api/stats", "{}")[0] == 405
    server.sim.ui_busy = True
    status, doc, _ = call(server, "GET", "/api/stats")
    assert status == 503 and doc["error"] == "ui_busy"
    server.sim.ui_busy = False
    server.sim.web_interface = False
    status, doc, _ = call(server, "GET", "/api/stats")
    assert status == 403 and doc["error"] == "web_interface_off"


def test_no_insights_answers_404_like_a_firmware_without_the_route(server):
    server.sim.insights = False
    server.sim.web_interface = False  # the HTTP server answers 404 before any gate of ours
    for method in ("GET", "POST", "PATCH"):
        status, doc, _ = call(server, method, "/api/stats", None if method == "GET" else "{}")
        assert status == 404 and doc is None, method
    assert call(server, "GET", "/dev/state")[1]["insights"] is False
    assert call(server, "POST", "/dev/state", '{"insights":true}')[1]["insights"] is True


# ------------------------------------------------------------------------------------------ diagnostics (chg021 G3a)
def _vector_input(case):
    """The vector input as Python values (the JSON strings "nan"/"inf"/"-inf" stand for the non-finite floats)."""
    return {k: float(v) if isinstance(v, str) and v in ("nan", "inf", "-inf") else v for k, v in case["input"].items()}


def test_diagnostics_vectors():
    assert len(VECTORS["diagnostics_cases"]) >= 20
    for c in VECTORS["diagnostics_cases"]:
        doc = model.diagnostics_document(_vector_input(c))
        assert list(doc) == list(model.DIAGNOSTICS_KEYS), c["name"]
        assert model.encode_json(doc) == c["json"], c["name"]
        assert len(c["json"]) < 320, c["name"]


def test_diagnostics_document_replaces_words_outside_the_vocabulary_and_defaults_missing_members():
    doc = model.diagnostics_document({"ble": "wifi", "board": "v2", "board_source": None})
    assert (doc["ble"], doc["board"], doc["board_source"]) == ("off", "UNKNOWN", "none")
    assert doc["wifi_rssi_dbm"] is None and doc["calibrated"] is False and doc["noise_ok"] is True
    assert doc["motor_latency_ms"] == 0 and doc["cal_factor"] == 0.0


def test_diagnostics_route_shape_headers_and_gates(server):
    status, doc, res = call(server, "GET", "/api/diagnostics")
    assert status == 200 and list(doc) == list(model.DIAGNOSTICS_KEYS)
    assert doc == model.diagnostics_document(dev.START_DIAGNOSTICS)
    assert res.getheader("Cache-Control") == "no-store"
    assert call(server, "GET", "/dev/state")[1]["diagnostics"] == doc
    assert call(server, "GET", "/api/diagnostics", host="evil.com")[1]["error"] == "bad_host"
    assert call(server, "GET", "/api/diagnostics", headers={"Origin": "http://evil.com"})[1]["error"] == "bad_origin"
    assert call(server, "POST", "/api/diagnostics", "{}")[0] == 405
    assert call(server, "PATCH", "/api/diagnostics", "{}")[0] == 405
    server.sim.ui_busy = True
    status, doc, _ = call(server, "GET", "/api/diagnostics")
    assert status == 503 and doc["error"] == "ui_busy"
    server.sim.ui_busy = False
    server.sim.web_interface = False
    status, doc, _ = call(server, "GET", "/api/diagnostics")
    assert status == 403 and doc["error"] == "web_interface_off"


def test_no_diagnostics_answers_404_like_a_firmware_without_the_route(server):
    server.sim.diagnostics_route = False
    for method in ("GET", "POST", "PATCH"):
        status, doc, _ = call(server, method, "/api/diagnostics", None if method == "GET" else "{}")
        assert status == 404 and doc is None, method
    assert call(server, "GET", "/api/stats")[0] == 200 and call(server, "GET", "/api/history")[0] == 200
    assert call(server, "GET", "/dev/state")[1]["diagnostics_route"] is False
    assert call(server, "POST", "/dev/state", '{"diagnostics_route":true}')[1]["diagnostics_route"] is True
    args = dev.build_parser().parse_args(["--no-diagnostics"])
    assert dev.make_simulator(args).diagnostics_route is False
    assert dev.make_simulator(dev.build_parser().parse_args([])).diagnostics_route is True


def test_dev_state_overrides_diagnostics_members_and_bounds_the_reply(server):
    body = ('{"diagnostics":{"cal_factor":NaN,"std_dev_g":Infinity,"sample_rate_sps":-5,"motor_latency_ms":1e30,'
            '"wifi_rssi_dbm":null,"ble":"link_linked","board":"V1","board_source":"user","calibrated":false,'
            '"noise_ok":false,"push_armed":true}}')
    status, state, _ = call(server, "POST", "/dev/state", body)
    assert status == 200
    doc = call(server, "GET", "/api/diagnostics")[1]
    assert doc == {"calibrated": False, "cal_factor": 0.0, "std_dev_g": 0.0, "noise_ok": False, "sample_rate_sps": 0.0,
                   "motor_latency_ms": 1000000, "wifi_rssi_dbm": None, "ble": "link_linked", "board": "V1",
                   "board_source": "user", "push_armed": True}
    assert state["diagnostics"] == doc
    for bad in ('{"diagnostics":{"ble":"wifi"}}', '{"diagnostics":{"board":"V3"}}', '{"diagnostics":{"nope":1}}',
                '{"diagnostics":{"calibrated":1}}', '{"diagnostics":{"cal_factor":"1"}}',
                '{"diagnostics":{"wifi_rssi_dbm":-50.5}}', '{"diagnostics":{"cal_factor":1' + "0" * 400 + '}}',
                '{"diagnostics":[]}'):
        status, err, _ = call(server, "POST", "/dev/state", bad)
        assert status == 400 and err["error"] == "dev_state", bad
    assert call(server, "GET", "/api/diagnostics")[1] == doc  # a refused body changes nothing


def test_a_finished_grind_joins_the_statistics_and_a_stopped_one_does_not(server):
    before = call(server, "GET", "/api/stats")[1]
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')  # 9 g in weight mode
    server.clock.now += 3.5
    call(server, "POST", "/api/command", '{"cmd":"stop"}')
    assert call(server, "GET", "/api/stats")[1] == before
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    server.clock.now += 3.5 + 20
    after = call(server, "GET", "/api/stats")[1]
    assert after["total_grinds"] == before["total_grinds"] + 1
    assert after["single"] == before["single"] + 1 and after["double"] == before["double"]
    assert after["weight_mode"] == before["weight_mode"] + 1 and after["time_mode"] == before["time_mode"]
    assert after["total_weight_kg"] == pytest.approx(before["total_weight_kg"] + 0.009, abs=0.001)
    assert after["total_pulses"] == before["total_pulses"] + dev.SIM_GRIND_PULSES
    assert after["motor_runtime_s"] > before["motor_runtime_s"]


def test_the_double_bucket_and_time_mode_are_counted(server):
    call(server, "PATCH", "/api/settings", '{"grind_mode":"time"}')
    before = call(server, "GET", "/api/stats")[1]
    call(server, "POST", "/api/command", '{"cmd":"start","profile":1}')
    server.clock.now += 3.5 + 60
    after = call(server, "GET", "/api/stats")[1]
    assert after["time_mode"] == before["time_mode"] + 1 and after["weight_mode"] == before["weight_mode"]
    assert after["avg_abs_error_g"] == before["avg_abs_error_g"]  # time mode has no error sample
    assert after["single"] + after["double"] + after["custom"] == before["single"] + before["double"] + before["custom"] + 1


def test_a_motor_test_adds_only_run_time(server):
    before = call(server, "GET", "/api/stats")[1]
    call(server, "POST", "/api/command", '{"cmd":"motor_test"}')
    server.clock.now += 2
    after = call(server, "GET", "/api/stats")[1]
    assert after["total_grinds"] == before["total_grinds"]
    assert after["motor_runtime_s"] == before["motor_runtime_s"] + 1  # 1.5 s on top of a whole second, rounded down


def test_powered_minutes_follow_the_clock(server):
    before = call(server, "GET", "/api/stats")[1]["powered_minutes"]
    server.clock.now += 125
    assert call(server, "GET", "/api/stats")[1]["powered_minutes"] == before + 2


def test_stats_reset_makes_a_new_grinder_and_the_next_grind_is_the_first(server):
    s, d, _ = call(server, "POST", "/dev/state", '{"stats_reset":true}')
    assert s == 200 and all(v == 0 for v in d["stats"].values())
    assert call(server, "GET", "/api/stats")[1] == d["stats"]
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    server.clock.now += 3.5 + 20
    doc = call(server, "GET", "/api/stats")[1]
    assert doc["total_grinds"] == 1 and doc["single"] == 1 and doc["weight_mode"] == 1
    assert doc["avg_abs_error_g"] == dev.SIM_GRIND_ERROR_G and doc["avg_pulses"] == dev.SIM_GRIND_PULSES


def test_stats_override_sets_members_and_rejects_bad_ones(server):
    s, d, _ = call(server, "POST", "/dev/state", '{"stats":{"total_grinds":4294967295,"total_weight_kg":1234.5,"powered_minutes":900}}')
    assert s == 200 and d["stats"]["total_grinds"] == 4294967295 and d["stats"]["total_weight_kg"] == 1234.5
    assert d["stats"]["powered_minutes"] == 900 and d["stats"]["single"] == 180  # the rest is kept
    for bad in ('{"stats":5}', '{"stats":{"nope":1}}', '{"stats":{"total_grinds":-1}}', '{"stats":{"total_grinds":"x"}}',
                '{"stats":{"total_grinds":true}}', '{"stats_reset":false}', '{"insights":"yes"}'):
        assert call(server, "POST", "/dev/state", bad)[0] == 400, bad


# ------------------------------------------------------------------------------------------ recent grinds (history)
def _vector_entries(case):
    entries = []
    for e in case["input"]["entries"]:
        e = dict(e)
        for key in ("target", "final_g"):
            if isinstance(e[key], str):
                e[key] = float(e[key])  # "nan", "inf", "-inf"
        entries.append(e)
    return entries


def test_history_vectors():
    assert len(VECTORS["history_cases"]) >= 10
    for c in VECTORS["history_cases"]:
        doc = model.history_document(_vector_entries(c), c["input"]["persistent"], c["input"]["now_s"])
        assert model.encode_json(doc) == c["json"], c["name"]


def test_history_document_uses_at_most_ten_entries_and_never_a_negative_age():
    entry = {"id": 1, "ended_s": 50, "earlier_boot": False, "profile": 1, "mode": 0, "target": 18.0, "final_g": 18.0,
             "time_ms": 1, "motor_ms": 1, "pulses": 1, "result": 0}
    doc = model.history_document([entry] * 14, True, 10)
    assert doc["count"] == 10 == len(doc["grinds"]) and doc["grinds"][0]["age_s"] == 0
    assert list(doc["grinds"][0]) == list(model.HISTORY_KEYS)


def _finish_grinds(server, count, profile=0):
    for _ in range(count):
        call(server, "POST", "/api/command", '{"cmd":"start","profile":%d}' % profile)
        server.clock.now += 3.5 + 20
        call(server, "GET", "/api/status")  # the grind finishes and the complete screen shows
        server.clock.now += 3.5  # the complete screen ends, the next start is accepted


def test_history_route_shape_and_headers(server):
    status, doc, res = call(server, "GET", "/api/history")
    assert status == 200 and doc == {"count": 0, "persistent": True, "grinds": []}
    assert res.getheader("Cache-Control") == "no-store"
    assert call(server, "POST", "/api/history", "{}")[0] == 405
    assert call(server, "PATCH", "/api/history", "{}")[0] == 405
    assert call(server, "GET", "/api/history", host="evil.com")[1]["error"] == "bad_host"
    assert call(server, "GET", "/api/history", headers={"Origin": "http://evil.com"})[1]["error"] == "bad_origin"


def test_history_route_is_gated_and_follows_ui_busy(server):
    server.sim.ui_busy = True
    status, doc, _ = call(server, "GET", "/api/history")
    assert status == 503 and doc["error"] == "ui_busy"
    server.sim.ui_busy = False
    server.sim.web_interface = False
    status, doc, _ = call(server, "GET", "/api/history")
    assert status == 403 and doc["error"] == "web_interface_off"


def test_finished_grinds_are_listed_newest_first_and_a_stopped_one_is_not(server):
    call(server, "POST", "/api/command", '{"cmd":"start","profile":0}')
    server.clock.now += 3.5
    call(server, "POST", "/api/command", '{"cmd":"stop"}')
    assert call(server, "GET", "/api/history")[1]["count"] == 0
    _finish_grinds(server, 1, profile=0)
    _finish_grinds(server, 1, profile=1)
    _finish_grinds(server, 1, profile=2)
    doc = call(server, "GET", "/api/history")[1]
    assert doc["count"] == 3 and [g["profile"] for g in doc["grinds"]] == [2, 1, 0]
    assert [g["id"] for g in doc["grinds"]] == [3, 2, 1]
    first = doc["grinds"][2]
    assert first["mode"] == "weight" and first["target_g"] == 9.0 and first["target_s"] is None
    assert first["final_g"] == pytest.approx(9.1) and first["pulses"] == dev.SIM_GRIND_PULSES
    assert first["result"] == "complete"
    ages = [g["age_s"] for g in doc["grinds"]]
    assert ages == sorted(ages) and all(isinstance(a, int) for a in ages)


def test_age_grows_between_two_requests_without_a_new_grind(server):
    _finish_grinds(server, 1)
    before = call(server, "GET", "/api/history")[1]["grinds"][0]["age_s"]
    server.clock.now += 90
    assert call(server, "GET", "/api/history")[1]["grinds"][0]["age_s"] == before + 90


def test_a_time_mode_grind_has_target_s_and_no_target_g(server):
    call(server, "PATCH", "/api/settings", '{"grind_mode":"time"}')
    _finish_grinds(server, 1, profile=1)
    g = call(server, "GET", "/api/history")[1]["grinds"][0]
    assert g["mode"] == "time" and g["target_g"] is None and g["target_s"] == 10.0 and g["pulses"] == 0


def test_history_keeps_the_newest_ten(server):
    _finish_grinds(server, 12)
    doc = call(server, "GET", "/api/history")[1]
    assert doc["count"] == 10 and doc["grinds"][0]["id"] == 12 and doc["grinds"][-1]["id"] == 3


def test_history_dev_switches(server):
    s, d, _ = call(server, "POST", "/dev/state", '{"history_fill":3}')
    assert s == 200 and call(server, "GET", "/api/history")[1]["count"] == 3
    grinds = call(server, "GET", "/api/history")[1]["grinds"]
    assert [g["age_s"] for g in grinds] == [120, 1260, 2400]  # the simulated grinder is made old enough for them
    assert call(server, "POST", "/dev/state", '{"history_fill":10}')[0] == 200
    full = call(server, "GET", "/api/history")[1]
    assert full["count"] == 10 and {g["result"] for g in full["grinds"]} == {"complete", "max_pulses", "overshoot", "timeout"}
    assert any(g["mode"] == "time" for g in full["grinds"])
    call(server, "POST", "/dev/state", '{"history_restart":true}')
    restarted = call(server, "GET", "/api/history")[1]["grinds"]
    assert len(restarted) == 10 and all(g["age_s"] is None for g in restarted)
    call(server, "POST", "/dev/state", '{"history_persistent":false}')
    assert call(server, "GET", "/api/history")[1]["persistent"] is False
    assert call(server, "GET", "/dev/state")[1]["history_persistent"] is False
    call(server, "POST", "/dev/state", '{"history_clear":true}')
    doc = call(server, "GET", "/api/history")[1]
    assert doc == {"count": 0, "persistent": False, "grinds": []}


@pytest.mark.parametrize("body", ['{"history_fill":11}', '{"history_fill":-1}', '{"history_fill":true}',
                                  '{"history_fill":"3"}', '{"history_clear":false}', '{"history_restart":1}',
                                  '{"history_persistent":"no"}', '{"history_route":null}'])
def test_history_dev_switches_reject_wrong_values(server, body):
    s, d, _ = call(server, "POST", "/dev/state", body)
    assert (s, d["error"]) == (400, "dev_state")


def test_no_history_answers_404_but_keeps_the_statistics(server):
    server.sim.history_route = False
    for method in ("GET", "POST", "PATCH"):
        status, doc, _ = call(server, method, "/api/history", None if method == "GET" else "{}")
        assert status == 404 and doc is None, method
    assert call(server, "GET", "/api/stats")[0] == 200
    assert call(server, "GET", "/dev/state")[1]["history_route"] is False
    server.sim.history_route = True
    server.sim.insights = False
    assert call(server, "GET", "/api/history")[0] == 404  # --no-insights takes the older firmware's routes away
    args = dev.build_parser().parse_args(["--no-history"])
    assert dev.make_simulator(args).history_route is False
    assert dev.make_simulator(dev.build_parser().parse_args([])).history_route is True


# ------------------------------------------------------------------------------------ diagnostics edge cases (G3a tester)
def test_diagnostics_g3a_model_edges_are_strict_json_in_contract_order():
    import json as _json
    inf, nan = float("inf"), float("nan")
    for rssi in (None, 0, 99, -127, -128, 5, -(2 ** 31), 2 ** 31 - 1):
        for ble in model.BLE_STATES:
            doc = model.diagnostics_document({"wifi_rssi_dbm": rssi, "ble": ble})
            text = model.encode_json(doc)
            assert len(text) < 320
            assert list(_json.loads(text, parse_constant=lambda c: pytest.fail(c))) == list(model.DIAGNOSTICS_KEYS)
            assert doc["wifi_rssi_dbm"] is None if rssi is None else -127 <= doc["wifi_rssi_dbm"] <= 0
    for v, want in ((nan, 0), (inf, 0), (-inf, 0), (-0.0, 0), (1e9, 1e9), (-1e9, -1e9), (1e10, 1e9), (-1e10, -1e9), (1e-9, 0)):
        d = model.diagnostics_document({"cal_factor": v})
        assert d["cal_factor"] == want and "-0.0" not in model.encode_json(d)
    for v, want in ((-5, 0), (0, 0), (0.4999, 0), (0.5, 1), (1e6, 1000000), (1e7, 1000000), (nan, 0), (inf, 0)):
        assert model.diagnostics_document({"motor_latency_ms": v})["motor_latency_ms"] == want
    d = model.diagnostics_document({"std_dev_g": -1, "sample_rate_sps": 1e30})
    assert d["std_dev_g"] == 0.0 and d["sample_rate_sps"] == 1e9
    for bad in ("", '"', "\\", 'a"b' + chr(92) + 'c', None, "v2"):
        d = model.diagnostics_document({"board": bad, "board_source": bad, "ble": bad})
        assert (d["board"], d["board_source"], d["ble"]) == ("UNKNOWN", "none", "off")


def test_diagnostics_g3a_dev_state_extremes_stay_valid_json_under_320_bytes(server):
    import json as _json
    for body in ('{"diagnostics":{"cal_factor":-Infinity,"std_dev_g":NaN,"sample_rate_sps":1e308,"motor_latency_ms":-1e308,'
                 '"wifi_rssi_dbm":-2147483648,"ble":"link_connected","board_source":"default","board":"UNKNOWN"}}',
                 '{"diagnostics":{"wifi_rssi_dbm":99,"cal_factor":-0.0,"motor_latency_ms":0.4999}}'):
        assert call(server, "POST", "/dev/state", body)[0] == 200
        status, doc, res = call(server, "GET", "/api/diagnostics")
        assert status == 200 and list(doc) == list(model.DIAGNOSTICS_KEYS)
        raw = model.encode_json(doc)
        _json.loads(raw, parse_constant=lambda c: pytest.fail(c))
        assert len(raw) < 320

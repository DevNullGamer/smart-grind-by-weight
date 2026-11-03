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
    # hard budget is 64 KB (embed_portal.py); chg016 aims for half of it
    assert len(gzip.compress((ROOT / "web" / "portal" / "index.html").read_bytes(), 9)) < 32 * 1024


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


@pytest.mark.parametrize("path", ["/", "/api/status", "/api/settings", "/api/schema"])
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
                        "motor_test_block", "uptime_s", "build", "web_interface"}
    assert doc["web_interface"] is True and doc["start_block"] is None


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
    ids |= {f"{prefix}{tab}" for prefix in ("panel-", "tab-") for tab in ("grind", "settings", "insights")}
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
                        "motor_test_block", "uptime_s", "build", "web_interface"}
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
    assert set(doc) == set(dev.DEV_STATE_KEYS)
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
    assert keys == set(dev.DEV_STATE_KEYS)
    for key in keys:
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

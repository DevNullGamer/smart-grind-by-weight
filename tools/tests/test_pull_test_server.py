"""Tests for tools/wifi/pull_test_server.py (documents, fault injection, the real HTTP server on loopback)."""

import hashlib
import http.client
import importlib.util
import json
import struct
import sys
import threading
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import update_index as ui  # noqa: E402


def _load_server():
    path = TOOLS_DIR / "wifi" / "pull_test_server.py"
    spec = importlib.util.spec_from_file_location("pull_test_server_under_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


server = _load_server()
BASE = "http://192.168.1.20:8080/"
SIZE = 200 * 1024


def make_image(version="1.4.1-rc.3", size=SIZE):
    """Fake ESP32 app image: right magic and version where esp_app_desc_t keeps them, deterministic body."""
    body = bytearray((i * 7 + 3) & 0xFF for i in range(size))
    body[0] = 0xE9
    struct.pack_into("<I", body, 32, server.APP_DESC_MAGIC)
    body[48:80] = version.encode("ascii").ljust(32, b"\0")
    return bytes(body)


IMAGE = make_image()


# --- image version -----------------------------------------------------------------------------------------------

def test_read_image_version():
    assert server.read_image_version(make_image("1.5.0")) == "1.5.0"
    assert server.read_image_version(make_image("1.4.1-rc.3")) == "1.4.1-rc.3"
    assert server.read_image_version(make_image("A" * 31)) == "A" * 31


def test_read_image_version_rejects_non_images():
    assert server.read_image_version(b"") is None
    assert server.read_image_version(b"\xe9" * 40) is None  # too short
    assert server.read_image_version(bytes(1000)) is None  # no magic
    broken = bytearray(make_image())
    broken[50] = 0xFF  # not ASCII
    assert server.read_image_version(bytes(broken)) is None


# --- documents -----------------------------------------------------------------------------------------------------

def test_plan_documents_follow_the_generator_format():
    plan = server.build_plan(IMAGE, BASE, "1.4.1-rc.3")
    beta = plan.documents["beta"]
    assert beta == {
        "schema": 1,
        "channel": "beta",
        "version": "1.4.1-rc.3",
        "tag": "v1.4.1-rc.3",
        "prerelease": True,
        "published": "2026-01-01",
        "size": SIZE,
        "sha256": hashlib.sha256(IMAGE).hexdigest(),
        "url": BASE + "firmware/v1.4.1-rc.3/smart-grind-by-weight-v1.4.1-rc.3.bin",
    }
    assert plan.image_path == "/firmware/v1.4.1-rc.3/smart-grind-by-weight-v1.4.1-rc.3.bin"
    assert plan.image_path == "/" + beta["url"][len(BASE):]
    assert ui.limit_problem({k: v for k, v in beta.items() if k not in ("schema", "channel")}) is None


def test_prerelease_is_beta_only():
    plan = server.build_plan(IMAGE, BASE, "1.4.1-rc.3")
    assert plan.documents["stable"] == {"schema": 1, "channel": "stable", "none": True}
    assert plan.documents["beta"]["version"] == "1.4.1-rc.3"


def test_final_release_is_on_both_channels():
    plan = server.build_plan(IMAGE, BASE, "1.5.0")
    for channel in ("stable", "beta"):
        document = plan.documents[channel]
        assert document["channel"] == channel
        assert document["version"] == "1.5.0"
        assert document["prerelease"] is False


def test_index_body_matches_the_generator_output(tmp_path):
    plan = server.build_plan(IMAGE, BASE, "1.5.0", published="2026-02-03")
    entry = {"version": ui.semver_parse("1.5.0"), "tag": "v1.5.0", "document": dict(plan.documents["stable"])}
    for key in ("schema", "channel"):
        del entry["document"][key]
    for path in ui.write_channel_files(str(tmp_path), [entry]):
        channel = "stable" if path.endswith("stable.json") else "beta"
        assert Path(path).read_bytes() == plan.index_body(channel)


def test_base_url_gets_a_trailing_slash():
    plan = server.build_plan(IMAGE, "http://10.0.0.5:9000", "1.5.0")
    assert plan.documents["beta"]["url"].startswith("http://10.0.0.5:9000/firmware/v1.5.0/")


def test_unusable_input_is_refused():
    with pytest.raises(ValueError):
        server.build_plan(IMAGE, BASE, "v1.5.0")  # leading v
    with pytest.raises(ValueError):
        server.build_plan(IMAGE, BASE, "1.5")
    with pytest.raises(ValueError):
        server.build_plan(b"x" * 1000, BASE, "1.5.0")  # below the device minimum
    with pytest.raises(ValueError):
        server.build_plan(IMAGE, BASE, "1.5.0", fault="explode")
    with pytest.raises(ValueError):
        server.build_plan(IMAGE, "http://example.org/" + "a" * 130 + "/", "1.5.0")  # url past the device parser limit


# --- faults --------------------------------------------------------------------------------------------------------

def test_fault_wrong_sha():
    good = server.build_plan(IMAGE, BASE, "1.5.0").documents["beta"]
    bad = server.build_plan(IMAGE, BASE, "1.5.0", fault="wrong-sha").documents["beta"]
    assert bad["sha256"] != good["sha256"] and len(bad["sha256"]) == 64
    assert bad["sha256"] != hashlib.sha256(IMAGE).hexdigest()
    assert {k: v for k, v in bad.items() if k != "sha256"} == {k: v for k, v in good.items() if k != "sha256"}


def test_fault_wrong_sha_when_digest_starts_with_zero():
    assert server.flip_first_hex_digit("0abc") == "1abc"
    assert server.flip_first_hex_digit("fabc") == "0abc"


def test_fault_older_offers_a_lower_version():
    plan = server.build_plan(IMAGE, BASE, "1.5.0", fault="older")
    assert plan.documents["stable"]["version"] == server.OLDER_VERSION
    assert plan.documents["stable"]["prerelease"] is False
    assert "v0.0.1" in plan.image_path


def test_fault_not_image_describes_the_blob_it_serves():
    plan = server.build_plan(IMAGE, BASE, "1.5.0", fault="not-image")
    assert len(plan.image) == server.NOT_IMAGE_SIZE
    assert plan.image[:1] != b"\xe9"
    document = plan.documents["beta"]
    assert document["size"] == len(plan.image)
    assert document["sha256"] == hashlib.sha256(plan.image).hexdigest()


def test_fault_oversize_declares_more_than_the_index():
    plan = server.build_plan(IMAGE, BASE, "1.5.0", fault="oversize")
    assert plan.declared_length == plan.documents["beta"]["size"] + server.OVERSIZE_EXTRA


def test_fault_no_release_empties_both_channels():
    plan = server.build_plan(IMAGE, BASE, "1.5.0", fault="no-release")
    assert plan.documents["stable"] == {"schema": 1, "channel": "stable", "none": True}
    assert plan.documents["beta"] == {"schema": 1, "channel": "beta", "none": True}


@pytest.mark.parametrize("fault,action", [("none", "normal"), ("wrong-sha", "normal"), ("404", "404"),
                                          ("truncate", "truncate"), ("kill", "kill"), ("stall", "stall")])
def test_image_actions(fault, action):
    assert server.build_plan(IMAGE, BASE, "1.5.0", fault=fault).action == action


# --- the real server on loopback -----------------------------------------------------------------------------------

class Running:
    def __init__(self, fault="none", version="1.5.0", stall_seconds=0.2):
        self.plan = server.build_plan(IMAGE, BASE, version, fault=fault)
        self.killed = threading.Event()
        handler = server.make_handler(self.plan, stall_seconds, self.killed.set)
        self.httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()

    def get(self, path):
        conn = http.client.HTTPConnection("127.0.0.1", self.httpd.server_address[1], timeout=5)
        conn.request("GET", path)
        return conn, conn.getresponse()


def test_server_serves_index_and_image():
    with Running() as srv:
        conn, response = srv.get("/update-stable.json")
        body = response.read()
        assert response.status == 200
        assert response.getheader("Content-Length") == str(len(body))
        assert json.loads(body) == srv.plan.documents["stable"]
        conn.close()

        conn, response = srv.get(srv.plan.image_path)
        data = response.read()
        assert response.status == 200
        assert int(response.getheader("Content-Length")) == SIZE
        assert data == IMAGE
        conn.close()


def test_server_unknown_path_is_404():
    with Running() as srv:
        conn, response = srv.get("/nothing")
        assert response.status == 404
        response.read()
        conn.close()


def test_server_404_fault_hides_only_the_image():
    with Running(fault="404") as srv:
        conn, response = srv.get(srv.plan.image_path)
        assert response.status == 404
        response.read()
        conn.close()
        conn, response = srv.get("/update-beta.json")
        assert response.status == 200
        response.read()
        conn.close()


def test_server_truncate_fault_closes_early():
    with Running(fault="truncate") as srv:
        conn, response = srv.get(srv.plan.image_path)
        assert int(response.getheader("Content-Length")) == SIZE
        with pytest.raises(http.client.IncompleteRead) as raised:
            response.read()
        assert len(raised.value.partial) == SIZE // 2
        conn.close()


def test_server_kill_fault_calls_the_kill_hook():
    with Running(fault="kill") as srv:
        conn, response = srv.get(srv.plan.image_path)
        with pytest.raises((http.client.IncompleteRead, ConnectionError)):
            response.read()
        conn.close()
        assert srv.killed.wait(5)


def test_server_stall_fault_pauses_then_closes():
    with Running(fault="stall", stall_seconds=0.3) as srv:
        conn, response = srv.get(srv.plan.image_path)
        with pytest.raises(http.client.IncompleteRead):
            response.read()
        conn.close()


def test_server_oversize_fault_declares_more_than_it_sends():
    with Running(fault="oversize") as srv:
        conn, response = srv.get(srv.plan.image_path)
        assert int(response.getheader("Content-Length")) == SIZE + server.OVERSIZE_EXTRA
        with pytest.raises(http.client.IncompleteRead):
            response.read()
        conn.close()


def test_listen_host_defaults_to_the_lan_address(monkeypatch):
    monkeypatch.setattr(server, "guess_lan_ip", lambda: "192.168.1.20")
    assert server.resolve_listen_host(None) == "192.168.1.20"
    assert server.resolve_listen_host("") == "192.168.1.20"
    assert server.resolve_listen_host("127.0.0.1") == "127.0.0.1"
    assert server.resolve_listen_host("0.0.0.0") == "0.0.0.0"  # only when asked for explicitly
    assert server.parse_args(["--image", "x"]).host is None


def test_lan_ip_never_wildcard():
    assert server.guess_lan_ip() != "0.0.0.0"


# --- command line --------------------------------------------------------------------------------------------------

def test_print_index_cli(tmp_path, capsys):
    image = tmp_path / "firmware.bin"
    image.write_bytes(make_image("1.5.0"))
    code = server.main(["--image", str(image), "--base-url", BASE, "--print-index"])
    out = capsys.readouterr().out.strip().splitlines()
    assert code == 0
    assert [json.loads(line)["channel"] for line in out] == ["stable", "beta"]
    assert json.loads(out[0])["version"] == "1.5.0"  # taken from the image


def test_cli_needs_a_version_when_the_image_has_none(tmp_path, capsys):
    image = tmp_path / "blob.bin"
    image.write_bytes(bytes(SIZE))
    assert server.main(["--image", str(image), "--base-url", BASE, "--print-index"]) == 2
    assert "pass --version" in capsys.readouterr().err
    assert server.main(["--image", str(image), "--base-url", BASE, "--version", "1.2.3", "--print-index"]) == 0


def test_cli_missing_image(tmp_path, capsys):
    assert server.main(["--image", str(tmp_path / "nope.bin"), "--print-index"]) == 2

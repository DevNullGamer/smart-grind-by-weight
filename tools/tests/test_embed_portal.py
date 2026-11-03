"""Build script that embeds the web panel page (tools/build-scripts/embed_portal.py, chg014 step 3).

Imported as a plain module: outside PlatformIO the Import("env") call fails and the script does nothing on import.
Checks the budget and failure behaviour on throw-away project trees plus the real source page and config.
"""

import gzip
import importlib.util
import os
import re
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / "tools" / "build-scripts" / "embed_portal.py"


def load_script():
    spec = importlib.util.spec_from_file_location("embed_portal_under_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


embed = load_script()


def make_project(tmp_path, budget=65536, page=b"<html>hello</html>", source="web/portal/index.html"):
    """Minimal project tree: src/config/portal.h with the defines the script reads, plus the page."""
    config = tmp_path / "src" / "config"
    config.mkdir(parents=True)
    (config / "portal.h").write_text(
        f'#define PORTAL_PAGE_SOURCE "{source}"\n#define PORTAL_PAGE_BUDGET_BYTES {budget}\n', encoding="utf-8"
    )
    if page is not None:
        page_path = tmp_path / Path(*source.split("/"))
        page_path.parent.mkdir(parents=True, exist_ok=True)
        page_path.write_bytes(page)
    return tmp_path


def test_script_is_inert_outside_platformio():
    assert embed.platformio_mode is False


def test_real_config_matches_the_documented_budget_and_source():
    assert embed.read_define(str(ROOT), "PORTAL_PAGE_BUDGET_BYTES", numeric=True) == 64 * 1024
    assert embed.read_define(str(ROOT), "PORTAL_PAGE_SOURCE") == "web/portal/index.html"


def test_real_page_embeds_within_budget_and_is_labelled(tmp_path):
    header, gz_size, raw_size = embed.embed_page(str(ROOT), str(tmp_path))
    assert 0 < gz_size <= 64 * 1024
    assert raw_size > gz_size or raw_size < 1024
    page = (ROOT / "web" / "portal" / "index.html").read_text(encoding="utf-8")
    assert "<title>Grinder panel</title>" in page and "REFERENCE PAGE" not in page
    assert "prefers-color-scheme" in page
    assert Path(header).name == "portal_page_data.h"


def test_header_has_size_crc_and_exact_bytes(tmp_path):
    project = make_project(tmp_path / "p")
    out = tmp_path / "out"
    header, gz_size, raw_size = embed.embed_page(str(project), str(out))
    text = Path(header).read_text(encoding="utf-8")
    gz = embed.gzip_page(b"<html>hello</html>")
    assert gz_size == len(gz) and raw_size == len(b"<html>hello</html>")
    assert f"#define PORTAL_PAGE_GZ_SIZE {len(gz)}u" in text
    assert f"#define PORTAL_PAGE_GZ_CRC32 0x{zlib.crc32(gz) & 0xFFFFFFFF:08x}u" in text
    values = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})(?=,)", text.split("= {", 1)[1]))
    assert values == gz
    assert gzip.decompress(values) == b"<html>hello</html>"
    assert "#pragma once" in text


def test_gzip_is_deterministic():
    data = b"<html>" + b"x" * 500 + b"</html>"
    assert embed.gzip_page(data) == embed.gzip_page(data)
    assert embed.gzip_page(data)[4:8] == bytes(4)  # gzip header mtime: dropping mtime=0 would break this


def test_unchanged_page_keeps_header_untouched(tmp_path):
    project = make_project(tmp_path / "p")
    out = tmp_path / "out"
    header, _, _ = embed.embed_page(str(project), str(out))
    os.utime(header, (1_000_000, 1_000_000))
    embed.embed_page(str(project), str(out))
    assert int(os.path.getmtime(header)) == 1_000_000


def test_changed_page_rewrites_header(tmp_path):
    project = make_project(tmp_path / "p")
    out = tmp_path / "out"
    header, size_a, _ = embed.embed_page(str(project), str(out))
    before = Path(header).read_text(encoding="utf-8")
    (project / "web" / "portal" / "index.html").write_bytes(b"<html>changed " + b"y" * 200 + b"</html>")
    _, size_b, _ = embed.embed_page(str(project), str(out))
    assert size_a != size_b
    assert Path(header).read_text(encoding="utf-8") != before


def test_missing_source_fails_clearly(tmp_path):
    project = make_project(tmp_path / "p", page=None)
    with pytest.raises(embed.PortalEmbedError) as error:
        embed.embed_page(str(project), str(tmp_path / "out"))
    assert "web/portal/index.html" in str(error.value)
    assert "missing" in str(error.value).lower()
    assert not (tmp_path / "out" / "portal_page_data.h").exists()


def test_empty_source_fails(tmp_path):
    project = make_project(tmp_path / "p", page=b"  \n")
    with pytest.raises(embed.PortalEmbedError):
        embed.embed_page(str(project), str(tmp_path / "out"))


def test_over_budget_fails_and_names_the_budget(tmp_path):
    incompressible = os.urandom(4096)  # random bytes do not compress below the 1 KB budget
    project = make_project(tmp_path / "p", budget=1024, page=incompressible)
    with pytest.raises(embed.PortalEmbedError) as error:
        embed.embed_page(str(project), str(tmp_path / "out"))
    message = str(error.value)
    assert "too large" in message and "1024" in message
    assert not (tmp_path / "out" / "portal_page_data.h").exists()


def test_exactly_at_budget_passes_and_one_over_fails(tmp_path):
    page = os.urandom(300)
    gz_size = len(embed.gzip_page(page))
    ok = make_project(tmp_path / "ok", budget=gz_size, page=page)
    embed.embed_page(str(ok), str(tmp_path / "out_ok"))
    over = make_project(tmp_path / "over", budget=gz_size - 1, page=page)
    with pytest.raises(embed.PortalEmbedError):
        embed.embed_page(str(over), str(tmp_path / "out_over"))


def test_missing_config_define_fails(tmp_path):
    config = tmp_path / "src" / "config"
    config.mkdir(parents=True)
    (config / "portal.h").write_text("// nothing here\n", encoding="utf-8")
    with pytest.raises(embed.PortalEmbedError):
        embed.embed_page(str(tmp_path), str(tmp_path / "out"))


def _first_statement(source, signature):
    """First non-blank line of the function body that starts at `signature`."""
    lines = source.splitlines()
    start = next(i for i, line in enumerate(lines) if signature in line)
    return next(line.strip() for line in lines[start + 1:] if line.strip() and not line.strip().startswith("//"))


@pytest.mark.parametrize("signature", ["int UpdateServer::handle_begin(", "int UpdateServer::handle_data("])
def test_update_routes_check_receive_first(signature):
    source = (ROOT / "src" / "network" / "update_server.cpp").read_text(encoding="utf-8")
    first = _first_statement(source, signature)
    assert first.startswith("if (!update_routes_open())"), (
        f"{signature} must start with `if (!update_routes_open())`: the HTTP server also runs for the web interface "
        "alone (chg014 step 3), so without this check the Receive switch no longer protects firmware push."
    )


def test_build_is_wired_into_platformio_ini():
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    assert "tools/build-scripts/embed_portal.py" in ini

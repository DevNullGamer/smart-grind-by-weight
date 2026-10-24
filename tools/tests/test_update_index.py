"""Tests for tools/update_index.py (release index generator + SemVer ordering)."""

import hashlib
import json
import re
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import update_index as ui  # noqa: E402

UNIFIED = "Smart Grind By Weight - Waveshare 1.64 (V1 + V2 auto-detect)"
SINGLE_BOARD = "Smart Grind By Weight - Waveshare 1.64"

# Shared SemVer vectors: test_native/test_release_index.cpp carries the identical lists, change both together.
ASCENDING = [
    "0.9.9",
    "1.0.0-alpha",
    "1.0.0-alpha.1",
    "1.0.0-alpha.beta",
    "1.0.0-beta",
    "1.0.0-beta.2",
    "1.0.0-beta.11",
    "1.0.0-rc.1",
    "1.0.0",
    "1.4.1-rc.3",
    "1.4.1-rc.10",
    "1.4.1",
    "1.5.0-beta",
    "1.5.0",
    "1.10.0",
    "2.0.0",
]
EQUAL = [
    ("1.5.0", "1.5.0+build.7"),
    ("1.5.0+a", "1.5.0+b"),
    ("1.5.0-rc.1+x", "1.5.0-rc.1"),
]
INVALID = [
    "",
    "v1.5.0",
    "1.5",
    "1.5.x",
    "1.5.0.1",
    "1.05.0",
    "1.5.0-",
    "1.5.0-01",
    "1.5.0-rc..1",
    "1.5.0-rc.",
    "1.5.0+",
    "1.5.0 ",
    " 1.5.0",
    "-1.5.0",
    "1.5.0-rc_1",
    "1.5.0\n",
    "1234567890.0.0",
    "1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
]


def parsed(text):
    value = ui.semver_parse(text)
    assert value is not None, text
    return value


@pytest.mark.parametrize("i", range(len(ASCENDING)))
def test_semver_ascending_order(i):
    for j, other in enumerate(ASCENDING):
        want = (i > j) - (i < j)
        assert ui.semver_compare(parsed(ASCENDING[i]), parsed(other)) == want, (ASCENDING[i], other)


@pytest.mark.parametrize("a,b", EQUAL)
def test_semver_equal_ignores_build(a, b):
    assert ui.semver_compare(parsed(a), parsed(b)) == 0
    assert ui.semver_compare(parsed(b), parsed(a)) == 0


@pytest.mark.parametrize("text", INVALID)
def test_semver_rejects_invalid(text):
    assert ui.semver_parse(text) is None


def test_semver_non_string_rejected():
    assert ui.semver_parse(None) is None
    assert ui.semver_parse(150) is None


def test_semver_fields():
    assert ui.semver_parse("12.3.456-rc.10+meta") == (12, 3, 456, ("rc", "10"))
    assert ui.semver_parse("1.0.0+meta") == (1, 0, 0, ())


def test_tag_version_needs_v_and_no_build():
    assert ui.tag_version("v1.5.0-rc.2") == (1, 5, 0, ("rc", "2"))
    for bad in ("1.5.0", "latest", "v1.5", "v1.5.0+7", "vv1.5.0", None):
        assert ui.tag_version(bad) is None


# --- generator -------------------------------------------------------------------------------------------------

def make_release(root, tag, name=UNIFIED, size=200000, manifest=True, image=True):
    tag_dir = root / tag
    tag_dir.mkdir(parents=True)
    if manifest:
        (tag_dir / f"smart-grind-by-weight-{tag}.manifest.json").write_text(json.dumps({"name": name}), encoding="utf-8")
    content = bytes((i * 7 + len(tag)) % 251 for i in range(size))
    if image:
        (tag_dir / f"smart-grind-by-weight-{tag}.bin").write_bytes(content)
    return content


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


@pytest.fixture
def tree(tmp_path):
    root = tmp_path / "firmware"
    root.mkdir()
    return root


def test_stable_and_beta_selection(tree, tmp_path):
    make_release(tree, "v1.3.0", name=SINGLE_BOARD)
    make_release(tree, "v1.4.0")
    make_release(tree, "v1.5.0-rc.2")
    last = make_release(tree, "v1.5.0-rc.10", size=220000)
    out = tmp_path / "out"
    assert ui.main(["--firmware-root", str(tree), "--output-dir", str(out)]) == 0

    stable = read(out / "update-stable.json")
    beta = read(out / "update-beta.json")
    assert stable["channel"] == "stable" and stable["version"] == "1.4.0" and stable["prerelease"] is False
    assert beta["channel"] == "beta" and beta["version"] == "1.5.0-rc.10" and beta["prerelease"] is True
    assert beta["tag"] == "v1.5.0-rc.10" and beta["schema"] == 1
    assert beta["size"] == len(last) and beta["sha256"] == hashlib.sha256(last).hexdigest()
    assert beta["url"] == (
        "https://devnullgamer.github.io/smart-grind-by-weight/firmware/v1.5.0-rc.10/"
        "smart-grind-by-weight-v1.5.0-rc.10.bin"
    )
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", beta["published"])


def test_release_beats_its_prereleases(tree, tmp_path):
    make_release(tree, "v1.5.0-rc.1")
    make_release(tree, "v1.5.0")
    make_release(tree, "v1.4.9")
    out = tmp_path / "out"
    ui.write_channel_files(out, ui.collect_releases(str(tree)))
    assert read(out / "update-stable.json")["version"] == "1.5.0"
    assert read(out / "update-beta.json")["version"] == "1.5.0"


def test_only_prereleases_leaves_stable_empty(tree, tmp_path):
    make_release(tree, "v1.5.0-beta")
    out = tmp_path / "out"
    ui.write_channel_files(out, ui.collect_releases(str(tree)))
    assert read(out / "update-stable.json") == {"schema": 1, "channel": "stable", "none": True}
    assert read(out / "update-beta.json")["version"] == "1.5.0-beta"


def test_no_releases_writes_none_documents(tmp_path):
    out = tmp_path / "out"
    ui.write_channel_files(out, ui.collect_releases(str(tmp_path / "does-not-exist")))
    assert read(out / "update-stable.json") == {"schema": 1, "channel": "stable", "none": True}
    assert read(out / "update-beta.json") == {"schema": 1, "channel": "beta", "none": True}
    assert (out / "update-stable.json").read_bytes() == b'{"schema":1,"channel":"stable","none":true}\n'


def test_only_unified_releases_are_offered(tree):
    make_release(tree, "v1.3.0", name=SINGLE_BOARD)
    make_release(tree, "v1.3.1", manifest=False)
    assert ui.collect_releases(str(tree)) == []


def test_bad_tags_and_images_skipped(tree):
    make_release(tree, "latest")
    make_release(tree, "v1.4")
    make_release(tree, "v1.4.0+7")
    make_release(tree, "v1.4.1", image=False)
    make_release(tree, "v1.4.2", size=1000)  # smaller than any application image
    (tree / "index.json").write_text("[]", encoding="utf-8")  # a file next to the tag directories
    make_release(tree, "v1.4.3")
    assert [e["tag"] for e in ui.collect_releases(str(tree))] == ["v1.4.3"]


def test_over_long_release_skipped_shorter_chosen(tree, tmp_path):
    long_tag = "v1.5.0-release-candidate.1"
    make_release(tree, long_tag)
    make_release(tree, "v1.4.0")
    # A long base URL pushes the image URL past the device's 128 character limit
    long_base = "https://example.com/" + "a" * 60 + "/"
    releases = ui.collect_releases(str(tree), base_url=long_base)
    assert [e["tag"] for e in releases] == []  # both urls exceed 128 with this base
    short_base = "https://example.com/" + "a" * 18 + "/"
    tags = [e["tag"] for e in ui.collect_releases(str(tree), base_url=short_base)]
    assert tags == [long_tag, "v1.4.0"]
    # url length 128 is the limit: find a base where only the longer tag exceeds it
    pad = 128 - len(f"firmware/v1.4.0/smart-grind-by-weight-v1.4.0.bin") - len("https://x.io/")
    base = "https://x.io/" + "a" * (pad - 1) + "/"
    tags = [e["tag"] for e in ui.collect_releases(str(tree), base_url=base)]
    assert tags == ["v1.4.0"]


def test_limit_problem_checks(tree):
    doc = {"version": "1.4.0", "tag": "v1.4.0", "url": "https://x.io/firmware/v1.4.0/a.bin"}
    assert ui.limit_problem(doc) is None
    assert ui.limit_problem(dict(doc, url="https://x.io/" + "a" * 130 + ".bin"))
    assert ui.limit_problem(dict(doc, url="https://x.io/a b.bin"))
    assert ui.limit_problem(dict(doc, tag="v" + "1" * 200))


def test_corrupt_manifest_skipped(tree):
    make_release(tree, "v1.4.0")
    (tree / "v1.4.0" / "smart-grind-by-weight-v1.4.0.manifest.json").write_text("{not json", encoding="utf-8")
    assert ui.collect_releases(str(tree)) == []


def test_published_from_tsv_and_mtime_fallback(tree, tmp_path):
    make_release(tree, "v1.4.0")
    make_release(tree, "v1.4.1")
    tsv = tmp_path / "published.tsv"
    tsv.write_text("v1.4.0\t2026-03-02T10:11:12Z\r\nv9.9.9\t2026-01-01T00:00:00Z\ngarbage\n", encoding="utf-8")
    dates = {e["tag"]: e["document"]["published"] for e in ui.collect_releases(str(tree), published=ui.read_published_tsv(str(tsv)))}
    assert dates["v1.4.0"] == "2026-03-02"
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", dates["v1.4.1"])
    assert ui.read_published_tsv(None) == {}
    assert ui.read_published_tsv(str(tmp_path / "missing.tsv")) == {}


def test_documents_are_flat_with_stable_key_order(tree, tmp_path):
    make_release(tree, "v1.4.0")
    out = tmp_path / "out"
    ui.write_channel_files(out, ui.collect_releases(str(tree)))
    raw = (out / "update-stable.json").read_text(encoding="utf-8")
    assert raw.endswith("}\n") and "\n" not in raw[:-1] and "\r" not in raw
    document = json.loads(raw)
    assert list(document) == ["schema", "channel", "version", "tag", "prerelease", "published", "size", "sha256", "url"]
    assert all(not isinstance(v, (dict, list)) for v in document.values())
    assert len(raw.encode("utf-8")) < 2048


def test_base_url_option_gets_trailing_slash(tree, tmp_path):
    make_release(tree, "v1.4.0")
    out = tmp_path / "out"
    assert ui.main(["--firmware-root", str(tree), "--output-dir", str(out), "--base-url", "http://127.0.0.1:8000"]) == 0
    assert read(out / "update-stable.json")["url"].startswith("http://127.0.0.1:8000/firmware/v1.4.0/")


def test_constants_match_firmware_config():
    text = (TOOLS_DIR.parent / "src" / "config" / "update.h").read_text(encoding="utf-8")
    assert f'#define UPDATE_PULL_BASE_URL "{ui.BASE_URL}"' in text
    assert f'#define UPDATE_PULL_STABLE_FILE "{ui.CHANNEL_FILES["stable"]}"' in text
    assert f'#define UPDATE_PULL_BETA_FILE "{ui.CHANNEL_FILES["beta"]}"' in text
    assert "#define UPDATE_IMAGE_MIN_SIZE (128 * 1024)" in text and ui.MIN_IMAGE_SIZE == 128 * 1024
    assert f"#define UPDATE_VERSION_MAX_LEN {ui.MAX_VERSION_LEN}" in text
    assert f"#define UPDATE_PULL_STRING_MAX {ui.MAX_STRING_LEN}" in text
    assert f"#define UPDATE_PULL_INDEX_MAX_BYTES {ui.MAX_INDEX_BYTES}" in text
    assert f"#define UPDATE_PULL_INDEX_SCHEMA {ui.SCHEMA}" in text

"""Contract guard for gaggimate-plugin/ (chg020, steps 8a and 8b).

No GaggiMate checkout, no PlatformIO and no hardware are needed. It keeps the files the plugin shares with the grinder
firmware byte-identical (line endings aside), keeps the pure plugin code free of Arduino/NimBLE/heap, checks the patch
list and README, and runs the plugin's own host tests (g++) against the shared vectors. When GAGGIMATE_CHECKOUT points
to a GaggiMate checkout it also checks, read-only, that the patches apply.
"""

import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
PLUGIN = ROOT / "gaggimate-plugin"
GLINK = PLUGIN / "src" / "display" / "plugins" / "grinderlink"

# plugin file -> firmware file it must equal
SHARED = {
    GLINK / "config" / "bluetooth.h": ROOT / "src" / "config" / "bluetooth.h",
    GLINK / "bluetooth" / "glink_frame.h": ROOT / "src" / "bluetooth" / "glink_frame.h",
    GLINK / "bluetooth" / "glink_frame.cpp": ROOT / "src" / "bluetooth" / "glink_frame.cpp",
    GLINK / "bluetooth" / "glink_json.h": ROOT / "src" / "bluetooth" / "glink_json.h",
    GLINK / "bluetooth" / "glink_json.cpp": ROOT / "src" / "bluetooth" / "glink_json.cpp",
    PLUGIN / "docs" / "GLINK-PROTOCOL.md": ROOT / "docs" / "GLINK-PROTOCOL.md",
    PLUGIN / "tools" / "tests" / "glink_vectors.json": ROOT / "tools" / "tests" / "glink_vectors.json",
    PLUGIN / "test" / "test_grinderlink" / "glink_vectors.h": ROOT / "test_native" / "glink_vectors.h",
}

# Pure code: the plugin's own transport-independent files plus the shared ones.
PURE = [
    GLINK / "GLinkMessages.h",
    GLINK / "GLinkMessages.cpp",
    GLINK / "GLinkClient.h",
    GLINK / "GLinkClient.cpp",
    GLINK / "bluetooth" / "glink_frame.h",
    GLINK / "bluetooth" / "glink_frame.cpp",
    GLINK / "bluetooth" / "glink_json.h",
    GLINK / "bluetooth" / "glink_json.cpp",
]

# Files a patch may touch (8a: settings, registration, web settings args; 8b adds the web card hook-up and locales).
PATCH_ALLOWED = re.compile(
    r"^(src/display/core/(Settings|Controller)\.(h|cpp)|src/display/plugins/WebUIPlugin\.(h|cpp)|web/src/.+)$"
)
PINNED_COMMIT = "b78beee"


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


# ---- shared files ----------------------------------------------------------------------------------------------

@pytest.mark.parametrize("copy,original", list(SHARED.items()), ids=lambda p: p.name)
def test_shared_file_is_identical_with_the_firmware(copy, original):
    assert copy.exists(), f"{copy} is missing"
    assert read_text(copy) == read_text(original), f"{copy.relative_to(ROOT)} differs from {original.relative_to(ROOT)}"


def test_no_shared_file_is_forgotten():
    # every file in the plugin's shared locations is either in SHARED or is the plugin's own
    own = {"GLinkMessages.h", "GLinkMessages.cpp", "GLinkClient.h", "GLinkClient.cpp", "GLinkPlugin.h", "GLinkPlugin.cpp"}
    for folder in (GLINK / "bluetooth", GLINK / "config"):
        for f in folder.iterdir():
            assert f in SHARED, f"{f.relative_to(ROOT)} is neither shared nor listed"
    for f in GLINK.iterdir():
        if f.is_file():
            assert f.name in own, f"unexpected file {f.relative_to(ROOT)}"


# ---- pure code --------------------------------------------------------------------------------------------------

FORBIDDEN = re.compile(
    r"#\s*include\s*<(Arduino|NimBLE|esp_|freertos|WiFi|ArduinoJson)|"
    r"#\s*include\s*<(vector|string|memory|map|functional)>|"
    r"\b(malloc|calloc|realloc|free|new|delete|String|std::)\b|\bNimBLE"
)


@pytest.mark.parametrize("path", PURE, ids=lambda p: p.name)
def test_pure_files_have_no_platform_or_heap_dependency(path):
    for n, line in enumerate(read_text(path).splitlines(), 1):
        code = line.split("//")[0]
        assert not FORBIDDEN.search(code), f"{path.name}:{n}: {line.strip()}"


def test_client_limits_match_the_protocol_constants():
    vectors = json.loads((PLUGIN / "tools" / "tests" / "glink_vectors.json").read_text(encoding="utf-8"))
    limits = vectors["limits"]
    messages = read_text(GLINK / "GLinkMessages.cpp")
    lo = round(limits["GLINK_DOSE_MIN_G"] * 10)
    hi = round(limits["GLINK_DOSE_MAX_G"] * 10)
    assert f"tenths >= {lo} && tenths <= {hi}" in messages
    assert f"u.size_tenths > {round(limits['GLINK_GRIND_SIZE_MAX'] * 10)}" in messages
    client = read_text(GLINK / "GLinkClient.h")
    assert f"RESPONSE_TIMEOUT_MS = 5000" in client
    assert "READY_TIMEOUT_MS = 5000" in client
    assert "START_CONFIRM_EXTRA_MS = 2000" in client


# ---- patches ----------------------------------------------------------------------------------------------------

PATCHES = sorted((PLUGIN / "patches").glob("*.patch"))


def patch_files(patch: Path):
    return re.findall(r"^diff --git a/(\S+) b/(\S+)$", patch.read_bytes().decode("utf-8"), flags=re.M)


def test_patch_list_is_consecutive_and_documented():
    assert PATCHES, "no patches"
    for i, p in enumerate(PATCHES, 1):
        assert p.name.startswith(f"{i:04d}-"), f"patch numbering broken at {p.name}"
    readme = read_text(PLUGIN / "README.md")
    for p in PATCHES:
        assert p.name in readme, f"{p.name} is not listed in the README"


@pytest.mark.parametrize("patch", PATCHES, ids=lambda p: p.name)
def test_patch_touches_only_allowed_files(patch):
    files = patch_files(patch)
    assert files, f"{patch.name} has no diff header"
    for a, b in files:
        assert a == b, f"{patch.name}: rename in {a}"
        assert PATCH_ALLOWED.match(a), f"{patch.name} touches {a}"


def test_patch_nvs_keys_fit_the_15_character_limit():
    keys = []
    for p in PATCHES:
        for line in p.read_bytes().decode("utf-8").splitlines():
            if line.startswith("+") and "Property<" in line:
                keys += re.findall(r'registry,\s*"([^"]+)"', line)
    assert keys, "the settings patch adds no Property"
    assert len(set(keys)) == len(keys)
    for k in keys:
        assert 0 < len(k) <= 15, k


def test_patches_do_not_expose_a_secret_or_a_token():
    for p in PATCHES:
        assert not re.search(r"(?i)password|token|secret", p.read_bytes().decode("utf-8")), p.name


def test_patch_script_blocks_without_a_checkout():
    sh = shutil.which("sh")
    if sh is None:
        pytest.skip("no sh")
    env = {k: v for k, v in os.environ.items() if k != "GAGGIMATE_CHECKOUT"}
    r = subprocess.run([sh, str(PLUGIN / "scripts" / "check_patches.sh")], capture_output=True, text=True, env=env, timeout=60)
    assert r.returncode == 2 and "BLOCKED" in r.stdout


@pytest.mark.skipif(not os.environ.get("GAGGIMATE_CHECKOUT"), reason="GAGGIMATE_CHECKOUT not set")
def test_patches_apply_to_the_checkout():
    sh = shutil.which("sh")
    if sh is None:
        pytest.skip("no sh")
    r = subprocess.run([sh, str(PLUGIN / "scripts" / "check_patches.sh")], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr


# ---- NimBLE glue and web card (8b) ---------------------------------------------------------------------------------

PLUGIN_CPP = GLINK / "GLinkPlugin.cpp"
CARD = PLUGIN / "web" / "src" / "pages" / "Settings" / "GrinderLinkCard.jsx"


def code_lines(path: Path):
    """Source lines with // comments dropped."""
    return [line.split("//")[0] for line in read_text(path).splitlines()]


def test_plugin_glue_is_device_only_and_checks_the_connection_count():
    for path in (GLINK / "GLinkPlugin.h", PLUGIN_CPP):
        text = read_text(path)
        assert "#ifndef GAGGIMATE_SIM" in text, f"{path.name} lacks the simulator guard"
    assert re.search(r"static_assert\(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 3", read_text(PLUGIN_CPP))


def test_plugin_never_replays_a_start():
    code = chr(10).join(code_lines(PLUGIN_CPP))
    # one place sends a start, and it is the explicit request path (never connect, drop, auto fetch or loop)
    assert code.count("client->start(") == 1
    start_at = code.index("client->start(")
    assert code.rfind("void GLinkPlugin::", 0, start_at) == code.index("void GLinkPlugin::beginClientOp")
    # the dose that guards the start is always passed
    assert re.search(r"client->start\(cmd\.profile, expect, now\)", code)
    # a request queued before a link change is refused
    assert "linkEpoch" in code and '"STALE"' in code
    # a start timeout keeps the Client's own block until a periodInfo has been seen, and the plugin watches it
    assert "startTimedOut" in code and "stopUnconfirmed" in code


def test_plugin_keeps_the_big_objects_off_the_stack_and_the_ws_threads_out_of_ble():
    code = chr(10).join(code_lines(PLUGIN_CPP))
    assert "new (std::nothrow) glinkc::Client" in code
    # the AsyncTCP handlers only enqueue
    enqueue = code[code.index("void GLinkPlugin::enqueue") : code.index("void GLinkPlugin::loop")]
    assert "NimBLE" not in enqueue and "client->" not in enqueue


def test_card_requests_match_what_the_plugin_registers():
    registered = set(re.findall(r'\{"(req:grinderlink:[a-z:]+)"', read_text(PLUGIN_CPP)))
    used = set(re.findall(r"'(req:grinderlink:[a-z:]+)'", read_text(CARD)))
    assert registered and used
    assert used <= registered, used - registered
    events = set(re.findall(r"'(evt:grinderlink:[a-z]+)'", read_text(CARD)))
    assert events and all(f'"{e}"' in read_text(PLUGIN_CPP) for e in events)


def test_card_and_patches_use_the_same_setting_names():
    card = read_text(CARD)
    assert "grinderLinkActive" in card
    hook = read_text(PLUGIN / "patches" / "0004-webui-grinderlink-card-hook.patch")
    assert "GrinderLinkCard" in hook and "'grinderLinkActive'" in hook


def test_check_script_refuses_a_directory_that_is_not_empty(tmp_path):
    sh = shutil.which("sh")
    if sh is None:
        pytest.skip("no sh")
    (tmp_path / "keep.txt").write_text("x", encoding="utf-8")
    r = subprocess.run([sh, str(PLUGIN / "scripts" / "check_against_gaggimate.sh"), str(tmp_path)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 2 and "BLOCKED" in r.stdout
    assert (tmp_path / "keep.txt").exists()


# ---- README -----------------------------------------------------------------------------------------------------

def test_readme_states_what_was_and_was_not_verified():
    readme = read_text(PLUGIN / "README.md")
    for heading in ("## Verified", "## Not verified", "## Still to do"):
        assert heading in readme, f"README lacks '{heading}'"
    assert PINNED_COMMIT in readme
    for f in SHARED:
        if f.suffix in (".h", ".cpp") and "grinderlink" in f.parts:
            assert f.name in readme, f"README does not mention the shared file {f.name}"
    assert "contributed upstream" in readme


def test_no_machine_specific_paths_in_the_plugin():
    for f in PLUGIN.rglob("*"):
        if f.is_file() and f.suffix in {".md", ".h", ".cpp", ".sh", ".patch"} and f.name != "GLINK-PROTOCOL.md":
            text = f.read_bytes().decode("utf-8", errors="replace")
            assert "C:\\Users" not in text and "/Users/" not in text, f.relative_to(ROOT)


# ---- host tests -------------------------------------------------------------------------------------------------

def test_plugin_host_tests_pass(tmp_path):
    sh, gxx = shutil.which("sh"), shutil.which("g++")
    if sh is None or gxx is None:
        pytest.skip("needs sh and g++")
    env = dict(os.environ, TMPDIR=str(tmp_path))
    r = subprocess.run([sh, str(PLUGIN / "scripts" / "run_host_tests.sh")], capture_output=True, text=True, env=env, timeout=600)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    assert re.search(r"\b\d+ tests, 0 failed", r.stdout), r.stdout[-500:]

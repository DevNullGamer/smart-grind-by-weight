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
    GLINK / "bluetooth" / "glink_disconnect.h": ROOT / "src" / "bluetooth" / "glink_disconnect.h",
    GLINK / "bluetooth" / "glink_disconnect.cpp": ROOT / "src" / "bluetooth" / "glink_disconnect.cpp",
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
    GLINK / "GLinkDoseSave.h",
    GLINK / "GLinkDoseSave.cpp",
    GLINK / "GLinkDevState.h",
    GLINK / "GLinkDevState.cpp",
    GLINK / "GLinkSettingsSync.h",
    GLINK / "GLinkSettingsSync.cpp",
    GLINK / "GLinkResultGate.h",
    GLINK / "GLinkResultGate.cpp",
    GLINK / "GLinkGrinderCore.h",
    GLINK / "GLinkGrinderCore.cpp",
    GLINK / "RxRing.h",
    GLINK / "bluetooth" / "glink_frame.h",
    GLINK / "bluetooth" / "glink_frame.cpp",
    GLINK / "bluetooth" / "glink_json.h",
    GLINK / "bluetooth" / "glink_json.cpp",
]

# Files a patch may touch (chg025 spec 3.6): the BluetoothDevices framework, Controller registration, the web model name.
PATCH_ALLOWED = re.compile(
    r"^(lib/BluetoothDevices/src/BluetoothGrinder\.h"
    r"|src/display/plugins/BluetoothDevicesPlugin\.(h|cpp)"
    r"|src/display/core/Controller\.cpp"
    r"|web/src/utils/bluetoothDevices\.js"
    r"|web/test/bluetooth-devices\.test\.js)$"
)
PINNED_COMMIT = "2e64abd"  # short form; the full SHA in the scripts is checked against it, the README against the scripts


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


# ---- shared files ----------------------------------------------------------------------------------------------

@pytest.mark.parametrize("copy,original", list(SHARED.items()), ids=lambda p: p.name)
def test_shared_file_is_identical_with_the_firmware(copy, original):
    assert copy.exists(), f"{copy} is missing"
    assert read_text(copy) == read_text(original), f"{copy.relative_to(ROOT)} differs from {original.relative_to(ROOT)}"


def test_no_shared_file_is_forgotten():
    # every file in the plugin's shared locations is either in SHARED or is the plugin's own
    own = {"GLinkMessages.h", "GLinkMessages.cpp", "GLinkClient.h", "GLinkClient.cpp", "GLinkDoseSave.h", "GLinkDoseSave.cpp", "RxRing.h",
           "GLinkDevState.h", "GLinkDevState.cpp", "GLinkSettingsSync.h", "GLinkSettingsSync.cpp",
           "GLinkResultGate.h", "GLinkResultGate.cpp", "GLinkGrinderCore.h", "GLinkGrinderCore.cpp",
           "GLinkGrinder.h", "GLinkGrinder.cpp", "GLinkPlugin.h", "GLinkPlugin.cpp"}
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
        code = re.sub(r"=\s*delete\s*;", "", line.split("//")[0])  # a deleted special member is not a heap delete
        assert not FORBIDDEN.search(code), f"{path.name}:{n}: {line.strip()}"


@pytest.mark.parametrize("name", ["GLinkGrinderCore.h", "GLinkGrinderCore.cpp"])
def test_grinder_core_never_calls_client_start_or_stop(name):
    # chg025: the driver has no start/stop; the Client keeps them, the core must not use them
    for n, line in enumerate(read_text(GLINK / name).splitlines(), 1):
        code = line.split("//")[0]
        assert not re.search(r"\b(start|stop)\s*\(", code), f"{name}:{n}: {line.strip()}"


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
    for i, p in enumerate(PATCHES, 1):
        assert p.name.startswith(f"{i:04d}-"), f"patch numbering broken at {p.name}"
    readme = read_text(PLUGIN / "README.md")
    for p in PATCHES:
        assert p.name in readme, f"{p.name} is not listed in the README"


@pytest.mark.parametrize("script", ["check_against_gaggimate.sh", "check_patches.sh"])
def test_scripts_pin_the_full_sha_of_the_pinned_commit(script):
    m = re.search(r"^PINNED=([0-9a-f]{40})$", read_text(PLUGIN / "scripts" / script), flags=re.M)
    assert m, f"{script} has no full PINNED SHA"
    assert m.group(1).startswith(PINNED_COMMIT), f"{script} pins {m.group(1)}"


@pytest.mark.parametrize("patch", PATCHES, ids=lambda p: p.name)
def test_patch_touches_only_allowed_files(patch):
    files = patch_files(patch)
    assert files, f"{patch.name} has no diff header"
    for a, b in files:
        assert a == b, f"{patch.name}: rename in {a}"
        assert PATCH_ALLOWED.match(a), f"{patch.name} touches {a}"


def test_patches_do_not_expose_a_secret_or_a_token():
    for p in PATCHES:
        # only lines the patch adds; context lines are GaggiMate's own (e.g. its homeAssistantPassword setting)
        # only the two file headers are skipped; any other added line that starts with "+++" is checked
        headers = ("+++ b/", "+++ /dev/null")
        lines = p.read_bytes().decode("utf-8").splitlines()
        added = "\n".join(l for l in lines if l.startswith("+") and not l.startswith(headers))
        assert not re.search(r"(?i)password|token|secret", added), p.name


def test_take_grind_finished_has_a_single_trigger_site():
    # chg025 step 4: the patched BluetoothDevicesPlugin.cpp calls takeGrindFinished exactly once (the event trigger);
    # the only other mention is the definition in BluetoothGrinder.h
    calls, definitions = [], []
    for p in PATCHES:
        current = None
        for line in p.read_bytes().decode("utf-8").splitlines():
            m = re.match(r"^diff --git a/(\S+) b/", line)
            if m:
                current = m.group(1)
            elif line.startswith("+") and not line.startswith("+++") and "takeGrindFinished" in line:
                (definitions if re.search(r"\bbool\s+takeGrindFinished\s*\(", line) else calls).append((current, line))
    assert len(calls) == 1, calls
    assert calls[0][0] == "src/display/plugins/BluetoothDevicesPlugin.cpp", calls
    assert 'trigger("grinder:grind:finished"' in "\n".join(
        l for p in PATCHES for l in p.read_bytes().decode("utf-8").splitlines() if l.startswith("+"))
    assert [d[0] for d in definitions] == ["lib/BluetoothDevices/src/BluetoothGrinder.h"], definitions


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
    # the README names the pin by its full SHA, the same one the scripts pin; the old base is gone
    full = re.search(r"^PINNED=([0-9a-f]{40})$", read_text(PLUGIN / "scripts" / "check_against_gaggimate.sh"), flags=re.M)
    assert full and full.group(1) in readme, "README lacks the full SHA the scripts pin"
    assert "b78beee" not in readme
    assert "b78beee" not in read_text(ROOT / "docs" / "DEVELOPMENT.md")
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


# ---- device glue guards (chg025 step 5; the glue cannot run on the host, so these are text-level) -----------------

def code_of(path: Path) -> str:
    """The file's text without // comments (string literals with // are not used in these files)."""
    return "\n".join(line.split("//")[0] for line in read_text(path).splitlines())


def block_after(text: str, start: str) -> str:
    """Text from `start` to the matching closing brace of the first '{' after it."""
    i = text.index(start)
    j = text.index("{", i)
    depth = 0
    for k in range(j, len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[i:k + 1]
    raise AssertionError(f"unbalanced braces after {start}")


GRINDER_CPP = GLINK / "GLinkGrinder.cpp"
GRINDER_H = GLINK / "GLinkGrinder.h"
PLUGIN_CPP = GLINK / "GLinkPlugin.cpp"
PLUGIN_H = GLINK / "GLinkPlugin.h"


@pytest.mark.parametrize("path", [GRINDER_CPP, GLINK / "GLinkGrinderCore.cpp", PLUGIN_CPP], ids=lambda p: p.name)
def test_no_client_start_or_stop_call_in_the_driver_files(path):
    for n, line in enumerate(code_of(path).splitlines(), 1):
        assert not re.search(r"\b(start|stop)\s*\(", line), f"{path.name}:{n}: {line.strip()}"


def test_driver_does_not_override_shutdown():
    assert not re.search(r"\bshutdown\b", code_of(GRINDER_H))


@pytest.mark.parametrize("path", [GRINDER_H, GRINDER_CPP, PLUGIN_H, PLUGIN_CPP], ids=lambda p: p.name)
def test_device_files_start_with_the_sim_guard(path):
    first = next(l for l in read_text(path).splitlines() if l.strip())
    assert first.strip() == "#ifndef GAGGIMATE_SIM", f"{path.name}: {first}"


def test_grinder_cpp_asserts_enough_nimble_connections():
    assert re.search(r"static_assert\(\s*CONFIG_BT_NIMBLE_MAX_CONNECTIONS\s*>=\s*3\b", code_of(GRINDER_CPP))


def test_plugin_header_includes_no_glink_header():
    includes = re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]", code_of(PLUGIN_H))
    assert includes, "GLinkPlugin.h has no includes"
    for inc in includes:
        assert not re.search(r"(?i)glink|bluetooth|RxRing", inc), f"GLinkPlugin.h includes {inc}"


def test_profile_dose_is_written_once_in_apply_pending_dose_under_the_lock():
    code = code_of(PLUGIN_CPP)
    assert len(re.findall(r"\bprofile\.dose\s*=(?!=)", code)) == 1
    assert len(re.findall(r"\.dose\s*=(?!=)", code)) == 1
    body = block_after(code, "void GLinkPlugin::applyPendingDose")
    assert re.search(r"\bprofile\.dose\s*=(?!=)", body)
    lock = re.search(r"\{[^{}]*getProcessLock\(\)", body)
    assert lock, "no lock scope with getProcessLock()"
    assert body.index("getProcessLock()") < re.search(r"\bprofile\.dose\s*=(?!=)", body).start()
    for needle in ("MODE_BREW", "isActive()", "isUpdating()", "isAutotuning()", "profile.utility"):
        assert needle in body, f"busy check {needle} missing"
    assert body.index("getProcessLock()") < body.index("MODE_BREW") < body.index("take(")
    assert body.index("take(") < body.index("profile.dose")


def test_event_handler_only_records():
    handler = block_after(code_of(PLUGIN_CPP), "void onGrindFinished")
    for bad in ("getProfileManager", "getSelectedProfile", "getProcessLock", "Settings", "Preferences"):
        assert bad not in handler, bad
    assert not re.search(r"\.dose\b", handler)
    assert "on_finished(" in handler


def test_connect_checks_the_busy_probe_before_the_first_radio_use():
    body = block_after(code_of(GRINDER_CPP), "bool GLinkGrinder::connect()")
    probe = body.index("busyProbe()")
    assert probe < body.index("clientConnect(")
    assert probe < body.index("ble_gap_event_listener_register")
    assert probe < body.index("new (std::nothrow)")


def test_allocation_is_nothrow_new_and_only_in_connect():
    code = code_of(GRINDER_CPP)
    connect = block_after(code, "bool GLinkGrinder::connect()")
    outside = re.sub(r"#\s*include[^\n]*", "", code.replace(connect, ""))  # `#include <new>` is not an allocation
    assert not re.search(r"\bnew\b|\b(malloc|calloc|realloc)\s*\(", outside), "allocation outside connect()"
    news = re.findall(r"\bnew\b[^;]*", connect)
    assert news, "connect() allocates nothing?"
    for n in news:
        assert n.startswith("new (std::nothrow)"), n
    assert not re.search(r"\bnew\b", code_of(PLUGIN_CPP))
    assert not re.search(r"\bnew\b", code_of(GRINDER_H))


def test_notification_lambda_neither_allocates_nor_logs():
    code = code_of(GRINDER_CPP)
    lam = block_after(code, "auto callback = [this]")
    assert "ring_.push(" in lam
    assert not re.search(r"\bnew\b|\b(malloc|calloc|realloc)\s*\(|\blog\s*\(|ESP_LOG|Serial|printf|LOG_|std::string|String\b", lam), lam


def test_grind_finished_is_reported_only_from_update():
    code = code_of(GRINDER_CPP)
    assert len(re.findall(r"\breportGrindFinished\s*\(", code)) == 1
    assert "reportGrindFinished" in block_after(code, "void GLinkGrinder::publishToFramework")
    assert "reportGrindFinished" not in block_after(code, "auto callback = [this]")
    assert "reportGrindFinished" not in block_after(code, "int onGapEvent")
    assert len(re.findall(r"\bpublishToFramework\s*\(", code)) == 2  # definition + the one call
    assert "publishToFramework()" in block_after(code, "void GLinkGrinder::update()")


def test_patch_0002_registers_the_plugin_and_touches_only_controller_cpp():
    patch = next(p for p in PATCHES if p.name.startswith("0002-"))
    assert [a for a, _ in patch_files(patch)] == ["src/display/core/Controller.cpp"]
    added = "\n".join(l[1:] for l in patch.read_bytes().decode("utf-8").splitlines()
                      if l.startswith("+") and not l.startswith("+++"))
    assert "registerPlugin(new GLinkPlugin())" in added
    assert "Settings" not in added and "settings" not in added


def added_lines(patch: Path) -> str:
    return "\n".join(l[1:] for l in patch.read_bytes().decode("utf-8").splitlines()
                     if l.startswith("+") and not l.startswith("+++"))


def test_patches_0001_to_0004_exist_consecutively_and_are_in_the_readme():
    assert [p.name[:4] for p in PATCHES][:4] == ["0001", "0002", "0003", "0004"]
    readme = read_text(PLUGIN / "README.md")
    for p in PATCHES[:4]:
        assert p.name in readme, p.name


def test_patch_0004_adds_supports_dose_default_false_and_the_dose_condition():
    patch = next(p for p in PATCHES if p.name.startswith("0004-"))
    assert sorted(a for a, _ in patch_files(patch)) == [
        "lib/BluetoothDevices/src/BluetoothGrinder.h", "src/display/plugins/BluetoothDevicesPlugin.cpp"]
    added = added_lines(patch)
    assert re.search(r"virtual\s+bool\s+supportsDose\(\)\s+const\s*\{\s*return\s+false;\s*\}", added)
    assert "supportsDose()" in added and "settings.dose > 0.0f" in added
    assert not re.search(r"(?i)password|token|secret", added)
    assert "BookooMotto" not in patch.read_bytes().decode("utf-8")


def test_glink_grinder_overrides_supports_dose_true_inside_the_sim_guard():
    text = read_text(GRINDER_H)
    assert re.search(r"bool\s+supportsDose\(\)\s+const\s+override\s*\{\s*return\s+true;\s*\}", code_of(GRINDER_H))
    assert text.lstrip().startswith("#ifndef GAGGIMATE_SIM")
    assert text.index("supportsDose") > text.index("#ifndef GAGGIMATE_SIM")
    assert text.rstrip().endswith("#endif") or "#endif" in text[text.index("supportsDose"):]


def test_readme_no_longer_lists_g1_as_open():
    for line in read_text(PLUGIN / "README.md").splitlines():
        if re.search(r"\bG1\b", line):
            assert re.search(r"0004|chg026|fixed", line), f"G1 still open? {line.strip()[:160]}"


def test_host_script_keeps_the_single_translation_unit_check():
    script = read_text(PLUGIN / "scripts" / "run_host_tests.sh")
    assert re.search(r"-fsyntax-only\s+test/test_grinderlink/glink_sources\.cpp", script)


# ---- documentation guards (chg025 step 7) ------------------------------------------------------------------------

DOC_FILES = [PLUGIN / "README.md", ROOT / "docs" / "DOC.md", ROOT / "docs" / "DEVELOPMENT.md", ROOT / "CLAUDE.md"]
RETIRED_NOTE = re.compile(r"(?i)retired|history|snapshot|old NVS|no disconnect button|no button of ours|not .{0,20}disconnect button")


def paragraphs(path: Path):
    return [p for p in read_text(path).split("\n\n") if p.strip()]


@pytest.mark.parametrize("path", DOC_FILES, ids=lambda p: p.name)
def test_docs_have_no_live_claim_about_the_retired_plugin(path):
    stale = re.compile(r"GrinderLinkCard|req:grinderlink|evt:grinderlink|b564759|Disconnect button|Disconnect on the (GaggiMate )?card")
    for para in paragraphs(path):
        if stale.search(para):
            assert RETIRED_NOTE.search(para), f"{path.name}: unretired mention: {para.strip()[:160]}"


@pytest.mark.parametrize("path", DOC_FILES, ids=lambda p: p.name)
def test_docs_name_gl_settings_keys_only_in_the_retired_keys_note(path):
    for para in paragraphs(path):
        if re.search(r"(?<![A-Za-z0-9_])gl_[a-z]{1,3}\b", para):
            assert re.search(r"(?i)retired|old NVS", para), f"{path.name}: gl_ key outside the retired note: {para.strip()[:160]}"


def test_readme_mentions_every_shared_file_and_every_patch():
    readme = read_text(PLUGIN / "README.md")
    for f in SHARED:
        assert f.name in readme, f"README does not mention {f.name}"
    for p in PATCHES:
        assert p.name in readme, f"README does not list {p.name}"


def test_no_machine_paths_in_the_docs():
    for path in DOC_FILES + [ROOT / "docs" / "DESIGN-NOTES.md"]:
        text = read_text(path)
        assert ":" + chr(92) + "Users" not in text and "/Users/" not in text and "/home/" not in text, path.name


def test_claude_md_stays_under_15_kb():
    assert (ROOT / "CLAUDE.md").stat().st_size < 15 * 1024

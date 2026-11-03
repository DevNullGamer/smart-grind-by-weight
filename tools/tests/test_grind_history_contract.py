"""Contract guard for the persistent recent-grinds history (chg016 steps 10 and 11, docs/DESIGN-NOTES.md).

Pure text checks, no build and no hardware. The behaviour of the pure modules is executed by
test_native/test_grind_history.cpp and test_grind_history_store.cpp; this file pins what only the firmware wiring can
break: purity of the core, the result strings, thread rules, the NVS key, the placement of the hook.
"""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


PURE = ["src/logging/grind_history.h", "src/logging/grind_history.cpp", "src/logging/grind_history_store.h",
        "src/logging/grind_history_store.cpp"]


@pytest.mark.parametrize("module", PURE)
def test_history_core_stays_pure(module):
    text = read(module)
    for include in re.findall(r'#include\s+[<"]([^>"]+)[>"]', text):
        for forbidden in ("Arduino", "Preferences", "LittleFS", "FS.h", "lvgl", "esp_", "freertos", "nvs", "WiFi"):
            assert forbidden not in include, f"{module} must stay host-testable: includes {include}"
    for token in ("malloc", "std::vector", "std::string", "LOG_", "Serial", "printf"):
        assert token not in text, f"{module} must not use {token} (no heap, no logging in the core)"
    code = "\n".join(line.split("//")[0] for line in text.splitlines())
    assert not re.search(r"\bnew\s+\w", code), f"{module} must not allocate"


def test_core_is_in_the_native_build_and_registered():
    ini = read("platformio-native.ini")
    for name in ("logging/grind_history.cpp", "logging/grind_history_store.cpp"):
        assert f"+<{name}>" in ini
    main = read("test_native/test_main.cpp")
    for name in ("run_grind_history_tests", "run_grind_history_store_tests"):
        assert main.count(name) == 2


def test_result_codes_cover_every_result_string_of_the_grind_controller():
    controller = read("src/controllers/grind_controller.cpp")
    sent = set(re.findall(r'result_string = "([A-Z][A-Z -]+)"', controller))
    sent |= set(re.findall(r'request\.result_string, "([A-Z][A-Z -]+)"', controller))
    assert sent == {"COMPLETE", "OVERSHOOT", "COMPLETE - MAX PULSES", "TIMEOUT"}
    mapping = read("src/logging/grind_history.cpp")
    for name in sent:
        assert f'strcmp(result, "{name}") == 0' in mapping, f"{name} is not mapped to a history result"


def test_only_the_adapter_includes_littlefs_for_the_history():
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in (".h", ".cpp"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="ignore")
        if "grind_history" in rel and rel != "src/logging/grind_history_fs.cpp":
            assert "LittleFS.h" not in text and "LittleFS." not in text, f"{rel} touches LittleFS for the history"
            assert "Preferences" not in text, f"{rel} touches NVS"
        if rel != "src/logging/grind_history_fs.cpp":
            assert '"/hist' not in text, f"{rel} names a history file"
    adapter = read("src/logging/grind_history_fs.cpp")
    assert "#include <LittleFS.h>" in adapter and '"/hist0.bin"' in adapter and '"/hist1.bin"' in adapter


def test_http_task_files_do_not_know_the_history_modules():
    for module in ("src/network/portal_routes.cpp", "src/network/portal_routes.h", "src/network/portal_command_gate.cpp",
                   "src/network/portal_command_gate.h"):
        text = read(module)
        for token in ("grind_history", "GrindHistory", "LittleFS", "hist_epoch"):
            assert token not in text, f"{module} (HTTP task) mentions {token}"


def test_file_operations_run_outside_the_ring_lock():
    fs = read("src/logging/grind_history_fs.cpp")
    snapshot = re.search(r"void GrindHistoryService::snapshot\(.*?\n\}\n", fs, re.S).group(0)
    for token in ("LOG_", "g_files", "history_append", "history_load", "malloc"):
        assert token not in snapshot, f"{token} inside the snapshot lock"
    record = re.search(r"void GrindHistoryService::record\(.*?\n\}\n", fs, re.S).group(0)
    assert record.index("ring_.push(entry)") < record.index("history_append(")
    # exactly two lock sections in record(): the ring push and the flag write, each a block of its own
    assert record.count("Section section(&mux_);") == 2
    for block in re.findall(r"\{\n\s*Section section\(&mux_\);(.*?)\n    \}", record, re.S):
        for token in ("LOG_", "history_append", "g_files"):
            assert token not in block
    begin = re.search(r"void GrindHistoryService::begin\(.*?\n\}\n", fs, re.S).group(0)
    assert begin.index("history_load(") < begin.index("Section section(&mux_);")


def test_hook_is_last_in_end_grind_session_after_statistics_and_session_save():
    logging = read("src/logging/grind_logging.cpp")
    body = re.search(r"void GrindLogger::end_grind_session\(.*?\n\}\n", logging, re.S).group(0)
    stats = body.index("statistics_manager.update_grind_session(")
    save = body.index("flush_session_to_flash()")
    hook = body.index("grind_history.record(")
    clear = body.index("clear_buffers();")
    assert stats < save < hook < clear
    assert body.count("grind_history.record(") == 1
    assert "if (!is_cancelled)" in body[hook - 120:hook]
    # independent of the Logging setting: the hook is not inside the logging_enabled branches
    assert body[save:hook].count("{") == body[save:hook].count("}"), "the hook sits inside a branch"
    # stopped grinds are discarded and never reach end_grind_session
    discard = re.search(r"void GrindLogger::discard_current_session\(.*?\n\}\n", logging, re.S).group(0)
    assert "grind_history" not in discard


def test_history_epoch_key_and_factory_reset():
    logging = read("src/logging/grind_logging.cpp")
    key = re.search(r'kHistoryEpochKey = "([^"]+)"', logging).group(1)
    assert key == "hist_epoch" and len(key) <= 15
    # the id lives in the NVS that the factory reset erases; Purge logs only touches /sessions
    assert "nvs_flash_erase()" in read("src/ui/controllers/menu_controller.cpp")
    purge = re.search(r"bool GrindLogger::clear_all_sessions_from_flash\(.*?\n\}\n", logging, re.S).group(0)
    assert "GRIND_SESSIONS_DIR" in purge and "/hist" not in purge and "hist_" not in purge
    # the file-IO task loads once, before it drains the flash-operation queue
    task = read("src/tasks/file_io_task.cpp")
    assert task.index("grind_history.begin(grind_logger.history_epoch())") < task.index(
        "grind_controller.process_queued_flash_operations()")


def test_ui_task_publishes_the_history_through_its_own_slot():
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    assert "grind_history.snapshot(&history_snap_)" in controller
    assert "publish_history(history_view_)" in controller
    gate = read("src/network/portal_command_gate.cpp")
    read_history = re.search(r"bool PortalCommandGate::read_history\(.*?\n\}\n", gate, re.S).group(0)
    assert "history_mux_" in read_history and "PORTAL_HISTORY_STALE_MS" in read_history
    assert "PORTAL_SNAPSHOT_STALE_MS" not in read_history

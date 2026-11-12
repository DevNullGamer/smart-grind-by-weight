"""Contract guard for the web panel API (chg014, docs/PORTAL-API.md).

Pure text checks, no build and no hardware. The shared vectors (portal_api_vectors.json) must agree with the
firmware sources they describe: the setting keys with the menu tables and UiSettings, the profile and setting
limits and defaults with the config headers, the error table with portal_api.cpp, and the document with both.
The vectors themselves are executed by test_native/test_portal_api.cpp and test_portal_guard.cpp.
"""

import json
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
VECTORS = json.loads((Path(__file__).resolve().parent / "portal_api_vectors.json").read_text(encoding="utf-8"))
FIELDS = {f["key"]: f for f in VECTORS["fields"]}
LIMITS = VECTORS["limits"]

# Portal setting key -> UiSettings getter (the one place the web names meet the stored settings)
SETTING_GETTER = {
    "ble_startup": "ble_startup",
    "logging": "logging_enabled",
    "swipe": "swipe_enabled",
    "auto_start": "auto_start",
    "auto_return": "auto_return",
    "brightness_normal": "brightness_normal",
    "brightness_screensaver": "brightness_screensaver",
    "purge_mode": "purge_mode",
    "purge_amount_g": "purge_amount_g",
    "freshness_hours": "freshness_hours",
    "grind_mode": "grind_mode_raw",
    "screen_dim_min": "screen_dim_minutes",
    "screen_off_min": "screen_off_minutes",
}
# Portal setting key -> menu table entry (ToggleSetting / SliderSetting / RadioSetting enumerator)
SETTING_MENU_ENTRY = {
    "ble_startup": "BLE_STARTUP",
    "logging": "LOGGING",
    "swipe": "SWIPE",
    "auto_start": "AUTO_START",
    "auto_return": "AUTO_RETURN",
    "brightness_normal": "BRIGHTNESS_NORMAL",
    "brightness_screensaver": "BRIGHTNESS_SCREENSAVER",
    "purge_amount_g": "PURGE_AMOUNT",
    "freshness_hours": "FRESHNESS",
    "purge_mode": "PURGE_MODE",
    "grind_mode": "GRIND_MODE",
    "screen_dim_min": "SCREEN_DIM_TIMEOUT",
    "screen_off_min": "SCREEN_OFF_TIMEOUT",
}
# Radio groups of the menu that the web API deliberately does not expose
RADIO_NOT_EXPOSED = {"BOARD_OVERRIDE"}
# chg020: the GaggiMate link toggle is touch-only (never in /api/settings or /api/schema)
TOUCH_ONLY_GETTERS = {"glink_link"}
TOUCH_ONLY_TOGGLES = {"GLINK_LINK"}
PROFILE_KEYS = [f"{p}_{k}" for p in ("single", "double", "custom") for k in ("weight_g", "time_s")]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def macro(text: str, name: str) -> float:
    match = re.search(rf"#define\s+{name}\s+(-?[0-9.]+)f?\b", text)
    assert match, f"{name} not found"
    return float(match.group(1))


def enum_members(text: str, enum_name: str) -> list[str]:
    body = re.search(rf"enum class {enum_name}\b[^{{]*\{{(.*?)\}};", text, re.S)
    assert body, f"enum {enum_name} not found"
    members = []
    for line in body.group(1).splitlines():
        match = re.match(r"\s*([A-Z][A-Z0-9_]*)\b", line.split("//")[0])
        if match:
            members.append(match.group(1))
    return members


# ---- keys ----------------------------------------------------------------------------------------------

def test_setting_keys_are_exactly_the_thirteen_menu_settings():
    settings = [f["key"] for f in VECTORS["fields"] if f["group"] == "settings"]
    assert len(settings) == 13
    assert sorted(settings) == sorted(SETTING_GETTER)
    assert [f["key"] for f in VECTORS["fields"] if f["group"] == "profile"] == PROFILE_KEYS
    assert [f["key"] for f in VECTORS["fields"] if f["group"] == "device"] == ["device_name"]
    assert len(set(FIELDS)) == len(VECTORS["fields"]) == 20
    assert VECTORS["fields"][-1]["key"] == "device_name"  # last: keeps the profile indices stable
    # settings first, then the six profile targets, then the device name (the order of PortalFieldIndex)
    keys = [f["key"] for f in VECTORS["fields"]]
    assert keys[:13] == settings and keys[13:19] == PROFILE_KEYS


def test_ui_settings_getters_equal_the_portal_settings():
    header = read("src/system/ui_settings.h")
    klass = header[header.index("class UiSettings"):]
    getters = set(re.findall(r"^\s*(?:bool|int|float)\s+([a-z_0-9]+)\(\) const;", klass, re.M))
    assert getters - TOUCH_ONLY_GETTERS == set(SETTING_GETTER.values()), "a UiSettings setting is missing in (or extra to) the web API"
    assert TOUCH_ONLY_GETTERS <= getters and not TOUCH_ONLY_GETTERS & set(SETTING_GETTER.values())


def test_menu_tables_equal_the_portal_settings():
    table_h = read("src/system/menu_setting_table.h")
    table_cpp = read("src/system/menu_setting_table.cpp")
    toggles = [m for m in enum_members(table_h, "ToggleSetting") if m not in TOUCH_ONLY_TOGGLES]
    sliders = enum_members(table_h, "SliderSetting")
    radios = enum_members(table_h, "RadioSetting")
    menu = {m for m in toggles + sliders + radios if m != "COUNT"} - RADIO_NOT_EXPOSED
    assert menu == set(SETTING_MENU_ENTRY.values())
    # The toggle rows read the same UiSettings getters the portal maps
    row_getters = set(re.findall(r"&UiSettings::([a-z_0-9]+),\s*&UiSettings::set_", table_cpp)) - TOUCH_ONLY_GETTERS
    bool_keys = [k for k, f in FIELDS.items() if f["type"] == "bool"]
    assert row_getters == {SETTING_GETTER[k] for k in bool_keys}
    for key in bool_keys:
        assert key in SETTING_MENU_ENTRY and SETTING_MENU_ENTRY[key] in toggles


def test_field_types_follow_the_menu_widgets():
    table_h = read("src/system/menu_setting_table.h")
    sliders = {m for m in enum_members(table_h, "SliderSetting") if m != "COUNT"}
    radios = {m for m in enum_members(table_h, "RadioSetting") if m != "COUNT"} - RADIO_NOT_EXPOSED
    for key, field in FIELDS.items():
        if field["group"] != "settings":
            continue
        entry = SETTING_MENU_ENTRY[key]
        if entry in sliders:
            assert field["type"] in ("int", "number")
        elif entry in radios:
            assert field["type"] == "enum"
        else:
            assert field["type"] == "bool"


# ---- limits and defaults ---------------------------------------------------------------------------------

def test_profile_limits_and_defaults_equal_user_h_and_profile_controller():
    user = read("src/config/user.h")
    # The web panel knows the three base profiles; USER_PROFILE_COUNT also counts the GaggiMate Profile Dose (chg020)
    assert macro(user, "USER_BASE_PROFILE_COUNT") == LIMITS["profile_count"] == 3
    assert macro(user, "USER_PROFILE_COUNT") == 4
    assert macro(user, "USER_MIN_TARGET_WEIGHT_G") == LIMITS["profile_weight_min_g"]
    assert macro(user, "USER_MAX_TARGET_WEIGHT_G") == LIMITS["profile_weight_max_g"]
    assert macro(user, "USER_MIN_TARGET_TIME_S") == LIMITS["profile_time_min_s"]
    assert macro(user, "USER_MAX_TARGET_TIME_S") == LIMITS["profile_time_max_s"]
    defaults = {
        "single_weight_g": "USER_SINGLE_ESPRESSO_WEIGHT_G", "single_time_s": "USER_SINGLE_ESPRESSO_TIME_S",
        "double_weight_g": "USER_DOUBLE_ESPRESSO_WEIGHT_G", "double_time_s": "USER_DOUBLE_ESPRESSO_TIME_S",
        "custom_weight_g": "USER_CUSTOM_PROFILE_WEIGHT_G", "custom_time_s": "USER_CUSTOM_PROFILE_TIME_S",
    }
    for key in PROFILE_KEYS:
        field = FIELDS[key]
        kind = "WEIGHT" if key.endswith("weight_g") else "TIME"
        suffix = "G" if kind == "WEIGHT" else "S"
        assert field["min"] == macro(user, f"USER_MIN_TARGET_{kind}_{suffix}")
        assert field["max"] == macro(user, f"USER_MAX_TARGET_{kind}_{suffix}")
        assert field["default"] == macro(user, defaults[key])

    controller = read("src/controllers/profile_controller.cpp")
    for fn, lo, hi in (("is_weight_valid", "USER_MIN_TARGET_WEIGHT_G", "USER_MAX_TARGET_WEIGHT_G"),
                       ("is_time_valid", "USER_MIN_TARGET_TIME_S", "USER_MAX_TARGET_TIME_S")):
        body = re.search(rf"ProfileController::{fn}\(float \w+\) const \{{(.*?)\}}", controller, re.S).group(1)
        assert re.search(rf">=\s*{lo}\b", body) and re.search(rf"<=\s*{hi}\b", body), f"{fn} no longer a closed range"
    for index, name in enumerate(VECTORS["profile_names"]):
        assert re.search(rf'strcpy\(profiles\[{index}\]\.name,\s*"{name}"\)', controller)


def test_setting_limits_and_defaults_equal_the_firmware_constants():
    hardware = read("src/config/hardware.h")
    grind = read("src/config/grind_control.h")
    user = read("src/config/user.h")
    models = read("src/system/menu_setting_models.cpp")
    assert macro(hardware, "HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT") == LIMITS["brightness_min_percent"]
    for key in ("brightness_normal", "brightness_screensaver"):
        assert FIELDS[key]["min"] == LIMITS["brightness_min_percent"]
        assert FIELDS[key]["max"] == LIMITS["brightness_max_percent"] == 100
    assert FIELDS["brightness_normal"]["default"] == round(macro(user, "USER_SCREEN_BRIGHTNESS_NORMAL") * 100)
    assert FIELDS["brightness_screensaver"]["default"] == round(macro(user, "USER_SCREEN_BRIGHTNESS_DIMMED") * 100)
    # Slider range of the menu (table row: min, max) matches the API limits
    table = read("src/system/menu_setting_table.cpp")
    assert '"Brightness", THEME_COLOR_ACCENT, 0, 100,' in table
    assert '"Screensaver", THEME_COLOR_WARNING, 0, 100,' in table

    assert macro(grind, "GRIND_PURGE_AMOUNT_MIN_G") == LIMITS["purge_amount_min_g"] == FIELDS["purge_amount_g"]["min"]
    assert macro(grind, "GRIND_PURGE_AMOUNT_MAX_G") == LIMITS["purge_amount_max_g"] == FIELDS["purge_amount_g"]["max"]
    assert macro(grind, "GRIND_PURGE_AMOUNT_DEFAULT_G") == FIELDS["purge_amount_g"]["default"]
    assert macro(grind, "GRIND_FRESHNESS_DEFAULT_HOURS") == FIELDS["freshness_hours"]["default"]

    steps = re.search(r"kFreshnessStepsHours\[kFreshnessStepCount\]\s*=\s*\{([^}]*)\}", models).group(1)
    firmware_steps = [float(s.strip().rstrip("f")) for s in steps.split(",")]
    assert firmware_steps == [float(s) for s in LIMITS["freshness_steps_hours"]]
    assert FIELDS["freshness_hours"]["min"] == firmware_steps[0]
    assert FIELDS["freshness_hours"]["max"] == firmware_steps[-1]

    # Enumerations: the choice index is the stored value
    assert enum_members(grind, "GrinderPurgeMode") == ["PRIME", "PURGE"]
    assert FIELDS["purge_mode"]["choices"] == ["keep", "remove"]  # Keep = Prime = 0, Remove = Purge = 1
    assert re.search(r"GRIND_PURGE_MODE_DEFAULT\s+static_cast<int>\(GrinderPurgeMode::PURGE\)", grind)
    assert FIELDS["purge_mode"]["default"] == "remove"
    assert enum_members(read("src/controllers/grind_mode.h"), "GrindMode") == ["WEIGHT", "TIME"]
    assert FIELDS["grind_mode"]["choices"] == ["weight", "time"]
    assert FIELDS["grind_mode"]["default"] == "weight"
    assert '{{"Weight", "Time", nullptr}' in table and '{{"Keep", "Remove", nullptr}' in table


def test_screen_timeout_fields_equal_the_firmware():
    sys.path.insert(0, str(ROOT / "tools" / "portal"))
    import portal_model
    power = read("src/system/screen_power.cpp")
    user = read("src/config/user.h")
    api = read("src/network/portal_api.cpp")
    for name, key, vector_key, mirror, default_macro, max_const in (
            ("kDimMinutesSteps", "screen_dim_min", "screen_dim_steps_min", portal_model.SCREEN_DIM_STEPS_MIN,
             "USER_SCREEN_DIM_DEFAULT_MIN", "kScreenDimMaxMin"),
            ("kOffMinutesSteps", "screen_off_min", "screen_off_steps_min", portal_model.SCREEN_OFF_STEPS_MIN,
             "USER_SCREEN_OFF_DEFAULT_MIN", "kScreenOffMaxMin")):
        body = re.search(rf"const int {name}\[\w+\]\s*=\s*\{{([^}}]*)\}}", power).group(1)
        steps = [int(s) for s in body.split(",")]
        assert steps == LIMITS[vector_key] == list(mirror), f"{name}: firmware, vectors and Python mirror differ"
        assert steps[-1] == 0, "Never (0) is the last step"
        field = FIELDS[key]
        # the API range is 0 (Never) .. the largest listed step; the default is the menu's default
        assert field["type"] == "int" and field["unit"] == "min" and field["min"] == 0 and field["max"] == max(steps)
        assert field["default"] == macro(user, default_macro) and field["default"] in steps
        assert float(re.search(rf"constexpr float {max_const} = (\d+)\.0f;", api).group(1)) == field["max"]
        assert f"normalise_screen_minutes({name}," in api, f"{key} must use the shared normaliser"
        mirror_field = portal_model.FIELD_BY_KEY[key]
        assert all(mirror_field[k] == field[k] for k in ("type", "group", "unit", "min", "max", "default"))
    assert "screen_minutes_normalise_api(clamp_input(in), steps, count)" in api
    # Stored values come back through the storage normaliser, like the menu slider
    assert api.count("screen_minutes_normalise_storage(") == 2
    # The edge of the normaliser the vectors rely on: Never below one minute, ties go to the lower step
    norm = portal_model.normalise_screen_minutes
    assert norm(0.4, portal_model.SCREEN_DIM_STEPS_MIN) == 0 and norm(45, portal_model.SCREEN_DIM_STEPS_MIN) == 30
    assert norm(1e6, portal_model.SCREEN_OFF_STEPS_MIN) == 480 and norm(20, portal_model.SCREEN_OFF_STEPS_MIN) == 10


def test_toggle_defaults_equal_ui_settings():
    cpp = read("src/system/ui_settings.cpp")
    defaults = dict(re.findall(r"bool UiSettings::(\w+)\(\) const \{ return store_\.get_bool\(\w+,\s*\w+,\s*(true|false)\)", cpp))
    for key, field in FIELDS.items():
        if field["type"] == "bool":
            assert defaults[SETTING_GETTER[key]] == ("true" if field["default"] else "false"), key


def test_device_name_equals_wifi_manager():
    wifi = read("src/config/wifi.h")
    field = FIELDS["device_name"]
    assert field["type"] == "string" and field["group"] == "device"
    # Stored in the key WiFiManager reads, default is WiFiManager's default
    assert re.search(r'WIFI_NVS_KEY_NAME\s+"name"', wifi) and re.search(r'WIFI_NVS_NAMESPACE\s+"wifi"', wifi)
    default = re.search(r'WIFI_DEFAULT_HOSTNAME\s+"([^"]+)"', wifi).group(1)
    assert field["default"] == default
    manager = read("src/network/wifi_manager.cpp")
    assert "WIFI_NVS_KEY_NAME" in manager and "wifi_validate_hostname" in manager
    # Limits: stricter than (a subset of) the 63-character label limit WiFiManager accepts
    assert field["min"] == 1 and field["max"] == LIMITS["device_name_max_len"] == 24
    assert field["max"] <= macro(wifi, "WIFI_HOSTNAME_MAX_LEN")
    portal = read("src/config/portal.h")
    assert macro(portal, "PORTAL_DEVICE_NAME_MAX_LEN") == field["max"]
    # The portal validator reuses WiFiManager's validator instead of a second copy of the character rule
    assert "wifi_validate_hostname" in read("src/network/portal_api.cpp")
    creds = read("src/network/wifi_credentials.cpp")
    body = re.search(r"bool wifi_validate_hostname\(const char\* name\) \{(.*?)\n\}", creds, re.S).group(1)
    assert "'a' && c <= 'z'" in body and "'0' && c <= '9'" in body and "c == '-'" in body
    assert "name[0] == '-'" in body and "name[len - 1] == '-'" in body


def test_request_limits_equal_portal_h():
    header = read("src/config/portal.h")
    assert macro(header, "PORTAL_BODY_MAX") == LIMITS["body_max"]
    assert macro(header, "PORTAL_STRING_MAX_LEN") == LIMITS["string_max_len"]
    assert macro(header, "PORTAL_KEY_MAX_LEN") == LIMITS["key_max_len"]
    assert macro(header, "PORTAL_HEADER_MAX_LEN") == LIMITS["header_max_len"]
    assert macro(header, "PORTAL_LEAD_IN_MS") == LIMITS["lead_in_ms"]
    assert re.search(rf'PORTAL_CUSTOM_HEADER_NAME\s+"{LIMITS["custom_header_name"]}"', header)
    assert re.search(rf'PORTAL_CUSTOM_HEADER_VALUE\s+"{LIMITS["custom_header_value"]}"', header)
    assert "portal.h" in read("src/config/constants.h")


# ---- errors ----------------------------------------------------------------------------------------------

def firmware_errors() -> list[tuple[str, int]]:
    cpp = read("src/network/portal_api.cpp")
    table = re.search(r"kErrors\[[^\]]*\]\s*=\s*\{(.*?)\n\};", cpp, re.S).group(1)
    return [(code, int(status)) for status, code in re.findall(r'\{(\d+),\s*"(\w+)",\s*"', table)][1:]  # drop "ok"


def test_error_table_equals_vectors_and_enum():
    vectors = [(e["error"], e["status"]) for e in VECTORS["errors"]]
    assert firmware_errors() == vectors
    header = read("src/network/portal_api.h")
    assert len(enum_members(header, "PortalError")) - 2 == len(vectors)  # NONE and COUNT


def test_every_case_uses_known_codes_and_keys():
    codes = {e["error"] for e in VECTORS["errors"]}
    for group in ("patch_cases", "command_cases"):
        names = [c["name"] for c in VECTORS[group]]
        assert len(names) == len(set(names)), f"duplicate case names in {group}"
        for case in VECTORS[group]:
            assert case["error"] is None or case["error"] in codes
    for case in VECTORS["patch_cases"]:
        for key in case.get("values", {}):
            assert key in FIELDS
        for key in case.get("adjusted", []):
            assert key in case["values"]
    for group in ("decision_cases", "write_cases"):
        for case in VECTORS[group]:
            assert case["error"] is None or case["error"] in codes
    for case in VECTORS["gate_cases"]:
        assert case["refusal"] is None or case["refusal"] in codes


def test_vectors_cover_the_required_refusals():
    patch_errors = {c["error"] for c in VECTORS["patch_cases"]}
    for needed in ("empty_body", "body_too_large", "malformed_json", "nested_value", "duplicate_key", "unknown_key",
                   "empty_patch", "wrong_type", "invalid_value", "out_of_range", "invalid_device_name"):
        assert needed in patch_errors, needed
    command_errors = {c["error"] for c in VECTORS["command_cases"]}
    for needed in ("missing_field", "unexpected_field", "unknown_command", "wrong_type", "out_of_range"):
        assert needed in command_errors, needed
    bodies = " ".join(c["body"] for c in VECTORS["patch_cases"])
    for token in ("NaN", "Infinity", "1e999"):
        assert token in bodies
    assert any(len(c["body"]) == LIMITS["body_max"] for c in VECTORS["patch_cases"])
    assert any(len(c["body"]) == LIMITS["body_max"] + 1 for c in VECTORS["patch_cases"])
    # Settings clamp, profiles refuse: both directions of every limit are covered
    adjusted_keys = {k for c in VECTORS["patch_cases"] for k in c.get("adjusted", [])}
    assert {"brightness_normal", "brightness_screensaver", "purge_amount_g", "freshness_hours", "screen_dim_min",
            "screen_off_min"} <= adjusted_keys
    refused_profiles = " ".join(c["body"] for c in VECTORS["patch_cases"] if c["error"] == "out_of_range")
    for key in PROFILE_KEYS:
        assert key in refused_profiles, key


# ---- document and purity ---------------------------------------------------------------------------------

def test_document_names_every_key_error_and_command():
    doc = read("docs/PORTAL-API.md")
    for key in FIELDS:
        assert f"`{key}`" in doc, f"{key} missing in PORTAL-API.md"
    for error in VECTORS["errors"]:
        assert f"`{error['error']}`" in doc, f"{error['error']} missing in PORTAL-API.md"
    for cmd in ("stop", "tare", "start", "motor_test"):
        assert f"`{cmd}`" in doc
    assert "X-Grinder" in doc
    for text in ("Web interface", "local network"):
        assert text in doc


PURE_MODULES = ["portal_api", "portal_guard", "portal_mailbox", "portal_apply", "portal_snapshot", "portal_insights"]


@pytest.mark.parametrize("module", [f"src/network/{name}.{ext}" for name in PURE_MODULES for ext in ("h", "cpp")])
def test_portal_modules_stay_pure(module):
    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', read(module))
    for include in includes:
        for forbidden in ("Arduino", "Preferences", "lvgl", "lv_", "WiFi", "esp_", "freertos", "update_server",
                          "profile_controller", "grind_controller", "ui_manager", "nvs"):
            assert forbidden not in include, f"{module} must stay host-testable: includes {include}"
    assert "Preferences" not in read(module)


def test_portal_modules_are_in_the_native_build_and_registered():
    ini = read("platformio-native.ini")
    for name in PURE_MODULES:
        assert f"+<network/{name}.cpp>" in ini
    main = read("test_native/test_main.cpp")
    for name in ("run_portal_api_tests", "run_portal_guard_tests", "run_portal_mailbox_tests",
                 "run_portal_apply_tests", "run_portal_snapshot_tests", "run_portal_insights_tests"):
        assert main.count(name) == 2


# ---- step 4: status names, UI-task wiring, thread rules ----------------------------------------------------

def test_status_names_follow_the_firmware_enums_and_the_document():
    snapshot = read("src/network/portal_snapshot.cpp")
    doc = read("docs/PORTAL-API.md")
    # UIState: one lower-case name per enumerator, in the document
    for member in enum_members(read("src/system/state_machine.h"), "UIState"):
        assert f'case UIState::{member}: return "{member.lower()}";' in snapshot, member
        assert f"`{member.lower()}`" in doc, f"ui_state {member.lower()} missing in PORTAL-API.md"
    # GrindPhase: the name table is in declaration order and the count constant matches
    phases = enum_members(read("src/controllers/grind_controller.h"), "GrindPhase")
    table = re.search(r"kGrindPhaseNames\[PORTAL_GRIND_PHASE_COUNT\] = \{(.*?)\};", snapshot, re.S).group(1)
    assert re.findall(r'"([a-z_]+)"', table) == [p.lower() for p in phases]
    assert re.search(r"PORTAL_GRIND_PHASE_COUNT\s*=\s*(\d+)", read("src/network/portal_snapshot.h")).group(1) == str(len(phases))
    for phase in phases:
        assert f"`{phase.lower()}`" in doc, f"grind_phase {phase.lower()} missing in PORTAL-API.md"


def test_warning_codes_follow_diagnostics_priority_and_the_document():
    diagnostics = read("src/system/diagnostics_controller.cpp")
    codes = enum_members(read("src/system/diagnostics_controller.h"), "DiagnosticCode")
    assert codes[0] == "NONE"
    # Priority order = the order of the checks in get_highest_priority_warning
    body = re.search(r"get_highest_priority_warning\(\) const \{(.*?)\n\}", diagnostics, re.S).group(1)
    priority = re.findall(r"find_diagnostic\(DiagnosticCode::([A-Z0-9_]+)\)\) \{\s*return", body)
    assert sorted(priority) == sorted(codes[1:]), "every DiagnosticCode except NONE has a priority"
    # The API code is the lower-case enumerator, except the sustained-noise code
    rename = {"LOAD_CELL_NOISY_SUSTAINED": "load_cell_noisy"}
    expected = [rename.get(c, c.lower()) for c in priority]
    api = read("src/network/portal_api.cpp")
    table = re.search(r"kWarningNames\[PORTAL_WARNING_COUNT\] = \{(.*?)\};", api, re.S).group(1)
    assert re.findall(r'"([a-z0-9_]+)"', table) == expected
    assert re.search(r"PORTAL_WARNING_COUNT\s*=\s*(\d+)", read("src/network/portal_api.h")).group(1) == str(len(expected))
    # The snapshot's DiagnosticCode values (bit n of the mask) follow the enum declaration order
    snapshot = read("src/network/portal_snapshot.h")
    values = re.search(r"PORTAL_WARNING_DIAG_CODES\[PORTAL_WARNING_COUNT\] = \{(.*?)\};", snapshot, re.S).group(1)
    assert [int(v) for v in re.findall(r"^\s*(\d+),", values, re.M)] == [codes.index(c) for c in priority]
    # The dev server mirror and the document
    sys.path.insert(0, str(ROOT / "tools" / "portal"))
    import portal_model
    assert list(portal_model.WARNING_CODES) == expected
    doc = read("docs/PORTAL-API.md")
    for name in expected:
        assert f"`{name}`" in doc, f"warning {name} missing in PORTAL-API.md"
    # Vectors: every code appears in a status case
    used = {w for c in VECTORS["status_cases"] for w in c["input"]["warnings"]}
    assert used == set(expected)


def test_stats_route_members_and_sources_follow_the_document():
    sys.path.insert(0, str(ROOT / "tools" / "portal"))
    import portal_model
    doc = read("docs/PORTAL-API.md")
    assert "`GET /api/stats`" in doc
    api = read("src/network/portal_api.cpp")
    encoder = re.search(r"bool portal_encode_stats\(.*?\n\}\n", api, re.S).group(0)
    members = re.findall(r'\\"([a-z_]+)\\":', encoder)
    assert members == list(portal_model.STATS_KEYS), "the encoder's members and their order equal the Python mirror"
    for name in members:
        assert f"`{name}`" in doc, f"stats member {name} missing in PORTAL-API.md"
    for case in VECTORS["stats_cases"]:
        assert list(case["input"]) == members and list(json.loads(case["json"])) == members, case["name"]
    # The UI task copies the counters under one statistics lock (the HTTP side only reads the snapshot slot) and the
    # pure function converts them
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    assert "portal_stats_from_counters(statistics_manager.get_snapshot())" in controller
    assert "get_snapshot() const" in read("src/system/statistics_manager.h")
    snapshot_body = re.search(r"StatisticsSnapshot StatisticsManager::get_snapshot\(\) const \{.*?\r?\n\}\r?\n",
                              read("src/system/statistics_manager.cpp"), re.S).group(0)
    assert snapshot_body.count("StatsLockGuard") == 1 and "LOG_" not in snapshot_body
    assert "publish_stats(gather_stats())" in controller
    assert "read_stats(" in read("src/network/portal_routes.cpp")
    assert "`GET /api/stats`" in doc.split("## 1.")[1].split("## 2.")[0], "gate table row"
    assert macro(read("src/config/portal.h"), "PORTAL_STATS_JSON_MAX") >= 400
    # The statistics slot has its own age limit and its own mutex, not the status snapshot's
    gate = read("src/network/portal_command_gate.cpp")
    read_stats = re.search(r"bool PortalCommandGate::read_stats\(.*?\r?\n\}\r?\n", gate, re.S).group(0)
    assert re.search(r"\bPORTAL_STATS_STALE_MS\b", read_stats) and re.search(r"\bstats_mux_\b", read_stats)
    assert "PORTAL_SNAPSHOT_STALE_MS" not in read_stats


def test_history_route_members_and_sources_follow_the_document():
    sys.path.insert(0, str(ROOT / "tools" / "portal"))
    import portal_model
    doc = read("docs/PORTAL-API.md")
    assert "`GET /api/history`" in doc
    api = read("src/network/portal_api.cpp")
    encoder = re.search(r"bool portal_encode_history\(.*?\n\}\n", api, re.S).group(0)
    members = re.findall(r'\\"([a-z_]+)\\":', encoder)
    assert members == ["count", "persistent", "grinds"] + list(portal_model.HISTORY_KEYS), (
        "the encoder's members and their order equal the Python mirror")
    for name in members:
        assert f"`{name}`" in doc, f"history member {name} missing in PORTAL-API.md"
    for name in ("complete", "max_pulses", "overshoot", "timeout", "unknown"):
        assert f"`{name}`" in doc, f"history result {name} missing in PORTAL-API.md"
    names = re.findall(r'return "([a-z_]+)";', re.search(r"portal_history_result_name\(.*?\n\}\n",
                                                          read("src/network/portal_insights.cpp"), re.S).group(0))
    assert names == [portal_model.HISTORY_RESULTS[i] for i in range(4)] + ["unknown"]
    cases = VECTORS["history_cases"]
    for case in cases:
        assert set(case["input"]) == {"now_s", "persistent", "entries"}, case["name"]
        assert list(json.loads(case["json"])) == ["count", "persistent", "grinds"], case["name"]
        for grind in json.loads(case["json"])["grinds"]:
            assert list(grind) == list(portal_model.HISTORY_KEYS), case["name"]
    assert {g["result"] for c in cases for g in json.loads(c["json"])["grinds"]} == {
        "complete", "max_pulses", "overshoot", "timeout", "unknown"}
    assert any(g["age_s"] is None for c in cases for g in json.loads(c["json"])["grinds"])
    assert any(g["target_g"] is None for c in cases for g in json.loads(c["json"])["grinds"])
    # The route reads its own slot on the HTTP task, answers 503 ui_busy until the boot load finished, and its buffer
    # fits the worst case; the document names the gate row
    routes = read("src/network/portal_routes.cpp")
    handler = re.search(r"int portal_handle_history\(.*?\n\}\n", routes, re.S).group(0)
    assert "read_history(" in handler and "!history.loaded" in handler and "PortalError::UI_BUSY" in handler
    assert "PortalRoute::API_READ" in handler and "ReplyBuffer json(PORTAL_HISTORY_JSON_MAX)" in handler
    assert "`GET /api/history`" in doc.split("## 1.")[1].split("## 2.")[0], "gate table row"
    assert macro(read("src/config/portal.h"), "PORTAL_HISTORY_JSON_MAX") >= 2400
    assert "portal_history_result_name(" in encoder and "portal_encode_history(" in handler


def test_diagnostics_route_members_words_and_sources_follow_the_document():
    sys.path.insert(0, str(ROOT / "tools" / "portal"))
    import portal_model
    doc = read("docs/PORTAL-API.md")
    assert "`GET /api/diagnostics`" in doc
    assert "`GET /api/diagnostics`" in doc.split("## 1.")[1].split("## 2.")[0], "gate table row"
    assert "| `GET /api/diagnostics` |" in doc.split("## 3.")[1].split("## 4.")[0], "routes table row"
    api = read("src/network/portal_api.cpp")
    encoder = re.search(r"bool portal_encode_diagnostics\(.*?\n\}\n", api, re.S).group(0)
    members = re.findall(r'\\"([a-z_]+)\\":', encoder)
    assert members == list(portal_model.DIAGNOSTICS_KEYS), "the encoder's members and their order equal the Python mirror"
    assert len(members) == 11
    for name in members:
        assert f"`{name}`" in doc, f"diagnostics member {name} missing in PORTAL-API.md"
    # ble: exactly six words, in the order of the enum, the Python mirror and the document
    header = read("src/network/portal_api.h")
    enum_words = [m.lower() for m in enum_members(header, "PortalBleState") if m != "COUNT"]
    table = re.search(r"kBleStateNames\[PORTAL_BLE_STATE_COUNT\] = \{(.*?)\};", api, re.S).group(1)
    assert re.findall(r'"([a-z_]+)"', table) == enum_words == list(portal_model.BLE_STATES)
    assert enum_words == ["off", "advertising", "connected", "link_listening", "link_connected", "link_linked"]
    for word in enum_words:
        assert f"`{word}`" in doc, f"ble word {word} missing in PORTAL-API.md"
    # board and board_source: the words BoardConfig::name() / source_name() return, nothing invented
    board = read("src/config/board_config.h")
    name_fn = re.search(r"const char\* name\(\) const \{(.*?)\n    \}", board, re.S).group(1)
    source_fn = re.search(r"const char\* source_name\(\) const \{(.*?)\n    \}", board, re.S).group(1)
    firmware_boards = re.findall(r'return "(\w+)";', name_fn)
    firmware_sources = re.findall(r'return "(\w+)";', source_fn)
    assert sorted(firmware_boards) == sorted(portal_model.BOARD_NAMES) == sorted(["V1", "V2", "UNKNOWN"])
    assert sorted(firmware_sources) == sorted(portal_model.BOARD_SOURCES)
    assert re.findall(r'"(\w+)"', re.search(r"kBoardNames\[\] = \{(.*?)\};", api, re.S).group(1)) == list(portal_model.BOARD_NAMES)
    assert re.findall(r'"(\w+)"', re.search(r"kBoardSourceNames\[\] = \{(.*?)\};", api, re.S).group(1)) == list(portal_model.BOARD_SOURCES)
    for word in firmware_boards + firmware_sources:
        assert f"`{word}`" in doc, f"board word {word} missing in PORTAL-API.md"
    # /api/info reports the same board name
    assert "board().name()" in read("src/network/update_server.cpp")
    # bounds: the constants of the encoder, the mirror and the document agree
    assert re.search(r"PORTAL_DIAGNOSTICS_FLOAT_MAX\s*=\s*1000000000\.0;", header)
    assert portal_model.DIAGNOSTICS_FLOAT_MAX == 1e9
    assert re.search(r"PORTAL_DIAGNOSTICS_LATENCY_MAX_MS\s*=\s*1000000\.0f;", header)
    assert portal_model.DIAGNOSTICS_LATENCY_MAX_MS == 1e6
    assert re.search(r"PORTAL_DIAGNOSTICS_RSSI_MIN_DBM\s*=\s*-127;", header)
    assert portal_model.DIAGNOSTICS_RSSI_MIN_DBM == -127
    # vectors: every word, the unknown signal and the non-finite inputs are covered; the longest reply fits its buffer
    cases = VECTORS["diagnostics_cases"]
    for case in cases:
        assert list(case["input"]) == members and list(json.loads(case["json"])) == members, case["name"]
    assert {c["input"]["ble"] for c in cases} == set(enum_words)
    assert {c["input"]["board"] for c in cases} == set(firmware_boards)
    assert {c["input"]["board_source"] for c in cases} == set(firmware_sources)
    assert any(json.loads(c["json"])["wifi_rssi_dbm"] is None for c in cases)
    assert any(c["input"]["cal_factor"] == "nan" for c in cases)
    buffer_size = macro(read("src/config/portal.h"), "PORTAL_DIAGNOSTICS_JSON_MAX")
    assert buffer_size == 320 and max(len(c["json"]) for c in cases) < buffer_size
    assert f"{max(len(c['json']) for c in cases)} bytes" in doc, "the document names the longest reply"
    # the route is read-only and additive: no write member, no new error code, the contract version is unchanged
    assert VECTORS["version"] == 1


# The GET /api/diagnostics handler only copies its snapshot slot and encodes it: none of these may appear in its text
DIAGNOSTICS_HANDLER_FORBIDDEN = ("grind_controller", "hardware_manager", "weight_sensor", "noise_level", "lv_",
                                 "Preferences", "ui_settings", "profile_controller", "LittleFS")
DIAGNOSTICS_HANDLER_FORBIDDEN_WORDS = ("SD",)  # whole word only


def diagnostics_handler_text(routes: str) -> str:
    return re.search(r"int portal_handle_diagnostics\(.*?\n\}\n", routes, re.S).group(0)


def assert_diagnostics_handler_reads_only_the_slot(handler: str) -> None:
    for token in DIAGNOSTICS_HANDLER_FORBIDDEN:
        assert token not in handler, f"portal_handle_diagnostics mentions {token}"
    for word in DIAGNOSTICS_HANDLER_FORBIDDEN_WORDS:
        assert not re.search(rf"\b{word}\b", handler), f"portal_handle_diagnostics mentions {word}"


def test_diagnostics_route_is_wired_through_its_own_slot():
    doc = read("docs/PORTAL-API.md")
    routes = read("src/network/portal_routes.cpp")
    handler = diagnostics_handler_text(routes)
    assert_diagnostics_handler_reads_only_the_slot(handler)
    assert "PortalRoute::API_READ" in handler and "read_diagnostics(" in handler and "PortalError::UI_BUSY" in handler
    assert "ReplyBuffer json(PORTAL_DIAGNOSTICS_JSON_MAX)" in handler and "portal_encode_diagnostics(" in handler
    assert 'log_stack_once(&stack_logged, "GET /api/diagnostics")' in handler
    assert "wifi_manager" not in handler and "board()" not in handler, "the HTTP task only copies the slot"
    # the gate: own lock, own age limit, cleared when the UI task goes away, no call out of the lock
    gate = read("src/network/portal_command_gate.cpp")
    reader = re.search(r"bool PortalCommandGate::read_diagnostics\(.*?\r?\n\}\r?\n", gate, re.S).group(0)
    assert "diagnostics_mux_" in reader and "PORTAL_DIAGNOSTICS_STALE_MS" in reader
    assert "PORTAL_SNAPSHOT_STALE_MS" not in reader and "LOG_" not in reader
    writer = re.search(r"void PortalCommandGate::publish_diagnostics\(.*?\r?\n\}\r?\n", gate, re.S).group(0)
    assert "diagnostics_mux_" in writer and "LOG_" not in writer and "malloc" not in writer and "new " not in writer
    attach = re.search(r"void PortalCommandGate::attach_ui\(.*?\r?\n\}\r?\n", gate, re.S).group(0)
    assert "diagnostics_published_ = false" in attach
    # the UI task gathers on its own cadence, only inside the Web interface gate, from the getters the pages use
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    update = controller[controller.index("void PortalUIController::update()"):controller.index("// Snapshot")]
    gate_at = update.index("if (!wifi_manager.web_enabled())")
    gather_at = update.index("publish_diagnostics(gather_diagnostics())")
    assert gate_at < gather_at
    assert "return;" in update[gate_at:update.index("}", gate_at)], "the web gate must return before the gather"
    assert "PORTAL_DIAGNOSTICS_REFRESH_MS" in update
    gather = re.search(r"PortalDiagnostics PortalUIController::gather_diagnostics\(.*?\n\}\n", controller, re.S).group(0)
    for call in ("is_calibrated()", "get_calibration_factor()", "get_standard_deviation_g(GRIND_SCALE_PRECISION_SETTLING_TIME_MS)",
                 "noise_level_diagnostic()", "get_detected_sample_rate_sps()", "get_motor_response_latency()",
                 "wifi_manager.is_connected()", "wifi_manager.rssi()", "link_mode() == GlinkLinkMode::LINK_ONLY",
                 "is_link_handshaken()", "bluetooth->is_enabled()", "board().name()", "board().source_name()",
                 "wifi_manager.push_enabled()"):
        assert call in gather, call
    for token in ("LOG_", "malloc", "update_async", "get_raw_adc_data", "calibrate(", "Preferences"):
        assert token not in gather, f"gather_diagnostics uses {token}"
    # the sample rate does not depend on the heartbeat build flag
    assert "get_current_sps" not in gather
    # constants and the document agree
    assert macro(read("src/config/portal.h"), "PORTAL_DIAGNOSTICS_STALE_MS") == 15000
    assert macro(read("src/config/portal.h"), "PORTAL_DIAGNOSTICS_REFRESH_MS") == 2000
    assert "13 of 16 used" in doc and "12 of 16" not in doc and "planned, G3b" not in doc
    # the handler count the document states is the number of registered routes
    server = read("src/network/update_server.cpp")
    assert len(re.findall(r'\{"/[^"]*", HTTP_', server)) == 13
    assert '{"/api/diagnostics", HTTP_GET, diagnostics_trampoline}' in server


def test_diagnostics_route_slot_margin_and_document_numbers():
    server = read("src/network/update_server.cpp")
    routes = len(re.findall(r'\{"/[^"]*", HTTP_', server))
    limit = int(macro(read("src/config/update.h"), "UPDATE_HTTP_MAX_URI_HANDLERS"))
    assert routes == 13, f"{routes} registered routes"
    assert routes + 3 <= limit, "fewer than 3 free handler slots"
    assert "config.max_uri_handlers = UPDATE_HTTP_MAX_URI_HANDLERS;" in server
    assert "esp_err_t diagnostics_trampoline(httpd_req_t* req) { return portal_handle_diagnostics(req); }" in server
    # the document states the same numbers as the firmware: 13 of 16 slots, 15 s age limit, 2 s republish
    doc = read("docs/PORTAL-API.md")
    assert macro(read("src/config/portal.h"), "PORTAL_DIAGNOSTICS_STALE_MS") == 15000
    assert macro(read("src/config/portal.h"), "PORTAL_DIAGNOSTICS_REFRESH_MS") == 2000
    assert re.search(r"15 s \(`PORTAL_DIAGNOSTICS_STALE_MS`", doc), "doc does not state the 15 s age limit"
    assert re.search(r"`PORTAL_DIAGNOSTICS_REFRESH_MS`, 2 s\)", doc), "doc does not state the 2 s republish"
    assert f"{routes} of {limit} used" in doc
    assert "handler slot 13 of 16" in read("docs/DESIGN-NOTES.md")


def test_diagnostics_gather_runs_on_the_ui_task_only():
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    assert "publish_diagnostics(gather_diagnostics())" in controller
    # the HTTP side, the gate and the server never gather: they only copy the slot
    for module in ("src/network/portal_routes.cpp", "src/network/portal_routes.h", "src/network/update_server.cpp",
                   "src/network/portal_command_gate.cpp", "src/network/portal_command_gate.h"):
        assert "gather_diagnostics" not in read(module), f"{module} mentions gather_diagnostics"
    # the one HTTP-side file that gets the same token guard as the other handlers
    routes = read("src/network/portal_routes.cpp")
    assert_diagnostics_handler_reads_only_the_slot(diagnostics_handler_text(routes))


def test_http_task_code_stays_off_the_ui_side():
    # The HTTP task never calls LVGL, GrindController, ProfileController, UiSettings or NVS: its files do not even
    # include them (wifi_manager.h only gives the RAM copies of the switches and the host name)
    for module in ("src/network/portal_routes.cpp", "src/network/portal_routes.h", "src/network/portal_command_gate.cpp",
                   "src/network/portal_command_gate.h"):
        text = read(module)
        for include in re.findall(r'#include\s+[<"]([^>"]+)[>"]', text):
            for forbidden in ("lvgl", "ui_manager", "profile_controller", "grind_controller", "ui_settings",
                              "nvs_settings_store", "Preferences", "hardware_manager", "menu_screen"):
                assert forbidden not in include, f"{module} (HTTP task) includes {include}"
        for token in ("Preferences", "ui_settings", "lv_", "profile_controller", "set_device_name", "nvs_",
                      "statistics_manager", "diagnostics_controller", "grind_logger", "grind_history", "LittleFS"):
            assert token not in text, f"{module} (HTTP task) mentions {token}"


def test_screen_timeout_fields_reach_their_own_slider_and_reload_once():
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    # field -> slider (a swap would write the dim value into the off setting)
    assert "case PF_SCREEN_DIM_MIN: return SliderSetting::SCREEN_DIM_TIMEOUT;" in controller
    assert "case PF_SCREEN_OFF_MIN: return SliderSetting::SCREEN_OFF_TIMEOUT;" in controller
    # normalised minutes -> slider position through the matching step table
    assert re.search(r"SCREEN_DIM_TIMEOUT:\s*return screen_minutes_to_index\([^;]*kDimMinutesSteps, kDimMinutesStepCount\)",
                     controller)
    assert re.search(r"SCREEN_OFF_TIMEOUT:\s*return screen_minutes_to_index\([^;]*kOffMinutesSteps, kOffMinutesStepCount\)",
                     controller)
    # both fields are stored through the slider hook, and exactly one reload covers them
    write_field = controller[controller.index("bool PortalUIController::write_field"):]
    assert "case PF_SCREEN_DIM_MIN:" in write_field and "case PF_SCREEN_OFF_MIN:" in write_field
    assert "slider_setting_def(id).store(ui_settings" in write_field
    assert controller.count("refresh_screen_timeout_settings()") == 1
    assert "field_bit(PF_SCREEN_DIM_MIN) | field_bit(PF_SCREEN_OFF_MIN)" in controller
    # a web write never wakes or darkens the screen: no brightness or wake call in that branch
    branch = controller[controller.index("field_bit(PF_SCREEN_DIM_MIN)"):]
    branch = branch[:branch.index("}")]
    assert "reapply_brightness" not in branch and "wake(" not in branch


def test_ui_task_wiring():
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    # every write goes through the shared apply logic and the touchscreen's own handlers
    assert "portal_apply_patch(" in controller and "portal_write_decide(" in controller
    assert "wifi_manager.set_device_name(" in controller and "set_grind_mode(" in controller
    assert "slider_setting_def(" in controller and "toggle_setting_def(" in controller
    assert "reapply_brightness()" in controller and "refresh_auto_action_settings()" in controller
    assert "refresh_screen_timeout_settings()" in controller  # a web write reloads the cached timeouts like the menu
    # the field order and the grind phase order are pinned at compile time
    assert controller.count("static_assert") >= 6
    # the controller is polled on the UI task, before the update screens can consume the frame
    ui = read("src/ui/ui_manager.cpp")
    assert ui.index("portal_controller_->update()") < ui.index("wifi_update_controller_->update()")
    # the routes are registered with the methods the document names
    server = read("src/network/update_server.cpp")
    for uri, method in (("/api/status", "HTTP_GET"), ("/api/stats", "HTTP_GET"), ("/api/schema", "HTTP_GET"),
                        ("/api/settings", "HTTP_GET"), ("/api/history", "HTTP_GET"),
                        ("/api/diagnostics", "HTTP_GET"), ("/api/settings", "HTTP_PATCH"), ("/api/command", "HTTP_POST")):
        assert f'{{"{uri}", {method}, ' in server, f"{method} {uri} not registered"
    max_handlers = macro(read("src/config/update.h"), "UPDATE_HTTP_MAX_URI_HANDLERS")
    assert len(re.findall(r'\{"/[^"]*", HTTP_', server)) <= max_handlers


def test_stop_path_does_not_depend_on_the_ui_task():
    # stop is never queued: the route calls portal_stop_now() before (and instead of) the mailbox ...
    routes = read("src/network/portal_routes.cpp")
    handler = routes[routes.index("int portal_handle_command("):]
    stop_branch = handler[handler.index("PortalCommand::STOP"):handler.index("} else {")]
    assert "portal_stop_now()" in stop_branch and "submit_command" not in stop_branch
    # ... and portal_stop.cpp, the one HTTP-task file that reaches the controllers, touches only the narrow
    # thread-safe entry points (no LVGL, UI manager, profiles, settings, NVS)
    stop = read("src/network/portal_stop.cpp")
    for call in ("grind_controller.request_stop()", "grinder->stop()", "portal_command_gate.request_stop()"):
        assert call in stop, call
    for token in ("lv_", "ui_manager", "profile_controller", "ui_settings", "Preferences", "nvs_", "stop_grind"):
        assert token not in stop, f"portal_stop.cpp mentions {token}"
    # the grind control task consumes the flag on every cycle, before anything can return early
    grind = read("src/controllers/grind_controller.cpp")
    update = grind[grind.index("void GrindController::update()"):]
    assert update.index("stop_requested_.exchange(false)") < update.index("if (!is_active())")
    # the UI task only cancels the autotune it drives and never gates the stop on the mailbox
    ui = read("src/ui/controllers/portal_ui_controller.cpp")
    assert "portal_stop_pending(" in ui and "handle_cancel()" in ui
    doc = read("docs/PORTAL-API.md")
    assert "portal_stop_now" in doc and "does not wait for the UI task" in doc


def test_rename_restarts_only_mdns_and_keeps_the_http_service():
    manager = read("src/network/wifi_manager.cpp")
    # a rename is applied from tick(): stop + start mDNS, both of which forget the registered "http" service so the
    # tick() block advertises it again under the new name
    tick = manager[manager.index("void WiFiManager::tick()"):]
    assert "mdns_restart_requested" in tick
    restart = tick[tick.index("if (mdns_restart_requested)"):tick.index("if (apply_requested)")]
    assert "stop_mdns();" in restart and "start_mdns();" in restart
    assert "WiFi.disconnect" not in restart and "stop_radio" not in restart
    for fn in ("void WiFiManager::start_mdns()", "void WiFiManager::stop_mdns()"):
        body = manager[manager.index(fn):]
        body = body[:body.index("\n}\n")]
        assert "http_service_registered = false;" in body, fn
    # the UI-task writer: NVS key shared with load_host_name(), name switched through two buffers
    setter = manager[manager.index("bool WiFiManager::set_device_name"):]
    setter = setter[:setter.index("\n}\n")]
    assert "WIFI_NVS_KEY_NAME" in setter and "wifi_validate_hostname" in setter and "host_name_index" in setter


def test_document_states_apply_order_and_partial_failure():
    doc = read("docs/PORTAL-API.md")
    for phrase in ("ascending field index", "device name last", "stops at the first", "`store_failed`",
                   "shortened to 24 characters"):
        assert phrase in doc, phrase


def test_profile_controller_reports_store_failures():
    header = read("src/controllers/profile_controller.h")
    for signature in ("bool set_profile_weight(", "bool set_profile_time(", "bool set_grind_mode(", "bool save_profiles("):
        assert signature in header, signature


def test_address_flag_wiring_and_ram_rollback():
    # outcome -> mailbox result -> JSON are pure functions; both call sites must use them
    assert "portal_result_from_outcome(outcome, patch.adjusted, values_)" in read("src/ui/controllers/portal_ui_controller.cpp")
    assert "portal_encode_patch_result(result, " in read("src/network/portal_routes.cpp")
    apply_cpp = read("src/network/portal_apply.cpp")
    assert "result.device_name_changed = outcome.device_name_changed;" in apply_cpp
    assert "result.device_name_changed)" in apply_cpp
    # a store that fails rolls the RAM value back (screen and next start agree with what is saved)
    profiles = read("src/controllers/profile_controller.cpp")
    assert profiles.count("not stored: RAM keeps matching") == 3
    controller = read("src/ui/controllers/portal_ui_controller.cpp")
    assert "if (!profiles->set_grind_mode(mode)) return false;" in controller


def test_touchscreen_grind_mode_paths_use_the_store_result():
    menu = read("src/ui/controllers/menu_controller_settings.cpp")
    assert "if (!ui_manager_->profile_controller->set_grind_mode(new_mode))" in menu
    ready = read("src/ui/controllers/ready_controller.cpp")
    assert "!ui_manager_->profile_controller->set_grind_mode(new_mode)" in ready
    manager = read("src/network/wifi_manager.cpp")
    start = manager[manager.index("void WiFiManager::start_mdns()"):]
    assert "MDNS.end();" in start[:start.index("\n}\n")]


# In-memory mutations (chg021 G3b): each guard must fail when the forbidden text is present. The edits are applied to
# what read() returns for one test run only; no repo file changes on disk.
THIS_MODULE = sys.modules[__name__]
HANDLER_ANCHOR = "int portal_handle_diagnostics(httpd_req* req) {"


@pytest.fixture
def mutate(monkeypatch):
    original = read

    def apply(rel: str, edit) -> None:
        def patched(path: str) -> str:
            text = original(path)
            if path != rel:
                return text
            edited = edit(text)
            assert edited != text, f"mutation did not apply to {rel}"
            return edited

        monkeypatch.setattr(THIS_MODULE, "read", patched)

    return apply


def insert_after(anchor: str, addition: str):
    return lambda text: text.replace(anchor, anchor + addition, 1)


@pytest.mark.parametrize("token", [*DIAGNOSTICS_HANDLER_FORBIDDEN, *DIAGNOSTICS_HANDLER_FORBIDDEN_WORDS])
def test_handler_guard_fails_when_a_forbidden_token_is_inserted(token):
    handler = diagnostics_handler_text(read("src/network/portal_routes.cpp"))
    assert_diagnostics_handler_reads_only_the_slot(handler)
    mutated = handler.replace(HANDLER_ANCHOR, f"{HANDLER_ANCHOR}\n    // {token}", 1)
    assert mutated != handler
    with pytest.raises(AssertionError, match=re.escape(f"portal_handle_diagnostics mentions {token}")):
        assert_diagnostics_handler_reads_only_the_slot(mutated)


@pytest.mark.parametrize("token", ["lv_", "Preferences", "weight_sensor"])
def test_diagnostics_route_test_fails_when_the_handler_file_is_mutated(mutate, token):
    mutate("src/network/portal_routes.cpp", insert_after(HANDLER_ANCHOR, f"\n    // {token}"))
    with pytest.raises(AssertionError, match=re.escape(f"portal_handle_diagnostics mentions {token}")):
        test_diagnostics_route_is_wired_through_its_own_slot()


@pytest.mark.parametrize("module", ["src/network/portal_routes.cpp", "src/network/portal_routes.h",
                                    "src/network/update_server.cpp", "src/network/portal_command_gate.cpp",
                                    "src/network/portal_command_gate.h"])
def test_gather_guard_fails_when_the_http_side_calls_gather(mutate, module):
    mutate(module, lambda text: text + "\n// gather_diagnostics();\n")
    with pytest.raises(AssertionError, match=re.escape(f"{module} mentions gather_diagnostics")):
        test_diagnostics_gather_runs_on_the_ui_task_only()


def test_gather_guard_fails_when_gather_runs_before_the_web_gate(mutate):
    mutate("src/ui/controllers/portal_ui_controller.cpp",
           insert_after("void PortalUIController::update() {", "\n    portal_command_gate.publish_diagnostics(gather_diagnostics());"))
    with pytest.raises(AssertionError):
        test_diagnostics_route_is_wired_through_its_own_slot()


@pytest.mark.parametrize("module", ["src/network/portal_routes.cpp", "src/network/portal_routes.h",
                                    "src/network/portal_command_gate.cpp", "src/network/portal_command_gate.h"])
def test_http_task_guard_covers_the_diagnostics_files(mutate, module):
    mutate(module, lambda text: text + "\n// LittleFS\n")
    with pytest.raises(AssertionError, match=re.escape(f"{module} (HTTP task) mentions LittleFS")):
        test_http_task_code_stays_off_the_ui_side()


def test_http_task_guard_fails_on_a_forbidden_include(mutate):
    mutate("src/network/portal_command_gate.cpp", lambda text: '#include "lvgl.h"\n' + text)
    with pytest.raises(AssertionError, match=re.escape("portal_command_gate.cpp (HTTP task) includes lvgl.h")):
        test_http_task_code_stays_off_the_ui_side()


def test_route_count_guard_fails_on_a_fourteenth_route(mutate):
    anchor = '        {"/api/diagnostics", HTTP_GET, diagnostics_trampoline},\n'
    mutate("src/network/update_server.cpp", insert_after(anchor, '        {"/api/extra", HTTP_GET, status_trampoline},\n'))
    with pytest.raises(AssertionError, match="14 registered routes"):
        test_diagnostics_route_slot_margin_and_document_numbers()


def test_margin_guard_fails_when_fewer_than_three_slots_are_free(mutate):
    mutate("src/config/update.h", lambda text: text.replace("#define UPDATE_HTTP_MAX_URI_HANDLERS 16",
                                                           "#define UPDATE_HTTP_MAX_URI_HANDLERS 15", 1))
    with pytest.raises(AssertionError, match="fewer than 3 free handler slots"):
        test_diagnostics_route_slot_margin_and_document_numbers()


def test_document_guard_fails_on_a_wrong_age_limit(mutate):
    mutate("docs/PORTAL-API.md", lambda text: text.replace("15 s (`PORTAL_DIAGNOSTICS_STALE_MS`", "20 s (`PORTAL_DIAGNOSTICS_STALE_MS`", 1))
    with pytest.raises(AssertionError, match="15 s age limit"):
        test_diagnostics_route_slot_margin_and_document_numbers()


def test_document_guard_fails_on_a_wrong_republish_interval(mutate):
    mutate("docs/PORTAL-API.md", lambda text: text.replace("`PORTAL_DIAGNOSTICS_REFRESH_MS`, 2 s)", "`PORTAL_DIAGNOSTICS_REFRESH_MS`, 3 s)", 1))
    with pytest.raises(AssertionError, match="2 s republish"):
        test_diagnostics_route_slot_margin_and_document_numbers()

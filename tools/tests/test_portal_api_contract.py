"""Contract guard for the web panel API (chg014, docs/PORTAL-API.md).

Pure text checks, no build and no hardware. The shared vectors (portal_api_vectors.json) must agree with the
firmware sources they describe: the setting keys with the menu tables and UiSettings, the profile and setting
limits and defaults with the config headers, the error table with portal_api.cpp, and the document with both.
The vectors themselves are executed by test_native/test_portal_api.cpp and test_portal_guard.cpp.
"""

import json
import re
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
}
# Radio groups of the menu that the web API deliberately does not expose
RADIO_NOT_EXPOSED = {"BOARD_OVERRIDE"}
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

def test_setting_keys_are_exactly_the_eleven_menu_settings():
    settings = [f["key"] for f in VECTORS["fields"] if f["group"] == "settings"]
    assert len(settings) == 11
    assert sorted(settings) == sorted(SETTING_GETTER)
    assert [f["key"] for f in VECTORS["fields"] if f["group"] == "profile"] == PROFILE_KEYS
    assert [f["key"] for f in VECTORS["fields"] if f["group"] == "device"] == ["device_name"]
    assert len(set(FIELDS)) == len(VECTORS["fields"]) == 18
    assert VECTORS["fields"][-1]["key"] == "device_name"  # last: keeps the other indices stable


def test_ui_settings_getters_equal_the_portal_settings():
    header = read("src/system/ui_settings.h")
    klass = header[header.index("class UiSettings"):]
    getters = set(re.findall(r"^\s*(?:bool|int|float)\s+([a-z_0-9]+)\(\) const;", klass, re.M))
    assert getters == set(SETTING_GETTER.values()), "a UiSettings setting is missing in (or extra to) the web API"


def test_menu_tables_equal_the_portal_settings():
    table_h = read("src/system/menu_setting_table.h")
    table_cpp = read("src/system/menu_setting_table.cpp")
    toggles = enum_members(table_h, "ToggleSetting")
    sliders = enum_members(table_h, "SliderSetting")
    radios = enum_members(table_h, "RadioSetting")
    menu = {m for m in toggles + sliders + radios if m != "COUNT"} - RADIO_NOT_EXPOSED
    assert menu == set(SETTING_MENU_ENTRY.values())
    # The toggle rows read the same UiSettings getters the portal maps
    row_getters = set(re.findall(r"&UiSettings::([a-z_0-9]+),\s*&UiSettings::set_", table_cpp))
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
    assert macro(user, "USER_PROFILE_COUNT") == LIMITS["profile_count"] == 3
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
    assert {"brightness_normal", "brightness_screensaver", "purge_amount_g", "freshness_hours"} <= adjusted_keys
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


@pytest.mark.parametrize("module", ["src/network/portal_api.h", "src/network/portal_api.cpp",
                                    "src/network/portal_guard.h", "src/network/portal_guard.cpp"])
def test_portal_modules_stay_pure(module):
    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', read(module))
    for include in includes:
        for forbidden in ("Arduino", "Preferences", "lvgl", "lv_", "WiFi", "esp_", "freertos", "update_server",
                          "profile_controller", "grind_controller", "ui_manager", "nvs"):
            assert forbidden not in include, f"{module} must stay host-testable: includes {include}"
    assert "Preferences" not in read(module)


def test_portal_modules_are_in_the_native_build_and_registered():
    ini = read("platformio-native.ini")
    for source in ("network/portal_api.cpp", "network/portal_guard.cpp"):
        assert f"+<{source}>" in ini
    main = read("test_native/test_main.cpp")
    for name in ("run_portal_api_tests", "run_portal_guard_tests"):
        assert main.count(name) == 2

#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "config/grind_control.h"
#include "config/user.h"
#include "network/portal_api.h"
#include "portal_vectors.h"
#include "system/menu_setting_models.h"
#include "system/menu_setting_table.h"
#include "system/settings_store.h"
#include "system/ui_settings.h"

namespace {

const float kEps = 1e-4f;

// In-memory store with legacy defaults (an empty store reads the factory defaults)
class MapStore : public SettingsStore {
public:
    std::map<std::string, bool> bools;
    std::map<std::string, int32_t> ints;
    std::map<std::string, float> floats;
    static std::string k(const char* ns, const char* key) { return std::string(ns) + "/" + key; }

    bool get_bool(const char* ns, const char* key, bool d) override {
        auto it = bools.find(k(ns, key));
        return it == bools.end() ? d : it->second;
    }
    bool put_bool(const char* ns, const char* key, bool v) override { bools[k(ns, key)] = v; return true; }
    int32_t get_int(const char* ns, const char* key, int32_t d) override {
        auto it = ints.find(k(ns, key));
        return it == ints.end() ? d : it->second;
    }
    bool put_int(const char* ns, const char* key, int32_t v) override { ints[k(ns, key)] = v; return true; }
    float get_float(const char* ns, const char* key, float d) override {
        auto it = floats.find(k(ns, key));
        return it == floats.end() ? d : it->second;
    }
    bool put_float(const char* ns, const char* key, float v) override { floats[k(ns, key)] = v; return true; }
};

PortalError error_from_code(const std::string& code) {
    for (size_t i = 0; i < static_cast<size_t>(PortalError::COUNT); i++) {
        if (code == portal_error_info(static_cast<PortalError>(i)).code) return static_cast<PortalError>(i);
    }
    TEST_FAIL_MESSAGE(code.c_str());
    return PortalError::NONE;
}

int index_of(const char* key) {
    const int i = portal_field_find(key, strlen(key));
    if (i < 0) TEST_FAIL_MESSAGE(key);
    return i;
}

// The vector's value as a normalised float: bool 0/1, enum choice index, number as is
float expected_value(size_t field, const PJson& v) {
    const PortalField& f = portal_field(field);
    switch (v.kind) {
        case PJson::BOOL: return v.b ? 1.0f : 0.0f;
        case PJson::NUM: return static_cast<float>(v.n);
        case PJson::STR:
            for (uint8_t i = 0; i < f.choice_count; i++) if (v.s == f.choices[i]) return static_cast<float>(i);
            break;
        default: break;
    }
    TEST_FAIL_MESSAGE(f.key);
    return 0;
}

const char* type_name(PortalFieldType t) {
    switch (t) {
        case PortalFieldType::BOOL: return "bool";
        case PortalFieldType::INT: return "int";
        case PortalFieldType::NUMBER: return "number";
        case PortalFieldType::ENUM: return "enum";
    }
    return "";
}

// Body as an exact, non-NUL-terminated buffer (like the HTTP layer hands it over)
PortalError parse_patch_text(const std::string& body, PortalPatch* out) {
    std::vector<char> buf(body.begin(), body.end());
    return portal_parse_patch(buf.data(), buf.size(), out);
}

PortalError parse_command_text(const std::string& body, PortalCommandRequest* out) {
    std::vector<char> buf(body.begin(), body.end());
    return portal_parse_command(buf.data(), buf.size(), out);
}

}  // namespace

static void test_error_table_matches_vectors() {
    const PJson& errors = portal_vectors().at("errors");
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(PortalError::COUNT) - 1, errors.a.size());
    for (size_t i = 0; i < errors.a.size(); i++) {
        const PortalErrorInfo& info = portal_error_info(static_cast<PortalError>(i + 1));
        TEST_ASSERT_EQUAL_STRING(errors.a[i].at("error").s.c_str(), info.code);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(errors.a[i].at("status").n), info.status);
        TEST_ASSERT_TRUE(strlen(info.reason) > 0);
    }
    TEST_ASSERT_EQUAL_INT(200, portal_error_info(PortalError::NONE).status);
    TEST_ASSERT_EQUAL_INT(200, portal_error_info(PortalError::COUNT).status);  // out of range maps to NONE
}

static void test_field_table_matches_vectors() {
    const PJson& fields = portal_vectors().at("fields");
    TEST_ASSERT_EQUAL_UINT32(PORTAL_FIELD_COUNT, fields.a.size());
    const PortalValues defaults = portal_default_values();
    for (size_t i = 0; i < fields.a.size(); i++) {
        const PJson& v = fields.a[i];
        const PortalField& f = portal_field(i);
        TEST_ASSERT_EQUAL_STRING(v.at("key").s.c_str(), f.key);
        TEST_ASSERT_EQUAL_STRING(v.at("type").s.c_str(), type_name(f.type));
        TEST_ASSERT_EQUAL_STRING(v.at("group").s.c_str(), f.is_profile ? "profile" : "settings");
        TEST_ASSERT_EQUAL_STRING(v.find("unit") ? v.at("unit").s.c_str() : "", f.unit);
        if (f.type == PortalFieldType::INT || f.type == PortalFieldType::NUMBER) {
            TEST_ASSERT_FLOAT_WITHIN(kEps, static_cast<float>(v.at("min").n), f.min);
            TEST_ASSERT_FLOAT_WITHIN(kEps, static_cast<float>(v.at("max").n), f.max);
            TEST_ASSERT_FLOAT_WITHIN(kEps, v.find("step") ? static_cast<float>(v.at("step").n) : 0.0f, f.step);
        }
        if (f.type == PortalFieldType::ENUM) {
            const PJson& choices = v.at("choices");
            TEST_ASSERT_EQUAL_UINT32(f.choice_count, choices.a.size());
            for (size_t c = 0; c < choices.a.size(); c++) TEST_ASSERT_EQUAL_STRING(choices.a[c].s.c_str(), f.choices[c]);
        }
        TEST_ASSERT_FLOAT_WITHIN(kEps, expected_value(i, v.at("default")), f.def);
        TEST_ASSERT_FLOAT_WITHIN(kEps, f.def, defaults.v[i]);
    }
    const PJson& names = portal_vectors().at("profile_names");
    for (size_t i = 0; i < names.a.size(); i++) TEST_ASSERT_EQUAL_STRING(names.a[i].s.c_str(), portal_profile_name(static_cast<int>(i)));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", portal_profile_name(3));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", portal_profile_name(-1));
    TEST_ASSERT_EQUAL_INT(1, defaults.current_profile);
}

static void test_field_find() {
    TEST_ASSERT_EQUAL_INT(PF_LOGGING, portal_field_find("logging", 7));
    TEST_ASSERT_EQUAL_INT(PF_CUSTOM_TIME_S, portal_field_find("custom_time_s", 13));
    TEST_ASSERT_EQUAL_INT(-1, portal_field_find("logging", 6));      // prefix
    TEST_ASSERT_EQUAL_INT(-1, portal_field_find("loggingx", 8));
    TEST_ASSERT_EQUAL_INT(-1, portal_field_find("Logging", 7));
    TEST_ASSERT_EQUAL_INT(-1, portal_field_find("", 0));
    TEST_ASSERT_EQUAL_INT(-1, portal_field_find(nullptr, 0));
}

static void test_patch_vectors() {
    const PJson& cases = portal_vectors().at("patch_cases");
    TEST_ASSERT_TRUE(cases.a.size() > 100);
    for (const PJson& c : cases.a) {
        const std::string& name = c.at("name").s;
        PortalPatch patch;
        patch.present = 0xFFFFFFFFu;  // must be reset by the parser
        const PortalError err = parse_patch_text(c.at("body").s, &patch);
        if (!c.at("error").is_null()) {
            TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(error_from_code(c.at("error").s)), static_cast<int>(err), name.c_str());
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, patch.present, name.c_str());  // all or nothing
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, patch.adjusted, name.c_str());
            continue;
        }
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(PortalError::NONE), static_cast<int>(err), name.c_str());
        uint32_t want_present = 0, want_adjusted = 0;
        for (const auto& kv : c.at("values").o) {
            const int idx = index_of(kv.first.c_str());
            want_present |= 1u << idx;
            TEST_ASSERT_TRUE_MESSAGE(patch.has(idx), name.c_str());
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, expected_value(idx, kv.second), patch.v[idx], name.c_str());
        }
        for (const PJson& k : c.at("adjusted").a) want_adjusted |= 1u << index_of(k.s.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_present, patch.present, name.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_adjusted, patch.adjusted, name.c_str());
    }
}

static void test_command_vectors() {
    const PJson& cases = portal_vectors().at("command_cases");
    TEST_ASSERT_TRUE(cases.a.size() > 30);
    for (const PJson& c : cases.a) {
        const std::string& name = c.at("name").s;
        PortalCommandRequest req;
        req.cmd = PortalCommand::MOTOR_TEST;
        req.profile = 7;
        const PortalError err = parse_command_text(c.at("body").s, &req);
        if (!c.at("error").is_null()) {
            TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(error_from_code(c.at("error").s)), static_cast<int>(err), name.c_str());
            TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(PortalCommand::STOP), static_cast<int>(req.cmd), name.c_str());  // reset
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, req.profile, name.c_str());
            continue;
        }
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(PortalError::NONE), static_cast<int>(err), name.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.at("cmd").s.c_str(), portal_command_name(req.cmd), name.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(c.at("profile").is_null() ? -1 : static_cast<int>(c.at("profile").n), req.profile, name.c_str());
    }
}

static void test_parse_null_and_empty_inputs() {
    PortalPatch patch;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::EMPTY_BODY), static_cast<int>(portal_parse_patch(nullptr, 0, &patch)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::EMPTY_BODY), static_cast<int>(portal_parse_patch("{}", 0, &patch)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::EMPTY_PATCH), static_cast<int>(portal_parse_patch("{}", 2, nullptr)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch("{\"logging\":true}", 16, nullptr)));
    PortalCommandRequest req;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::EMPTY_BODY), static_cast<int>(portal_parse_command(nullptr, 0, &req)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_command("{\"cmd\":\"stop\"}", 14, nullptr)));
}

static void test_parse_ignores_bytes_past_len() {
    // The buffer continues with a valid-looking tail; only len bytes count
    const char body[] = "{\"logging\":true}{\"swipe\":true}";
    PortalPatch patch;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, 16, &patch)));
    TEST_ASSERT_TRUE(patch.has(PF_LOGGING));
    TEST_ASSERT_FALSE(patch.has(PF_SWIPE));
    // A NUL inside the body is never valid
    const char with_nul[] = {'{', '"', 'l', 'o', 'g', 'g', 'i', 'n', 'g', '"', ':', 't', 'r', 'u', 'e', '\0', '}'};
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MALFORMED_JSON), static_cast<int>(portal_parse_patch(with_nul, sizeof(with_nul), &patch)));
    // Non-ASCII bytes in a string are refused too
    const char utf8[] = "{\"grind_mode\":\"w\xC3\xA9\"}";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MALFORMED_JSON), static_cast<int>(portal_parse_patch(utf8, sizeof(utf8) - 1, &patch)));
}

static void test_brightness_clamps_exactly_like_the_menu_slider() {
    // Every whole input from far below to far above the slider: result == menu clamp of the slider position
    for (int in = -300; in <= 400; in++) {
        const SliderSettingDef& def = slider_setting_def(SliderSetting::BRIGHTNESS_NORMAL);
        int pos = in < def.min ? def.min : (in > def.max ? def.max : in);
        pos = def.clamp_pos(pos);
        char body[64];
        snprintf(body, sizeof(body), "{\"brightness_normal\":%d}", in);
        PortalPatch patch;
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, strlen(body), &patch)));
        TEST_ASSERT_EQUAL_INT_MESSAGE(pos, static_cast<int>(patch.v[PF_BRIGHTNESS_NORMAL]), body);
    }
    for (int in = -300; in <= 400; in += 7) {
        const SliderSettingDef& def = slider_setting_def(SliderSetting::BRIGHTNESS_SCREENSAVER);
        int pos = in < def.min ? def.min : (in > def.max ? def.max : in);
        pos = def.clamp_pos(pos);
        char body[64];
        snprintf(body, sizeof(body), "{\"brightness_screensaver\":%d}", in);
        PortalPatch patch;
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, strlen(body), &patch)));
        TEST_ASSERT_EQUAL_INT_MESSAGE(pos, static_cast<int>(patch.v[PF_BRIGHTNESS_SCREENSAVER]), body);
    }
}

static void test_purge_amount_matches_every_slider_position() {
    const SliderSettingDef& def = slider_setting_def(SliderSetting::PURGE_AMOUNT);
    for (int pos = def.min; pos <= def.max; pos++) {
        char body[64];
        snprintf(body, sizeof(body), "{\"purge_amount_g\":%.1f}", pos / 10.0);
        PortalPatch patch;
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, strlen(body), &patch)));
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, purge_amount_from_slider(pos), patch.v[PF_PURGE_AMOUNT_G], body);
        TEST_ASSERT_FALSE_MESSAGE((patch.adjusted >> PF_PURGE_AMOUNT_G) & 1u, body);
    }
    // Hundredths: the same slider position as the menu would pick
    for (int hundredths = -20; hundredths <= 400; hundredths++) {
        char body[64];
        snprintf(body, sizeof(body), "{\"purge_amount_g\":%.2f}", hundredths / 100.0);
        PortalPatch patch;
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, strlen(body), &patch)));
        const float expected = purge_amount_from_slider(purge_slider_from_amount(static_cast<float>(hundredths / 100.0)));
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, expected, patch.v[PF_PURGE_AMOUNT_G], body);
        TEST_ASSERT_TRUE(patch.v[PF_PURGE_AMOUNT_G] >= GRIND_PURGE_AMOUNT_MIN_G - kEps);
        TEST_ASSERT_TRUE(patch.v[PF_PURGE_AMOUNT_G] <= GRIND_PURGE_AMOUNT_MAX_G + kEps);
    }
}

static void test_freshness_lands_on_a_menu_step() {
    for (int tenths = -10; tenths <= 600; tenths++) {
        char body[64];
        snprintf(body, sizeof(body), "{\"freshness_hours\":%.1f}", tenths / 10.0);
        PortalPatch patch;
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_parse_patch(body, strlen(body), &patch)));
        const float result = patch.v[PF_FRESHNESS_HOURS];
        bool is_step = false;
        for (int i = 0; i < kFreshnessStepCount; i++) is_step = is_step || fabsf(result - kFreshnessStepsHours[i]) < kEps;
        TEST_ASSERT_TRUE_MESSAGE(is_step, body);
        // The menu would load the stored value back to the same step
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, result, freshness_hours_from_index(freshness_index_from_hours(result)), body);
    }
}

static void test_profile_limits_follow_the_firmware_constants() {
    TEST_ASSERT_EQUAL_FLOAT(USER_MIN_TARGET_WEIGHT_G, portal_field(PF_SINGLE_WEIGHT_G).min);
    TEST_ASSERT_EQUAL_FLOAT(USER_MAX_TARGET_WEIGHT_G, portal_field(PF_CUSTOM_WEIGHT_G).max);
    TEST_ASSERT_EQUAL_FLOAT(USER_MIN_TARGET_TIME_S, portal_field(PF_DOUBLE_TIME_S).min);
    TEST_ASSERT_EQUAL_FLOAT(USER_MAX_TARGET_TIME_S, portal_field(PF_CUSTOM_TIME_S).max);
    TEST_ASSERT_EQUAL_FLOAT(USER_SINGLE_ESPRESSO_WEIGHT_G, portal_field(PF_SINGLE_WEIGHT_G).def);
    TEST_ASSERT_EQUAL_FLOAT(USER_DOUBLE_ESPRESSO_TIME_S, portal_field(PF_DOUBLE_TIME_S).def);
    TEST_ASSERT_EQUAL_FLOAT(USER_CUSTOM_PROFILE_WEIGHT_G, portal_field(PF_CUSTOM_WEIGHT_G).def);
    // The accepted range is [min, max] on the stored float, like ProfileController::is_*_valid
    PortalPatch patch;
    const char* just_below = "{\"single_weight_g\":4.999}";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::OUT_OF_RANGE), static_cast<int>(portal_parse_patch(just_below, strlen(just_below), &patch)));
    const char* just_above = "{\"single_time_s\":25.001}";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::OUT_OF_RANGE), static_cast<int>(portal_parse_patch(just_above, strlen(just_above), &patch)));
}

static void test_defaults_equal_an_empty_ui_settings_store() {
    MapStore store;
    UiSettings settings(store);
    PortalValues loaded = portal_default_values();
    for (size_t i = 0; i < PORTAL_SETTING_COUNT; i++) loaded.v[i] = -1.0f;
    portal_load_settings(settings, &loaded);
    const PortalValues defaults = portal_default_values();
    for (size_t i = 0; i < PORTAL_SETTING_COUNT; i++) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, defaults.v[i], loaded.v[i], portal_field(i).key);
    }
    // Profile targets are not part of UiSettings and stay untouched
    TEST_ASSERT_EQUAL_FLOAT(defaults.v[PF_SINGLE_WEIGHT_G], loaded.v[PF_SINGLE_WEIGHT_G]);
    portal_load_settings(settings, nullptr);  // must not crash
}

static void test_load_settings_converts_like_the_menu() {
    MapStore store;
    UiSettings settings(store);
    settings.set_ble_startup(false);
    settings.set_logging_enabled(true);
    settings.set_swipe_enabled(true);
    settings.set_auto_start(true);
    settings.set_auto_return(true);
    settings.set_brightness_normal(0.57f);
    settings.set_brightness_screensaver(0.0f);   // below the floor
    settings.set_purge_mode(0);
    settings.set_purge_amount_g(1.7f);
    settings.set_freshness_hours(12.0f);
    store.ints["grinder/grind_mode"] = 1;
    PortalValues v = portal_default_values();
    portal_load_settings(settings, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.v[PF_BLE_STARTUP]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_LOGGING]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_SWIPE]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_AUTO_START]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_AUTO_RETURN]);
    TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(brightness_percent_from_fraction(0.57f)), v.v[PF_BRIGHTNESS_NORMAL]);
    TEST_ASSERT_EQUAL_FLOAT(15.0f, v.v[PF_BRIGHTNESS_SCREENSAVER]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.v[PF_PURGE_MODE]);
    TEST_ASSERT_FLOAT_WITHIN(kEps, 1.7f, v.v[PF_PURGE_AMOUNT_G]);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, v.v[PF_FRESHNESS_HOURS]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_GRIND_MODE]);
    // Odd stored values: purge mode other than 0 reads as Remove, freshness off-step as the menu default (8 h)
    settings.set_purge_mode(7);
    settings.set_freshness_hours(5.5f);
    store.ints["grinder/grind_mode"] = 9;
    portal_load_settings(settings, &v);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.v[PF_PURGE_MODE]);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, v.v[PF_FRESHNESS_HOURS]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.v[PF_GRIND_MODE]);
}

static void test_load_settings_off_grid_purge_amount() {
    MapStore store;
    UiSettings settings(store);
    PortalValues v;
    settings.set_purge_amount_g(1.74f);
    portal_load_settings(settings, &v);
    TEST_ASSERT_FLOAT_WITHIN(kEps, 1.7f, v.v[PF_PURGE_AMOUNT_G]);
    settings.set_purge_amount_g(5.0f);
    portal_load_settings(settings, &v);
    TEST_ASSERT_FLOAT_WITHIN(kEps, 2.5f, v.v[PF_PURGE_AMOUNT_G]);
}

static void test_brightness_store_round_trip_is_within_one_percent() {
    // What a PATCH applies (percent -> fraction -> store) reads back through the menu's own conversion
    // ((int)(fraction * 100) truncates), so some percentages come back one lower. The API reports the stored
    // reading (PORTAL-API.md "Brightness read-back"); this pins the size of that quirk.
    int off_by_one = 0;
    for (int pct = 15; pct <= 100; pct++) {
        MapStore store;
        UiSettings settings(store);
        settings.set_brightness_normal(brightness_fraction_from_percent(pct));
        PortalValues v;
        portal_load_settings(settings, &v);
        const int back = static_cast<int>(v.v[PF_BRIGHTNESS_NORMAL]);
        TEST_ASSERT_TRUE_MESSAGE(back == pct || back == pct - 1, "round trip");
        if (back != pct) off_by_one++;
    }
    TEST_ASSERT_TRUE(off_by_one <= 5);  // two today (53 and 59)
    TEST_ASSERT_EQUAL_INT(100, [] {  // the extremes are exact
        MapStore store;
        UiSettings settings(store);
        settings.set_brightness_normal(brightness_fraction_from_percent(100));
        PortalValues v;
        portal_load_settings(settings, &v);
        return static_cast<int>(v.v[PF_BRIGHTNESS_NORMAL]);
    }());
}

static bool encoded_is_json(const char* text, PJson* out) {
    *out = portal_json_parse(text);
    return out->kind == PJson::OBJ;
}

static void test_encode_error() {
    char buf[256];
    TEST_ASSERT_TRUE(portal_encode_error(PortalError::BAD_HOST, buf, sizeof(buf)));
    PJson j;
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    TEST_ASSERT_EQUAL_STRING("bad_host", j.at("error").s.c_str());
    TEST_ASSERT_EQUAL_STRING(portal_error_info(PortalError::BAD_HOST).reason, j.at("reason").s.c_str());
    TEST_ASSERT_TRUE(portal_encode_error(PortalError::NONE, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", buf);
    // Every error encodes into a modest buffer as valid JSON
    for (size_t i = 1; i < static_cast<size_t>(PortalError::COUNT); i++) {
        TEST_ASSERT_TRUE(portal_encode_error(static_cast<PortalError>(i), buf, sizeof(buf)));
        TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
        TEST_ASSERT_EQUAL_STRING(portal_error_info(static_cast<PortalError>(i)).code, j.at("error").s.c_str());
    }
}

static void test_encode_values_round_trips_the_defaults() {
    char buf[1024];
    PortalValues values = portal_default_values();
    values.current_profile = 2;
    TEST_ASSERT_TRUE(portal_encode_values(values, buf, sizeof(buf)));
    PJson j;
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(j.at("current_profile").n));
    const PJson& v = j.at("values");
    TEST_ASSERT_EQUAL_UINT32(PORTAL_FIELD_COUNT, v.o.size());
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(portal_field(i).key, v.o[i].first.c_str());  // table order
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, values.v[i], expected_value(i, v.o[i].second), portal_field(i).key);
    }
    // Typed like the contract says: booleans and strings, not 0/1
    TEST_ASSERT_EQUAL(static_cast<int>(PJson::BOOL), static_cast<int>(v.at("ble_startup").kind));
    TEST_ASSERT_EQUAL(static_cast<int>(PJson::STR), static_cast<int>(v.at("purge_mode").kind));
    TEST_ASSERT_EQUAL(static_cast<int>(PJson::NUM), static_cast<int>(v.at("brightness_normal").kind));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"single_weight_g\":9.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"custom_weight_g\":21.5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"brightness_normal\":100"));
}

static void test_encode_values_enum_and_bool_out_of_range_are_clamped() {
    char buf[1024];
    PortalValues values = portal_default_values();
    values.v[PF_PURGE_MODE] = 7.0f;
    values.v[PF_GRIND_MODE] = -3.0f;
    TEST_ASSERT_TRUE(portal_encode_values(values, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"purge_mode\":\"remove\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"grind_mode\":\"weight\""));
}

static void test_encode_patch_reply_lists_adjusted_keys() {
    char buf[1024];
    PortalValues applied = portal_default_values();
    const uint32_t mask = (1u << PF_BRIGHTNESS_NORMAL) | (1u << PF_FRESHNESS_HOURS);
    TEST_ASSERT_TRUE(portal_encode_patch_reply(applied, mask, buf, sizeof(buf)));
    PJson j;
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    TEST_ASSERT_TRUE(j.at("ok").b);
    TEST_ASSERT_EQUAL_UINT32(PORTAL_FIELD_COUNT, j.at("values").o.size());
    const PJson& adjusted = j.at("adjusted");
    TEST_ASSERT_EQUAL_UINT32(2, adjusted.a.size());
    TEST_ASSERT_EQUAL_STRING("brightness_normal", adjusted.a[0].s.c_str());
    TEST_ASSERT_EQUAL_STRING("freshness_hours", adjusted.a[1].s.c_str());
    TEST_ASSERT_TRUE(portal_encode_patch_reply(applied, 0, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"adjusted\":[]"));
}

static void test_encode_schema_matches_the_field_table() {
    static char buf[4096];
    TEST_ASSERT_TRUE(portal_encode_schema(buf, sizeof(buf)));
    PJson j;
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    const PJson& fields = j.at("fields");
    TEST_ASSERT_EQUAL_UINT32(PORTAL_FIELD_COUNT, fields.a.size());
    const PJson& vec_fields = portal_vectors().at("fields");
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        // The encoded schema carries exactly what the vectors list (key, type, group, unit, limits, choices, default)
        const PJson& got = fields.a[i];
        const PJson& want = vec_fields.a[i];
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.o.size(), got.o.size(), portal_field(i).key);
        for (const auto& kv : want.o) {
            const PJson& g = got.at(kv.first.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(kv.second.kind), static_cast<int>(g.kind), kv.first.c_str());
            if (kv.second.kind == PJson::STR) TEST_ASSERT_EQUAL_STRING(kv.second.s.c_str(), g.s.c_str());
            if (kv.second.kind == PJson::NUM) TEST_ASSERT_FLOAT_WITHIN(kEps, static_cast<float>(kv.second.n), static_cast<float>(g.n));
            if (kv.second.kind == PJson::BOOL) TEST_ASSERT_EQUAL(kv.second.b, g.b);
        }
    }
    const PJson& profiles = j.at("profiles");
    TEST_ASSERT_EQUAL_UINT32(3, profiles.a.size());
    TEST_ASSERT_EQUAL_STRING("DOUBLE", profiles.a[1].at("name").s.c_str());
    TEST_ASSERT_EQUAL_STRING("double_weight_g", profiles.a[1].at("weight_key").s.c_str());
    TEST_ASSERT_EQUAL_STRING("custom_time_s", profiles.a[2].at("time_key").s.c_str());
}

static void test_encode_command_reply() {
    char buf[128];
    PortalCommandRequest req;
    req.cmd = PortalCommand::START;
    req.profile = 2;
    TEST_ASSERT_TRUE(portal_encode_command_reply(req, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"cmd\":\"start\",\"profile\":2,\"lead_in_ms\":3000}", buf);
    req.cmd = PortalCommand::STOP;
    req.profile = -1;
    TEST_ASSERT_TRUE(portal_encode_command_reply(req, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"cmd\":\"stop\"}", buf);
    req.cmd = PortalCommand::MOTOR_TEST;
    TEST_ASSERT_TRUE(portal_encode_command_reply(req, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"cmd\":\"motor_test\"}", buf);
}

static void test_encode_status() {
    char buf[512];
    PortalStatus s;
    s.ui_state = "grinding";
    s.grind_phase = "predictive";
    s.busy = true;
    s.weight_g = 12.345f;
    s.motor_available = true;
    s.uptime_s = 4000000000u;
    s.build = 731;
    TEST_ASSERT_TRUE(portal_encode_status(s, buf, sizeof(buf)));
    PJson j;
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    TEST_ASSERT_EQUAL_STRING("grinding", j.at("ui_state").s.c_str());
    TEST_ASSERT_EQUAL_STRING("predictive", j.at("grind_phase").s.c_str());
    TEST_ASSERT_TRUE(j.at("busy").b);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.345f, static_cast<float>(j.at("weight_g").n));
    TEST_ASSERT_TRUE(j.at("start_block").is_null());
    TEST_ASSERT_TRUE(j.at("motor_test_block").is_null());
    TEST_ASSERT_TRUE(j.at("uptime_s").n == 4000000000.0);  // Unity double support is off
    TEST_ASSERT_EQUAL_INT(731, static_cast<int>(j.at("build").n));
    TEST_ASSERT_TRUE(j.at("web_interface").b);

    s.weight_g = NAN;
    s.build = -1;
    s.start_block = PortalError::LOAD_CELL_NO_DATA;
    s.motor_test_block = PortalError::MOTOR_FAULT;
    s.ui_state = "we\"ird";
    TEST_ASSERT_TRUE(portal_encode_status(s, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
    TEST_ASSERT_TRUE(j.at("weight_g").is_null());
    TEST_ASSERT_TRUE(j.at("build").is_null());
    TEST_ASSERT_EQUAL_STRING("load_cell_no_data", j.at("start_block").s.c_str());
    TEST_ASSERT_EQUAL_STRING("motor_fault", j.at("motor_test_block").s.c_str());
    TEST_ASSERT_EQUAL_STRING("we\"ird", j.at("ui_state").s.c_str());  // escaped on the wire
    s.weight_g = INFINITY;
    s.ui_state = nullptr;
    TEST_ASSERT_TRUE(portal_encode_status(s, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(encoded_is_json(buf, &j));
}

static void test_encode_escapes_control_characters() {
    char buf[256];
    PortalStatus s;
    s.ui_state = "a\x01" "b\n\\";
    TEST_ASSERT_TRUE(portal_encode_status(s, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "a\\u0001b"));
    PJson j = portal_json_parse(buf);
    TEST_ASSERT_EQUAL_STRING("a\x01" "b\n\\", j.at("ui_state").s.c_str());
}

static void test_encoders_truncate_safely() {
    char buf[64];
    PortalValues values = portal_default_values();
    for (size_t cap = 1; cap < sizeof(buf); cap += 7) {
        memset(buf, 'x', sizeof(buf));
        TEST_ASSERT_FALSE(portal_encode_values(values, buf, cap));
        TEST_ASSERT_EQUAL_UINT32(cap - 1, strlen(buf));  // cut at the buffer, NUL-terminated
        memset(buf, 'x', sizeof(buf));
        TEST_ASSERT_FALSE(portal_encode_schema(buf, cap));
        TEST_ASSERT_EQUAL_UINT32(cap - 1, strlen(buf));
    }
    TEST_ASSERT_FALSE(portal_encode_error(PortalError::BUSY, buf, 0));  // no room at all, no write
    TEST_ASSERT_FALSE(portal_encode_error(PortalError::BUSY, nullptr, 0));
    char tiny[1] = {'x'};
    TEST_ASSERT_FALSE(portal_encode_error(PortalError::BUSY, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
}

static void test_encoded_sizes_fit_a_reasonable_buffer() {
    // Documented buffer sizes of PORTAL-API.md: values/patch reply 1 KB, schema 4 KB
    static char buf[4096];
    PortalValues values = portal_default_values();
    TEST_ASSERT_TRUE(portal_encode_values(values, buf, 1024));
    values.v[PF_SINGLE_WEIGHT_G] = 1000.0f;
    TEST_ASSERT_TRUE(portal_encode_patch_reply(values, (1u << PORTAL_FIELD_COUNT) - 1, buf, 1024));
    TEST_ASSERT_TRUE(portal_encode_schema(buf, 4096));
    TEST_ASSERT_TRUE(strlen(buf) < 4096);
}

void run_portal_api_tests() {
    RUN_TEST(test_error_table_matches_vectors);
    RUN_TEST(test_field_table_matches_vectors);
    RUN_TEST(test_field_find);
    RUN_TEST(test_patch_vectors);
    RUN_TEST(test_command_vectors);
    RUN_TEST(test_parse_null_and_empty_inputs);
    RUN_TEST(test_parse_ignores_bytes_past_len);
    RUN_TEST(test_brightness_clamps_exactly_like_the_menu_slider);
    RUN_TEST(test_purge_amount_matches_every_slider_position);
    RUN_TEST(test_freshness_lands_on_a_menu_step);
    RUN_TEST(test_profile_limits_follow_the_firmware_constants);
    RUN_TEST(test_defaults_equal_an_empty_ui_settings_store);
    RUN_TEST(test_load_settings_converts_like_the_menu);
    RUN_TEST(test_load_settings_off_grid_purge_amount);
    RUN_TEST(test_brightness_store_round_trip_is_within_one_percent);
    RUN_TEST(test_encode_error);
    RUN_TEST(test_encode_values_round_trips_the_defaults);
    RUN_TEST(test_encode_values_enum_and_bool_out_of_range_are_clamped);
    RUN_TEST(test_encode_patch_reply_lists_adjusted_keys);
    RUN_TEST(test_encode_schema_matches_the_field_table);
    RUN_TEST(test_encode_command_reply);
    RUN_TEST(test_encode_status);
    RUN_TEST(test_encode_escapes_control_characters);
    RUN_TEST(test_encoders_truncate_safely);
    RUN_TEST(test_encoded_sizes_fit_a_reasonable_buffer);
}

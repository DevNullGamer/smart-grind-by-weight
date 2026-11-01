#include "network/portal_api.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "config/grind_control.h"
#include "config/hardware.h"
#include "config/user.h"
#include "network/wifi_credentials.h"
#include "system/menu_setting_models.h"
#include "system/menu_setting_table.h"
#include "system/ui_settings.h"

//------------------------------------------------------------------------------
// Error table (indexed by PortalError; the order is the enum order)
//------------------------------------------------------------------------------
namespace {

const PortalErrorInfo kErrors[static_cast<size_t>(PortalError::COUNT)] = {
    {200, "ok", "ok"},
    {400, "empty_body", "The request body is empty."},
    {413, "body_too_large", "The request body is larger than the grinder accepts."},
    {400, "malformed_json", "The body is not a flat JSON object of simple values."},
    {400, "nested_value", "Nested objects and arrays are not accepted."},
    {400, "duplicate_key", "A key appears more than once."},
    {400, "unknown_key", "The body contains a key the grinder does not know."},
    {400, "unexpected_field", "A field is not allowed for this command."},
    {400, "missing_field", "A required field is missing."},
    {400, "empty_patch", "The body does not change anything."},
    {400, "wrong_type", "A value has the wrong JSON type."},
    {400, "invalid_value", "A value is not acceptable."},
    {400, "out_of_range", "A value is outside the allowed range."},
    {400, "unknown_command", "The command is not known."},
    {400, "invalid_device_name", "The device name must be 1-24 characters of a-z, 0-9 and hyphen, not starting or ending with a hyphen."},
    {403, "web_interface_off", "The web interface is switched off on the grinder."},
    {403, "bad_host", "The Host header is not a .local name or an IP address."},
    {403, "bad_origin", "The Origin header does not match the Host header."},
    {403, "missing_custom_header", "The X-Grinder: 1 header is required."},
    {415, "unsupported_media_type", "The body must be sent as application/json."},
    {409, "command_in_flight", "Another request is still being handled."},
    {409, "busy", "A grind, autotune or motor run is active."},
    {409, "not_ready", "The grinder screen is not in a state that allows this."},
    {409, "update_in_progress", "A firmware update is running."},
    {409, "motor_disabled", "The motor is disabled (board not confirmed)."},
    {409, "motor_fault", "The motor output fault is latched until reboot."},
    {409, "load_cell_not_connected", "The load cell is not connected."},
    {409, "load_cell_no_data", "The load cell delivers no data."},
    {409, "load_cell_rate_invalid", "The load cell sample rate is invalid."},
    {503, "ui_busy", "The grinder screen did not answer in time, try again."},
    {500, "store_failed", "The value could not be saved."},
};

//------------------------------------------------------------------------------
// Field table (indexed by PortalFieldIndex). Limits come from the same constants as the menu and
// ProfileController; tools/tests/test_portal_api_contract.py guards them against the sources.
//------------------------------------------------------------------------------
constexpr float kBrightnessNormalDefault = static_cast<float>(static_cast<int>(USER_SCREEN_BRIGHTNESS_NORMAL * 100.0f + 0.5f));
constexpr float kBrightnessDimmedDefault = static_cast<float>(static_cast<int>(USER_SCREEN_BRIGHTNESS_DIMMED * 100.0f + 0.5f));
constexpr float kFreshnessMin = 0.5f;   // first and last of kFreshnessStepsHours
constexpr float kFreshnessMax = 48.0f;

const PortalField kFields[PORTAL_FIELD_COUNT] = {
    {"ble_startup", PortalFieldType::BOOL, "", 0, 1, 0, 1, {nullptr, nullptr}, 0, false},
    {"logging", PortalFieldType::BOOL, "", 0, 1, 0, 0, {nullptr, nullptr}, 0, false},
    {"swipe", PortalFieldType::BOOL, "", 0, 1, 0, 0, {nullptr, nullptr}, 0, false},
    {"auto_start", PortalFieldType::BOOL, "", 0, 1, 0, 0, {nullptr, nullptr}, 0, false},
    {"auto_return", PortalFieldType::BOOL, "", 0, 1, 0, 0, {nullptr, nullptr}, 0, false},
    {"brightness_normal", PortalFieldType::INT, "%", HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT, 100, 1,
     kBrightnessNormalDefault, {nullptr, nullptr}, 0, false},
    {"brightness_screensaver", PortalFieldType::INT, "%", HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT, 100, 1,
     kBrightnessDimmedDefault, {nullptr, nullptr}, 0, false},
    {"purge_mode", PortalFieldType::ENUM, "", 0, 1, 0, static_cast<float>(GRIND_PURGE_MODE_DEFAULT),
     {"keep", "remove"}, 2, false},
    {"purge_amount_g", PortalFieldType::NUMBER, "g", GRIND_PURGE_AMOUNT_MIN_G, GRIND_PURGE_AMOUNT_MAX_G, 0.1f,
     GRIND_PURGE_AMOUNT_DEFAULT_G, {nullptr, nullptr}, 0, false},
    {"freshness_hours", PortalFieldType::NUMBER, "h", kFreshnessMin, kFreshnessMax, 0, GRIND_FRESHNESS_DEFAULT_HOURS,
     {nullptr, nullptr}, 0, false},
    {"grind_mode", PortalFieldType::ENUM, "", 0, 1, 0, 0, {"weight", "time"}, 2, false},
    {"single_weight_g", PortalFieldType::NUMBER, "g", USER_MIN_TARGET_WEIGHT_G, USER_MAX_TARGET_WEIGHT_G, 0,
     USER_SINGLE_ESPRESSO_WEIGHT_G, {nullptr, nullptr}, 0, true},
    {"single_time_s", PortalFieldType::NUMBER, "s", USER_MIN_TARGET_TIME_S, USER_MAX_TARGET_TIME_S, 0,
     USER_SINGLE_ESPRESSO_TIME_S, {nullptr, nullptr}, 0, true},
    {"double_weight_g", PortalFieldType::NUMBER, "g", USER_MIN_TARGET_WEIGHT_G, USER_MAX_TARGET_WEIGHT_G, 0,
     USER_DOUBLE_ESPRESSO_WEIGHT_G, {nullptr, nullptr}, 0, true},
    {"double_time_s", PortalFieldType::NUMBER, "s", USER_MIN_TARGET_TIME_S, USER_MAX_TARGET_TIME_S, 0,
     USER_DOUBLE_ESPRESSO_TIME_S, {nullptr, nullptr}, 0, true},
    {"custom_weight_g", PortalFieldType::NUMBER, "g", USER_MIN_TARGET_WEIGHT_G, USER_MAX_TARGET_WEIGHT_G, 0,
     USER_CUSTOM_PROFILE_WEIGHT_G, {nullptr, nullptr}, 0, true},
    {"custom_time_s", PortalFieldType::NUMBER, "s", USER_MIN_TARGET_TIME_S, USER_MAX_TARGET_TIME_S, 0,
     USER_CUSTOM_PROFILE_TIME_S, {nullptr, nullptr}, 0, true},
    {"device_name", PortalFieldType::STRING, "", 1, PORTAL_DEVICE_NAME_MAX_LEN, 0, 0, {nullptr, nullptr}, 0, false},
};

const char* const kProfileNames[PORTAL_PROFILE_COUNT] = {"SINGLE", "DOUBLE", "CUSTOM"};
const char* const kCommandNames[] = {"stop", "tare", "start", "motor_test"};
constexpr size_t kCommandCount = sizeof(kCommandNames) / sizeof(kCommandNames[0]);

// Largest magnitude any normaliser works with (keeps double -> float/int casts defined)
constexpr double kInputLimit = 1.0e6;

double clamp_input(double in) { return in < -kInputLimit ? -kInputLimit : (in > kInputLimit ? kInputLimit : in); }

//------------------------------------------------------------------------------
// Normalisers of the settings: the menu's own clamp/convert functions, so a remote value ends up exactly
// where the same value dragged on the screen would.
//------------------------------------------------------------------------------
float normalise_brightness(SliderSetting id, double in) {
    const SliderSettingDef& def = slider_setting_def(id);
    double r = round(clamp_input(in));
    if (r < def.min) r = def.min;
    if (r > def.max) r = def.max;
    return static_cast<float>(def.clamp_pos(static_cast<int>(r)));
}

float normalise_purge_amount(double in) {
    return purge_amount_from_slider(purge_slider_from_amount(static_cast<float>(clamp_input(in))));
}

float normalise_freshness(double in) {
    int best = 0;
    double best_diff = fabs(in - kFreshnessStepsHours[0]);
    for (int i = 1; i < kFreshnessStepCount; i++) {
        const double diff = fabs(in - kFreshnessStepsHours[i]);
        if (diff < best_diff) {  // strict: a tie keeps the lower step
            best = i;
            best_diff = diff;
        }
    }
    return freshness_hours_from_index(best);
}

float normalise_number(size_t index, double in) {
    switch (index) {
        case PF_BRIGHTNESS_NORMAL: return normalise_brightness(SliderSetting::BRIGHTNESS_NORMAL, in);
        case PF_BRIGHTNESS_SCREENSAVER: return normalise_brightness(SliderSetting::BRIGHTNESS_SCREENSAVER, in);
        case PF_PURGE_AMOUNT_G: return normalise_purge_amount(in);
        case PF_FRESHNESS_HOURS: return normalise_freshness(in);
        default: return static_cast<float>(in);  // profile targets: range-checked by the caller, stored as given
    }
}

//------------------------------------------------------------------------------
// Strict flat-object scanner: the shared front end of the PATCH and command parsers
//------------------------------------------------------------------------------
struct Scalar {
    enum Kind : uint8_t { BOOLEAN, NUMBER, STRING, NUL } kind = NUL;
    bool b = false;
    double num = 0.0;
    char str[PORTAL_STRING_MAX_LEN + 1] = {0};
    bool str_too_long = false;
};

struct MemberHandler {
    PortalError (*on_key)(void* ctx, const char* key, size_t key_len);  // unknown keys are refused here
    PortalError (*on_value)(void* ctx, const Scalar& value);            // type/range checks
    void* ctx;
};

struct Cursor {
    const char* p;
    const char* end;
    bool at_end() const { return p >= end; }
};

void skip_ws(Cursor& c) {
    while (!c.at_end() && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n')) c.p++;
}

// Cursor on the opening quote. Plain printable ASCII only: no escapes, control or non-ASCII bytes.
bool scan_string(Cursor& c, const char** start, size_t* length) {
    if (c.at_end() || *c.p != '"') return false;
    c.p++;
    *start = c.p;
    while (!c.at_end() && *c.p != '"') {
        const unsigned char ch = static_cast<unsigned char>(*c.p);
        if (ch < 0x20 || ch > 0x7E || ch == '\\') return false;
        c.p++;
    }
    if (c.at_end()) return false;
    *length = static_cast<size_t>(c.p - *start);
    c.p++;
    return true;
}

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

// JSON number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?  (no NaN, Infinity, hex, '+', '.5', '5.')
bool number_token_valid(const char* s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i >= n || !is_digit(s[i])) return false;
    if (s[i] == '0') {
        i++;
    } else {
        while (i < n && is_digit(s[i])) i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || !is_digit(s[i])) return false;
        while (i < n && is_digit(s[i])) i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= n || !is_digit(s[i])) return false;
        while (i < n && is_digit(s[i])) i++;
    }
    return i == n;
}

bool match_word(Cursor& c, const char* word) {
    const size_t n = strlen(word);
    if (static_cast<size_t>(c.end - c.p) < n || memcmp(c.p, word, n) != 0) return false;
    c.p += n;
    return true;
}

PortalError scan_value(Cursor& c, Scalar* out) {
    if (c.at_end()) return PortalError::MALFORMED_JSON;
    const char ch = *c.p;
    if (ch == '{' || ch == '[') return PortalError::NESTED_VALUE;
    if (ch == '"') {
        const char* s;
        size_t n;
        if (!scan_string(c, &s, &n)) return PortalError::MALFORMED_JSON;
        out->kind = Scalar::STRING;
        out->str_too_long = n > PORTAL_STRING_MAX_LEN;
        if (!out->str_too_long) {
            memcpy(out->str, s, n);
            out->str[n] = '\0';
        }
        return PortalError::NONE;
    }
    if (ch == 't') {
        if (!match_word(c, "true")) return PortalError::MALFORMED_JSON;
        out->kind = Scalar::BOOLEAN;
        out->b = true;
        return PortalError::NONE;
    }
    if (ch == 'f') {
        if (!match_word(c, "false")) return PortalError::MALFORMED_JSON;
        out->kind = Scalar::BOOLEAN;
        out->b = false;
        return PortalError::NONE;
    }
    if (ch == 'n') {
        if (!match_word(c, "null")) return PortalError::MALFORMED_JSON;
        out->kind = Scalar::NUL;
        return PortalError::NONE;
    }
    const char* start = c.p;
    while (!c.at_end() && (is_digit(*c.p) || *c.p == '-' || *c.p == '+' || *c.p == '.' || *c.p == 'e' || *c.p == 'E')) c.p++;
    const size_t n = static_cast<size_t>(c.p - start);
    if (n == 0 || n > 32 || !number_token_valid(start, n)) return PortalError::MALFORMED_JSON;
    char tmp[33];
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    out->kind = Scalar::NUMBER;
    out->num = strtod(tmp, nullptr);  // may overflow to +-inf: the handlers refuse non-finite numbers
    return PortalError::NONE;
}

// Per member, in document order: key (duplicate, then known) -> value (shape) -> type/range. First error wins.
PortalError scan_object(const char* body, size_t len, const MemberHandler& h, size_t* members) {
    *members = 0;
    if (!body || len == 0) return PortalError::EMPTY_BODY;
    if (len > PORTAL_BODY_MAX) return PortalError::BODY_TOO_LARGE;

    Cursor c{body, body + len};
    struct Seen { const char* key; size_t len; } seen[PORTAL_MAX_FIELDS];
    size_t count = 0;

    skip_ws(c);
    if (c.at_end() || *c.p != '{') return PortalError::MALFORMED_JSON;
    c.p++;
    skip_ws(c);
    if (!c.at_end() && *c.p == '}') {
        c.p++;
    } else {
        while (true) {
            const char* key;
            size_t key_len;
            if (!scan_string(c, &key, &key_len) || key_len == 0 || key_len > PORTAL_KEY_MAX_LEN) {
                return PortalError::MALFORMED_JSON;
            }
            for (size_t i = 0; i < count; i++) {
                if (seen[i].len == key_len && memcmp(seen[i].key, key, key_len) == 0) return PortalError::DUPLICATE_KEY;
            }
            if (count >= PORTAL_MAX_FIELDS) return PortalError::MALFORMED_JSON;
            seen[count++] = {key, key_len};

            skip_ws(c);
            if (c.at_end() || *c.p != ':') return PortalError::MALFORMED_JSON;
            c.p++;
            skip_ws(c);

            PortalError err = h.on_key(h.ctx, key, key_len);
            if (err != PortalError::NONE) return err;
            Scalar value;
            err = scan_value(c, &value);
            if (err != PortalError::NONE) return err;
            err = h.on_value(h.ctx, value);
            if (err != PortalError::NONE) return err;

            skip_ws(c);
            if (c.at_end()) return PortalError::MALFORMED_JSON;
            if (*c.p == ',') {
                c.p++;
                skip_ws(c);
                continue;
            }
            if (*c.p == '}') {
                c.p++;
                break;
            }
            return PortalError::MALFORMED_JSON;
        }
    }
    skip_ws(c);
    if (!c.at_end()) return PortalError::MALFORMED_JSON;  // trailing bytes
    *members = count;
    return PortalError::NONE;
}

//------------------------------------------------------------------------------
// PATCH handler
//------------------------------------------------------------------------------
struct PatchCtx {
    PortalPatch patch;
    int field = -1;
};

PortalError patch_on_key(void* ctx, const char* key, size_t key_len) {
    PatchCtx* p = static_cast<PatchCtx*>(ctx);
    p->field = portal_field_find(key, key_len);
    return p->field < 0 ? PortalError::UNKNOWN_KEY : PortalError::NONE;
}

PortalError patch_on_value(void* ctx, const Scalar& value) {
    PatchCtx* p = static_cast<PatchCtx*>(ctx);
    const size_t idx = static_cast<size_t>(p->field);
    const PortalField& f = kFields[idx];
    float result = 0.0f;
    bool adjusted = false;

    switch (f.type) {
        case PortalFieldType::BOOL:
            if (value.kind != Scalar::BOOLEAN) return PortalError::WRONG_TYPE;
            result = value.b ? 1.0f : 0.0f;
            break;
        case PortalFieldType::ENUM: {
            if (value.kind != Scalar::STRING) return PortalError::WRONG_TYPE;
            int found = -1;
            if (!value.str_too_long) {
                for (uint8_t i = 0; i < f.choice_count; i++) {
                    if (strcmp(value.str, f.choices[i]) == 0) found = i;
                }
            }
            if (found < 0) return PortalError::INVALID_VALUE;
            result = static_cast<float>(found);
            break;
        }
        case PortalFieldType::STRING:
            if (value.kind != Scalar::STRING) return PortalError::WRONG_TYPE;
            if (value.str_too_long || !portal_device_name_valid(value.str)) return PortalError::INVALID_DEVICE_NAME;
            memcpy(p->patch.device_name, value.str, strlen(value.str) + 1);
            break;
        case PortalFieldType::INT:
        case PortalFieldType::NUMBER:
            if (value.kind != Scalar::NUMBER) return PortalError::WRONG_TYPE;
            if (!isfinite(value.num)) return PortalError::INVALID_VALUE;
            if (f.is_profile) {
                // Same test as ProfileController::is_weight_valid / is_time_valid (on the stored float)
                const float candidate = static_cast<float>(clamp_input(value.num));
                if (!(candidate >= f.min && candidate <= f.max)) return PortalError::OUT_OF_RANGE;
                result = candidate;
            } else {
                result = normalise_number(idx, value.num);
                adjusted = fabs(static_cast<double>(result) - value.num) > 1.0e-4;
            }
            break;
    }
    p->patch.present |= 1u << idx;
    if (adjusted) p->patch.adjusted |= 1u << idx;
    p->patch.v[idx] = result;
    return PortalError::NONE;
}

//------------------------------------------------------------------------------
// Command handler
//------------------------------------------------------------------------------
struct CommandCtx {
    PortalCommandRequest req;
    bool have_cmd = false;
    bool have_profile = false;
    int field = 0;  // 0 = cmd, 1 = profile
};

PortalError command_on_key(void* ctx, const char* key, size_t key_len) {
    CommandCtx* c = static_cast<CommandCtx*>(ctx);
    if (key_len == 3 && memcmp(key, "cmd", 3) == 0) {
        c->field = 0;
    } else if (key_len == 7 && memcmp(key, "profile", 7) == 0) {
        c->field = 1;
    } else {
        return PortalError::UNKNOWN_KEY;
    }
    return PortalError::NONE;
}

PortalError command_on_value(void* ctx, const Scalar& value) {
    CommandCtx* c = static_cast<CommandCtx*>(ctx);
    if (c->field == 0) {
        if (value.kind != Scalar::STRING) return PortalError::WRONG_TYPE;
        if (!value.str_too_long) {
            for (size_t i = 0; i < kCommandCount; i++) {
                if (strcmp(value.str, kCommandNames[i]) == 0) {
                    c->req.cmd = static_cast<PortalCommand>(i);
                    c->have_cmd = true;
                    return PortalError::NONE;
                }
            }
        }
        return PortalError::UNKNOWN_COMMAND;
    }
    if (value.kind != Scalar::NUMBER) return PortalError::WRONG_TYPE;
    if (!isfinite(value.num) || value.num != floor(value.num)) return PortalError::INVALID_VALUE;
    if (value.num < 0 || value.num >= static_cast<double>(PORTAL_PROFILE_COUNT)) return PortalError::OUT_OF_RANGE;
    c->req.profile = static_cast<int>(value.num);
    c->have_profile = true;
    return PortalError::NONE;
}

//------------------------------------------------------------------------------
// JSON writer
//------------------------------------------------------------------------------
struct JsonOut {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool overflow = false;

    JsonOut(char* b, size_t c) : buf(b), cap(c) {
        if (cap > 0) buf[0] = '\0';
        if (cap == 0) overflow = true;
    }
    void raw(const char* s) {
        for (; *s; s++) put(*s);
    }
    void put(char ch) {
        if (overflow) return;
        if (len + 1 >= cap) {
            overflow = true;
            buf[len] = '\0';
            return;
        }
        buf[len++] = ch;
        buf[len] = '\0';
    }
    void str(const char* s) {
        put('"');
        for (; *s; s++) {
            const unsigned char ch = static_cast<unsigned char>(*s);
            if (ch == '"' || ch == '\\') {
                put('\\');
                put(static_cast<char>(ch));
            } else if (ch < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", ch);
                raw(esc);
            } else {
                put(static_cast<char>(ch));
            }
        }
        put('"');
    }
    // At most 3 decimals, trailing zeros trimmed but one decimal kept ("9.0", "21.5", "0.1")
    void num(double v) {
        if (!isfinite(v)) {
            raw("null");
            return;
        }
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%.3f", v);
        char* dot = strchr(tmp, '.');
        if (dot) {
            char* end = tmp + strlen(tmp) - 1;
            while (end > dot + 1 && *end == '0') *end-- = '\0';
        }
        raw(tmp);
    }
    void integer(long v) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%ld", v);
        raw(tmp);
    }
    void uinteger(uint32_t v) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(v));
        raw(tmp);
    }
    void boolean(bool v) { raw(v ? "true" : "false"); }
    void key(const char* k) {
        str(k);
        put(':');
    }
    bool done() {
        return !overflow;
    }
};

void write_field_value(JsonOut& j, size_t index, const PortalValues& values) {
    const PortalField& f = kFields[index];
    const float value = values.v[index];
    switch (f.type) {
        case PortalFieldType::STRING: j.str(values.device_name); break;
        case PortalFieldType::BOOL: j.boolean(value >= 0.5f); break;
        case PortalFieldType::INT: j.integer(lroundf(value)); break;
        case PortalFieldType::NUMBER: j.num(value); break;
        case PortalFieldType::ENUM: {
            int idx = lroundf(value);
            if (idx < 0) idx = 0;
            if (idx >= f.choice_count) idx = f.choice_count - 1;
            j.str(f.choices[idx]);
            break;
        }
    }
}

void write_values_object(JsonOut& j, const PortalValues& values) {
    j.put('{');
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        if (i > 0) j.put(',');
        j.key(kFields[i].key);
        write_field_value(j, i, values);
    }
    j.put('}');
}

const char* type_name(PortalFieldType t) {
    switch (t) {
        case PortalFieldType::BOOL: return "bool";
        case PortalFieldType::INT: return "int";
        case PortalFieldType::NUMBER: return "number";
        case PortalFieldType::ENUM: return "enum";
        case PortalFieldType::STRING: return "string";
    }
    return "";
}

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------
const PortalErrorInfo& portal_error_info(PortalError error) {
    const size_t i = static_cast<size_t>(error);
    return kErrors[i < static_cast<size_t>(PortalError::COUNT) ? i : 0];
}

const PortalField& portal_field(size_t index) { return kFields[index]; }

const char* portal_field_group(size_t index) {
    if (index == PF_DEVICE_NAME) return "device";
    return kFields[index].is_profile ? "profile" : "settings";
}

bool portal_device_name_valid(const char* name) {
    return name && strlen(name) <= PORTAL_DEVICE_NAME_MAX_LEN && wifi_validate_hostname(name);
}

int portal_field_find(const char* key, size_t key_len) {
    if (!key) return -1;
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        if (strlen(kFields[i].key) == key_len && memcmp(kFields[i].key, key, key_len) == 0) return static_cast<int>(i);
    }
    return -1;
}

const char* portal_profile_name(int profile) {
    return (profile >= 0 && profile < static_cast<int>(PORTAL_PROFILE_COUNT)) ? kProfileNames[profile] : "UNKNOWN";
}

PortalValues portal_default_values() {
    PortalValues values;
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) values.v[i] = kFields[i].def;
    values.current_profile = 1;
    memcpy(values.device_name, WIFI_DEFAULT_HOSTNAME, sizeof(WIFI_DEFAULT_HOSTNAME));
    return values;
}

PortalError portal_parse_patch(const char* body, size_t len, PortalPatch* out) {
    if (out) *out = PortalPatch();
    PatchCtx ctx;
    const MemberHandler handler{patch_on_key, patch_on_value, &ctx};
    size_t members = 0;
    PortalError err = scan_object(body, len, handler, &members);
    if (err != PortalError::NONE) return err;
    if (members == 0) return PortalError::EMPTY_PATCH;
    if (out) *out = ctx.patch;
    return PortalError::NONE;
}

PortalError portal_parse_command(const char* body, size_t len, PortalCommandRequest* out) {
    if (out) *out = PortalCommandRequest();
    CommandCtx ctx;
    const MemberHandler handler{command_on_key, command_on_value, &ctx};
    size_t members = 0;
    PortalError err = scan_object(body, len, handler, &members);
    if (err != PortalError::NONE) return err;
    if (!ctx.have_cmd) return PortalError::MISSING_FIELD;
    if (ctx.req.cmd == PortalCommand::START) {
        if (!ctx.have_profile) return PortalError::MISSING_FIELD;
    } else if (ctx.have_profile) {
        return PortalError::UNEXPECTED_FIELD;
    }
    if (out) *out = ctx.req;
    return PortalError::NONE;
}

const char* portal_command_name(PortalCommand cmd) {
    const size_t i = static_cast<size_t>(cmd);
    return i < kCommandCount ? kCommandNames[i] : "";
}

void portal_load_settings(const UiSettings& s, PortalValues* values) {
    if (!values) return;
    values->v[PF_BLE_STARTUP] = s.ble_startup() ? 1.0f : 0.0f;
    values->v[PF_LOGGING] = s.logging_enabled() ? 1.0f : 0.0f;
    values->v[PF_SWIPE] = s.swipe_enabled() ? 1.0f : 0.0f;
    values->v[PF_AUTO_START] = s.auto_start() ? 1.0f : 0.0f;
    values->v[PF_AUTO_RETURN] = s.auto_return() ? 1.0f : 0.0f;
    values->v[PF_BRIGHTNESS_NORMAL] = static_cast<float>(brightness_percent_from_fraction(s.brightness_normal()));
    values->v[PF_BRIGHTNESS_SCREENSAVER] = static_cast<float>(brightness_percent_from_fraction(s.brightness_screensaver()));
    values->v[PF_PURGE_MODE] = s.purge_mode() == 0 ? 0.0f : 1.0f;
    values->v[PF_PURGE_AMOUNT_G] = purge_amount_from_slider(purge_slider_from_amount(s.purge_amount_g()));
    values->v[PF_FRESHNESS_HOURS] = freshness_hours_from_index(freshness_index_from_hours(s.freshness_hours()));
    values->v[PF_GRIND_MODE] = static_cast<float>(grind_mode_radio_index(s.grind_mode_raw()));
}

bool portal_encode_error(PortalError error, char* out, size_t cap) {
    JsonOut j(out, cap);
    if (error == PortalError::NONE) {
        j.raw("{\"ok\":true}");
        return j.done();
    }
    const PortalErrorInfo& info = portal_error_info(error);
    j.put('{');
    j.key("error");
    j.str(info.code);
    j.put(',');
    j.key("reason");
    j.str(info.reason);
    j.put('}');
    return j.done();
}

bool portal_encode_values(const PortalValues& values, char* out, size_t cap) {
    JsonOut j(out, cap);
    j.put('{');
    j.key("values");
    write_values_object(j, values);
    j.raw(",\"current_profile\":");
    j.integer(values.current_profile);
    j.put('}');
    return j.done();
}

bool portal_encode_patch_reply(const PortalValues& applied, uint32_t adjusted_mask, char* out, size_t cap,
                               bool device_name_changed) {
    JsonOut j(out, cap);
    j.raw("{\"ok\":true,\"values\":");
    write_values_object(j, applied);
    j.raw(",\"current_profile\":");
    j.integer(applied.current_profile);
    j.raw(",\"adjusted\":[");
    bool first = true;
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        if (!((adjusted_mask >> i) & 1u)) continue;
        if (!first) j.put(',');
        first = false;
        j.str(kFields[i].key);
    }
    j.put(']');
    if (device_name_changed) {
        j.raw(",\"address\":");
        char address[PORTAL_DEVICE_NAME_MAX_LEN + 8];
        snprintf(address, sizeof(address), "%s.local", applied.device_name);
        j.str(address);
    }
    j.put('}');
    return j.done();
}

bool portal_encode_schema(char* out, size_t cap) {
    JsonOut j(out, cap);
    const PortalValues defaults = portal_default_values();
    j.raw("{\"fields\":[");
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) {
        const PortalField& f = kFields[i];
        if (i > 0) j.put(',');
        j.put('{');
        j.key("key");
        j.str(f.key);
        j.put(',');
        j.key("type");
        j.str(type_name(f.type));
        j.put(',');
        j.key("group");
        j.str(portal_field_group(i));
        if (f.type == PortalFieldType::STRING) {
            j.raw(",\"min\":");
            j.num(f.min);
            j.raw(",\"max\":");
            j.num(f.max);
        }
        if (f.type == PortalFieldType::INT || f.type == PortalFieldType::NUMBER) {
            if (f.unit[0]) {
                j.put(',');
                j.key("unit");
                j.str(f.unit);
            }
            j.raw(",\"min\":");
            j.num(f.min);
            j.raw(",\"max\":");
            j.num(f.max);
            if (f.step > 0) {
                j.raw(",\"step\":");
                j.num(f.step);
            }
        }
        if (f.type == PortalFieldType::ENUM) {
            j.raw(",\"choices\":[");
            for (uint8_t c = 0; c < f.choice_count; c++) {
                if (c > 0) j.put(',');
                j.str(f.choices[c]);
            }
            j.put(']');
        }
        j.raw(",\"default\":");
        write_field_value(j, i, defaults);
        j.put('}');
    }
    j.raw("],\"profiles\":[");
    for (size_t p = 0; p < PORTAL_PROFILE_COUNT; p++) {
        if (p > 0) j.put(',');
        j.raw("{\"index\":");
        j.integer(static_cast<long>(p));
        j.raw(",\"name\":");
        j.str(kProfileNames[p]);
        j.raw(",\"weight_key\":");
        j.str(kFields[PF_SINGLE_WEIGHT_G + 2 * p].key);
        j.raw(",\"time_key\":");
        j.str(kFields[PF_SINGLE_TIME_S + 2 * p].key);
        j.put('}');
    }
    j.raw("]}");
    return j.done();
}

bool portal_encode_command_reply(const PortalCommandRequest& request, char* out, size_t cap) {
    JsonOut j(out, cap);
    j.raw("{\"ok\":true,\"cmd\":");
    j.str(portal_command_name(request.cmd));
    if (request.cmd == PortalCommand::START) {
        j.raw(",\"profile\":");
        j.integer(request.profile);
        j.raw(",\"lead_in_ms\":");
        j.integer(PORTAL_LEAD_IN_MS);
    }
    j.put('}');
    return j.done();
}

bool portal_encode_status(const PortalStatus& s, char* out, size_t cap) {
    JsonOut j(out, cap);
    j.put('{');
    j.key("ui_state");
    j.str(s.ui_state ? s.ui_state : "");
    j.put(',');
    j.key("grind_phase");
    j.str(s.grind_phase ? s.grind_phase : "");
    j.raw(",\"busy\":");
    j.boolean(s.busy);
    j.raw(",\"weight_g\":");
    j.num(s.weight_g);
    j.raw(",\"motor_available\":");
    j.boolean(s.motor_available);
    j.raw(",\"start_block\":");
    if (s.start_block == PortalError::NONE) j.raw("null"); else j.str(portal_error_info(s.start_block).code);
    j.raw(",\"motor_test_block\":");
    if (s.motor_test_block == PortalError::NONE) j.raw("null"); else j.str(portal_error_info(s.motor_test_block).code);
    j.raw(",\"uptime_s\":");
    j.uinteger(s.uptime_s);
    j.raw(",\"build\":");
    if (s.build < 0) j.raw("null"); else j.integer(s.build);
    j.raw(",\"web_interface\":");
    j.boolean(s.web_interface);
    j.put('}');
    return j.done();
}

#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"

// Pure model of the web panel API (docs/PORTAL-API.md): the settings/profile field table, the strict parser
// for PATCH /api/settings and POST /api/command bodies, the error table and the JSON encoders. No
// Arduino/ESP/LVGL/NVS dependencies (unit tested on the host); the HTTP task only calls this module, the UI
// task applies a parsed patch through the same handlers as the touchscreen.

//------------------------------------------------------------------------------
// Errors: one table for every refusal (HTTP status, {"error"} code, {"reason"} text)
//------------------------------------------------------------------------------
enum class PortalError : uint8_t {
    NONE = 0,
    // Body (400/413)
    EMPTY_BODY,
    BODY_TOO_LARGE,
    MALFORMED_JSON,
    NESTED_VALUE,
    DUPLICATE_KEY,
    UNKNOWN_KEY,
    UNEXPECTED_FIELD,
    MISSING_FIELD,
    EMPTY_PATCH,
    WRONG_TYPE,
    INVALID_VALUE,
    OUT_OF_RANGE,
    UNKNOWN_COMMAND,
    // Browser guards and the web interface gate (403/415)
    WEB_INTERFACE_OFF,
    BAD_HOST,
    BAD_ORIGIN,
    MISSING_CUSTOM_HEADER,
    UNSUPPORTED_MEDIA_TYPE,
    // Command / write decision (409)
    COMMAND_IN_FLIGHT,
    BUSY,
    NOT_READY,
    UPDATE_IN_PROGRESS,
    MOTOR_DISABLED,
    MOTOR_FAULT,
    LOAD_CELL_NOT_CONNECTED,
    LOAD_CELL_NO_DATA,
    LOAD_CELL_RATE_INVALID,
    // HTTP layer outcomes (decided outside the pure modules, listed so the table is complete)
    UI_BUSY,       // 503: the UI task did not pick the request up in time
    STORE_FAILED,  // 500: a value could not be persisted
    COUNT
};

struct PortalErrorInfo {
    int status;          // HTTP status code
    const char* code;    // machine-readable {"error"}
    const char* reason;  // plain text {"reason"}
};

// Status/code/reason of an error (NONE maps to 200 "ok"; out-of-range ids map to NONE)
const PortalErrorInfo& portal_error_info(PortalError error);

//------------------------------------------------------------------------------
// Field table: 11 menu settings (incl. grind mode) and 6 profile targets, all addressed by flat keys
//------------------------------------------------------------------------------
enum class PortalFieldType : uint8_t {
    BOOL,    // JSON true/false
    INT,     // JSON number, rounded to a whole number
    NUMBER,  // JSON number
    ENUM     // JSON string, one of the choices (value = index of the choice)
};

constexpr size_t PORTAL_SETTING_COUNT = 11;
constexpr size_t PORTAL_PROFILE_FIELD_COUNT = 6;  // single/double/custom x weight/time
constexpr size_t PORTAL_FIELD_COUNT = PORTAL_SETTING_COUNT + PORTAL_PROFILE_FIELD_COUNT;
constexpr size_t PORTAL_PROFILE_COUNT = 3;
constexpr size_t PORTAL_ENUM_MAX_CHOICES = 2;

// Field indices: the first PORTAL_SETTING_COUNT are settings, the rest profile targets
enum PortalFieldIndex : uint8_t {
    PF_BLE_STARTUP = 0,
    PF_LOGGING,
    PF_SWIPE,
    PF_AUTO_START,
    PF_AUTO_RETURN,
    PF_BRIGHTNESS_NORMAL,
    PF_BRIGHTNESS_SCREENSAVER,
    PF_PURGE_MODE,
    PF_PURGE_AMOUNT_G,
    PF_FRESHNESS_HOURS,
    PF_GRIND_MODE,
    PF_SINGLE_WEIGHT_G,
    PF_SINGLE_TIME_S,
    PF_DOUBLE_WEIGHT_G,
    PF_DOUBLE_TIME_S,
    PF_CUSTOM_WEIGHT_G,
    PF_CUSTOM_TIME_S
};

struct PortalField {
    const char* key;
    PortalFieldType type;
    const char* unit;  // "" when unit-less
    float min;         // BOOL/ENUM: 0 and choice_count - 1 (informational)
    float max;
    float step;        // 0 = continuous / not applicable
    float def;         // factory default (BOOL 0/1, ENUM choice index)
    const char* choices[PORTAL_ENUM_MAX_CHOICES];
    uint8_t choice_count;
    bool is_profile;   // true: out-of-range is refused; false (settings): clamped like the menu
};

const PortalField& portal_field(size_t index);  // index must be below PORTAL_FIELD_COUNT
// Index of a key, or -1 (exact, case-sensitive match of key_len bytes)
int portal_field_find(const char* key, size_t key_len);
// Profile name by index (0 SINGLE, 1 DOUBLE, 2 CUSTOM); "UNKNOWN" for anything else
const char* portal_profile_name(int profile);

// Normalised values, one float per field: BOOL 0/1, ENUM choice index (= stored value: purge mode 0 Keep/Prime,
// 1 Remove/Purge; grind mode 0 Weight, 1 Time), brightness in percent, purge amount in grams, freshness in
// hours, profile weights in grams and times in seconds.
struct PortalValues {
    float v[PORTAL_FIELD_COUNT] = {0};
    int current_profile = 1;  // read-only here (the selection on the screen)
};

// Values of a successfully parsed PATCH body. Only fields named in the body are present.
struct PortalPatch {
    uint32_t present = 0;   // bit per field index
    uint32_t adjusted = 0;  // subset of present: the value was clamped or snapped like the menu does
    float v[PORTAL_FIELD_COUNT] = {0};
    bool has(size_t index) const { return (present >> index) & 1u; }
};

// Factory defaults of every field (current_profile = 1, DOUBLE, like ProfileController)
PortalValues portal_default_values();

// Parses a PATCH /api/settings body (not NUL-terminated): a flat JSON object of known keys, at least one.
// All or nothing: on any error nothing is filled (*out is reset). Settings are clamped/snapped exactly like the
// menu (see PORTAL-API.md), profile targets outside their limits are refused (OUT_OF_RANGE).
PortalError portal_parse_patch(const char* body, size_t len, PortalPatch* out);

// Commands of POST /api/command
enum class PortalCommand : uint8_t { STOP, TARE, START, MOTOR_TEST };

struct PortalCommandRequest {
    PortalCommand cmd = PortalCommand::STOP;
    int profile = -1;  // START only: 0..2; -1 otherwise
};

// Parses {"cmd":"stop"|"tare"|"start"|"motor_test"[,"profile":0..2]}; "profile" is required for start and
// refused for the others. All or nothing.
PortalError portal_parse_command(const char* body, size_t len, PortalCommandRequest* out);
const char* portal_command_name(PortalCommand cmd);

//------------------------------------------------------------------------------
// Settings as stored (the facade the menu uses), converted exactly like the menu screens do
//------------------------------------------------------------------------------
class UiSettings;
// Fills the 11 settings of *values from UiSettings (profile targets and current_profile are left alone).
void portal_load_settings(const UiSettings& settings, PortalValues* values);

//------------------------------------------------------------------------------
// JSON encoders: write at most cap bytes, always NUL-terminate (cap > 0), return false when truncated
//------------------------------------------------------------------------------
// {"error":"code","reason":"text"} (NONE encodes {"ok":true})
bool portal_encode_error(PortalError error, char* out, size_t cap);
// {"values":{<17 flat keys>},"current_profile":N}  (GET /api/settings)
bool portal_encode_values(const PortalValues& values, char* out, size_t cap);
// {"ok":true,"values":{...},"current_profile":N,"adjusted":["key",...]}  (PATCH reply: the applied state)
bool portal_encode_patch_reply(const PortalValues& applied, uint32_t adjusted_mask, char* out, size_t cap);
// {"fields":[{"key","type","unit","min","max","step","default","choices"?,"group"}],"profiles":[{"index","name","weight_key","time_key"}]}
bool portal_encode_schema(char* out, size_t cap);
// {"ok":true,"cmd":"start","profile":1,"lead_in_ms":3000} (profile and lead_in_ms only for start)
bool portal_encode_command_reply(const PortalCommandRequest& request, char* out, size_t cap);

// GET /api/status snapshot (published by the UI task)
struct PortalStatus {
    const char* ui_state = "ready";     // lower-case snake_case name of UIState
    const char* grind_phase = "idle";   // lower-case snake_case name of GrindPhase
    bool busy = false;                  // grind, autotune or motor output active
    float weight_g = 0.0f;              // non-finite encodes null
    bool motor_available = false;
    PortalError start_block = PortalError::NONE;       // what a start would be refused with, NONE if allowed
    PortalError motor_test_block = PortalError::NONE;  // same for a motor test
    uint32_t uptime_s = 0;
    int32_t build = -1;                 // -1 encodes null
    bool web_interface = true;
};
bool portal_encode_status(const PortalStatus& status, char* out, size_t cap);

#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"

// GLink messages (docs/GLINK-PROTOCOL.md sections 4 to 6): the vocabularies, the error table, the strict request
// parser (typed structs) and the response / broadcast encoders. Pure: no Arduino/ESP dependencies, no heap, unit
// tested on the host against tools/tests/glink_vectors.json. Numbers (dose, grind size, weight, target) are scaled
// integers in tenths, so nothing here depends on binary floating point.

//------------------------------------------------------------------------------
// Limits derived from the constants in config/bluetooth.h (in tenths of a gram)
//------------------------------------------------------------------------------
constexpr int32_t GLINK_DOSE_MIN_TENTHS = 50;       // GLINK_DOSE_MIN_G * 10
constexpr int32_t GLINK_DOSE_MAX_TENTHS = 600;      // GLINK_DOSE_MAX_G * 10
constexpr int32_t GLINK_GRIND_SIZE_MAX_TENTHS = 9999;  // GLINK_GRIND_SIZE_MAX * 10
constexpr size_t GLINK_UNIT_BUF = GLINK_GRIND_UNIT_MAX_LEN + 1;
constexpr size_t GLINK_FIELD_BUF = 48;              // "selector." plus a 32 character key, with room

//------------------------------------------------------------------------------
// Error codes (6.4) and messages
//------------------------------------------------------------------------------
enum class GlinkError : uint8_t {
    NONE = 0,  // no error; as a `blocked` value it means null
    INVALID_PARAM,
    MISSING_FIELD,
    OUT_OF_RANGE,
    INVALID_STATE,
    NOT_FOUND,
    CONFLICT,
    MALFORMED_JSON,
    UNKNOWN_OBJECT,
    UNKNOWN_OP,
    PAYLOAD_TOO_LARGE,
    BUSY,
    UI_BUSY,
    STORE_FAILED,
    RATE_LIMITED,
    PROFILE_READ_ONLY,
    EDIT_IN_PROGRESS,
    PROFILE_NOT_SET,
    NOT_READY,
    UPDATING,
    MOTOR_DISABLED,
    LOAD_CELL_NOT_CONNECTED,
    LOAD_CELL_NO_DATA,
    LOAD_CELL_RATE_INVALID,
    COMMAND_IN_FLIGHT,
    COUNT
};

const char* glink_error_code(GlinkError error);  // wire name ("OUT_OF_RANGE"); NONE gives "NONE"

// The short English texts of the "message" field (not a contract; the vectors pin them for the host tests).
namespace glink_msg {
constexpr const char* MALFORMED = "malformed json";
constexpr const char* TOO_LARGE = "request larger than 512 bytes";
constexpr const char* UNKNOWN_OBJECT = "unknown object";
constexpr const char* UNKNOWN_TOP_LEVEL = "unknown top-level member";
constexpr const char* REQUEST_MISSING = "request missing";
constexpr const char* REQUEST_NOT_OBJECT = "request must be an object";
constexpr const char* NO_OBJECT = "no object in request";
constexpr const char* ONE_OBJECT = "exactly one object per request";
constexpr const char* OBJECT_NOT_OBJECT = "object must be an object";
constexpr const char* OP_MISSING = "op missing";
constexpr const char* OP_NOT_STRING = "op must be a string";
constexpr const char* OP_UNSUPPORTED = "op not supported";
constexpr const char* UNKNOWN_FIELD = "unknown field";
constexpr const char* SELECTOR_NOT_OBJECT = "selector must be an object";
constexpr const char* SELECTOR_TYPE_MISSING = "selector.type missing";
constexpr const char* SELECTOR_TYPE_UNSUPPORTED = "selector.type not supported";
constexpr const char* SELECTOR_VALUE_MISSING = "selector.value missing";
constexpr const char* SELECTOR_VALUE_NOT_ALLOWED = "selector.value not allowed";
constexpr const char* SELECTOR_VALUE_NOT_INT = "selector.value must be an integer";
constexpr const char* INDEX_MISSING = "index missing";
constexpr const char* INDEX_NOT_INT = "index must be an integer";
constexpr const char* INDEX_RANGE = "index out of range";
constexpr const char* PROFILE_MISSING = "profile missing";
constexpr const char* PROFILE_NOT_INT = "profile must be an integer";
constexpr const char* PROFILE_RANGE = "profile out of range";
constexpr const char* DOSE_NOT_NUMBER = "dose must be a number";
constexpr const char* DOSE_RANGE = "dose out of range";
constexpr const char* EXPECT_NOT_NUMBER = "expectDose must be a number";
constexpr const char* EXPECT_RANGE = "expectDose out of range";
constexpr const char* SIZE_NOT_NUMBER = "grindSize must be a number or null";
constexpr const char* SIZE_RANGE = "grindSize out of range";
constexpr const char* UNIT_NOT_STRING = "grindUnit must be a string or null";
constexpr const char* UNIT_BAD_CHAR = "grindUnit has an invalid character";
constexpr const char* UNIT_TOO_LONG = "grindUnit too long";
constexpr const char* UNIT_NEEDS_SIZE = "grindUnit needs a grindSize";
constexpr const char* NOTHING_TO_UPDATE = "nothing to update";
constexpr const char* DOSE_REQUIRED = "dose required to create the profile";
constexpr const char* READ_ONLY = "profile is read-only over BLE";
constexpr const char* PROFILE_NOT_SET = "profile not set";
constexpr const char* NO_RESULT = "no result since boot";
constexpr const char* HANDSHAKE_REQUIRED = "handshake required";
constexpr const char* BUSY = "previous request still pending";
constexpr const char* UI_BUSY = "ui task did not answer in time";
constexpr const char* STORE_FAILED = "could not store the value";
constexpr const char* RATE_LIMITED = "stored changes at most once per second";
constexpr const char* NOT_READY_READY = "not on the ready screen";
constexpr const char* NOT_READY_READY_OR_MENU = "not on the ready or menu screen";
constexpr const char* EDIT_IN_PROGRESS = "profile is being edited on the grinder";
constexpr const char* IN_FLIGHT = "a start is in progress";
constexpr const char* UPDATING = "update in progress";
constexpr const char* MOTOR_DISABLED = "motor is disabled";
constexpr const char* LOAD_CELL_NOT_CONNECTED = "load cell not connected";
constexpr const char* LOAD_CELL_NO_DATA = "load cell sends no data";
constexpr const char* LOAD_CELL_RATE_INVALID = "load cell sample rate invalid";
constexpr const char* MOTOR_FAULT = "motor fault, restart the grinder";
constexpr const char* DOSE_OUT_OF_BLE_RANGE = "stored dose outside 5.0 to 60.0 g";
constexpr const char* CONFLICT = "dose differs from expectDose";
}  // namespace glink_msg

// One refusal: error code, text and optional field path ("" = none). Also the result of a failed parse.
struct GlinkFailure {
    GlinkError code = GlinkError::NONE;
    const char* message = "";
    char field[GLINK_FIELD_BUF] = {};
    // The object the failure is answered under: a GlinkObject value, or -1 for the pseudo-object "error".
    int object = -1;
};

// Fills a failure (field may be nullptr or ""; object is a GlinkObject value or -1). Always returns false.
bool glink_set_failure(GlinkFailure& failure, GlinkError code, const char* message, const char* field, int object);

//------------------------------------------------------------------------------
// Request objects and operations
//------------------------------------------------------------------------------
enum class GlinkObject : uint8_t { APP_HELLO, GRIND_PROFILE, GRIND_CONTROL, GRIND_RESULT, COUNT };
enum class GlinkOp : uint8_t { NONE, HANDSHAKE, GET, SELECT, UPDATE, START, STOP };

const char* glink_object_name(GlinkObject object);  // "appHello", ...
const char* glink_op_name(GlinkOp op);              // "handshake", ...

struct GlinkRequest {
    GlinkObject object = GlinkObject::APP_HELLO;
    GlinkOp op = GlinkOp::NONE;
    // grindProfile get: selector (absent = all)
    bool selector_index = false;
    uint8_t selector_value = 0;
    // grindProfile select / update: index; grindControl start: profile
    uint8_t index = 0;
    // grindProfile update
    bool has_dose = false;
    int32_t dose_tenths = 0;
    bool has_grind_size = false;
    bool grind_size_null = false;
    int32_t grind_size_tenths = 0;
    bool has_grind_unit = false;  // grind_unit is "" for null / empty / spaces only (clears the unit)
    char grind_unit[GLINK_UNIT_BUF] = {};
    // grindControl start
    bool has_expect_dose = false;
    int32_t expect_dose_tenths = 0;
};

// Parses one request message (4.1 steps 1 to 5): size, reader, envelope, operation, unknown keys, required
// fields, types and ranges, in that order. true = the request is valid and `request` is filled; false = `failure`
// holds the first error. A `stop` is valid whatever else the object contains (executed before the handshake gate).
bool glink_parse_request(const char* text, size_t len, GlinkRequest& request, GlinkFailure& failure);

//------------------------------------------------------------------------------
// Vocabularies (6.1 to 6.3, origin, mode)
//------------------------------------------------------------------------------
enum class GlinkDevState : uint8_t { IDLE, STARTING, GRINDING, DONE, SETTING, UPDATING, WARNING, UNKNOWN, COUNT };
enum class GlinkPhase : uint8_t {
    NONE,  // absent in periodInfo
    INITIALIZING,
    SETUP,
    TARING,
    TARE_CONFIRM,
    PREDICTIVE,
    PULSE_DECISION,
    PULSE_EXECUTE,
    PULSE_SETTLING,
    FINAL_SETTLING,
    TIME_GRINDING,
    TIME_ADDITIONAL_PULSE,
    PRIME,
    PRIME_SETTLING,
    PURGE_CONFIRM,
    COUNT
};
enum class GlinkResultCode : uint8_t {
    OK,
    OVERSHOOT,
    MAX_PULSES,
    TIMEOUT,
    ERR_STALLED,
    ERR_NO_FLOW,
    ERR_SENSOR,
    ERR_TARE,
    ERR_MOTOR,
    ERR_NEG_WEIGHT,
    STOPPED,
    UPDATE,
    COUNT
};
enum class GlinkOrigin : uint8_t { BLE, TOUCH, WEB, COUNT };
enum class GlinkMode : uint8_t { WEIGHT, TIME, COUNT };

const char* glink_dev_state_name(GlinkDevState state);
const char* glink_phase_name(GlinkPhase phase);  // NONE gives ""
const char* glink_result_code_name(GlinkResultCode code);
const char* glink_origin_name(GlinkOrigin origin);
const char* glink_mode_name(GlinkMode mode);
const char* glink_profile_name(uint8_t index);   // SINGLE, DOUBLE, CUSTOM, PROFILE; "" out of range

//------------------------------------------------------------------------------
// Response and broadcast encoders. Each returns the message length, or 0 if it would not fit in `cap` bytes or in
// GLINK_MAX_REPLY_BYTES. The output is NUL-terminated (the NUL is not counted).
//------------------------------------------------------------------------------
struct GlinkProfileEntry {
    uint8_t index = 0;
    int32_t dose_tenths = 0;
    bool has_grind_size = false;
    int32_t grind_size_tenths = 0;
    char grind_unit[GLINK_UNIT_BUF] = {};  // "" = none (only with a size)
};

struct GlinkBaseInfo {
    uint32_t release_ver = 0;
    const char* sn_code = "";
    const char* fw_version = "";
    const char* board = "";
};

struct GlinkPeriodInfo {
    GlinkDevState dev_state = GlinkDevState::UNKNOWN;
    bool weight_valid = false;
    int32_t weight_tenths = 0;
    uint8_t select_profile = 0;
    GlinkPhase phase = GlinkPhase::NONE;
    bool has_target = false;  // target and mode go together
    int32_t target_tenths = 0;
    GlinkMode mode = GlinkMode::WEIGHT;
    bool has_lead_in = false;
    uint32_t lead_in_ms = 0;
    GlinkError blocked = GlinkError::NONE;  // NONE = null
    uint32_t result_seq = 0;
    uint32_t profile_rev = 0;
};

struct GlinkResultInfo {
    uint32_t seq = 0;
    GlinkResultCode code = GlinkResultCode::OK;
    GlinkOrigin origin = GlinkOrigin::BLE;
    uint8_t profile = 0;
    GlinkMode mode = GlinkMode::WEIGHT;
    int32_t target_tenths = 0;
    int32_t final_weight_tenths = 0;
    bool weight_settled = false;
    uint32_t pulses = 0;
    uint32_t duration_ms = 0;
    uint32_t motor_ms = 0;
};

// {"response":{"<object>":{"result":"fail","error":{...}}}}; object < 0 uses the pseudo-object "error".
size_t glink_encode_failure(char* out, size_t cap, const GlinkFailure& failure);
size_t glink_encode_error(char* out, size_t cap, int object, GlinkError code, const char* message, const char* field);

size_t glink_encode_profile_list(char* out, size_t cap, const GlinkProfileEntry* entries, size_t count);
size_t glink_encode_profile_entry(char* out, size_t cap, const GlinkProfileEntry& entry);
size_t glink_encode_profile_update(char* out, size_t cap, const GlinkProfileEntry& entry, bool changed, bool stored);
size_t glink_encode_select(char* out, size_t cap, uint8_t select_profile);
size_t glink_encode_start(char* out, size_t cap, uint32_t lead_in_ms);
size_t glink_encode_stop(char* out, size_t cap);
size_t glink_encode_result_get(char* out, size_t cap, const GlinkResultInfo& result);

size_t glink_encode_base_info(char* out, size_t cap, const GlinkBaseInfo& info);
size_t glink_encode_period_info(char* out, size_t cap, const GlinkPeriodInfo& info);
size_t glink_encode_result_broadcast(char* out, size_t cap, const GlinkResultInfo& result);

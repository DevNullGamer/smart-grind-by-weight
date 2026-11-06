#include "glink_messages.h"
#include <string.h>
#include "glink_json.h"

//------------------------------------------------------------------------------
// Names
//------------------------------------------------------------------------------
namespace {

const char* const kErrorNames[] = {"NONE", "INVALID_PARAM", "MISSING_FIELD", "OUT_OF_RANGE", "INVALID_STATE",
                                   "NOT_FOUND", "CONFLICT", "MALFORMED_JSON", "UNKNOWN_OBJECT", "UNKNOWN_OP",
                                   "PAYLOAD_TOO_LARGE", "BUSY", "UI_BUSY", "STORE_FAILED", "RATE_LIMITED",
                                   "PROFILE_READ_ONLY", "EDIT_IN_PROGRESS", "PROFILE_NOT_SET", "NOT_READY",
                                   "UPDATING", "MOTOR_DISABLED", "LOAD_CELL_NOT_CONNECTED", "LOAD_CELL_NO_DATA",
                                   "LOAD_CELL_RATE_INVALID", "COMMAND_IN_FLIGHT"};
static_assert(sizeof(kErrorNames) / sizeof(kErrorNames[0]) == static_cast<size_t>(GlinkError::COUNT),
              "error name table");

const char* const kObjectNames[] = {"appHello", "grindProfile", "grindControl", "grindResult"};
const char* const kOpNames[] = {"", "handshake", "get", "select", "update", "start", "stop"};
const char* const kDevStateNames[] = {"IDLE", "STARTING", "GRINDING", "DONE", "SETTING", "UPDATING", "WARNING", "UNKNOWN"};
const char* const kPhaseNames[] = {"", "INITIALIZING", "SETUP", "TARING", "TARE_CONFIRM", "PREDICTIVE", "PULSE_DECISION",
                                   "PULSE_EXECUTE", "PULSE_SETTLING", "FINAL_SETTLING", "TIME_GRINDING",
                                   "TIME_ADDITIONAL_PULSE", "PRIME", "PRIME_SETTLING", "PURGE_CONFIRM"};
const char* const kResultNames[] = {"OK", "OVERSHOOT", "MAX_PULSES", "TIMEOUT", "ERR_STALLED", "ERR_NO_FLOW", "ERR_SENSOR",
                                    "ERR_TARE", "ERR_MOTOR", "ERR_NEG_WEIGHT", "STOPPED", "UPDATE"};
const char* const kOriginNames[] = {"BLE", "TOUCH", "WEB"};
const char* const kModeNames[] = {"WEIGHT", "TIME"};
const char* const kProfileNames[] = {"SINGLE", "DOUBLE", "CUSTOM", "PROFILE"};

}  // namespace

const char* glink_error_code(GlinkError error) {
    const size_t i = static_cast<size_t>(error);
    return i < static_cast<size_t>(GlinkError::COUNT) ? kErrorNames[i] : "NONE";
}

const char* glink_object_name(GlinkObject object) {
    const size_t i = static_cast<size_t>(object);
    return i < static_cast<size_t>(GlinkObject::COUNT) ? kObjectNames[i] : "";
}

const char* glink_op_name(GlinkOp op) {
    const size_t i = static_cast<size_t>(op);
    return i < sizeof(kOpNames) / sizeof(kOpNames[0]) ? kOpNames[i] : "";
}

const char* glink_dev_state_name(GlinkDevState state) {
    const size_t i = static_cast<size_t>(state);
    return i < static_cast<size_t>(GlinkDevState::COUNT) ? kDevStateNames[i] : "UNKNOWN";
}

const char* glink_phase_name(GlinkPhase phase) {
    const size_t i = static_cast<size_t>(phase);
    return i < static_cast<size_t>(GlinkPhase::COUNT) ? kPhaseNames[i] : "";
}

const char* glink_result_code_name(GlinkResultCode code) {
    const size_t i = static_cast<size_t>(code);
    return i < static_cast<size_t>(GlinkResultCode::COUNT) ? kResultNames[i] : "";
}

const char* glink_origin_name(GlinkOrigin origin) {
    const size_t i = static_cast<size_t>(origin);
    return i < static_cast<size_t>(GlinkOrigin::COUNT) ? kOriginNames[i] : "";
}

const char* glink_mode_name(GlinkMode mode) {
    const size_t i = static_cast<size_t>(mode);
    return i < static_cast<size_t>(GlinkMode::COUNT) ? kModeNames[i] : "";
}

const char* glink_profile_name(uint8_t index) { return index < GLINK_PROFILE_COUNT_MAX ? kProfileNames[index] : ""; }

//------------------------------------------------------------------------------
// Request parser
//------------------------------------------------------------------------------
namespace {

constexpr int NO_OBJECT = -1;

bool fail_with(GlinkFailure& f, GlinkError code, const char* message, const char* field, int object) {
    return glink_set_failure(f, code, message, field, object);
}

}  // namespace

bool glink_set_failure(GlinkFailure& f, GlinkError code, const char* message, const char* field, int object) {
    f.code = code;
    f.message = message;
    f.object = object;
    size_t n = 0;
    if (field != nullptr) {
        while (field[n] != '\0' && n < GLINK_FIELD_BUF - 1) {
            f.field[n] = field[n];
            n++;
        }
    }
    f.field[n] = '\0';
    return false;
}

namespace {

// Field path "<prefix><key of node>" (the key charset is limited by the reader, so this is safe to echo).
bool fail_key(GlinkFailure& f, const GlinkJsonDoc& doc, int node, const char* prefix, GlinkError code,
              const char* message, int object) {
    char key[GLINK_JSON_KEY_MAX_LEN + 1];
    glink_json_key(doc, node, key, sizeof(key));
    char path[GLINK_FIELD_BUF];
    size_t n = 0;
    for (const char* s = prefix; *s && n < sizeof(path) - 1; s++) path[n++] = *s;
    for (const char* s = key; *s && n < sizeof(path) - 1; s++) path[n++] = *s;
    path[n] = '\0';
    return fail_with(f, code, message, path, object);
}

// Reads the "op" member of `obj` and matches it against the allowed operations.
bool read_op(const GlinkJsonDoc& doc, int obj, int object_id, const GlinkOp* allowed, size_t n, GlinkOp& op,
             GlinkFailure& f) {
    const int m = glink_json_find(doc, obj, "op");
    if (m < 0) return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::OP_MISSING, "op", object_id);
    if (doc.nodes[m].type != GlinkJsonType::STRING) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::OP_NOT_STRING, "op", object_id);
    }
    for (size_t i = 0; i < n; i++) {
        if (glink_json_string_is(doc, m, glink_op_name(allowed[i]))) {
            op = allowed[i];
            return true;
        }
    }
    return fail_with(f, GlinkError::UNKNOWN_OP, glink_msg::OP_UNSUPPORTED, "op", object_id);
}

// Unknown keys are refused (INVALID_PARAM, field = prefix + key), in message order.
bool check_keys(const GlinkJsonDoc& doc, int obj, const char* const* allowed, size_t n, const char* prefix,
                int object_id, GlinkFailure& f) {
    for (int m = glink_json_first_member(doc, obj); m >= 0; m = glink_json_next_member(doc, obj, m)) {
        bool known = false;
        for (size_t i = 0; i < n && !known; i++) known = glink_json_key_is(doc, m, allowed[i]);
        if (!known) return fail_key(f, doc, m, prefix, GlinkError::INVALID_PARAM, glink_msg::UNKNOWN_FIELD, object_id);
    }
    return true;
}

// An integer in [lo, hi]. Not a number / not whole: INVALID_PARAM (not_int); outside: OUT_OF_RANGE (range).
bool read_index(const GlinkJsonDoc& doc, int node, int64_t lo, int64_t hi, const char* field, const char* not_int,
                const char* range, int object_id, GlinkFailure& f, uint8_t& out) {
    int64_t v = 0;
    if (doc.nodes[node].type != GlinkJsonType::NUMBER || !glink_json_number_integer(doc, node, &v)) {
        return fail_with(f, GlinkError::INVALID_PARAM, not_int, field, object_id);
    }
    if (v < lo || v > hi) return fail_with(f, GlinkError::OUT_OF_RANGE, range, field, object_id);
    out = static_cast<uint8_t>(v);
    return true;
}

// A number rounded to tenths and range checked after rounding.
bool read_tenths(const GlinkJsonDoc& doc, int node, int32_t lo, int32_t hi, const char* field, const char* not_number,
                 const char* range, int object_id, GlinkFailure& f, int32_t& out) {
    if (doc.nodes[node].type != GlinkJsonType::NUMBER) {
        return fail_with(f, GlinkError::INVALID_PARAM, not_number, field, object_id);
    }
    int64_t t = 0;
    glink_json_number_tenths(doc, node, &t);
    if (t < lo || t > hi) return fail_with(f, GlinkError::OUT_OF_RANGE, range, field, object_id);
    out = static_cast<int32_t>(t);
    return true;
}

bool unit_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
           c == '/' || c == '%' || c == '-' || c == '_';
}

// grindUnit: string or null; trimmed (spaces); charset first, then length.
bool read_unit(const GlinkJsonDoc& doc, int node, int object_id, GlinkFailure& f, GlinkRequest& req) {
    req.has_grind_unit = true;
    req.grind_unit[0] = '\0';
    const GlinkJsonType type = doc.nodes[node].type;
    if (type == GlinkJsonType::NULL_VALUE) return true;
    if (type != GlinkJsonType::STRING) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::UNIT_NOT_STRING, "grindUnit", object_id);
    }
    char raw[GLINK_JSON_STRING_MAX_LEN + 1];
    size_t n = glink_json_string(doc, node, raw, sizeof(raw));
    size_t start = 0;
    while (start < n && raw[start] == ' ') start++;
    while (n > start && raw[n - 1] == ' ') n--;
    for (size_t i = start; i < n; i++) {
        if (!unit_char_ok(raw[i])) {
            return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::UNIT_BAD_CHAR, "grindUnit", object_id);
        }
    }
    if (n - start > GLINK_GRIND_UNIT_MAX_LEN) {
        return fail_with(f, GlinkError::OUT_OF_RANGE, glink_msg::UNIT_TOO_LONG, "grindUnit", object_id);
    }
    memcpy(req.grind_unit, raw + start, n - start);
    req.grind_unit[n - start] = '\0';
    return true;
}

bool parse_get(const GlinkJsonDoc& doc, int obj, int oid, GlinkRequest& req, GlinkFailure& f) {
    static const char* const keys[] = {"op", "selector"};
    if (!check_keys(doc, obj, keys, 2, "", oid, f)) return false;
    const int sel = glink_json_find(doc, obj, "selector");
    if (sel < 0) return true;  // all
    if (doc.nodes[sel].type != GlinkJsonType::OBJECT) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::SELECTOR_NOT_OBJECT, "selector", oid);
    }
    static const char* const sel_keys[] = {"type", "value"};
    if (!check_keys(doc, sel, sel_keys, 2, "selector.", oid, f)) return false;
    const int type = glink_json_find(doc, sel, "type");
    if (type < 0) return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::SELECTOR_TYPE_MISSING, "selector.type", oid);
    const int value = glink_json_find(doc, sel, "value");
    if (glink_json_string_is(doc, type, "all")) {
        if (value >= 0) {
            return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::SELECTOR_VALUE_NOT_ALLOWED, "selector.value", oid);
        }
        return true;
    }
    if (!glink_json_string_is(doc, type, "index")) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::SELECTOR_TYPE_UNSUPPORTED, "selector.type", oid);
    }
    if (value < 0) return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::SELECTOR_VALUE_MISSING, "selector.value", oid);
    req.selector_index = true;
    return read_index(doc, value, 0, GLINK_PROFILE_COUNT_MAX - 1, "selector.value", glink_msg::SELECTOR_VALUE_NOT_INT,
                      glink_msg::INDEX_RANGE, oid, f, req.selector_value);
}

bool read_required_index(const GlinkJsonDoc& doc, int obj, const char* key, const char* missing, const char* not_int,
                         const char* range, int oid, GlinkRequest& req, GlinkFailure& f) {
    const int m = glink_json_find(doc, obj, key);
    if (m < 0) return fail_with(f, GlinkError::MISSING_FIELD, missing, key, oid);
    return read_index(doc, m, 0, GLINK_PROFILE_COUNT_MAX - 1, key, not_int, range, oid, f, req.index);
}

bool parse_select(const GlinkJsonDoc& doc, int obj, int oid, GlinkRequest& req, GlinkFailure& f) {
    static const char* const keys[] = {"op", "index"};
    return check_keys(doc, obj, keys, 2, "", oid, f) &&
           read_required_index(doc, obj, "index", glink_msg::INDEX_MISSING, glink_msg::INDEX_NOT_INT,
                               glink_msg::INDEX_RANGE, oid, req, f);
}

bool parse_update(const GlinkJsonDoc& doc, int obj, int oid, GlinkRequest& req, GlinkFailure& f) {
    static const char* const keys[] = {"op", "index", "dose", "grindSize", "grindUnit"};
    if (!check_keys(doc, obj, keys, 5, "", oid, f)) return false;
    if (!read_required_index(doc, obj, "index", glink_msg::INDEX_MISSING, glink_msg::INDEX_NOT_INT,
                             glink_msg::INDEX_RANGE, oid, req, f)) {
        return false;
    }
    if (req.index != GLINK_PROFILE_WRITABLE_INDEX) {
        return fail_with(f, GlinkError::PROFILE_READ_ONLY, glink_msg::READ_ONLY, "index", oid);
    }
    const int dose = glink_json_find(doc, obj, "dose");
    if (dose >= 0) {
        req.has_dose = true;
        if (!read_tenths(doc, dose, GLINK_DOSE_MIN_TENTHS, GLINK_DOSE_MAX_TENTHS, "dose", glink_msg::DOSE_NOT_NUMBER,
                         glink_msg::DOSE_RANGE, oid, f, req.dose_tenths)) {
            return false;
        }
    }
    const int size = glink_json_find(doc, obj, "grindSize");
    if (size >= 0) {
        req.has_grind_size = true;
        if (doc.nodes[size].type == GlinkJsonType::NULL_VALUE) {
            req.grind_size_null = true;
        } else if (!read_tenths(doc, size, 0, GLINK_GRIND_SIZE_MAX_TENTHS, "grindSize", glink_msg::SIZE_NOT_NUMBER,
                                glink_msg::SIZE_RANGE, oid, f, req.grind_size_tenths)) {
            return false;
        }
    }
    const int unit = glink_json_find(doc, obj, "grindUnit");
    if (unit >= 0 && !read_unit(doc, unit, oid, f, req)) return false;
    if (!req.has_dose && !req.has_grind_size && !req.has_grind_unit) {
        return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::NOTHING_TO_UPDATE, "", oid);
    }
    return true;
}

bool parse_start(const GlinkJsonDoc& doc, int obj, int oid, GlinkRequest& req, GlinkFailure& f) {
    static const char* const keys[] = {"op", "profile", "expectDose"};
    if (!check_keys(doc, obj, keys, 3, "", oid, f)) return false;
    if (!read_required_index(doc, obj, "profile", glink_msg::PROFILE_MISSING, glink_msg::PROFILE_NOT_INT,
                             glink_msg::PROFILE_RANGE, oid, req, f)) {
        return false;
    }
    const int expect = glink_json_find(doc, obj, "expectDose");
    if (expect >= 0) {
        req.has_expect_dose = true;
        return read_tenths(doc, expect, GLINK_DOSE_MIN_TENTHS, GLINK_DOSE_MAX_TENTHS, "expectDose",
                           glink_msg::EXPECT_NOT_NUMBER, glink_msg::EXPECT_RANGE, oid, f, req.expect_dose_tenths);
    }
    return true;
}

}  // namespace

bool glink_parse_request(const char* text, size_t len, GlinkRequest& req, GlinkFailure& f) {
    req = GlinkRequest();
    f = GlinkFailure();
    if (len > GLINK_MAX_REQUEST_BYTES) {
        return fail_with(f, GlinkError::PAYLOAD_TOO_LARGE, glink_msg::TOO_LARGE, "", NO_OBJECT);
    }
    GlinkJsonDoc doc;
    if (!glink_json_parse(text, len, doc)) {
        return fail_with(f, GlinkError::MALFORMED_JSON, glink_msg::MALFORMED, "", NO_OBJECT);
    }

    // Envelope (4.1 step 3): the first top-level member that is not "request" is refused.
    for (int m = glink_json_first_member(doc, 0); m >= 0; m = glink_json_next_member(doc, 0, m)) {
        if (!glink_json_key_is(doc, m, "request")) {
            return fail_key(f, doc, m, "", GlinkError::INVALID_PARAM, glink_msg::UNKNOWN_TOP_LEVEL, NO_OBJECT);
        }
    }
    const int request = glink_json_find(doc, 0, "request");
    if (request < 0) return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::REQUEST_MISSING, "request", NO_OBJECT);
    if (doc.nodes[request].type != GlinkJsonType::OBJECT) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::REQUEST_NOT_OBJECT, "request", NO_OBJECT);
    }
    const int count = doc.nodes[request].members;
    if (count == 0) return fail_with(f, GlinkError::MISSING_FIELD, glink_msg::NO_OBJECT, "request", NO_OBJECT);
    if (count > 1) return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::ONE_OBJECT, "request", NO_OBJECT);

    const int obj_node = glink_json_first_member(doc, request);
    int oid = NO_OBJECT;
    for (int i = 0; i < static_cast<int>(GlinkObject::COUNT); i++) {
        if (glink_json_key_is(doc, obj_node, kObjectNames[i])) oid = i;
    }
    if (oid == NO_OBJECT) return fail_with(f, GlinkError::UNKNOWN_OBJECT, glink_msg::UNKNOWN_OBJECT, "", NO_OBJECT);
    if (doc.nodes[obj_node].type != GlinkJsonType::OBJECT) {
        return fail_with(f, GlinkError::INVALID_PARAM, glink_msg::OBJECT_NOT_OBJECT, kObjectNames[oid], oid);
    }
    req.object = static_cast<GlinkObject>(oid);

    // A stop is executed whatever else the object contains (4.1 step 4).
    if (req.object == GlinkObject::GRIND_CONTROL) {
        const int op = glink_json_find(doc, obj_node, "op");
        if (op >= 0 && glink_json_string_is(doc, op, "stop")) {
            req.op = GlinkOp::STOP;
            return true;
        }
    }

    switch (req.object) {
        case GlinkObject::APP_HELLO: {  // unknown keys are ignored
            static const GlinkOp ops[] = {GlinkOp::HANDSHAKE};
            return read_op(doc, obj_node, oid, ops, 1, req.op, f);
        }
        case GlinkObject::GRIND_RESULT: {  // unknown keys are ignored
            static const GlinkOp ops[] = {GlinkOp::GET};
            return read_op(doc, obj_node, oid, ops, 1, req.op, f);
        }
        case GlinkObject::GRIND_PROFILE: {
            static const GlinkOp ops[] = {GlinkOp::GET, GlinkOp::SELECT, GlinkOp::UPDATE};
            if (!read_op(doc, obj_node, oid, ops, 3, req.op, f)) return false;
            if (req.op == GlinkOp::GET) return parse_get(doc, obj_node, oid, req, f);
            if (req.op == GlinkOp::SELECT) return parse_select(doc, obj_node, oid, req, f);
            return parse_update(doc, obj_node, oid, req, f);
        }
        case GlinkObject::GRIND_CONTROL: {
            static const GlinkOp ops[] = {GlinkOp::START, GlinkOp::STOP};
            if (!read_op(doc, obj_node, oid, ops, 2, req.op, f)) return false;
            return parse_start(doc, obj_node, oid, req, f);
        }
        case GlinkObject::COUNT: break;
    }
    return fail_with(f, GlinkError::UNKNOWN_OBJECT, glink_msg::UNKNOWN_OBJECT, "", NO_OBJECT);
}

//------------------------------------------------------------------------------
// Encoders
//------------------------------------------------------------------------------
namespace {

size_t finish(const GlinkJsonWriter& w, char* out, size_t cap) {
    if (w.ok() && w.length() <= GLINK_MAX_REPLY_BYTES) return w.length();
    if (out != nullptr && cap > 0) out[0] = '\0';
    return 0;
}

// {"response":{"<object>":{"result":"success","data": ...
void begin_success(GlinkJsonWriter& w, GlinkObject object) {
    w.begin_object().key("response").begin_object().key(glink_object_name(object)).begin_object();
    w.key("result").string("success").key("data");
}

void end_success(GlinkJsonWriter& w) { w.end_object().end_object().end_object(); }

void write_entry_fields(GlinkJsonWriter& w, const GlinkProfileEntry& e) {
    w.key("index").integer(e.index);
    w.key("name").string(glink_profile_name(e.index));
    w.key("dose").tenths(e.dose_tenths);
    w.key("grindSize");
    if (e.has_grind_size) {
        w.tenths(e.grind_size_tenths);
    } else {
        w.null();
    }
    w.key("grindUnit");
    if (e.has_grind_size && e.grind_unit[0] != '\0') {
        w.string(e.grind_unit);
    } else {
        w.null();
    }
    w.key("writable").boolean(e.index == GLINK_PROFILE_WRITABLE_INDEX);
}

void write_result_fields(GlinkJsonWriter& w, const GlinkResultInfo& r) {
    w.key("seq").integer(r.seq);
    w.key("code").string(glink_result_code_name(r.code));
    w.key("origin").string(glink_origin_name(r.origin));
    w.key("profile").integer(r.profile);
    w.key("mode").string(glink_mode_name(r.mode));
    w.key("target").tenths(r.target_tenths);
    w.key("finalWeight").tenths(r.final_weight_tenths);
    w.key("weightSettled").boolean(r.weight_settled);
    w.key("pulses").integer(r.pulses);
    w.key("durationMs").integer(r.duration_ms);
    w.key("motorMs").integer(r.motor_ms);
}

}  // namespace

size_t glink_encode_error(char* out, size_t cap, int object, GlinkError code, const char* message, const char* field) {
    GlinkJsonWriter w(out, cap);
    const char* name = (object >= 0 && object < static_cast<int>(GlinkObject::COUNT))
                           ? glink_object_name(static_cast<GlinkObject>(object))
                           : "error";
    w.begin_object().key("response").begin_object().key(name).begin_object();
    w.key("result").string("fail").key("error").begin_object();
    w.key("code").string(glink_error_code(code)).key("message").string(message);
    if (field != nullptr && field[0] != '\0') w.key("field").string(field);
    w.end_object().end_object().end_object().end_object();
    return finish(w, out, cap);
}

size_t glink_encode_failure(char* out, size_t cap, const GlinkFailure& f) {
    return glink_encode_error(out, cap, f.object, f.code, f.message, f.field);
}

size_t glink_encode_profile_list(char* out, size_t cap, const GlinkProfileEntry* entries, size_t count) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_PROFILE);
    w.begin_array();
    for (size_t i = 0; i < count; i++) {
        w.begin_object();
        write_entry_fields(w, entries[i]);
        w.end_object();
    }
    w.end_array();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_profile_entry(char* out, size_t cap, const GlinkProfileEntry& entry) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_PROFILE);
    w.begin_object();
    write_entry_fields(w, entry);
    w.end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_profile_update(char* out, size_t cap, const GlinkProfileEntry& entry, bool changed, bool stored) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_PROFILE);
    w.begin_object();
    write_entry_fields(w, entry);
    w.key("changed").boolean(changed).key("stored").boolean(stored);
    w.end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_select(char* out, size_t cap, uint8_t select_profile) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_PROFILE);
    w.begin_object().key("selectProfile").integer(select_profile).end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_start(char* out, size_t cap, uint32_t lead_in_ms) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_CONTROL);
    w.begin_object().key("leadInMs").integer(lead_in_ms).end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_stop(char* out, size_t cap) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_CONTROL);
    w.begin_object().key("accepted").boolean(true).end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_result_get(char* out, size_t cap, const GlinkResultInfo& result) {
    GlinkJsonWriter w(out, cap);
    begin_success(w, GlinkObject::GRIND_RESULT);
    w.begin_object();
    write_result_fields(w, result);
    w.end_object();
    end_success(w);
    return finish(w, out, cap);
}

size_t glink_encode_base_info(char* out, size_t cap, const GlinkBaseInfo& info) {
    GlinkJsonWriter w(out, cap);
    w.begin_object().key("broadcast").begin_object().key("baseInfo").begin_object();
    w.key("releaseVer").integer(info.release_ver);
    w.key("snCode").string(info.sn_code);
    w.key("wifiName").null();
    w.key("device").string("SGBW");
    w.key("proto").integer(GLINK_PROTO_VERSION);
    w.key("fwVersion").string(info.fw_version);
    w.key("board").string(info.board);
    w.key("doseMin").tenths(GLINK_DOSE_MIN_TENTHS);
    w.key("doseMax").tenths(GLINK_DOSE_MAX_TENTHS);
    w.key("maxRequestBytes").integer(GLINK_MAX_REQUEST_BYTES);
    w.end_object().end_object().end_object();
    return finish(w, out, cap);
}

size_t glink_encode_period_info(char* out, size_t cap, const GlinkPeriodInfo& p) {
    GlinkJsonWriter w(out, cap);
    w.begin_object().key("broadcast").begin_object().key("periodInfo").begin_object();
    w.key("devState").string(glink_dev_state_name(p.dev_state));
    w.key("weight");
    if (p.weight_valid) {
        w.tenths(p.weight_tenths);
    } else {
        w.null();
    }
    w.key("selectProfile").integer(p.select_profile);
    if (p.phase != GlinkPhase::NONE) w.key("phase").string(glink_phase_name(p.phase));
    if (p.has_target) {
        w.key("target").tenths(p.target_tenths);
        w.key("mode").string(glink_mode_name(p.mode));
    }
    if (p.has_lead_in) w.key("leadInMs").integer(p.lead_in_ms);
    w.key("blocked");
    if (p.blocked == GlinkError::NONE) {
        w.null();
    } else {
        w.string(glink_error_code(p.blocked));
    }
    w.key("resultSeq").integer(p.result_seq);
    w.key("profileRev").integer(p.profile_rev);
    w.end_object().end_object().end_object();
    return finish(w, out, cap);
}

size_t glink_encode_result_broadcast(char* out, size_t cap, const GlinkResultInfo& result) {
    GlinkJsonWriter w(out, cap);
    w.begin_object().key("broadcast").begin_object().key("grindResult").begin_object();
    write_result_fields(w, result);
    w.end_object().end_object().end_object();
    return finish(w, out, cap);
}

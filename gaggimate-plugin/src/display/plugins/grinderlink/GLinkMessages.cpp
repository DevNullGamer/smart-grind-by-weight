#include "GLinkMessages.h"
#include <string.h>
#include "bluetooth/glink_json.h"

namespace glinkc {

//------------------------------------------------------------------------------
// Request builders
//------------------------------------------------------------------------------
bool dose_ok(int32_t tenths) { return tenths >= 50 && tenths <= 600; }

bool unit_ok(const char* unit) {
    if (unit == nullptr) return false;
    const size_t n = strlen(unit);
    if (n > GLINK_GRIND_UNIT_MAX_LEN) return false;
    if (n > 0 && (unit[0] == ' ' || unit[n - 1] == ' ')) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = unit[i];
        const bool good = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                          c == '/' || c == '%' || c == '-' || c == '_';
        if (!good) return false;
    }
    return true;
}

bool update_ok(const ProfileUpdate& u) {
    if (!u.has_dose && !u.has_size && !u.has_unit) return false;
    if (u.has_dose && !dose_ok(u.dose_tenths)) return false;
    if (u.has_size && !u.size_null && (u.size_tenths < 0 || u.size_tenths > 9999)) return false;
    if (u.has_unit && !u.unit_null && !unit_ok(u.unit)) return false;
    // A unit needs a size in the same request (the stored size is not known here); null clears both.
    const bool unit_set = u.has_unit && !u.unit_null && u.unit[0] != '\0';
    if (unit_set && !(u.has_size && !u.size_null)) return false;
    if (u.has_unit && u.has_size && u.size_null && unit_set) return false;
    return true;
}

namespace {

// {"request":{"<object>":{ ... the caller writes the members ... }}} up to and including the opening of the object
GlinkJsonWriter& open_request(GlinkJsonWriter& w, const char* object) {
    return w.begin_object().key("request").begin_object().key(object).begin_object();
}

size_t close_request(GlinkJsonWriter& w, char* out, size_t cap) {
    w.end_object().end_object().end_object();
    if (!w.ok() || w.length() + 1 > cap) return 0;
    out[w.length()] = '\0';
    return w.length();
}

bool index_ok(int index) { return index >= 0 && index < PROFILES_MAX; }

}  // namespace

size_t build_hello(char* out, size_t cap) {
    GlinkJsonWriter w(out, cap);
    open_request(w, "appHello").key("op").string("handshake");
    return close_request(w, out, cap);
}

size_t build_profile_get(char* out, size_t cap, int index) {
    if (index >= PROFILES_MAX) return 0;
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindProfile").key("op").string("get");
    if (index >= 0) {
        w.key("selector").begin_object().key("type").string("index").key("value").integer(index).end_object();
    }
    return close_request(w, out, cap);
}

size_t build_profile_select(char* out, size_t cap, int index) {
    if (!index_ok(index)) return 0;
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindProfile").key("op").string("select").key("index").integer(index);
    return close_request(w, out, cap);
}

size_t build_profile_update(char* out, size_t cap, int index, const ProfileUpdate& u) {
    if (!index_ok(index) || !update_ok(u)) return 0;
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindProfile").key("op").string("update").key("index").integer(index);
    if (u.has_dose) w.key("dose").tenths(u.dose_tenths);
    if (u.has_size) {
        w.key("grindSize");
        if (u.size_null) w.null(); else w.tenths(u.size_tenths);
    }
    if (u.has_unit) {
        w.key("grindUnit");
        if (u.unit_null) w.null(); else w.string(u.unit);
    }
    return close_request(w, out, cap);
}

size_t build_start(char* out, size_t cap, int profile, bool has_expect, int32_t expect_tenths) {
    if (!index_ok(profile)) return 0;
    if (has_expect && !dose_ok(expect_tenths)) return 0;
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindControl").key("op").string("start").key("profile").integer(profile);
    if (has_expect) w.key("expectDose").tenths(expect_tenths);
    return close_request(w, out, cap);
}

size_t build_stop(char* out, size_t cap) {
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindControl").key("op").string("stop");
    return close_request(w, out, cap);
}

size_t build_result_get(char* out, size_t cap) {
    GlinkJsonWriter w(out, cap);
    open_request(w, "grindResult").key("op").string("get");
    return close_request(w, out, cap);
}

//------------------------------------------------------------------------------
// Parsing
//------------------------------------------------------------------------------
namespace {

// Typed member readers. Each returns false when the member is absent or has another type; the caller decides whether
// that matters.
bool get_string(const GlinkJsonDoc& d, int obj, const char* key, char* out, size_t cap) {
    const int n = glink_json_find(d, obj, key);
    if (n < 0 || d.nodes[n].type != GlinkJsonType::STRING) return false;
    glink_json_string(d, n, out, cap);
    return true;
}

bool get_tenths(const GlinkJsonDoc& d, int obj, const char* key, int32_t* out) {
    const int n = glink_json_find(d, obj, key);
    if (n < 0 || d.nodes[n].type != GlinkJsonType::NUMBER) return false;
    int64_t t = 0;
    glink_json_number_tenths(d, n, &t);
    *out = static_cast<int32_t>(t);
    return true;
}

bool get_int(const GlinkJsonDoc& d, int obj, const char* key, int64_t* out) {
    const int n = glink_json_find(d, obj, key);
    if (n < 0 || d.nodes[n].type != GlinkJsonType::NUMBER) return false;
    return glink_json_number_integer(d, n, out);
}

bool get_bool(const GlinkJsonDoc& d, int obj, const char* key, bool* out) {
    const int n = glink_json_find(d, obj, key);
    if (n < 0) return false;
    if (d.nodes[n].type == GlinkJsonType::TRUE_VALUE) { *out = true; return true; }
    if (d.nodes[n].type == GlinkJsonType::FALSE_VALUE) { *out = false; return true; }
    return false;
}

uint32_t clamp_u32(int64_t v) { return v < 0 ? 0 : (v > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<uint32_t>(v)); }

uint32_t get_u32(const GlinkJsonDoc& d, int obj, const char* key) {
    int64_t v = 0;
    return get_int(d, obj, key, &v) ? clamp_u32(v) : 0;
}

uint8_t get_profile_index(const GlinkJsonDoc& d, int obj, const char* key) {
    int64_t v = 0;
    return (get_int(d, obj, key, &v) && v >= 0 && v < PROFILES_MAX) ? static_cast<uint8_t>(v) : 0;
}

bool is_object(const GlinkJsonDoc& d, int n) { return n >= 0 && d.nodes[n].type == GlinkJsonType::OBJECT; }

void parse_base(const GlinkJsonDoc& d, int obj, BaseInfo& b) {
    b.release_ver = get_u32(d, obj, "releaseVer");
    get_string(d, obj, "snCode", b.sn_code, sizeof(b.sn_code));
    get_string(d, obj, "device", b.device, sizeof(b.device));
    int64_t v = 0;
    if (get_int(d, obj, "proto", &v)) b.proto = static_cast<int32_t>(v);
    get_string(d, obj, "fwVersion", b.fw_version, sizeof(b.fw_version));
    get_string(d, obj, "board", b.board, sizeof(b.board));
    get_tenths(d, obj, "doseMin", &b.dose_min_tenths);
    get_tenths(d, obj, "doseMax", &b.dose_max_tenths);
    if (get_int(d, obj, "maxRequestBytes", &v)) b.max_request_bytes = static_cast<int32_t>(v);
}

bool parse_period(const GlinkJsonDoc& d, int obj, PeriodInfo& p) {
    // devState is the one field every periodInfo has; without it the message is not usable.
    if (!get_string(d, obj, "devState", p.dev_state, sizeof(p.dev_state)) || p.dev_state[0] == '\0') return false;
    p.weight_valid = get_tenths(d, obj, "weight", &p.weight_tenths);
    p.select_profile = get_profile_index(d, obj, "selectProfile");
    get_string(d, obj, "phase", p.phase, sizeof(p.phase));
    p.has_target = get_tenths(d, obj, "target", &p.target_tenths);
    get_string(d, obj, "mode", p.mode, sizeof(p.mode));
    int64_t v = 0;
    if (get_int(d, obj, "leadInMs", &v)) { p.has_lead_in = true; p.lead_in_ms = clamp_u32(v); }
    get_string(d, obj, "blocked", p.blocked, sizeof(p.blocked));  // null stays ""
    p.result_seq = get_u32(d, obj, "resultSeq");
    p.profile_rev = get_u32(d, obj, "profileRev");
    return true;
}

bool parse_result(const GlinkJsonDoc& d, int obj, ResultInfo& r) {
    if (!get_string(d, obj, "code", r.code, sizeof(r.code)) || r.code[0] == '\0') return false;
    r.seq = get_u32(d, obj, "seq");
    get_string(d, obj, "origin", r.origin, sizeof(r.origin));
    r.profile = get_profile_index(d, obj, "profile");
    get_string(d, obj, "mode", r.mode, sizeof(r.mode));
    get_tenths(d, obj, "target", &r.target_tenths);
    get_tenths(d, obj, "finalWeight", &r.final_weight_tenths);
    get_bool(d, obj, "weightSettled", &r.weight_settled);
    r.pulses = get_u32(d, obj, "pulses");
    r.duration_ms = get_u32(d, obj, "durationMs");
    r.motor_ms = get_u32(d, obj, "motorMs");
    return true;
}

bool parse_entry(const GlinkJsonDoc& d, int obj, ProfileEntry& e) {
    int64_t idx = 0;
    if (!get_int(d, obj, "index", &idx) || idx < 0 || idx >= PROFILES_MAX) return false;
    e.index = static_cast<uint8_t>(idx);
    get_string(d, obj, "name", e.name, sizeof(e.name));
    if (!get_tenths(d, obj, "dose", &e.dose_tenths)) return false;
    e.has_grind_size = get_tenths(d, obj, "grindSize", &e.grind_size_tenths);
    get_string(d, obj, "grindUnit", e.grind_unit, sizeof(e.grind_unit));  // null stays ""
    get_bool(d, obj, "writable", &e.writable);
    return true;
}

// Finds `"data":[` in the text; returns the offset of the '[' or -1. The device writes no whitespace.
int find_data_array(const char* text, size_t len) {
    static const char kKey[] = "\"data\":[";
    const size_t k = sizeof(kKey) - 1;
    for (size_t i = 0; i + k <= len; i++) {
        if (memcmp(text + i, kKey, k) == 0) return static_cast<int>(i + k - 1);
    }
    return -1;
}

// Parses the entries of the array starting at text[open] ('['). Entries are flat objects (no brackets or braces inside
// strings: the vocabulary and grindUnit charset exclude them). Returns the offset one past the closing ']' or -1.
int parse_entry_array(const char* text, size_t len, size_t open, Response& r) {
    size_t i = open + 1;
    r.entries_are_list = true;
    r.entry_count = 0;
    while (i < len) {
        const char c = text[i];
        if (c == ']') return static_cast<int>(i + 1);
        if (c == ',' || c == ' ') { i++; continue; }
        if (c != '{') return -1;
        size_t j = i + 1;
        while (j < len && text[j] != '}') {
            if (text[j] == '{' || text[j] == '[') return -1;
            j++;
        }
        if (j >= len) return -1;
        GlinkJsonDoc ed;
        if (!glink_json_parse(text + i, j - i + 1, ed)) return -1;
        if (r.entry_count >= PROFILES_MAX) return -1;
        ProfileEntry& e = r.entries[r.entry_count];
        e = ProfileEntry();
        if (!parse_entry(ed, 0, e)) return -1;
        r.entry_count++;
        i = j + 1;
    }
    return -1;
}

void parse_failure(const GlinkJsonDoc& d, int err, Failure& f) {
    get_string(d, err, "code", f.code, sizeof(f.code));
    get_string(d, err, "message", f.message, sizeof(f.message));
    get_string(d, err, "field", f.field, sizeof(f.field));
}

bool parse_data(const GlinkJsonDoc& d, int data, Response& r) {
    int64_t v = 0;
    if (get_int(d, data, "selectProfile", &v)) {
        r.has_select = true;
        r.select_profile = (v >= 0 && v < PROFILES_MAX) ? static_cast<uint8_t>(v) : 0;
        return true;
    }
    if (get_int(d, data, "leadInMs", &v)) {
        r.has_lead_in = true;
        r.lead_in_ms = clamp_u32(v);
        return true;
    }
    if (get_bool(d, data, "accepted", &r.accepted)) {
        r.has_accepted = true;
        return true;
    }
    if (glink_json_find(d, data, "seq") >= 0) {
        r.has_result = parse_result(d, data, r.result);
        return r.has_result;
    }
    if (glink_json_find(d, data, "index") >= 0) {
        if (!parse_entry(d, data, r.entries[0])) return false;
        r.entry_count = 1;
        r.has_update_flags = get_bool(d, data, "changed", &r.changed) && get_bool(d, data, "stored", &r.stored);
        return true;
    }
    return false;
}

bool parse_response(const GlinkJsonDoc& d, int obj, const char* object_name, bool had_array, Response& r) {
    strncpy(r.object, object_name, sizeof(r.object) - 1);
    char result[STR] = {};
    if (!get_string(d, obj, "result", result, sizeof(result))) return false;
    if (strcmp(result, "fail") == 0) {
        const int err = glink_json_find(d, obj, "error");
        if (!is_object(d, err)) return false;
        parse_failure(d, err, r.failure);
        return r.failure.code[0] != '\0';
    }
    if (strcmp(result, "success") != 0) return false;
    r.success = true;
    if (had_array) return true;  // the entries were parsed from the array
    const int data = glink_json_find(d, obj, "data");
    if (data < 0) return true;  // e.g. a success without data: nothing more to read
    return is_object(d, data) ? parse_data(d, data, r) : false;
}

// The reader allows strings up to 32 bytes; the device's error `message` texts are longer (up to about 50). The text is
// not a contract: keep what fits in Failure::message and cut the rest out of the copy the reader sees.
size_t shorten_message(char* env, size_t len, char* kept, size_t kept_cap) {
    static const char kKey[] = "\"message\":\"";
    const size_t k = sizeof(kKey) - 1;
    for (size_t i = 0; i + k <= len; i++) {
        if (memcmp(env + i, kKey, k) != 0) continue;
        const size_t start = i + k;
        size_t end = start;
        while (end < len && env[end] != '"') end++;
        if (end >= len) return len;  // unterminated: the reader refuses it
        size_t n = end - start;
        if (n >= kept_cap) n = kept_cap - 1;
        memcpy(kept, env + start, n);
        kept[n] = 0;
        if (end - start > GLINK_JSON_STRING_MAX_LEN) {
            const size_t cut_from = start + GLINK_JSON_STRING_MAX_LEN;
            memmove(env + cut_from, env + end, len - end);
            len -= end - cut_from;
        }
        return len;
    }
    return len;
}

}  // namespace

bool parse_message(const char* text, size_t len, Message& out) {
    out = Message();
    if (text == nullptr || len == 0 || len > MSG_TEXT_MAX) return false;

    // Work on a copy: a list of profiles holds an array, which the shared reader refuses (parse the entries first and
    // let the reader see `"data":{}` in its place), and long error messages are shortened.
    char env[MSG_TEXT_MAX + 4];
    size_t doc_len = len;
    bool had_array = false;
    const int open = find_data_array(text, len);
    if (open >= 0) {
        const int after = parse_entry_array(text, len, static_cast<size_t>(open), out.response);
        if (after < 0) return false;
        memcpy(env, text, static_cast<size_t>(open));
        env[open] = '{';
        env[open + 1] = '}';
        memcpy(env + open + 2, text + after, len - static_cast<size_t>(after));
        doc_len = static_cast<size_t>(open) + 2 + (len - static_cast<size_t>(after));
        had_array = true;
    } else {
        memcpy(env, text, len);
    }
    char kept_message[sizeof(Failure::message)] = {};
    doc_len = shorten_message(env, doc_len, kept_message, sizeof(kept_message));
    const char* doc_text = env;

    GlinkJsonDoc d;
    if (!glink_json_parse(doc_text, doc_len, d)) return false;
    const int top = glink_json_first_member(d, 0);
    if (top < 0 || glink_json_next_member(d, 0, top) >= 0 || !is_object(d, top)) return false;
    const int inner = glink_json_first_member(d, top);
    if (inner < 0 || glink_json_next_member(d, top, inner) >= 0 || !is_object(d, inner)) return false;
    char name[STR] = {};
    glink_json_key(d, inner, name, sizeof(name));

    if (glink_json_key_is(d, top, "broadcast")) {
        if (had_array) return false;
        if (strcmp(name, "baseInfo") == 0) {
            parse_base(d, inner, out.base);
            out.kind = Kind::BASE_INFO;
            return true;
        }
        if (strcmp(name, "periodInfo") == 0) {
            if (!parse_period(d, inner, out.period)) return false;
            out.kind = Kind::PERIOD_INFO;
            return true;
        }
        if (strcmp(name, "grindResult") == 0) {
            if (!parse_result(d, inner, out.result)) return false;
            out.kind = Kind::GRIND_RESULT;
            return true;
        }
        return false;  // an unknown broadcast object is ignored by the caller
    }
    if (glink_json_key_is(d, top, "response")) {
        // out.response already holds the list entries when there was an array
        if (!parse_response(d, inner, name, had_array, out.response)) return false;
        if (kept_message[0] != 0) strcpy(out.response.failure.message, kept_message);
        out.kind = Kind::RESPONSE;
        return true;
    }
    return false;
}

bool failure_is_retryable(const char* code) {
    return code != nullptr && (strcmp(code, "BUSY") == 0 || strcmp(code, "UI_BUSY") == 0 || strcmp(code, "RATE_LIMITED") == 0);
}

}  // namespace glinkc

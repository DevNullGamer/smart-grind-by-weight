#pragma once
#include <stddef.h>
#include <stdint.h>
#include "config/bluetooth.h"

// GLink client side messages (docs/GLINK-PROTOCOL.md sections 4 to 6): request builders and parsers for the
// responses and broadcasts a grinder sends. Pure: no Arduino/ESP/NimBLE dependencies, no heap. Built on the shared
// bluetooth/glink_json.* (byte-identical with the grinder firmware). Numbers are scaled integers in tenths.
//
// The device writes only the strict subset of JSON the shared reader accepts; the one thing the reader refuses that a
// device sends is the array in a `grindProfile` `get` of all profiles, which is cut out and parsed entry by entry.
// Strings are kept as text and unknown values are tolerated (spec 6: unknown devState, phase, blocked, code, origin).

namespace glinkc {

constexpr size_t STR = GLINK_JSON_STRING_MAX_LEN + 1;  // a string value as NUL-terminated text
constexpr size_t MSG_TEXT_MAX = GLINK_MAX_REPLY_BYTES;
constexpr int PROFILES_MAX = GLINK_PROFILE_COUNT_MAX;

//------------------------------------------------------------------------------
// Request builders. Each returns the message length (NUL-terminated, NUL not counted), or 0 on invalid arguments or if
// it does not fit in cap. Arguments are checked with the same limits the grinder enforces (spec 4.4 and 5.4), so a
// request that leaves here is not refused for its values.
//------------------------------------------------------------------------------
struct ProfileUpdate {
    bool has_dose = false;
    int32_t dose_tenths = 0;  // 50 to 600
    bool has_size = false;
    bool size_null = false;   // true clears the size and the unit
    int32_t size_tenths = 0;  // 0 to 9999
    bool has_unit = false;
    bool unit_null = false;
    char unit[GLINK_GRIND_UNIT_MAX_LEN + 1] = {};
};

// True for a dose in tenths inside 5.0 to 60.0 g.
bool dose_ok(int32_t tenths);
// True for a unit of at most 6 characters of A-Z a-z 0-9 space . / % - _ (no trimming here: leading and trailing
// spaces are refused so what is sent equals what is stored).
bool unit_ok(const char* unit);
// The arguments of an update: at least one field, ranges, the unit only together with a size.
bool update_ok(const ProfileUpdate& update);

size_t build_hello(char* out, size_t cap);
size_t build_profile_get(char* out, size_t cap, int index);  // index < 0: all profiles
size_t build_profile_select(char* out, size_t cap, int index);
size_t build_profile_update(char* out, size_t cap, int index, const ProfileUpdate& update);
size_t build_start(char* out, size_t cap, int profile, bool has_expect, int32_t expect_tenths);
size_t build_stop(char* out, size_t cap);
size_t build_result_get(char* out, size_t cap);

//------------------------------------------------------------------------------
// Parsed messages
//------------------------------------------------------------------------------
struct BaseInfo {
    uint32_t release_ver = 0;
    char sn_code[STR] = {};
    char device[STR] = {};
    int32_t proto = 0;
    char fw_version[STR] = {};
    char board[STR] = {};
    int32_t dose_min_tenths = 0;
    int32_t dose_max_tenths = 0;
    int32_t max_request_bytes = 0;
};

struct PeriodInfo {
    char dev_state[STR] = {};
    bool weight_valid = false;  // false when weight is null
    int32_t weight_tenths = 0;
    uint8_t select_profile = 0;
    char phase[STR] = {};  // "" = absent
    bool has_target = false;
    int32_t target_tenths = 0;
    char mode[STR] = {};
    bool has_lead_in = false;
    uint32_t lead_in_ms = 0;
    char blocked[STR] = {};  // "" = null
    uint32_t result_seq = 0;
    uint32_t profile_rev = 0;
};

struct ResultInfo {
    uint32_t seq = 0;
    char code[STR] = {};
    char origin[STR] = {};
    uint8_t profile = 0;
    char mode[STR] = {};
    int32_t target_tenths = 0;
    int32_t final_weight_tenths = 0;
    bool weight_settled = false;
    uint32_t pulses = 0;
    uint32_t duration_ms = 0;
    uint32_t motor_ms = 0;
};

struct ProfileEntry {
    uint8_t index = 0;
    char name[STR] = {};
    int32_t dose_tenths = 0;
    bool has_grind_size = false;
    int32_t grind_size_tenths = 0;
    char grind_unit[STR] = {};  // "" = none
    bool writable = false;
};

struct Failure {
    char code[STR] = {};
    char message[64] = {};
    char field[48] = {};
};

// A response. `object` is the object name it answers ("error" for an unaddressable failure). Success data is
// recognised by shape; the has_* flags say which part is present.
struct Response {
    char object[STR] = {};
    bool success = false;
    Failure failure;
    bool has_select = false;
    uint8_t select_profile = 0;
    bool has_lead_in = false;
    uint32_t lead_in_ms = 0;
    bool has_accepted = false;
    bool accepted = false;
    bool has_result = false;
    ResultInfo result;
    int entry_count = 0;  // profile entries: all (array), one, or the updated one
    bool entries_are_list = false;
    ProfileEntry entries[PROFILES_MAX];
    bool has_update_flags = false;  // `changed` and `stored` of an update
    bool changed = false;
    bool stored = false;
};

enum class Kind : uint8_t { NONE, BASE_INFO, PERIOD_INFO, GRIND_RESULT, RESPONSE };

struct Message {
    Kind kind = Kind::NONE;
    BaseInfo base;
    PeriodInfo period;
    ResultInfo result;  // GRIND_RESULT broadcast
    Response response;
};

// Parses one complete message. false = not a GLink message this client understands (nothing in `out` is to be used).
bool parse_message(const char* text, size_t len, Message& out);

// Errors the spec says to retry shortly (BUSY, UI_BUSY) or after 1 s (RATE_LIMITED).
bool failure_is_retryable(const char* code);

}  // namespace glinkc

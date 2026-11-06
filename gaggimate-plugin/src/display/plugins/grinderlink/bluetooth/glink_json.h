#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"

// Bounded, allocation-free JSON reader and writer for GLink (docs/GLINK-PROTOCOL.md section 4.2). Pure: no
// Arduino/ESP dependencies, unit tested on the host.
//
// The reader accepts only the strict subset of the spec: one object, no string escapes, keys A-Za-z0-9_ (1 to
// GLINK_JSON_KEY_MAX_LEN), no duplicate keys, string values up to GLINK_JSON_STRING_MAX_LEN bytes, at most
// GLINK_JSON_MAX_DEPTH nested objects, no arrays, numbers without exponent (at most GLINK_JSON_NUMBER_MAX_LEN
// characters), literals true/false/null. The parsed document is a flat node table that points into the input text
// (the text must outlive the document).

//------------------------------------------------------------------------------
// Reader
//------------------------------------------------------------------------------
// Upper bound of nodes: a member needs at least 6 bytes of text ("a":1,), so a request of at most
// GLINK_MAX_REQUEST_BYTES holds at most 86 members plus the root; longer input simply fails if it runs out.
constexpr int GLINK_JSON_MAX_NODES = 96;

enum class GlinkJsonType : uint8_t { OBJECT, STRING, NUMBER, TRUE_VALUE, FALSE_VALUE, NULL_VALUE };

struct GlinkJsonNode {
    GlinkJsonType type;
    uint8_t key_len;    // 0 for the root
    uint8_t end;        // OBJECT: index one past the last node of its subtree; other types: own index + 1
    uint8_t members;    // OBJECT: number of members
    uint16_t key_off;   // offset of the key text in the input
    uint16_t val_off;   // STRING: first content byte; NUMBER: first character of the token
    uint16_t val_len;
};

struct GlinkJsonDoc {
    const char* text = nullptr;
    size_t len = 0;
    GlinkJsonNode nodes[GLINK_JSON_MAX_NODES];
    int count = 0;
};

// Strict parse. false = MALFORMED_JSON (any violation, never an overrun). The root (index 0) is an OBJECT.
bool glink_json_parse(const char* text, size_t len, GlinkJsonDoc& doc);

// Members of an object node, in message order. -1 = none / end.
int glink_json_first_member(const GlinkJsonDoc& doc, int object);
int glink_json_next_member(const GlinkJsonDoc& doc, int object, int member);
// Member by key, -1 if absent.
int glink_json_find(const GlinkJsonDoc& doc, int object, const char* key);

bool glink_json_key_is(const GlinkJsonDoc& doc, int node, const char* key);
// Copies the key into out (NUL-terminated, at most cap-1 bytes); returns its length.
size_t glink_json_key(const GlinkJsonDoc& doc, int node, char* out, size_t cap);
bool glink_json_string_is(const GlinkJsonDoc& doc, int node, const char* value);
// Copies a STRING value into out (NUL-terminated); returns its length. Values are at most 32 bytes.
size_t glink_json_string(const GlinkJsonDoc& doc, int node, char* out, size_t cap);

// NUMBER helpers. Both work on the decimal digits as written (never on a binary float).
// tenths: the value rounded half away from zero to 0.1, as a scaled integer (18.05 -> 181, -0.04 -> 0).
void glink_json_number_tenths(const GlinkJsonDoc& doc, int node, int64_t* tenths);
// integer: true when the token is a whole number (3, 3.0, -0); *value is that number (up to 15 digits, no wrap).
bool glink_json_number_integer(const GlinkJsonDoc& doc, int node, int64_t* value);

//------------------------------------------------------------------------------
// Writer
//------------------------------------------------------------------------------
// Compact JSON into a caller buffer, never beyond its capacity. After an overflow ok() is false and the output is
// not to be used. Strings are escaped defensively (quote, backslash, control bytes), although GLink only ever
// writes fixed vocabulary and the validated grindUnit.
class GlinkJsonWriter {
public:
    GlinkJsonWriter(char* buf, size_t cap);

    GlinkJsonWriter& begin_object();
    GlinkJsonWriter& end_object();
    GlinkJsonWriter& begin_array();
    GlinkJsonWriter& end_array();
    GlinkJsonWriter& key(const char* name);
    GlinkJsonWriter& string(const char* value);
    GlinkJsonWriter& integer(int64_t value);
    GlinkJsonWriter& tenths(int32_t value);  // scaled by 10, always one decimal: 180 -> 18.0, -3 -> -0.3
    GlinkJsonWriter& boolean(bool value);
    GlinkJsonWriter& null();

    bool ok() const { return ok_; }
    size_t length() const { return len_; }

private:
    static constexpr int MAX_DEPTH = 8;

    void value_start();
    void put(char c);
    void put_text(const char* s);

    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool ok_ = true;
    bool after_key_ = false;
    int depth_ = 0;
    bool need_comma_[MAX_DEPTH + 1] = {};
};

#include "glink_json.h"
#include <string.h>

//------------------------------------------------------------------------------
// Reader
//------------------------------------------------------------------------------
namespace {

struct Parser {
    const char* t;
    size_t n;
    size_t i;
    GlinkJsonDoc* d;
};

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '_';
}

void skip_ws(Parser& p) {
    while (p.i < p.n && is_ws(p.t[p.i])) p.i++;
}

int new_node(Parser& p, GlinkJsonType type) {
    if (p.d->count >= GLINK_JSON_MAX_NODES) return -1;
    GlinkJsonNode& nd = p.d->nodes[p.d->count];
    nd.type = type;
    nd.key_len = 0;
    nd.end = static_cast<uint8_t>(p.d->count + 1);
    nd.members = 0;
    nd.key_off = 0;
    nd.val_off = 0;
    nd.val_len = 0;
    return p.d->count++;
}

bool key_equals(const Parser& p, const GlinkJsonNode& a, size_t off, size_t len) {
    return a.key_len == len && memcmp(p.t + a.key_off, p.t + off, len) == 0;
}

// At the opening quote of a key. On success sets the key of `node` and leaves p.i after the closing quote.
bool parse_key(Parser& p, int node, int object) {
    if (p.i >= p.n || p.t[p.i] != '"') return false;
    p.i++;
    const size_t start = p.i;
    while (p.i < p.n && p.t[p.i] != '"') {
        if (!is_key_char(p.t[p.i])) return false;
        p.i++;
    }
    if (p.i >= p.n) return false;
    const size_t len = p.i - start;
    if (len < 1 || len > GLINK_JSON_KEY_MAX_LEN) return false;
    p.i++;
    // Duplicate keys among the members read so far.
    for (int c = object + 1; c < node; c = p.d->nodes[c].end) {
        if (key_equals(p, p.d->nodes[c], start, len)) return false;
    }
    p.d->nodes[node].key_off = static_cast<uint16_t>(start);
    p.d->nodes[node].key_len = static_cast<uint8_t>(len);
    return true;
}

bool parse_string(Parser& p, int node) {
    p.i++;  // opening quote
    const size_t start = p.i;
    while (p.i < p.n && p.t[p.i] != '"') {
        const unsigned char c = static_cast<unsigned char>(p.t[p.i]);
        if (c < 0x20 || c == '\\') return false;  // raw control bytes and every escape are refused
        p.i++;
    }
    if (p.i >= p.n) return false;
    const size_t len = p.i - start;
    if (len > GLINK_JSON_STRING_MAX_LEN) return false;
    p.i++;
    p.d->nodes[node].val_off = static_cast<uint16_t>(start);
    p.d->nodes[node].val_len = static_cast<uint16_t>(len);
    return true;
}

// -?(0|[1-9][0-9]*)(\.[0-9]+)? up to GLINK_JSON_NUMBER_MAX_LEN characters.
bool parse_number(Parser& p, int node) {
    const size_t start = p.i;
    if (p.i < p.n && p.t[p.i] == '-') p.i++;
    if (p.i >= p.n || !is_digit(p.t[p.i])) return false;
    if (p.t[p.i] == '0') {
        p.i++;
    } else {
        while (p.i < p.n && is_digit(p.t[p.i])) p.i++;
    }
    if (p.i < p.n && p.t[p.i] == '.') {
        p.i++;
        if (p.i >= p.n || !is_digit(p.t[p.i])) return false;
        while (p.i < p.n && is_digit(p.t[p.i])) p.i++;
    }
    const size_t len = p.i - start;
    if (len > GLINK_JSON_NUMBER_MAX_LEN) return false;
    p.d->nodes[node].val_off = static_cast<uint16_t>(start);
    p.d->nodes[node].val_len = static_cast<uint16_t>(len);
    return true;
}

bool parse_literal(Parser& p, int node, const char* word, GlinkJsonType type) {
    const size_t len = strlen(word);
    if (p.n - p.i < len || memcmp(p.t + p.i, word, len) != 0) return false;
    p.i += len;
    p.d->nodes[node].type = type;
    return true;
}

bool parse_object(Parser& p, int node, int depth);

bool parse_value(Parser& p, int node, int depth) {
    if (p.i >= p.n) return false;
    const char c = p.t[p.i];
    if (c == '{') return parse_object(p, node, depth + 1);
    if (c == '"') {
        p.d->nodes[node].type = GlinkJsonType::STRING;
        return parse_string(p, node);
    }
    if (c == '-' || is_digit(c)) {
        p.d->nodes[node].type = GlinkJsonType::NUMBER;
        return parse_number(p, node);
    }
    if (c == 't') return parse_literal(p, node, "true", GlinkJsonType::TRUE_VALUE);
    if (c == 'f') return parse_literal(p, node, "false", GlinkJsonType::FALSE_VALUE);
    if (c == 'n') return parse_literal(p, node, "null", GlinkJsonType::NULL_VALUE);
    return false;  // arrays and everything else
}

// p.i is at '{'; `node` is this object's node; depth counts this object (the root is 1).
bool parse_object(Parser& p, int node, int depth) {
    if (depth > GLINK_JSON_MAX_DEPTH) return false;
    p.d->nodes[node].type = GlinkJsonType::OBJECT;
    p.i++;
    skip_ws(p);
    if (p.i < p.n && p.t[p.i] == '}') {
        p.i++;
        p.d->nodes[node].end = static_cast<uint8_t>(p.d->count);
        return true;
    }
    int members = 0;
    while (true) {
        skip_ws(p);
        const int child = new_node(p, GlinkJsonType::NULL_VALUE);
        if (child < 0) return false;
        if (!parse_key(p, child, node)) return false;
        skip_ws(p);
        if (p.i >= p.n || p.t[p.i] != ':') return false;
        p.i++;
        skip_ws(p);
        if (!parse_value(p, child, depth)) return false;
        p.d->nodes[child].end = static_cast<uint8_t>(p.d->count);
        members++;
        skip_ws(p);
        if (p.i >= p.n) return false;
        if (p.t[p.i] == ',') {
            p.i++;
            continue;
        }
        if (p.t[p.i] == '}') {
            p.i++;
            break;
        }
        return false;
    }
    p.d->nodes[node].members = static_cast<uint8_t>(members);
    p.d->nodes[node].end = static_cast<uint8_t>(p.d->count);
    return true;
}

}  // namespace

bool glink_json_parse(const char* text, size_t len, GlinkJsonDoc& doc) {
    doc.text = text;
    doc.len = len;
    doc.count = 0;
    if (text == nullptr || len == 0 || len > 0xFFFF) return false;
    Parser p{text, len, 0, &doc};
    skip_ws(p);
    if (p.i >= p.n || p.t[p.i] != '{') return false;
    const int root = new_node(p, GlinkJsonType::OBJECT);
    if (root < 0 || !parse_object(p, root, 1)) return false;
    skip_ws(p);
    return p.i == p.n;
}

int glink_json_first_member(const GlinkJsonDoc& doc, int object) {
    if (object < 0 || object >= doc.count || doc.nodes[object].type != GlinkJsonType::OBJECT) return -1;
    return doc.nodes[object].members > 0 ? object + 1 : -1;
}

int glink_json_next_member(const GlinkJsonDoc& doc, int object, int member) {
    const int next = doc.nodes[member].end;
    return next < doc.nodes[object].end ? next : -1;
}

int glink_json_find(const GlinkJsonDoc& doc, int object, const char* key) {
    for (int m = glink_json_first_member(doc, object); m >= 0; m = glink_json_next_member(doc, object, m)) {
        if (glink_json_key_is(doc, m, key)) return m;
    }
    return -1;
}

bool glink_json_key_is(const GlinkJsonDoc& doc, int node, const char* key) {
    const GlinkJsonNode& nd = doc.nodes[node];
    return strlen(key) == nd.key_len && memcmp(doc.text + nd.key_off, key, nd.key_len) == 0;
}

size_t glink_json_key(const GlinkJsonDoc& doc, int node, char* out, size_t cap) {
    const GlinkJsonNode& nd = doc.nodes[node];
    size_t n = nd.key_len;
    if (cap == 0) return 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, doc.text + nd.key_off, n);
    out[n] = '\0';
    return n;
}

bool glink_json_string_is(const GlinkJsonDoc& doc, int node, const char* value) {
    const GlinkJsonNode& nd = doc.nodes[node];
    return nd.type == GlinkJsonType::STRING && strlen(value) == nd.val_len &&
           memcmp(doc.text + nd.val_off, value, nd.val_len) == 0;
}

size_t glink_json_string(const GlinkJsonDoc& doc, int node, char* out, size_t cap) {
    const GlinkJsonNode& nd = doc.nodes[node];
    size_t n = nd.val_len;
    if (cap == 0) return 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, doc.text + nd.val_off, n);
    out[n] = '\0';
    return n;
}

void glink_json_number_tenths(const GlinkJsonDoc& doc, int node, int64_t* tenths) {
    const GlinkJsonNode& nd = doc.nodes[node];
    const char* s = doc.text + nd.val_off;
    const char* e = s + nd.val_len;
    bool neg = false;
    if (s < e && *s == '-') {
        neg = true;
        s++;
    }
    int64_t whole = 0;
    while (s < e && is_digit(*s)) whole = whole * 10 + (*s++ - '0');  // at most 15 characters: no overflow
    int64_t t = whole * 10;
    if (s < e && *s == '.') {
        s++;
        if (s < e) t += *s++ - '0';
        if (s < e && *s >= '5') t++;  // second decimal digit decides, half away from zero
    }
    *tenths = neg ? -t : t;
}

bool glink_json_number_integer(const GlinkJsonDoc& doc, int node, int64_t* value) {
    const GlinkJsonNode& nd = doc.nodes[node];
    const char* s = doc.text + nd.val_off;
    const char* e = s + nd.val_len;
    bool neg = false;
    if (s < e && *s == '-') {
        neg = true;
        s++;
    }
    int64_t whole = 0;
    while (s < e && is_digit(*s)) whole = whole * 10 + (*s++ - '0');
    if (s < e && *s == '.') {
        s++;
        while (s < e) {
            if (*s++ != '0') return false;
        }
    }
    *value = neg ? -whole : whole;
    return true;
}

//------------------------------------------------------------------------------
// Writer
//------------------------------------------------------------------------------
GlinkJsonWriter::GlinkJsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {
    if (cap_ > 0) buf_[0] = '\0';
}

void GlinkJsonWriter::put(char c) {
    if (len_ + 1 >= cap_) {  // keep room for the terminating NUL
        ok_ = false;
        return;
    }
    buf_[len_++] = c;
    buf_[len_] = '\0';
}

void GlinkJsonWriter::put_text(const char* s) {
    while (*s) put(*s++);
}

void GlinkJsonWriter::value_start() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (need_comma_[depth_]) put(',');
    need_comma_[depth_] = true;
}

GlinkJsonWriter& GlinkJsonWriter::begin_object() {
    value_start();
    put('{');
    if (depth_ < MAX_DEPTH) {
        depth_++;
        need_comma_[depth_] = false;
    } else {
        ok_ = false;
    }
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::end_object() {
    put('}');
    if (depth_ > 0) depth_--;
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::begin_array() {
    value_start();
    put('[');
    if (depth_ < MAX_DEPTH) {
        depth_++;
        need_comma_[depth_] = false;
    } else {
        ok_ = false;
    }
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::end_array() {
    put(']');
    if (depth_ > 0) depth_--;
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::key(const char* name) {
    if (need_comma_[depth_]) put(',');
    need_comma_[depth_] = true;
    put('"');
    put_text(name);
    put('"');
    put(':');
    after_key_ = true;
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::string(const char* value) {
    value_start();
    put('"');
    for (const char* s = value; *s; s++) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            put('\\');
            put(static_cast<char>(c));
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            put_text("\\u00");
            put(hex[c >> 4]);
            put(hex[c & 0xF]);
        } else {
            put(static_cast<char>(c));
        }
    }
    put('"');
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::integer(int64_t value) {
    value_start();
    char tmp[24];
    int n = 0;
    uint64_t u = value < 0 ? static_cast<uint64_t>(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        tmp[n++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u > 0);
    if (value < 0) put('-');
    while (n > 0) put(tmp[--n]);
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::tenths(int32_t value) {
    value_start();
    int64_t v = value;
    if (v < 0) {
        put('-');
        v = -v;
    }
    char tmp[24];
    int n = 0;
    int64_t whole = v / 10;
    do {
        tmp[n++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole > 0);
    while (n > 0) put(tmp[--n]);
    put('.');
    put(static_cast<char>('0' + v % 10));
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::boolean(bool value) {
    value_start();
    put_text(value ? "true" : "false");
    return *this;
}

GlinkJsonWriter& GlinkJsonWriter::null() {
    value_start();
    put_text("null");
    return *this;
}

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "bluetooth/glink_json.h"
#include "glink_vectors.h"

// Host tests of the GLink JSON reader and writer (src/bluetooth/glink_json.*): the conformance reader cases, the
// number helpers and fuzz-style malformed inputs.

static bool parses(const std::string& text) {
    GlinkJsonDoc doc;
    return glink_json_parse(text.data(), text.size(), doc);
}

static void test_reader_cases() {
    const GJson& cases = glink_vectors().at("reader_cases");
    for (const GJson& c : cases.a) {
        TEST_ASSERT_EQUAL_MESSAGE(c.at("accepted").b, parses(c.at("text").s), c.at("name").s.c_str());
    }
}

static void test_reader_extra_rules() {
    TEST_ASSERT_TRUE(parses("{}"));
    TEST_ASSERT_TRUE(parses("{\"a\":{}}"));
    // Numbers: JSON grammar without exponent, at most 15 characters.
    for (const char* ok : {"0", "-0", "5", "-5", "0.5", "-0.5", "18.04", "123456789012345", "1234567890123.5", "0.0000000001"}) {
        TEST_ASSERT_TRUE_MESSAGE(parses(std::string("{\"a\":") + ok + "}"), ok);
    }
    for (const char* bad : {"1e999", "1E5", "1e2", ".5", "5.", "-", "-.5", "+1", "01", "00", "-01", "0x10", "NaN", "Infinity",
                            "-Infinity", "1_0", "1,5", "1.5.5", "1234567890123456", "12345678901234.5", "--1", "1-"}) {
        TEST_ASSERT_FALSE_MESSAGE(parses(std::string("{\"a\":") + bad + "}"), bad);
    }
    // Strings and structure.
    for (const char* bad : {"{\"a\":\"x}", "{\"a\":\"x\"", "{\"a\"", "{\"a\":", "{", "}", "{\"a\":1 \"b\":2}", "{\"a\":1;}",
                            "{\"a\":nul}", "{\"a\":nulll}", "{\"a\":tru}", "{\"a\":truee}", "{\"a\":falsey}",
                            "{\"a\":{\"b\":}}", "{\"a\":{,}}", "{,\"a\":1}", "{\"a\":1}{\"b\":2}", "{\"a\":\"\xff\x01\"}"}) {
        TEST_ASSERT_FALSE_MESSAGE(parses(bad), bad);
    }
    // Bytes of 0x80 and above are not validated by the reader.
    TEST_ASSERT_TRUE(parses("{\"a\":\"\xff\xfe\"}"));
    // A NUL anywhere is refused: inside a string it is a control byte, after the object it is not whitespace.
    TEST_ASSERT_FALSE(parses(std::string("{\"a\":\"x") + '\0' + "\"}"));
    TEST_ASSERT_FALSE(parses(std::string("{\"a\":1}") + '\0'));
    TEST_ASSERT_FALSE(parses(std::string("{\"a\":1") + '\0' + "}"));
    // Null and zero length input.
    GlinkJsonDoc doc;
    TEST_ASSERT_FALSE(glink_json_parse(nullptr, 5, doc));
    TEST_ASSERT_FALSE(glink_json_parse("{}", 0, doc));
    // The reader honours the length, not a terminator: trailing bytes beyond len are not looked at.
    TEST_ASSERT_TRUE(glink_json_parse("{\"a\":1}garbage", 7, doc));
}

static void test_depth_and_node_limits() {
    TEST_ASSERT_TRUE(parses("{\"a\":{\"b\":{\"c\":{}}}}"));            // depth 4
    TEST_ASSERT_FALSE(parses("{\"a\":{\"b\":{\"c\":{\"d\":{}}}}}"));    // depth 5
    // 86 members of 6 bytes fit in the node table; a larger count fails without overrunning anything.
    std::string many = "{";
    for (int i = 0; i < 86; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%02d", i);
        many += std::string(i ? "," : "") + "\"" + k + "\":1";
    }
    many += "}";
    TEST_ASSERT_TRUE(parses(many));
    std::string too_many = "{";
    for (int i = 0; i < 200; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%03d", i);
        too_many += std::string(i ? "," : "") + "\"" + k + "\":1";
    }
    too_many += "}";
    TEST_ASSERT_FALSE(parses(too_many));
    // Input beyond what the 16 bit offsets hold is refused.
    GlinkJsonDoc doc;
    std::string huge(70000, ' ');
    huge[0] = '{';
    huge[69999] = '}';
    TEST_ASSERT_FALSE(glink_json_parse(huge.data(), huge.size(), doc));
}

static void test_navigation() {
    const std::string text = "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.05}},\"x\":null}";
    GlinkJsonDoc doc;
    TEST_ASSERT_TRUE(glink_json_parse(text.data(), text.size(), doc));
    const int req = glink_json_find(doc, 0, "request");
    TEST_ASSERT_TRUE(req > 0);
    TEST_ASSERT_EQUAL(1, doc.nodes[req].members);
    const int obj = glink_json_first_member(doc, req);
    TEST_ASSERT_TRUE(glink_json_key_is(doc, obj, "grindProfile"));
    TEST_ASSERT_EQUAL(-1, glink_json_next_member(doc, req, obj));
    TEST_ASSERT_EQUAL(3, doc.nodes[obj].members);
    const int x = glink_json_find(doc, 0, "x");
    TEST_ASSERT_TRUE(x > obj);
    TEST_ASSERT_EQUAL(-1, glink_json_find(doc, 0, "grindProfile"));  // only direct members
    TEST_ASSERT_EQUAL(x, glink_json_next_member(doc, 0, req));        // skips the whole subtree
    TEST_ASSERT_EQUAL(-1, glink_json_first_member(doc, x));           // not an object
    char buf[8];
    TEST_ASSERT_EQUAL_UINT32(7, glink_json_key(doc, obj, buf, sizeof(buf)));  // truncated to the buffer, terminated
    TEST_ASSERT_EQUAL_STRING("grindPr", buf);
    const int op = glink_json_find(doc, obj, "op");
    TEST_ASSERT_TRUE(glink_json_string_is(doc, op, "update"));
    TEST_ASSERT_FALSE(glink_json_string_is(doc, op, "updat"));
    TEST_ASSERT_FALSE(glink_json_string_is(doc, op, "update2"));
    glink_json_string(doc, op, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("update", buf);
}

static int64_t tenths_of(const char* token) {
    const std::string text = std::string("{\"a\":") + token + "}";
    GlinkJsonDoc doc;
    TEST_ASSERT_TRUE_MESSAGE(glink_json_parse(text.data(), text.size(), doc), token);
    int64_t t = 0;
    glink_json_number_tenths(doc, 1, &t);
    return t;
}

static void test_number_tenths_round_half_away_from_zero() {
    TEST_ASSERT_EQUAL_INT64(180, tenths_of("18"));
    TEST_ASSERT_EQUAL_INT64(180, tenths_of("18.0"));
    TEST_ASSERT_EQUAL_INT64(180, tenths_of("18.04"));
    TEST_ASSERT_EQUAL_INT64(181, tenths_of("18.05"));
    TEST_ASSERT_EQUAL_INT64(180, tenths_of("17.95"));
    TEST_ASSERT_EQUAL_INT64(180, tenths_of("18.049999"));
    TEST_ASSERT_EQUAL_INT64(181, tenths_of("18.050000"));
    TEST_ASSERT_EQUAL_INT64(50, tenths_of("4.95"));
    TEST_ASSERT_EQUAL_INT64(49, tenths_of("4.94"));
    TEST_ASSERT_EQUAL_INT64(600, tenths_of("60.04"));
    TEST_ASSERT_EQUAL_INT64(601, tenths_of("60.05"));
    TEST_ASSERT_EQUAL_INT64(0, tenths_of("-0"));
    TEST_ASSERT_EQUAL_INT64(0, tenths_of("-0.04"));
    TEST_ASSERT_EQUAL_INT64(-1, tenths_of("-0.05"));
    TEST_ASSERT_EQUAL_INT64(-1, tenths_of("-0.1"));
    TEST_ASSERT_EQUAL_INT64(0, tenths_of("0.0000000001"));
    TEST_ASSERT_EQUAL_INT64(9999999999999990LL, tenths_of("999999999999999"));  // no overflow at 15 digits
    TEST_ASSERT_EQUAL_INT64(10000, tenths_of("999.95"));
    TEST_ASSERT_EQUAL_INT64(9999, tenths_of("999.94"));
}

static bool integer_of(const char* token, int64_t* v) {
    const std::string text = std::string("{\"a\":") + token + "}";
    GlinkJsonDoc doc;
    TEST_ASSERT_TRUE_MESSAGE(glink_json_parse(text.data(), text.size(), doc), token);
    return glink_json_number_integer(doc, 1, v);
}

static void test_number_integer() {
    int64_t v = 99;
    TEST_ASSERT_TRUE(integer_of("3", &v));
    TEST_ASSERT_EQUAL_INT64(3, v);
    TEST_ASSERT_TRUE(integer_of("3.0", &v));
    TEST_ASSERT_EQUAL_INT64(3, v);
    TEST_ASSERT_TRUE(integer_of("3.000", &v));
    TEST_ASSERT_TRUE(integer_of("-0", &v));
    TEST_ASSERT_EQUAL_INT64(0, v);
    TEST_ASSERT_TRUE(integer_of("-1", &v));
    TEST_ASSERT_EQUAL_INT64(-1, v);
    TEST_ASSERT_FALSE(integer_of("3.5", &v));
    TEST_ASSERT_FALSE(integer_of("3.01", &v));
    TEST_ASSERT_FALSE(integer_of("0.1", &v));
    // No wrap-around: 2^32 is not 0 and 2^32 + 3 is not 3.
    TEST_ASSERT_TRUE(integer_of("4294967296", &v));
    TEST_ASSERT_EQUAL_INT64(4294967296LL, v);
    TEST_ASSERT_TRUE(integer_of("4294967299", &v));
    TEST_ASSERT_EQUAL_INT64(4294967299LL, v);
    TEST_ASSERT_TRUE(integer_of("999999999999999", &v));
    TEST_ASSERT_EQUAL_INT64(999999999999999LL, v);
}

static void test_fuzz_never_overruns() {
    // Mutated and truncated variants of a valid request; guard bytes around the buffer must stay intact.
    const std::string base =
        "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"grindUnit\":\"um\","
        "\"selector\":{\"type\":\"index\",\"value\":3}}}}";
    uint32_t seed = 4242;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const char alphabet[] = "{}[]\":,-.0123456789eE+ \\\t\nnulltruefalse\x01\x80\xff";
    int accepted = 0;
    for (int i = 0; i < 20000; i++) {
        std::string s = base;
        const int edits = 1 + rnd() % 4;
        for (int e = 0; e < edits; e++) {
            const size_t pos = rnd() % s.size();
            switch (rnd() % 4) {
                case 0: s[pos] = alphabet[rnd() % (sizeof(alphabet) - 1)]; break;
                case 1: s.insert(pos, 1, alphabet[rnd() % (sizeof(alphabet) - 1)]); break;
                case 2: s.erase(pos, 1); break;
                default: s.resize(pos + 1); break;
            }
            if (s.empty()) s = "{";
        }
        // Exact-size heap copy so that an overrun is visible to the sanitizer / guard.
        std::string copy(s.data(), s.size());
        GlinkJsonDoc doc;
        if (glink_json_parse(copy.data(), copy.size(), doc)) {
            accepted++;
            TEST_ASSERT_TRUE(doc.count >= 1 && doc.count <= GLINK_JSON_MAX_NODES);
            TEST_ASSERT_EQUAL(GlinkJsonType::OBJECT, doc.nodes[0].type);
            for (int n = 1; n < doc.count; n++) {
                const GlinkJsonNode& nd = doc.nodes[n];
                TEST_ASSERT_TRUE(nd.key_len >= 1);
                TEST_ASSERT_TRUE(static_cast<size_t>(nd.key_off) + nd.key_len <= copy.size());
                if (nd.type == GlinkJsonType::STRING || nd.type == GlinkJsonType::NUMBER) {
                    TEST_ASSERT_TRUE(static_cast<size_t>(nd.val_off) + nd.val_len <= copy.size());
                }
            }
        }
    }
    TEST_ASSERT_TRUE(accepted > 0);
}

// ---- writer ----
static void test_writer_basic_object() {
    char buf[128];
    GlinkJsonWriter w(buf, sizeof(buf));
    w.begin_object().key("a").integer(1).key("b").begin_object().key("c").boolean(true).key("d").null().end_object();
    w.key("e").begin_array().integer(-5).string("x").tenths(180).end_array().end_object();
    TEST_ASSERT_TRUE(w.ok());
    TEST_ASSERT_EQUAL_STRING("{\"a\":1,\"b\":{\"c\":true,\"d\":null},\"e\":[-5,\"x\",18.0]}", buf);
    TEST_ASSERT_EQUAL_UINT32(strlen(buf), w.length());
}

static void test_writer_tenths_formats() {
    struct Case { int32_t v; const char* text; } cases[] = {{0, "0.0"},     {5, "0.5"},      {-3, "-0.3"},   {180, "18.0"},
                                                            {-181, "-18.1"}, {9999, "999.9"}, {600, "60.0"}, {-1, "-0.1"},
                                                            {2147483647, "214748364.7"}, {-2147483647, "-214748364.7"}};
    for (const Case& c : cases) {
        char buf[32];
        GlinkJsonWriter w(buf, sizeof(buf));
        w.tenths(c.v);
        TEST_ASSERT_EQUAL_STRING(c.text, buf);
    }
}

static void test_writer_integers() {
    char buf[64];
    GlinkJsonWriter w(buf, sizeof(buf));
    w.begin_array().integer(0).integer(-1).integer(4294967295LL).integer(INT64_MIN).end_array();
    TEST_ASSERT_EQUAL_STRING("[0,-1,4294967295,-9223372036854775808]", buf);
}

static void test_writer_escapes_strings() {
    char buf[64];
    GlinkJsonWriter w(buf, sizeof(buf));
    w.string("a\"b\\c\n");
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u000a\"", buf);
}

static void test_writer_overflow_is_safe() {
    // Every capacity from 0 to the exact size: never beyond the buffer, ok() false until it fits.
    const char* expect = "{\"key\":\"value\",\"n\":12}";
    const size_t full = strlen(expect);
    for (size_t cap = 0; cap <= full + 2; cap++) {
        char arena[64];
        memset(arena, 0x5A, sizeof(arena));
        GlinkJsonWriter w(arena + 8, cap);
        w.begin_object().key("key").string("value").key("n").integer(12).end_object();
        TEST_ASSERT_EQUAL_MESSAGE(cap > full, w.ok(), "ok() when it fits");
        for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<unsigned char>(arena[i]));
        for (size_t i = cap + 8; i < sizeof(arena); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<unsigned char>(arena[i]));
        if (cap > full) TEST_ASSERT_EQUAL_STRING(expect, arena + 8);
    }
}

static void test_writer_depth_limit() {
    char buf[64];
    GlinkJsonWriter w(buf, sizeof(buf));
    for (int i = 0; i < 20; i++) w.begin_array();
    TEST_ASSERT_FALSE(w.ok());
}

void run_glink_json_tests() {
    RUN_TEST(test_reader_cases);
    RUN_TEST(test_reader_extra_rules);
    RUN_TEST(test_depth_and_node_limits);
    RUN_TEST(test_navigation);
    RUN_TEST(test_number_tenths_round_half_away_from_zero);
    RUN_TEST(test_number_integer);
    RUN_TEST(test_fuzz_never_overruns);
    RUN_TEST(test_writer_basic_object);
    RUN_TEST(test_writer_tenths_formats);
    RUN_TEST(test_writer_integers);
    RUN_TEST(test_writer_escapes_strings);
    RUN_TEST(test_writer_overflow_is_safe);
    RUN_TEST(test_writer_depth_limit);
}

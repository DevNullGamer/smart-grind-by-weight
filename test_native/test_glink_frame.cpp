#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "bluetooth/glink_frame.h"
#include "glink_vectors.h"

// Host tests of the GLink fragment frame (src/bluetooth/glink_frame.*) against the conformance vectors and the rules
// of docs/GLINK-PROTOCOL.md section 3.

static std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    }
    return out;
}

static std::string to_hex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

static GlinkFragmentResult feed_hex(GlinkReassembler& r, const std::string& hex, uint32_t t) {
    const std::vector<uint8_t> pkt = from_hex(hex);
    return r.feed(pkt.data(), pkt.size(), t);
}

// A packet with explicit header fields and `n` payload bytes of 'x'.
static std::vector<uint8_t> make_packet(uint16_t seq, uint16_t total, uint16_t offset, size_t n) {
    std::vector<uint8_t> p(GLINK_FRAME_HEADER_BYTES + n, 'x');
    glink_frame_header_write(p.data(), seq, total, offset);
    return p;
}

static GlinkFragmentResult feed_packet(GlinkReassembler& r, const std::vector<uint8_t>& p, uint32_t t) {
    return r.feed(p.data(), p.size(), t);
}

// ---- golden frames (fragmenter) ----
static void test_fragmenter_matches_golden_frames() {
    const GJson& cases = glink_vectors().at("frame_cases");
    for (const GJson& c : cases.a) {
        const std::string payload = c.at("payload").s;
        const size_t packet_len = static_cast<size_t>(c.at("packet_len").n);
        const uint16_t seq = static_cast<uint16_t>(c.at("sequence").n);
        const GJson& packets = c.at("packets");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(packets.a.size(),
                                         glink_fragment_count(payload.size(), packet_len), c.at("name").s.c_str());
        for (size_t i = 0; i < packets.a.size(); i++) {
            uint8_t out[GLINK_MAX_PACKET_BYTES];
            const size_t n = glink_fragment_build(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), seq,
                                                  packet_len, i, out, sizeof(out));
            TEST_ASSERT_EQUAL_STRING_MESSAGE(packets.a[i].s.c_str(), to_hex(out, n).c_str(), c.at("name").s.c_str());
        }
    }
}

static void test_fragmenter_rejects_bad_arguments() {
    const uint8_t msg[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t out[64];
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_count(0, 20));
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_count(4097, 20));
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_count(10, 8));   // no room for payload
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_count(10, 0));
    TEST_ASSERT_EQUAL_UINT32(1, glink_fragment_count(10, 100000));  // clamped to the maximum packet size
    TEST_ASSERT_EQUAL_UINT32(2, glink_fragment_count(10, 14));
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_build(msg, 10, 1, 14, 2, out, sizeof(out)));  // index past the end
    TEST_ASSERT_EQUAL_UINT32(0, glink_fragment_build(msg, 10, 1, 14, 0, out, 13));           // output too small
    TEST_ASSERT_EQUAL_UINT32(14, glink_fragment_build(msg, 10, 1, 14, 0, out, 14));
    TEST_ASSERT_EQUAL_UINT32(12, glink_fragment_build(msg, 10, 1, 14, 1, out, 14));  // 8 + 4
}

static void test_fragmenter_rebuilds_every_size() {
    // Every message size and packet size round trips through the reassembler.
    uint8_t store[4096];
    for (size_t len : {1u, 11u, 12u, 13u, 24u, 100u, 511u, 512u, 513u, 1024u, 4096u}) {
        std::vector<uint8_t> msg(len);
        for (size_t i = 0; i < len; i++) msg[i] = static_cast<uint8_t>(i * 7 + 3);
        for (size_t packet : {9u, 20u, 21u, 185u, 512u}) {
            GlinkReassembler r(store, sizeof(store));
            const size_t count = glink_fragment_count(len, packet);
            TEST_ASSERT_TRUE(count > 0);
            for (size_t i = 0; i < count; i++) {
                uint8_t out[GLINK_MAX_PACKET_BYTES];
                const size_t n = glink_fragment_build(msg.data(), len, 0xFFFF, packet, i, out, sizeof(out));
                const GlinkFragmentResult res = r.feed(out, n, static_cast<uint32_t>(i));
                TEST_ASSERT_EQUAL(i + 1 == count ? GlinkFragmentResult::COMPLETE : GlinkFragmentResult::PARTIAL, res);
            }
            TEST_ASSERT_EQUAL_UINT32(len, r.message_length());
            TEST_ASSERT_EQUAL_MEMORY(msg.data(), r.message(), len);
        }
    }
}

// ---- header ----
static void test_header_parse_checks() {
    GlinkFrameHeader h;
    const std::vector<uint8_t> good = from_hex("a50102012b0000007b");
    TEST_ASSERT_TRUE(glink_frame_header_parse(good.data(), good.size(), h));
    TEST_ASSERT_EQUAL_UINT16(0x0102, h.sequence);
    TEST_ASSERT_EQUAL_UINT16(43, h.total_length);
    TEST_ASSERT_EQUAL_UINT16(0, h.offset);
    // Truncated at every length up to and including the header: refused.
    for (size_t len = 0; len <= GLINK_FRAME_HEADER_BYTES; len++) {
        TEST_ASSERT_FALSE(glink_frame_header_parse(good.data(), len, h));
    }
    TEST_ASSERT_FALSE(glink_frame_header_parse(nullptr, 20, h));
    std::vector<uint8_t> bad = good;
    bad[0] = 0xA4;
    TEST_ASSERT_FALSE(glink_frame_header_parse(bad.data(), bad.size(), h));
    bad = good;
    bad[1] = 2;
    TEST_ASSERT_FALSE(glink_frame_header_parse(bad.data(), bad.size(), h));
}

static void test_header_write_is_little_endian() {
    uint8_t out[8];
    TEST_ASSERT_EQUAL_UINT32(8, glink_frame_header_write(out, 0x0102, 0x0304, 0x0506));
    TEST_ASSERT_EQUAL_STRING("a501020104030605", to_hex(out, 8).c_str());
}

// ---- vectors: single packet and reassembly ----
static GlinkFragmentResult result_from_name(const std::string& n) {
    for (int i = 0; i <= static_cast<int>(GlinkFragmentResult::DISCARDED); i++) {
        if (n == glink_fragment_result_name(static_cast<GlinkFragmentResult>(i))) return static_cast<GlinkFragmentResult>(i);
    }
    TEST_FAIL_MESSAGE("unknown outcome name in the vectors");
    return GlinkFragmentResult::BAD_FRAGMENT;
}

static void test_single_packet_cases() {
    const GJson& cases = glink_vectors().at("single_packet_cases");
    for (const GJson& c : cases.a) {
        uint8_t store[GLINK_MAX_REQUEST_BYTES];
        GlinkReassembler r(store, sizeof(store));
        const GlinkFragmentResult got = feed_hex(r, c.at("packet").s, 0);
        TEST_ASSERT_EQUAL_MESSAGE(result_from_name(c.at("outcome").s), got, c.at("name").s.c_str());
    }
}

static void test_reassembly_cases() {
    const GJson& cases = glink_vectors().at("reassembly_cases");
    for (const GJson& c : cases.a) {
        uint8_t store[GLINK_MAX_REQUEST_BYTES];
        GlinkReassembler r(store, sizeof(store));
        for (const GJson& step : c.at("steps").a) {
            const uint32_t t = static_cast<uint32_t>(step.at("t_ms").n);
            if (step.find("event")) {
                r.reset();
                continue;
            }
            const GlinkFragmentResult got = feed_hex(r, step.at("packet").s, t);
            TEST_ASSERT_EQUAL_MESSAGE(result_from_name(step.at("outcome").s), got, c.at("name").s.c_str());
            if (const GJson* msg = step.find("message")) {
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(msg->s.size(), r.message_length(), c.at("name").s.c_str());
                TEST_ASSERT_EQUAL_MEMORY(msg->s.data(), r.message(), msg->s.size());
            }
        }
    }
}

static void test_request_frames_vector_is_payload_too_large() {
    const GJson& pairs = glink_vectors().at("pairs");
    int seen = 0;
    for (const GJson& p : pairs.a) {
        const GJson* frames = p.find("request_frames");
        if (!frames) continue;
        uint8_t store[GLINK_MAX_REQUEST_BYTES];
        GlinkReassembler r(store, sizeof(store));
        TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_hex(r, frames->a[0].s, 0));
        seen++;
    }
    TEST_ASSERT_TRUE(seen > 0);
}

static void test_bad_fragment_window_cases() {
    const GJson& cases = glink_vectors().at("bad_fragment_window_cases");
    for (const GJson& c : cases.a) {
        GlinkBadFrameWindow w;
        bool disconnect = false;
        for (const GJson& t : c.at("bad_at_ms").a) disconnect = w.record(static_cast<uint32_t>(t.n));
        TEST_ASSERT_EQUAL_MESSAGE(c.at("disconnect").b, disconnect, c.at("name").s.c_str());
    }
}

static void test_bad_fragment_window_edges() {
    GlinkBadFrameWindow w;
    for (int i = 0; i < 9; i++) TEST_ASSERT_FALSE(w.record(100u * i));
    TEST_ASSERT_TRUE(w.record(900));
    TEST_ASSERT_TRUE(w.record(901));  // stays over the limit while the window is full
    w.reset();
    for (int i = 0; i < 9; i++) TEST_ASSERT_FALSE(w.record(0));
    TEST_ASSERT_FALSE(w.record(GLINK_BAD_FRAME_WINDOW_MS));  // exactly 10 000 ms older: outside
    // The millisecond clock wraps.
    w.reset();
    for (int i = 0; i < 9; i++) TEST_ASSERT_FALSE(w.record(0xFFFFFFF0u + i));
    TEST_ASSERT_TRUE(w.record(5));
}

// ---- rules written out ----
static void test_total_length_bounds() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(1, 0, 0, 5), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(1, 512, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_packet(r, make_packet(1, 513, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_packet(r, make_packet(1, 4096, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(1, 4097, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(1, 65535, 0, 12), 0));
    // totalLength 1 and a one byte payload completes at once.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(1, 1, 0, 1), 0));
    TEST_ASSERT_EQUAL_UINT32(1, r.message_length());
}

static void test_offset_rules() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    // Gap: 12 bytes received, next offset 13.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(2, 40, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(2, 40, 13, 12), 1));
    // The message is gone: the right offset afterwards is refused too.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(2, 40, 12, 12), 2));
    // Overlap: offset below the bytes received.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(2, 40, 0, 12), 3));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(2, 40, 11, 12), 4));
    // A fragment longer than the remaining total.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(3, 20, 0, 12), 5));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(3, 20, 12, 9), 6));
    // Exactly the remaining total completes.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(3, 20, 0, 12), 7));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(3, 20, 12, 8), 8));
    // offset == total is refused.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(3, 20, 20, 1), 9));
}

static void test_new_offset_zero_mid_message_restarts() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(1, 30, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(9, 24, 0, 12), 1));
    // The old message is gone; the new one continues.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(1, 30, 12, 12), 2));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(9, 24, 0, 12), 3));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(9, 24, 12, 12), 4));
}

static void test_expiry_boundary() {
    for (uint32_t gap : {1999u, 2000u, 2001u}) {
        uint8_t store[GLINK_MAX_REQUEST_BYTES];
        GlinkReassembler r(store, sizeof(store));
        TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(5, 24, 0, 12), 1000));
        const GlinkFragmentResult got = feed_packet(r, make_packet(5, 24, 12, 12), 1000 + gap);
        TEST_ASSERT_EQUAL(gap <= 2000 ? GlinkFragmentResult::COMPLETE : GlinkFragmentResult::BAD_FRAGMENT, got);
    }
}

static void test_expiry_is_measured_between_fragments() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    // Three fragments 1500 ms apart: 3000 ms in total, but no gap above 2000 ms.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(5, 36, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(5, 36, 12, 12), 1500));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(5, 36, 24, 12), 3000));
}

static void test_clock_wraps() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(5, 24, 0, 12), 0xFFFFFF00u));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(5, 24, 12, 12), 0x00000010u));  // 272 ms
}

static void test_sequence_wrap() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    for (uint32_t seq : {65534u, 65535u, 0u, 1u}) {
        TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(static_cast<uint16_t>(seq), 24, 0, 12), 0));
        TEST_ASSERT_EQUAL(GlinkFragmentResult::COMPLETE, feed_packet(r, make_packet(static_cast<uint16_t>(seq), 24, 12, 12), 1));
    }
}

static void test_sequence_and_total_must_match() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PARTIAL, feed_packet(r, make_packet(65535, 24, 0, 12), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(0, 24, 12, 12), 1));
}

static void test_oversize_discard_ends_on_last_fragment() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_packet(r, make_packet(7, 600, 0, 300), 0));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::DISCARDED, feed_packet(r, make_packet(7, 600, 300, 300), 1));
    // The message is over; a further continuation is a bad fragment.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(7, 600, 300, 300), 2));
    // A continuation of an oversize message with another sequence is a bad fragment.
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_packet(r, make_packet(8, 600, 0, 300), 3));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(9, 600, 300, 300), 4));
}

static void test_bad_fragment_ends_discard_state() {
    uint8_t store[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler r(store, sizeof(store));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::PAYLOAD_TOO_LARGE, feed_packet(r, make_packet(7, 1000, 0, 12), 0));
    const uint8_t junk[3] = {0, 1, 2};
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, r.feed(junk, sizeof(junk), 1));
    TEST_ASSERT_EQUAL(GlinkFragmentResult::BAD_FRAGMENT, feed_packet(r, make_packet(7, 1000, 12, 12), 2));
}

static void test_never_overruns_storage() {
    // Fuzz-style: random packets against a tight buffer with guard bytes on both sides.
    uint8_t arena[64 + 100 + 64];
    memset(arena, 0xAB, sizeof(arena));
    GlinkReassembler r(arena + 64, 100);
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    uint32_t t = 0;
    for (int i = 0; i < 20000; i++) {
        const size_t n = GLINK_FRAME_HEADER_BYTES + rnd() % 40;
        std::vector<uint8_t> p(n, 0);
        p[0] = (rnd() % 8 == 0) ? static_cast<uint8_t>(rnd()) : GLINK_FRAME_MAGIC;
        p[1] = (rnd() % 8 == 0) ? static_cast<uint8_t>(rnd()) : GLINK_FRAME_VERSION;
        const uint16_t seq = static_cast<uint16_t>(rnd() % 3);
        const uint16_t total = static_cast<uint16_t>(rnd() % 130);
        const uint16_t off = static_cast<uint16_t>(rnd() % 3 == 0 ? rnd() % 130 : 0);
        p[2] = seq & 0xFF;
        p[3] = seq >> 8;
        p[4] = total & 0xFF;
        p[5] = total >> 8;
        p[6] = off & 0xFF;
        p[7] = off >> 8;
        t += rnd() % 2500;
        const GlinkFragmentResult res = r.feed(p.data(), p.size(), t);
        if (res == GlinkFragmentResult::COMPLETE) TEST_ASSERT_TRUE(r.message_length() <= 100);
    }
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xAB, arena[i]);
        TEST_ASSERT_EQUAL_HEX8(0xAB, arena[64 + 100 + i]);
    }
}

void run_glink_frame_tests() {
    RUN_TEST(test_fragmenter_matches_golden_frames);
    RUN_TEST(test_fragmenter_rejects_bad_arguments);
    RUN_TEST(test_fragmenter_rebuilds_every_size);
    RUN_TEST(test_header_parse_checks);
    RUN_TEST(test_header_write_is_little_endian);
    RUN_TEST(test_single_packet_cases);
    RUN_TEST(test_reassembly_cases);
    RUN_TEST(test_request_frames_vector_is_payload_too_large);
    RUN_TEST(test_bad_fragment_window_cases);
    RUN_TEST(test_bad_fragment_window_edges);
    RUN_TEST(test_total_length_bounds);
    RUN_TEST(test_offset_rules);
    RUN_TEST(test_new_offset_zero_mid_message_restarts);
    RUN_TEST(test_expiry_boundary);
    RUN_TEST(test_expiry_is_measured_between_fragments);
    RUN_TEST(test_clock_wraps);
    RUN_TEST(test_sequence_wrap);
    RUN_TEST(test_sequence_and_total_must_match);
    RUN_TEST(test_oversize_discard_ends_on_last_fragment);
    RUN_TEST(test_bad_fragment_ends_discard_state);
    RUN_TEST(test_never_overruns_storage);
}

// Notification ring (NimBLE host task -> loop task). Regression: with the 8 packet ring the plugin dropped the link ("notification
// queue overflow") about 1 s after a connect, because a grinder that does not know the MTU answers the hello with 20-byte
// packets and baseInfo alone is 17 of them (docs/GLINK-PROTOCOL.md section 8), arriving before the loop task runs.
#include <stdint.h>
#include <string.h>
#include <unity.h>
#include <vector>
#include "RxRing.h"
#include "bluetooth/glink_frame.h"
#include "config/bluetooth.h"

using namespace glinkc;

namespace {

constexpr size_t RING_BYTES = 8192; // as in GLinkPlugin.cpp (RX_RING_BYTES)

struct Fixture {
    uint8_t storage[RING_BYTES];
    RxRing ring;
    Fixture() { ring.init(storage, sizeof(storage)); }
};

std::vector<uint8_t> message_of(size_t n) {
    std::vector<uint8_t> m(n);
    for (size_t i = 0; i < n; i++) m[i] = static_cast<uint8_t>('a' + i % 26);
    return m;
}

// Pushes every fragment of the message the way the notify callback does; returns the number of packets that fit.
size_t push_message(RxRing& ring, const std::vector<uint8_t>& msg, size_t packet_len, size_t* total) {
    const size_t count = glink_fragment_count(msg.size(), packet_len);
    *total = count;
    size_t pushed = 0;
    uint8_t packet[GLINK_MAX_PACKET_BYTES];
    for (size_t i = 0; i < count; i++) {
        const size_t n = glink_fragment_build(msg.data(), msg.size(), 1, packet_len, i, packet, sizeof(packet));
        if (n == 0 || !ring.push(packet, n)) break;
        pushed++;
    }
    return pushed;
}

void test_baseinfo_at_20_byte_packets_fits_without_a_drain() {
    Fixture f;
    size_t total = 0;
    const size_t pushed = push_message(f.ring, message_of(200), GLINK_DEFAULT_PACKET_BYTES, &total);
    TEST_ASSERT_TRUE(total > 8); // the old 8 packet ring overflowed here
    TEST_ASSERT_EQUAL_UINT32(total, pushed);
    TEST_ASSERT_EQUAL_UINT32(total, f.ring.count());
}

void test_biggest_message_at_20_byte_packets_fits() {
    Fixture f;
    size_t total = 0;
    const size_t pushed = push_message(f.ring, message_of(GLINK_MAX_MESSAGE_BYTES), GLINK_DEFAULT_PACKET_BYTES, &total);
    TEST_ASSERT_EQUAL_UINT32(total, pushed);
}

void test_packets_come_out_in_order_and_unchanged() {
    Fixture f;
    uint8_t out[GLINK_MAX_PACKET_BYTES];
    for (int round = 0; round < 300; round++) { // many wraps of the 8 KB ring
        const size_t a = 1 + (round * 37) % 200, b = 1 + (round * 91) % 512;
        std::vector<uint8_t> pa = message_of(a), pb = message_of(b);
        pb[0] = 0xEE;
        TEST_ASSERT_TRUE(f.ring.push(pa.data(), pa.size()));
        TEST_ASSERT_TRUE(f.ring.push(pb.data(), pb.size()));
        TEST_ASSERT_EQUAL_UINT32(a, f.ring.pop(out, sizeof(out)));
        TEST_ASSERT_TRUE(memcmp(pa.data(), out, a) == 0);
        TEST_ASSERT_EQUAL_UINT32(b, f.ring.pop(out, sizeof(out)));
        TEST_ASSERT_TRUE(memcmp(pb.data(), out, b) == 0);
        TEST_ASSERT_EQUAL_UINT32(0, f.ring.count());
    }
}

void test_full_ring_refuses_and_keeps_what_it_has() {
    Fixture f;
    std::vector<uint8_t> p = message_of(100);
    size_t pushed = 0;
    while (f.ring.push(p.data(), p.size())) pushed++;
    TEST_ASSERT_EQUAL_UINT32(RING_BYTES / (100 + 2), pushed);
    TEST_ASSERT_EQUAL_UINT32(pushed, f.ring.count());
    uint8_t out[GLINK_MAX_PACKET_BYTES];
    for (size_t i = 0; i < pushed; i++) {
        TEST_ASSERT_EQUAL_UINT32(100, f.ring.pop(out, sizeof(out)));
        TEST_ASSERT_TRUE(memcmp(p.data(), out, 100) == 0);
    }
    TEST_ASSERT_EQUAL_UINT32(0, f.ring.pop(out, sizeof(out)));
}

void test_empty_packet_clear_and_small_output_buffer() {
    Fixture f;
    uint8_t out[8];
    uint8_t one = 1;
    TEST_ASSERT_FALSE(f.ring.push(&one, 0));
    TEST_ASSERT_EQUAL_UINT32(0, f.ring.pop(out, sizeof(out)));
    std::vector<uint8_t> big = message_of(40);
    TEST_ASSERT_TRUE(f.ring.push(big.data(), big.size()));
    TEST_ASSERT_TRUE(f.ring.push(&one, 1));
    TEST_ASSERT_EQUAL_UINT32(0, f.ring.pop(out, sizeof(out))); // too long for the caller: discarded
    TEST_ASSERT_EQUAL_UINT32(1, f.ring.count());
    TEST_ASSERT_EQUAL_UINT32(1, f.ring.pop(out, sizeof(out))); // the next packet is intact
    TEST_ASSERT_TRUE(f.ring.push(&one, 1));
    f.ring.clear();
    TEST_ASSERT_EQUAL_UINT32(0, f.ring.count());
    TEST_ASSERT_EQUAL_UINT32(0, f.ring.pop(out, sizeof(out)));
}

} // namespace

void run_glink_plugin_rx_ring_tests() {
    RUN_TEST(test_baseinfo_at_20_byte_packets_fits_without_a_drain);
    RUN_TEST(test_biggest_message_at_20_byte_packets_fits);
    RUN_TEST(test_packets_come_out_in_order_and_unchanged);
    RUN_TEST(test_full_ring_refuses_and_keeps_what_it_has);
    RUN_TEST(test_empty_packet_clear_and_small_output_buffer);
}

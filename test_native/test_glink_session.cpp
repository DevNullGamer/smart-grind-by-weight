#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "bluetooth/glink_session.h"

// Host tests of the GLink link session (src/bluetooth/glink_session.*): the mode / characteristic filter table, the
// RX hand-over, the TX fragment pump and pacer, the periodInfo cadence and the handshake rules of
// docs/GLINK-PROTOCOL.md sections 2, 3.3, 7.4 and 8.

// ---- mode and filter table ----
static bool expected_allowed(GlinkLinkMode mode, BleChar ch, BleAccess access) {
    if (mode == GlinkLinkMode::OFF) return false;
    if (mode == GlinkLinkMode::FULL) return true;
    return (ch == BleChar::GLINK_RX && access == BleAccess::WRITE) ||
           (ch == BleChar::GLINK_TX && (access == BleAccess::READ || access == BleAccess::NOTIFY));
}

static void test_filter_table_every_characteristic_in_every_mode() {
    const GlinkLinkMode modes[] = {GlinkLinkMode::OFF, GlinkLinkMode::FULL, GlinkLinkMode::LINK_ONLY};
    const BleAccess accesses[] = {BleAccess::WRITE, BleAccess::READ, BleAccess::NOTIFY};
    int allowed_link_only = 0;
    for (GlinkLinkMode mode : modes) {
        for (int c = 0; c < static_cast<int>(BleChar::COUNT); c++) {
            for (BleAccess access : accesses) {
                const BleChar ch = static_cast<BleChar>(c);
                const bool want = expected_allowed(mode, ch, access);
                char msg[96];
                snprintf(msg, sizeof(msg), "%s %s access %d", glink_link_mode_name(mode), ble_char_name(ch), static_cast<int>(access));
                TEST_ASSERT_EQUAL_MESSAGE(want, ble_char_access_allowed(mode, ch, access), msg);
                if (mode == GlinkLinkMode::LINK_ONLY && want) allowed_link_only++;
            }
        }
    }
    TEST_ASSERT_EQUAL(3, allowed_link_only);  // GLink RX write, GLink TX read, GLink TX notify
}

static void test_filter_link_only_refuses_the_old_services() {
    const BleChar old_writes[] = {BleChar::OTA_DATA, BleChar::OTA_CONTROL, BleChar::DATA_CONTROL, BleChar::DEBUG_RX,
                                  BleChar::SYSINFO_DIAGNOSTICS};
    for (BleChar ch : old_writes) {
        TEST_ASSERT_FALSE(ble_char_access_allowed(GlinkLinkMode::LINK_ONLY, ch, BleAccess::WRITE));
    }
    TEST_ASSERT_FALSE(ble_char_access_allowed(GlinkLinkMode::LINK_ONLY, BleChar::SYSINFO_SYSTEM, BleAccess::READ));
    TEST_ASSERT_FALSE(ble_char_access_allowed(GlinkLinkMode::LINK_ONLY, BleChar::OTA_BUILD_NUMBER, BleAccess::READ));
    TEST_ASSERT_FALSE(ble_char_access_allowed(GlinkLinkMode::LINK_ONLY, BleChar::GLINK_RX, BleAccess::READ));
    TEST_ASSERT_FALSE(ble_char_access_allowed(GlinkLinkMode::FULL, BleChar::COUNT, BleAccess::WRITE));  // out of range
}

static void test_mode_resolve() {
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkLinkMode::OFF), static_cast<int>(glink_link_mode_resolve(false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkLinkMode::OFF), static_cast<int>(glink_link_mode_resolve(false, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkLinkMode::FULL), static_cast<int>(glink_link_mode_resolve(true, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkLinkMode::LINK_ONLY), static_cast<int>(glink_link_mode_resolve(true, false)));
    TEST_ASSERT_EQUAL_STRING("LINK_ONLY", glink_link_mode_name(GlinkLinkMode::LINK_ONLY));
}

// ---- RX ----
static std::vector<uint8_t> packet(uint16_t seq, uint16_t total, uint16_t offset, const std::string& payload) {
    std::vector<uint8_t> p(GLINK_FRAME_HEADER_BYTES + payload.size());
    glink_frame_header_write(p.data(), seq, total, offset);
    memcpy(p.data() + GLINK_FRAME_HEADER_BYTES, payload.data(), payload.size());
    return p;
}

static void feed(GlinkRx& rx, const std::vector<uint8_t>& p, uint32_t t) { rx.on_fragment(p.data(), p.size(), t); }

static void test_rx_delivers_a_complete_message_once() {
    GlinkRx rx;
    const std::string msg = "{\"request\":{\"appHello\":{\"op\":\"handshake\"}}}";
    feed(rx, packet(1, msg.size(), 0, msg.substr(0, 12)), 0);
    TEST_ASSERT_FALSE(rx.has_message());
    feed(rx, packet(1, msg.size(), 12, msg.substr(12)), 10);
    TEST_ASSERT_TRUE(rx.has_message());
    TEST_ASSERT_EQUAL(msg.size(), rx.message_length());
    TEST_ASSERT_EQUAL_MEMORY(msg.data(), rx.message(), msg.size());
    rx.release_message();
    TEST_ASSERT_FALSE(rx.has_message());
    TEST_ASSERT_FALSE(rx.take_overrun());
}

static void test_rx_second_request_before_release_is_an_overrun() {
    GlinkRx rx;
    const std::string a = "{\"a\":1}", b = "{\"b\":2}";
    feed(rx, packet(1, a.size(), 0, a), 0);
    feed(rx, packet(2, b.size(), 0, b), 5);
    TEST_ASSERT_TRUE(rx.take_overrun());
    TEST_ASSERT_FALSE(rx.take_overrun());                        // once
    TEST_ASSERT_EQUAL(a.size(), rx.message_length());            // the first one is still intact
    TEST_ASSERT_EQUAL_MEMORY(a.data(), rx.message(), a.size());
}

static void test_rx_oversize_is_reported_once_and_rest_is_discarded() {
    GlinkRx rx;
    feed(rx, packet(7, 600, 0, std::string(100, 'x')), 0);
    TEST_ASSERT_TRUE(rx.take_too_large());
    TEST_ASSERT_FALSE(rx.take_too_large());
    feed(rx, packet(7, 600, 100, std::string(100, 'x')), 5);  // discarded silently, not a bad fragment
    TEST_ASSERT_FALSE(rx.take_too_large());
    TEST_ASSERT_FALSE(rx.disconnect_requested());
    TEST_ASSERT_FALSE(rx.has_message());
}

static void test_rx_ten_bad_fragments_in_the_window_request_a_disconnect() {
    GlinkRx rx;
    const std::vector<uint8_t> bad = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};  // bad magic
    for (int i = 0; i < 9; i++) rx.on_fragment(bad.data(), bad.size(), i * 100);
    TEST_ASSERT_FALSE(rx.disconnect_requested());
    rx.on_fragment(bad.data(), bad.size(), 900);
    TEST_ASSERT_TRUE(rx.disconnect_requested());
    rx.reset();
    TEST_ASSERT_FALSE(rx.disconnect_requested());
}

static void test_rx_bad_fragments_spread_over_time_do_not_disconnect() {
    GlinkRx rx;
    const std::vector<uint8_t> bad = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (int i = 0; i < 30; i++) rx.on_fragment(bad.data(), bad.size(), i * 1500);
    TEST_ASSERT_FALSE(rx.disconnect_requested());
}

static void test_rx_reset_forgets_everything() {
    GlinkRx rx;
    const std::string a = "{\"a\":1}";
    feed(rx, packet(1, a.size(), 0, a), 0);
    feed(rx, packet(7, 600, 0, std::string(100, 'x')), 1);
    rx.reset();
    TEST_ASSERT_FALSE(rx.has_message());
    TEST_ASSERT_FALSE(rx.take_too_large());
    TEST_ASSERT_FALSE(rx.take_overrun());
    // the oversize discard state is gone too: a continuation is now a bad fragment, not silently ignored
    feed(rx, packet(7, 600, 100, std::string(10, 'x')), 2);
    TEST_ASSERT_FALSE(rx.take_too_large());
}

// ---- TX and pacer ----
static void test_tx_fragments_a_message_in_order() {
    uint8_t storage[GLINK_MAX_REPLY_BYTES];
    GlinkTx tx(storage, sizeof(storage));
    const std::string msg = "{\"broadcast\":{\"periodInfo\":{}}}";  // 31 bytes
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_TRUE(tx.begin(msg.data(), msg.size(), 0x0102));
    TEST_ASSERT_FALSE(tx.begin(msg.data(), msg.size(), 3));  // busy
    std::string rebuilt;
    int fragments = 0;
    uint8_t out[GLINK_MAX_PACKET_BYTES];
    while (!tx.idle()) {
        const size_t n = tx.peek(20, out, sizeof(out));
        TEST_ASSERT_GREATER_THAN(GLINK_FRAME_HEADER_BYTES, n);
        TEST_ASSERT_TRUE(n <= 20);
        GlinkFrameHeader h;
        TEST_ASSERT_TRUE(glink_frame_header_parse(out, n, h));
        TEST_ASSERT_EQUAL(0x0102, h.sequence);
        TEST_ASSERT_EQUAL(msg.size(), h.total_length);
        TEST_ASSERT_EQUAL(rebuilt.size(), h.offset);
        rebuilt.append(reinterpret_cast<const char*>(out) + GLINK_FRAME_HEADER_BYTES, n - GLINK_FRAME_HEADER_BYTES);
        tx.advance();
        fragments++;
    }
    TEST_ASSERT_EQUAL_STRING(msg.c_str(), rebuilt.c_str());
    TEST_ASSERT_EQUAL(3, fragments);  // 12 + 12 + 7 payload bytes
}

static void test_tx_follows_an_mtu_change_mid_message() {
    uint8_t storage[GLINK_MAX_REPLY_BYTES];
    GlinkTx tx(storage, sizeof(storage));
    const std::string msg(100, 'a');
    TEST_ASSERT_TRUE(tx.begin(msg.data(), msg.size(), 1));
    uint8_t out[GLINK_MAX_PACKET_BYTES];
    size_t n = tx.peek(20, out, sizeof(out));
    TEST_ASSERT_EQUAL(20, n);
    tx.advance();
    n = tx.peek(185, out, sizeof(out));  // the MTU grew: the rest goes in one packet
    TEST_ASSERT_EQUAL(GLINK_FRAME_HEADER_BYTES + 88, n);
    GlinkFrameHeader h;
    TEST_ASSERT_TRUE(glink_frame_header_parse(out, n, h));
    TEST_ASSERT_EQUAL(12, h.offset);
    tx.advance();
    TEST_ASSERT_TRUE(tx.idle());
}

static void test_tx_refuses_bad_input_and_aborts() {
    uint8_t storage[16];
    GlinkTx tx(storage, sizeof(storage));
    TEST_ASSERT_FALSE(tx.begin("", 0, 1));
    TEST_ASSERT_FALSE(tx.begin(nullptr, 4, 1));
    TEST_ASSERT_FALSE(tx.begin("0123456789abcdefX", 17, 1));  // larger than the buffer
    TEST_ASSERT_TRUE(tx.begin("hello", 5, 1));
    uint8_t out[32];
    TEST_ASSERT_EQUAL(0, tx.peek(8, out, sizeof(out)));   // packet size must exceed the header
    TEST_ASSERT_EQUAL(0, tx.peek(20, out, 10));           // output buffer too small
    tx.abort();
    TEST_ASSERT_TRUE(tx.idle());
    TEST_ASSERT_EQUAL(0, tx.peek(20, out, sizeof(out)));
}

static void test_tx_scratch_encode_then_commit() {
    uint8_t storage[64];
    GlinkTx tx(storage, sizeof(storage));
    char* scratch = tx.scratch();
    TEST_ASSERT_NOT_NULL(scratch);
    TEST_ASSERT_EQUAL(sizeof(storage), tx.scratch_capacity());
    memcpy(scratch, "{\"a\":1}", 7);
    TEST_ASSERT_FALSE(tx.commit(0, 1));
    TEST_ASSERT_FALSE(tx.commit(65, 1));
    TEST_ASSERT_TRUE(tx.commit(7, 9));
    TEST_ASSERT_NULL(tx.scratch());          // busy: the buffer is in use
    TEST_ASSERT_FALSE(tx.commit(7, 10));
    uint8_t out[32];
    const size_t n = tx.peek(20, out, sizeof(out));
    TEST_ASSERT_EQUAL(GLINK_FRAME_HEADER_BYTES + 7, n);
    TEST_ASSERT_EQUAL_MEMORY("{\"a\":1}", out + GLINK_FRAME_HEADER_BYTES, 7);
    tx.advance();
    TEST_ASSERT_NOT_NULL(tx.scratch());
}

static void test_pacer_two_fragments_per_20ms() {
    GlinkTxPacer p;
    TEST_ASSERT_EQUAL(2, p.allowance(1000));
    p.note_sent(1000, 1);
    TEST_ASSERT_EQUAL(1, p.allowance(1005));
    p.note_sent(1005, 1);
    TEST_ASSERT_EQUAL(0, p.allowance(1019));
    TEST_ASSERT_EQUAL(2, p.allowance(1020));  // new window
    p.note_sent(1020, 2);
    TEST_ASSERT_EQUAL(0, p.allowance(1039));
    TEST_ASSERT_EQUAL(2, p.allowance(1040));
    p.reset();
    TEST_ASSERT_EQUAL(2, p.allowance(1000));
}

static void test_pacer_survives_clock_wrap() {
    GlinkTxPacer p;
    p.note_sent(0xFFFFFFF0u, 2);
    TEST_ASSERT_EQUAL(0, p.allowance(0xFFFFFFFFu));
    TEST_ASSERT_EQUAL(2, p.allowance(0x00000010u));  // 32 ms later across the wrap
}

// ---- cadence ----
static GlinkPeriodKey key(GlinkDevState s) {
    GlinkPeriodKey k;
    k.dev_state = s;
    return k;
}

static void test_cadence_first_send_then_heartbeat() {
    GlinkPeriodCadence c;
    GlinkPeriodKey k = key(GlinkDevState::IDLE);
    TEST_ASSERT_TRUE(c.due(0, k, false, 23));
    c.mark_sent(0, k);
    TEST_ASSERT_FALSE(c.due(1999, k, false, 23));
    TEST_ASSERT_TRUE(c.due(2000, k, false, 23));  // heartbeat
    c.mark_sent(2000, k);
    TEST_ASSERT_FALSE(c.due(2001, k, false, 23));
}

static void test_cadence_weight_alone_does_not_trigger_outside_grinding() {
    GlinkPeriodCadence c;
    GlinkPeriodKey k = key(GlinkDevState::IDLE);
    c.mark_sent(0, k);
    TEST_ASSERT_FALSE(c.due(500, k, true, 185));
}

static void test_cadence_immediate_on_each_key_field_outside_grinding() {
    GlinkPeriodCadence c;
    const GlinkPeriodKey base = key(GlinkDevState::IDLE);
    c.mark_sent(0, base);
    GlinkPeriodKey k = base;
    k.select_profile = 2;
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
    k = base;
    k.blocked = GlinkError::NOT_READY;
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
    k = base;
    k.result_seq = 1;
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
    k = base;
    k.profile_rev = 1;
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
    k = base;
    k.phase = GlinkPhase::PREDICTIVE;
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
    k = key(GlinkDevState::DONE);
    TEST_ASSERT_TRUE(c.due(1, k, false, 23));
}

static void test_cadence_grinding_limits_and_mtu() {
    GlinkPeriodCadence c;
    GlinkPeriodKey k = key(GlinkDevState::GRINDING);
    c.mark_sent(0, k);  // the devState change itself was sent
    TEST_ASSERT_FALSE(c.due(249, k, true, 185));
    TEST_ASSERT_TRUE(c.due(250, k, true, 185));   // fast cadence, weight changed
    TEST_ASSERT_FALSE(c.due(250, k, false, 185)); // nothing changed
    TEST_ASSERT_FALSE(c.due(999, k, true, 99));
    TEST_ASSERT_TRUE(c.due(1000, k, true, 99));   // slow cadence below MTU 100
    TEST_ASSERT_FALSE(c.due(999, k, true, 0));    // unknown MTU counts as small
    TEST_ASSERT_TRUE(c.due(1000, k, true, 0));
    GlinkPeriodKey phase = k;
    phase.phase = GlinkPhase::PULSE_EXECUTE;
    TEST_ASSERT_FALSE(c.due(100, phase, false, 185));  // a phase change is capped while GRINDING
    TEST_ASSERT_TRUE(c.due(250, phase, false, 185));
}

static void test_cadence_dev_state_change_is_not_capped_while_grinding() {
    GlinkPeriodCadence c;
    GlinkPeriodKey k = key(GlinkDevState::GRINDING);
    c.mark_sent(0, k);
    TEST_ASSERT_TRUE(c.due(10, key(GlinkDevState::DONE), false, 23));
    c.mark_sent(10, key(GlinkDevState::DONE));
    TEST_ASSERT_TRUE(c.due(20, k, true, 23));  // entering GRINDING is also immediate
}

static void test_cadence_force_next_and_clock_wrap() {
    GlinkPeriodCadence c;
    GlinkPeriodKey k = key(GlinkDevState::IDLE);
    c.mark_sent(0xFFFFFF00u, k);
    TEST_ASSERT_FALSE(c.due(0xFFFFFF10u, k, false, 23));
    TEST_ASSERT_TRUE(c.due(0x00000700u, k, false, 23));  // 1792 ms + 0x100 > 2000 across the wrap
    c.force_next();
    TEST_ASSERT_TRUE(c.due(0xFFFFFF11u, k, false, 23));
}

// ---- session ----
static void test_session_packet_len_follows_mtu() {
    GlinkSession s;
    s.on_connect(0);
    TEST_ASSERT_EQUAL(GLINK_DEFAULT_PACKET_BYTES, s.packet_len());  // unknown
    s.set_mtu(23);
    TEST_ASSERT_EQUAL(20, s.packet_len());
    s.set_mtu(185);
    TEST_ASSERT_EQUAL(182, s.packet_len());
    s.set_mtu(517);
    TEST_ASSERT_EQUAL(GLINK_MAX_PACKET_BYTES, s.packet_len());  // 514 capped at 512
    s.set_mtu(2);
    TEST_ASSERT_EQUAL(GLINK_DEFAULT_PACKET_BYTES, s.packet_len());
}

// Regression: the grinder saw an ATT MTU of 0 (callback value missing) while the stack had negotiated 247, so it sent the hello
// reply as 17 packets of 20 bytes and the GaggiMate plugin dropped the link ("notification queue overflow").
static void test_effective_mtu_falls_back_to_the_stack_value() {
    TEST_ASSERT_EQUAL(247, glink_effective_mtu(0, 247));
    TEST_ASSERT_EQUAL(247, glink_effective_mtu(247, 0));
    TEST_ASSERT_EQUAL(247, glink_effective_mtu(247, 23));
    TEST_ASSERT_EQUAL(0, glink_effective_mtu(0, 0));
    GlinkSession s;
    s.on_connect(0);
    s.set_mtu(glink_effective_mtu(0, 247));
    TEST_ASSERT_EQUAL(244, s.packet_len());
    // baseInfo (about 200 bytes) is then 1 packet instead of 17
    TEST_ASSERT_EQUAL(1, glink_fragment_count(200, s.packet_len()));
    TEST_ASSERT_EQUAL(17, glink_fragment_count(200, GLINK_DEFAULT_PACKET_BYTES));
}

static void test_session_handshake_timeout() {
    GlinkSession s;
    TEST_ASSERT_FALSE(s.handshake_timed_out(100000));  // nobody connected
    s.on_connect(1000);
    TEST_ASSERT_FALSE(s.handshake_timed_out(10999));
    TEST_ASSERT_TRUE(s.handshake_timed_out(11000));
    s.on_handshake();
    TEST_ASSERT_FALSE(s.handshake_timed_out(500000));  // a handshaken client may stay silent
}

static void test_session_full_client_without_glink_activity_is_never_dropped() {
    GlinkSession s;
    s.on_connect(0, false);  // FULL: grinder.py, web flasher
    TEST_ASSERT_FALSE(s.handshake_timed_out(10000));
    TEST_ASSERT_FALSE(s.handshake_timed_out(3600000));
}

static void test_session_full_clock_starts_at_first_activity() {
    GlinkSession s;
    s.on_connect(0, false);
    s.note_activity(5000);
    s.note_activity(9000);  // later activity does not restart the clock
    TEST_ASSERT_FALSE(s.handshake_timed_out(14999));
    TEST_ASSERT_TRUE(s.handshake_timed_out(15000));
    s.on_handshake();
    TEST_ASSERT_FALSE(s.handshake_timed_out(99999));
}

static void test_session_link_only_clock_runs_from_connect() {
    GlinkSession s;
    s.on_connect(1000, true);
    s.note_activity(8000);  // no effect, the clock already runs
    TEST_ASSERT_FALSE(s.handshake_timed_out(10999));
    TEST_ASSERT_TRUE(s.handshake_timed_out(11000));
}

static void test_session_handshake_timeout_across_clock_wrap() {
    GlinkSession s;
    s.on_connect(0xFFFFF000u);
    TEST_ASSERT_FALSE(s.handshake_timed_out(0x00001000u));  // 8192 ms
    TEST_ASSERT_TRUE(s.handshake_timed_out(0x00001800u));   // 10240 ms
}

static void test_session_broadcast_sequence() {
    GlinkSession s;
    const GlinkPeriodKey k = key(GlinkDevState::IDLE);
    s.on_connect(0);
    s.on_handshake();
    TEST_ASSERT_FALSE(s.base_info_due());              // not subscribed yet: nothing goes out
    TEST_ASSERT_FALSE(s.period_due(0, k, false));
    s.set_subscribed(true);
    TEST_ASSERT_TRUE(s.base_info_due());
    TEST_ASSERT_FALSE(s.period_due(0, k, false));      // baseInfo first
    s.mark_base_info_sent();
    TEST_ASSERT_FALSE(s.base_info_due());
    TEST_ASSERT_TRUE(s.period_due(1, k, false));       // then periodInfo at once
    s.mark_period_sent(1, k);
    TEST_ASSERT_FALSE(s.period_due(2, k, false));
    TEST_ASSERT_TRUE(s.period_due(2001, k, false));    // heartbeat
}

static void test_session_repeated_hello_sends_both_again() {
    GlinkSession s;
    const GlinkPeriodKey k = key(GlinkDevState::IDLE);
    s.on_connect(0);
    s.set_subscribed(true);
    s.on_handshake();
    s.mark_base_info_sent();
    s.mark_period_sent(1, k);
    s.on_handshake();
    TEST_ASSERT_TRUE(s.base_info_due());
    TEST_ASSERT_FALSE(s.period_due(2, k, false));
    s.mark_base_info_sent();
    TEST_ASSERT_TRUE(s.period_due(3, k, false));
}

static void test_session_no_broadcast_without_handshake_or_after_disconnect() {
    GlinkSession s;
    const GlinkPeriodKey k = key(GlinkDevState::IDLE);
    s.on_connect(0);
    s.set_subscribed(true);
    TEST_ASSERT_FALSE(s.base_info_due());
    TEST_ASSERT_FALSE(s.period_due(0, k, false));
    s.on_handshake();
    s.set_mtu(185);
    s.reset();  // disconnect clears every per-connection state
    TEST_ASSERT_FALSE(s.connected());
    TEST_ASSERT_FALSE(s.handshaken());
    TEST_ASSERT_FALSE(s.subscribed());
    TEST_ASSERT_EQUAL(0, s.mtu());
    TEST_ASSERT_FALSE(s.base_info_due());
    s.on_handshake();  // ignored while nobody is connected
    TEST_ASSERT_FALSE(s.handshaken());
}

static void test_session_new_connection_clears_the_previous_one() {
    GlinkSession s;
    s.on_connect(0);
    s.on_handshake();
    s.set_subscribed(true);
    s.set_mtu(185);
    s.on_connect(50);
    TEST_ASSERT_TRUE(s.connected());
    TEST_ASSERT_FALSE(s.handshaken());
    TEST_ASSERT_FALSE(s.subscribed());
    TEST_ASSERT_EQUAL(0, s.mtu());
    TEST_ASSERT_TRUE(s.handshake_timed_out(50 + GLINK_HANDSHAKE_TIMEOUT_MS));
}

void run_glink_session_tests() {
    RUN_TEST(test_filter_table_every_characteristic_in_every_mode);
    RUN_TEST(test_filter_link_only_refuses_the_old_services);
    RUN_TEST(test_mode_resolve);
    RUN_TEST(test_rx_delivers_a_complete_message_once);
    RUN_TEST(test_rx_second_request_before_release_is_an_overrun);
    RUN_TEST(test_rx_oversize_is_reported_once_and_rest_is_discarded);
    RUN_TEST(test_rx_ten_bad_fragments_in_the_window_request_a_disconnect);
    RUN_TEST(test_rx_bad_fragments_spread_over_time_do_not_disconnect);
    RUN_TEST(test_rx_reset_forgets_everything);
    RUN_TEST(test_tx_fragments_a_message_in_order);
    RUN_TEST(test_tx_follows_an_mtu_change_mid_message);
    RUN_TEST(test_tx_refuses_bad_input_and_aborts);
    RUN_TEST(test_tx_scratch_encode_then_commit);
    RUN_TEST(test_pacer_two_fragments_per_20ms);
    RUN_TEST(test_pacer_survives_clock_wrap);
    RUN_TEST(test_cadence_first_send_then_heartbeat);
    RUN_TEST(test_cadence_weight_alone_does_not_trigger_outside_grinding);
    RUN_TEST(test_cadence_immediate_on_each_key_field_outside_grinding);
    RUN_TEST(test_cadence_grinding_limits_and_mtu);
    RUN_TEST(test_cadence_dev_state_change_is_not_capped_while_grinding);
    RUN_TEST(test_cadence_force_next_and_clock_wrap);
    RUN_TEST(test_session_packet_len_follows_mtu);
    RUN_TEST(test_effective_mtu_falls_back_to_the_stack_value);
    RUN_TEST(test_session_handshake_timeout);
    RUN_TEST(test_session_full_client_without_glink_activity_is_never_dropped);
    RUN_TEST(test_session_full_clock_starts_at_first_activity);
    RUN_TEST(test_session_link_only_clock_runs_from_connect);
    RUN_TEST(test_session_handshake_timeout_across_clock_wrap);
    RUN_TEST(test_session_broadcast_sequence);
    RUN_TEST(test_session_repeated_hello_sends_both_again);
    RUN_TEST(test_session_no_broadcast_without_handshake_or_after_disconnect);
    RUN_TEST(test_session_new_connection_clears_the_previous_one);
}

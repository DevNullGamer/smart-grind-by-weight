// Client session (handshake, one request at a time, timeouts, start decision, tracking) with a fake link, driven by
// messages from the shared conformance vectors.
#include <stdint.h>
#include <string.h>
#include <unity.h>
#include <string>
#include <vector>
#include "GLinkClient.h"
#include "glink_vectors.h"

using namespace glinkc;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    return out;
}

struct FakeSink : Sink {
    std::vector<std::vector<uint8_t>> packets;
    bool fail = false;
    bool write_packet(const uint8_t* p, size_t n) override {
        if (fail) return false;
        packets.emplace_back(p, p + n);
        return true;
    }
    // The messages written so far, reassembled.
    std::vector<std::string> messages() const {
        std::vector<std::string> out;
        uint8_t storage[GLINK_MAX_MESSAGE_BYTES];
        GlinkReassembler r(storage, sizeof(storage));
        for (const auto& p : packets) {
            if (r.feed(p.data(), p.size(), 0) == GlinkFragmentResult::COMPLETE) {
                out.emplace_back(reinterpret_cast<const char*>(r.message()), r.message_length());
            }
        }
        return out;
    }
};

struct Recorder : Listener {
    int stop_timeouts = 0;
    int ready = 0, base = 0, periods = 0, rev_changes = 0, results = 0, profiles = 0, timeouts = 0, stops = 0, disconnects = 0, outcomes = 0;
    std::vector<RequestKind> response_kinds;
    std::vector<bool> response_ok;
    std::string last_failure_code;
    bool stop_accepted = false;
    bool started = false;
    std::string outcome_state, outcome_blocked, disconnect_reason;
    RequestKind timeout_kind = RequestKind::NONE;
    void on_ready() override { ready++; }
    void on_base_info(const BaseInfo&) override { base++; }
    void on_period(const PeriodInfo&, bool changed) override { periods++; if (changed) rev_changes++; }
    void on_result(const ResultInfo&) override { results++; }
    void on_profiles(const ProfileEntry*, int) override { profiles++; }
    void on_response(RequestKind k, const Response& r) override {
        response_kinds.push_back(k);
        response_ok.push_back(r.success);
        last_failure_code = r.failure.code;
    }
    void on_request_timeout(RequestKind k) override { timeouts++; timeout_kind = k; }
    void on_stop_timeout() override { stop_timeouts++; }
    void on_stop_reply(bool a) override { stops++; stop_accepted = a; }
    void on_start_outcome(bool s, const char* st, const char* bl) override { outcomes++; started = s; outcome_state = st; outcome_blocked = bl; }
    void on_must_disconnect(const char* why) override { disconnects++; disconnect_reason = why; }
};

const GJson& pair_named(const char* name) {
    for (const GJson& p : glink_vectors().at("pairs").a) {
        if (p.at("name").s == name) return p;
    }
    TEST_FAIL_MESSAGE(name);
    return glink_vectors();
}

std::string response_of(const char* name) { return pair_named(name).at("response").s; }

std::string period_json(const char* dev_state, uint32_t result_seq, uint32_t rev, const char* extra = "") {
    return std::string("{\"broadcast\":{\"periodInfo\":{\"devState\":\"") + dev_state + "\",\"weight\":0.0,\"selectProfile\":1," + extra +
           "\"blocked\":null,\"resultSeq\":" + std::to_string(result_seq) + ",\"profileRev\":" + std::to_string(rev) + "}}}";
}

struct Fixture {
    FakeSink sink;
    Recorder rec;
    Client client{sink, rec};
    uint32_t now = 1000;
    uint16_t device_seq = 100;

    // The device sends a message in packets of `packet` bytes.
    void device_sends(const std::string& text, size_t packet = 20) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(text.data());
        const size_t count = glink_fragment_count(text.size(), packet);
        TEST_ASSERT_TRUE(count > 0);
        device_seq++;
        for (size_t i = 0; i < count; i++) {
            uint8_t buf[GLINK_MAX_PACKET_BYTES];
            const size_t n = glink_fragment_build(m, text.size(), device_seq, packet, i, buf, sizeof(buf));
            TEST_ASSERT_TRUE(n > 0);
            now += 5;
            client.on_notify(buf, n, now);
        }
    }
    void connect_and_ready(uint32_t rev = 0, uint32_t result_seq = 0) {
        TEST_ASSERT_TRUE(client.on_connected(20, now));
        device_sends(pair_named("appHello_handshake").at("broadcasts").a[0].s);
        device_sends(period_json("IDLE", result_seq, rev));
        TEST_ASSERT_TRUE(client.state() == LinkState::READY);
    }
};

}  // namespace

void test_hello_is_the_golden_handshake() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    const GJson& golden = glink_vectors().at("frame_cases").a[0];
    TEST_ASSERT_EQUAL_STRING("handshake_20", golden.at("name").s.c_str());
    const auto& packets = golden.at("packets").a;
    TEST_ASSERT_EQUAL_INT(static_cast<long long>(packets.size()), static_cast<long long>(f.sink.packets.size()));
    for (size_t i = 0; i < packets.size(); i++) TEST_ASSERT_TRUE(f.sink.packets[i] == from_hex(packets[i].s));
    TEST_ASSERT_TRUE(f.client.state() == LinkState::CONNECTED);
}

void test_hello_uses_a_big_mtu_in_one_packet() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(182, f.now));
    TEST_ASSERT_EQUAL_INT(1, static_cast<long long>(f.sink.packets.size()));
    const std::string sent = f.sink.messages()[0];
    TEST_ASSERT_EQUAL_STRING("{\"request\":{\"appHello\":{\"op\":\"handshake\"}}}", sent.c_str());
}

void test_connect_fails_when_the_write_fails() {
    Fixture f;
    f.sink.fail = true;
    TEST_ASSERT_FALSE(f.client.on_connected(20, f.now));
}

void test_ready_after_base_and_period() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_EQUAL_INT(1, f.rec.base);
    TEST_ASSERT_EQUAL_INT(1, f.rec.ready);
    TEST_ASSERT_EQUAL_INT(1, f.rec.periods);
    TEST_ASSERT_TRUE(f.client.has_base());
    TEST_ASSERT_EQUAL_STRING("SGBW", f.client.base().device);
    TEST_ASSERT_TRUE(f.client.needs_profile_refresh());  // the first periodInfo counts as a change
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_EQUAL_INT(1, f.rec.ready);
    TEST_ASSERT_EQUAL_INT(1, f.rec.rev_changes);
}

void test_no_period_in_5s_asks_to_disconnect() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    f.client.tick(f.now + 4999);
    TEST_ASSERT_EQUAL_INT(0, f.rec.disconnects);
    f.client.tick(f.now + 5000);
    TEST_ASSERT_EQUAL_INT(1, f.rec.disconnects);
    f.client.tick(f.now + 9000);
    TEST_ASSERT_EQUAL_INT(1, f.rec.disconnects);  // once
}

void test_foreign_device_is_refused() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    f.device_sends("{\"broadcast\":{\"baseInfo\":{\"releaseVer\":1,\"snCode\":\"x\",\"wifiName\":null,\"device\":\"MT80\",\"proto\":1}}}");
    TEST_ASSERT_EQUAL_INT(1, f.rec.disconnects);
    TEST_ASSERT_EQUAL_INT(0, f.rec.base);
    Fixture g;
    TEST_ASSERT_TRUE(g.client.on_connected(20, g.now));
    g.device_sends("{\"broadcast\":{\"baseInfo\":{\"releaseVer\":1,\"snCode\":\"x\",\"wifiName\":null,\"device\":\"SGBW\",\"proto\":2}}}");
    TEST_ASSERT_EQUAL_INT(1, g.rec.disconnects);
}

void test_one_request_at_a_time_and_profile_cache() {
    Fixture f;
    f.connect_and_ready(3);
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::PROFILE_GET);
    TEST_ASSERT_FALSE(f.client.profile_get(-1, f.now));
    TEST_ASSERT_FALSE(f.client.result_get(f.now));
    const std::string sent = f.sink.messages().back();
    TEST_ASSERT_EQUAL_STRING("{\"request\":{\"grindProfile\":{\"op\":\"get\"}}}", sent.c_str());

    f.device_sends(response_of("profile_get_all_with_profile_3"));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    TEST_ASSERT_EQUAL_INT(4, f.client.profile_count());
    TEST_ASSERT_EQUAL_INT(3, f.client.profiles()[3].index);
    TEST_ASSERT_EQUAL_INT(1, f.rec.profiles);
    TEST_ASSERT_TRUE(f.rec.response_kinds.back() == RequestKind::PROFILE_GET);
    TEST_ASSERT_TRUE(f.rec.response_ok.back());
    TEST_ASSERT_FALSE(f.client.needs_profile_refresh());

    f.device_sends(period_json("IDLE", 0, 3));  // same revision
    TEST_ASSERT_FALSE(f.client.needs_profile_refresh());
    f.device_sends(period_json("IDLE", 0, 4));  // edited on the grinder
    TEST_ASSERT_TRUE(f.client.needs_profile_refresh());
}

void test_revision_change_during_a_read_keeps_the_refresh_flag() {
    Fixture f;
    f.connect_and_ready(3);
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    f.device_sends(period_json("IDLE", 0, 4));
    f.device_sends(response_of("profile_get_all_with_profile_3"));
    TEST_ASSERT_TRUE(f.client.needs_profile_refresh());
}

void test_update_response_updates_one_cache_slot() {
    Fixture f;
    f.connect_and_ready();
    ProfileUpdate u;
    u.has_dose = true;
    u.dose_tenths = 180;
    u.has_size = true;
    u.size_tenths = 145;
    u.has_unit = true;
    strcpy(u.unit, "um");
    TEST_ASSERT_TRUE(f.client.profile_update(3, u, f.now));
    f.device_sends(response_of("profile_update_create"));
    TEST_ASSERT_EQUAL_INT(1, f.client.profile_count());
    TEST_ASSERT_EQUAL_INT(3, f.client.profiles()[0].index);
    TEST_ASSERT_EQUAL_INT(180, f.client.profiles()[0].dose_tenths);
    TEST_ASSERT_EQUAL_STRING("um", f.client.profiles()[0].grind_unit);
}

void test_failures_reach_the_listener_and_free_the_slot() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.profile_select(3, f.now));
    f.device_sends(response_of("profile_select_not_set"));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    TEST_ASSERT_FALSE(f.rec.response_ok.back());
    TEST_ASSERT_EQUAL_STRING("PROFILE_NOT_SET", f.rec.last_failure_code.c_str());

    // an unaddressable failure answers whatever is pending
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
    f.device_sends(response_of("envelope_malformed_json"));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    TEST_ASSERT_EQUAL_STRING("MALFORMED_JSON", f.rec.last_failure_code.c_str());
}

void test_mismatching_response_is_ignored() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    f.device_sends(response_of("result_get"));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::PROFILE_GET);
    TEST_ASSERT_EQUAL_INT(0, static_cast<long long>(f.rec.response_kinds.size()));
}

void test_response_timeout_is_5s() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
    const uint32_t t0 = f.now;
    f.client.tick(t0 + 4999);
    TEST_ASSERT_EQUAL_INT(0, f.rec.timeouts);
    f.client.tick(t0 + 5000);
    TEST_ASSERT_EQUAL_INT(1, f.rec.timeouts);
    TEST_ASSERT_TRUE(f.rec.timeout_kind == RequestKind::RESULT_GET);
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    TEST_ASSERT_TRUE(f.client.result_get(f.now));  // free again
}

void test_timeout_survives_the_clock_wrap() {
    Fixture f;
    f.now = 0xFFFFFF00u;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
    const uint32_t t0 = f.now;
    f.client.tick(t0 + 4999);
    TEST_ASSERT_EQUAL_INT(0, f.rec.timeouts);
    f.client.tick(t0 + 5000);  // wraps past zero
    TEST_ASSERT_EQUAL_INT(1, f.rec.timeouts);
}

void test_stop_is_sent_any_time_and_does_not_use_the_slot() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    TEST_ASSERT_TRUE(f.client.stop(f.now));  // before the handshake finished
    const std::string sent = f.sink.messages().back();
    TEST_ASSERT_EQUAL_STRING("{\"request\":{\"grindControl\":{\"op\":\"stop\"}}}", sent.c_str());
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    TEST_ASSERT_TRUE(f.client.stop(f.now));  // while a request is pending
    f.device_sends(response_of("control_stop"));
    TEST_ASSERT_EQUAL_INT(1, f.rec.stops);
    TEST_ASSERT_TRUE(f.rec.stop_accepted);
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::PROFILE_GET);
}

void test_stop_refused_when_disconnected() {
    Fixture f;
    TEST_ASSERT_FALSE(f.client.stop(f.now));
    TEST_ASSERT_FALSE(f.client.profile_get(-1, f.now));
}

void test_start_decision_grinding() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(3, 180, f.now));
    const std::string sent = f.sink.messages().back();
    const std::string want = pair_named("control_start").at("request").s;
    TEST_ASSERT_EQUAL_STRING(want.c_str(), sent.c_str());
    f.device_sends(response_of("control_start"));
    TEST_ASSERT_TRUE(f.client.start_outcome_pending());
    f.device_sends(period_json("STARTING", 0, 0, "\"target\":18.0,\"mode\":\"WEIGHT\",\"leadInMs\":2000,"));
    TEST_ASSERT_EQUAL_INT(0, f.rec.outcomes);
    f.device_sends(period_json("GRINDING", 0, 0, "\"phase\":\"PREDICTIVE\",\"target\":18.0,\"mode\":\"WEIGHT\","));
    TEST_ASSERT_EQUAL_INT(1, f.rec.outcomes);
    TEST_ASSERT_TRUE(f.rec.started);
    TEST_ASSERT_FALSE(f.client.start_outcome_pending());
}

void test_start_decision_cancelled() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(1, 180, f.now));
    f.device_sends(response_of("control_start_without_expect"));
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_EQUAL_INT(1, f.rec.outcomes);
    TEST_ASSERT_FALSE(f.rec.started);
    TEST_ASSERT_EQUAL_STRING("IDLE", f.rec.outcome_state.c_str());
}

void test_start_decision_times_out_after_lead_in_plus_2s() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(3, 180, f.now));
    f.device_sends(response_of("control_start_selects_nothing"));
    TEST_ASSERT_TRUE(f.client.start_outcome_pending());
    const uint32_t answered = f.now;
    f.client.tick(answered + 4999);
    TEST_ASSERT_EQUAL_INT(0, f.rec.outcomes);
    f.client.tick(answered + 5000);  // 3000 + 2000
    TEST_ASSERT_EQUAL_INT(1, f.rec.outcomes);
    TEST_ASSERT_FALSE(f.rec.started);
}

void test_refused_start_has_no_outcome_wait() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(1, 180, f.now));
    f.device_sends(response_of("control_start_not_ready"));
    TEST_ASSERT_FALSE(f.client.start_outcome_pending());
    TEST_ASSERT_EQUAL_STRING("NOT_READY", f.rec.last_failure_code.c_str());
}

void test_result_tracking() {
    Fixture f;
    f.connect_and_ready(0, 5);
    TEST_ASSERT_TRUE(f.client.needs_result_fetch());
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
    f.device_sends(response_of("result_get"));
    TEST_ASSERT_TRUE(f.client.has_result());
    TEST_ASSERT_EQUAL_INT(5, f.client.last_result().seq);
    TEST_ASSERT_FALSE(f.client.needs_result_fetch());
    TEST_ASSERT_EQUAL_INT(1, f.rec.results);

    // a broadcast with a newer seq
    f.device_sends(period_json("DONE", 6, 0));
    TEST_ASSERT_TRUE(f.client.needs_result_fetch());
    f.device_sends(glink_vectors().at("result_broadcasts").a[1].at("message").s);  // seq 6
    TEST_ASSERT_FALSE(f.client.needs_result_fetch());
    TEST_ASSERT_EQUAL_STRING("STOPPED", f.client.last_result().code);
    // an old duplicate does not replace the newer one
    f.device_sends(glink_vectors().at("result_broadcasts").a[0].at("message").s);  // seq 5
    TEST_ASSERT_EQUAL_INT(6, f.client.last_result().seq);
}

void test_result_counter_restart_after_a_grinder_reboot() {
    Fixture f;
    f.connect_and_ready(0, 7);
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
    f.device_sends(response_of("result_get"));  // seq 5 seen
    f.client.on_disconnected();
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    f.device_sends(period_json("IDLE", 0, 0));  // rebooted: counter back to 0
    TEST_ASSERT_FALSE(f.client.needs_result_fetch());
    f.device_sends(period_json("IDLE", 1, 0));
    TEST_ASSERT_TRUE(f.client.needs_result_fetch());
}

void test_disconnect_clears_the_connection() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    f.client.on_disconnected();
    TEST_ASSERT_TRUE(f.client.state() == LinkState::DISCONNECTED);
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    TEST_ASSERT_FALSE(f.client.has_period());
    TEST_ASSERT_FALSE(f.client.has_base());
    TEST_ASSERT_FALSE(f.client.start_outcome_pending());
    f.client.tick(f.now + 60000);
    TEST_ASSERT_EQUAL_INT(0, f.rec.timeouts);
    // a notification after the disconnect is ignored
    const std::string text = period_json("IDLE", 0, 0);
    f.device_sends(text);
    TEST_ASSERT_FALSE(f.client.has_period());
}

void test_reconnect_starts_clean_and_marks_profiles_stale() {
    Fixture f;
    f.connect_and_ready(2);
    TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
    f.device_sends(response_of("profile_get_all_with_profile_3"));
    TEST_ASSERT_FALSE(f.client.needs_profile_refresh());
    f.client.on_disconnected();
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    f.device_sends(period_json("IDLE", 0, 2));  // same revision, but a new connection
    TEST_ASSERT_TRUE(f.client.needs_profile_refresh());
    TEST_ASSERT_EQUAL_INT(2, f.rec.ready);
}

void test_bad_fragments_disconnect_like_the_spec() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    const uint8_t junk[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (int i = 0; i < 9; i++) {
        f.now += 500;
        f.client.on_notify(junk, sizeof(junk), f.now);
    }
    TEST_ASSERT_EQUAL_INT(0, f.rec.disconnects);
    f.now += 500;
    f.client.on_notify(junk, sizeof(junk), f.now);
    TEST_ASSERT_EQUAL_INT(1, f.rec.disconnects);
}

void test_sequence_counts_from_1_and_wraps() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(182, f.now));
    TEST_ASSERT_EQUAL_INT(1, f.sink.packets.back()[2] | (f.sink.packets.back()[3] << 8));
    TEST_ASSERT_TRUE(f.client.stop(f.now));
    TEST_ASSERT_EQUAL_INT(2, f.sink.packets.back()[2] | (f.sink.packets.back()[3] << 8));
    for (int i = 0; i < 65532; i++) {
        f.sink.packets.clear();
        TEST_ASSERT_TRUE(f.client.stop(f.now));
    }
    TEST_ASSERT_EQUAL_INT(65534, f.sink.packets.back()[2] | (f.sink.packets.back()[3] << 8));
    f.sink.packets.clear();
    TEST_ASSERT_TRUE(f.client.stop(f.now));
    TEST_ASSERT_EQUAL_INT(65535, f.sink.packets.back()[2] | (f.sink.packets.back()[3] << 8));
    TEST_ASSERT_TRUE(f.client.stop(f.now));
    TEST_ASSERT_EQUAL_INT(1, f.sink.packets.back()[2] | (f.sink.packets.back()[3] << 8));
}

void test_write_failure_keeps_the_slot_free() {
    Fixture f;
    f.connect_and_ready();
    f.sink.fail = true;
    TEST_ASSERT_FALSE(f.client.result_get(f.now));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
    f.sink.fail = false;
    TEST_ASSERT_TRUE(f.client.result_get(f.now));
}

void test_invalid_arguments_send_nothing() {
    Fixture f;
    f.connect_and_ready();
    const size_t before = f.sink.packets.size();
    TEST_ASSERT_FALSE(f.client.profile_select(4, f.now));
    TEST_ASSERT_FALSE(f.client.start(3, 10, f.now));
    ProfileUpdate u;
    TEST_ASSERT_FALSE(f.client.profile_update(3, u, f.now));
    TEST_ASSERT_EQUAL_INT(static_cast<long long>(before), static_cast<long long>(f.sink.packets.size()));
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::NONE);
}

void test_message_split_across_many_small_packets_and_big_ones() {
    for (size_t packet : {9u, 20u, 100u, 512u}) {
        Fixture f;
        TEST_ASSERT_TRUE(f.client.on_connected(packet, f.now));
        f.device_sends(period_json("IDLE", 0, 0), packet);
        TEST_ASSERT_TRUE(f.client.state() == LinkState::READY);
        TEST_ASSERT_TRUE(f.client.profile_get(-1, f.now));
        f.device_sends(response_of("profile_get_all_with_profile_3"), packet);
        TEST_ASSERT_EQUAL_INT(4, f.client.profile_count());
    }
}

void test_start_needs_a_period_first() {
    Fixture f;
    TEST_ASSERT_TRUE(f.client.on_connected(20, f.now));
    TEST_ASSERT_FALSE(f.client.start(3, 180, f.now));  // no periodInfo yet
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.client.start(3, 180, f.now));
}

void test_start_after_a_start_timeout_waits_for_a_period() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(3, 180, f.now));
    f.client.tick(f.now + 5000);
    TEST_ASSERT_EQUAL_INT(1, f.rec.timeouts);
    TEST_ASSERT_FALSE(f.client.start(3, 180, f.now + 5000));
    f.device_sends(period_json("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.client.start(3, 180, f.now));
}

void test_start_always_carries_expect_dose() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.start(1, 185, f.now));
    const std::string sent = f.sink.messages().back();
    TEST_ASSERT_EQUAL_STRING("{\"request\":{\"grindControl\":{\"op\":\"start\",\"profile\":1,\"expectDose\":18.5}}}", sent.c_str());
}

void test_stop_timeout_and_failed_stop_reply() {
    Fixture f;
    f.connect_and_ready();
    TEST_ASSERT_TRUE(f.client.stop(f.now));
    f.client.tick(f.now + 4999);
    TEST_ASSERT_EQUAL_INT(0, f.rec.stop_timeouts);
    f.client.tick(f.now + 5000);
    TEST_ASSERT_EQUAL_INT(1, f.rec.stop_timeouts);

    // a failure under grindControl while stop and start are both outstanding belongs to the stop
    TEST_ASSERT_TRUE(f.client.start(1, 180, f.now));
    TEST_ASSERT_TRUE(f.client.stop(f.now));
    f.device_sends(response_of("control_start_not_ready"));
    TEST_ASSERT_EQUAL_INT(1, f.rec.stops);
    TEST_ASSERT_FALSE(f.rec.stop_accepted);
    TEST_ASSERT_TRUE(f.client.pending() == RequestKind::START);
}

void run_glink_plugin_client_tests() {
    RUN_TEST(test_start_needs_a_period_first);
    RUN_TEST(test_start_after_a_start_timeout_waits_for_a_period);
    RUN_TEST(test_start_always_carries_expect_dose);
    RUN_TEST(test_stop_timeout_and_failed_stop_reply);
    RUN_TEST(test_hello_is_the_golden_handshake);
    RUN_TEST(test_hello_uses_a_big_mtu_in_one_packet);
    RUN_TEST(test_connect_fails_when_the_write_fails);
    RUN_TEST(test_ready_after_base_and_period);
    RUN_TEST(test_no_period_in_5s_asks_to_disconnect);
    RUN_TEST(test_foreign_device_is_refused);
    RUN_TEST(test_one_request_at_a_time_and_profile_cache);
    RUN_TEST(test_revision_change_during_a_read_keeps_the_refresh_flag);
    RUN_TEST(test_update_response_updates_one_cache_slot);
    RUN_TEST(test_failures_reach_the_listener_and_free_the_slot);
    RUN_TEST(test_mismatching_response_is_ignored);
    RUN_TEST(test_response_timeout_is_5s);
    RUN_TEST(test_timeout_survives_the_clock_wrap);
    RUN_TEST(test_stop_is_sent_any_time_and_does_not_use_the_slot);
    RUN_TEST(test_stop_refused_when_disconnected);
    RUN_TEST(test_start_decision_grinding);
    RUN_TEST(test_start_decision_cancelled);
    RUN_TEST(test_start_decision_times_out_after_lead_in_plus_2s);
    RUN_TEST(test_refused_start_has_no_outcome_wait);
    RUN_TEST(test_result_tracking);
    RUN_TEST(test_result_counter_restart_after_a_grinder_reboot);
    RUN_TEST(test_disconnect_clears_the_connection);
    RUN_TEST(test_reconnect_starts_clean_and_marks_profiles_stale);
    RUN_TEST(test_bad_fragments_disconnect_like_the_spec);
    RUN_TEST(test_sequence_counts_from_1_and_wraps);
    RUN_TEST(test_write_failure_keeps_the_slot_free);
    RUN_TEST(test_invalid_arguments_send_nothing);
    RUN_TEST(test_message_split_across_many_small_packets_and_big_ones);
}

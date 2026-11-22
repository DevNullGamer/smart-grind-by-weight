// GrinderCore (chg025 step 3): the pure session orchestrator, driven through a fake link with golden frames from the shared
// conformance vectors. Written against spec 3.3 / step 3 "Tester must verify" and section 0 (Q1, Q2, Q4).
#include <stdint.h>
#include <string.h>
#include <unity.h>
#include <string>
#include <vector>
#include "GLinkGrinderCore.h"
#include "glink_vectors.h"

using namespace glinkc;

// Compares the last message written; the copy keeps the string alive for the comparison.
#define ASSERT_LAST(fixture, expected)                                  \
    do {                                                                \
        const std::string last_ = (fixture).last();                     \
        TEST_ASSERT_EQUAL_STRING(expected, last_.c_str());              \
    } while (0)

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

const GJson& pair_named(const char* name) {
    for (const GJson& p : glink_vectors().at("pairs").a) {
        if (p.at("name").s == name) return p;
    }
    TEST_FAIL_MESSAGE(name);
    return glink_vectors();
}

// ---- message builders (shapes taken from the vectors) ----

std::string period(const char* dev_state, uint32_t result_seq, uint32_t rev, int select = 1, const char* extra = "",
                   const char* weight = "0.0", const char* blocked = "null") {
    return std::string("{\"broadcast\":{\"periodInfo\":{\"devState\":\"") + dev_state + "\",\"weight\":" + weight +
           ",\"selectProfile\":" + std::to_string(select) + "," + extra + "\"blocked\":" + blocked +
           ",\"resultSeq\":" + std::to_string(result_seq) + ",\"profileRev\":" + std::to_string(rev) + "}}}";
}

std::string base_info() { return pair_named("appHello_handshake").at("broadcasts").a[0].s; }

// the same baseInfo as a 1.1 grinder sends it: no protoMinor member
std::string base_info_without_proto_minor() {
    std::string s = base_info();
    const std::string member = "\"protoMinor\":2,";
    const size_t at = s.find(member);
    TEST_ASSERT_TRUE(at != std::string::npos);
    s.erase(at, member.size());
    return s;
}
std::string base_info_with_proto_minor(int minor) {
    std::string s = base_info();
    const std::string member = "\"protoMinor\":2,";
    const size_t at = s.find(member);
    TEST_ASSERT_TRUE(at != std::string::npos);
    s.replace(at, member.size(), "\"protoMinor\":" + std::to_string(minor) + ",");
    return s;
}

// profiles 0 to 2 as on the real grinder, entry 3 only when dose3 is given (size3 "null" = none)
std::string profiles(const char* dose3 = nullptr, const char* size3 = "null") {
    std::string s =
        "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":["
        "{\"index\":0,\"name\":\"SINGLE\",\"dose\":9.0,\"grindSize\":null,\"grindUnit\":null,\"writable\":false},"
        "{\"index\":1,\"name\":\"DOUBLE\",\"dose\":18.0,\"grindSize\":null,\"grindUnit\":null,\"writable\":false},"
        "{\"index\":2,\"name\":\"CUSTOM\",\"dose\":21.5,\"grindSize\":null,\"grindUnit\":null,\"writable\":false}";
    if (dose3) {
        s += std::string(",{\"index\":3,\"name\":\"PROFILE\",\"dose\":") + dose3 + ",\"grindSize\":" + size3 +
             ",\"grindUnit\":null,\"writable\":true}";
    }
    return s + "]}}}";
}

// like profiles(), entry 3 with an rpm (nullptr = none)
std::string profiles_rpm(const char* dose3, const char* size3, const char* rpm3) {
    std::string s = profiles(dose3, size3);
    if (dose3 && rpm3) {
        const std::string tail = ",\"grindUnit\":null,\"writable\":true}]}}}";
        const size_t at = s.rfind(tail);
        TEST_ASSERT_TRUE(at != std::string::npos);
        s.replace(at, tail.size(), std::string(",\"grindUnit\":null,\"rpm\":") + rpm3 + ",\"writable\":true}]}}}");
    }
    return s;
}

std::string update_ok_rpm(const char* dose, const char* size, const char* rpm, bool changed) {
    return std::string("{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":{\"index\":3,\"name\":\"PROFILE\",\"dose\":") +
           dose + ",\"grindSize\":" + size + ",\"grindUnit\":null" + (rpm ? std::string(",\"rpm\":") + rpm : std::string()) +
           ",\"writable\":true,\"changed\":" + (changed ? "true" : "false") + ",\"stored\":" + (changed ? "true" : "false") +
           "}}}}";
}

std::string update_ok(const char* dose, const char* size, bool changed) {
    return std::string("{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":{\"index\":3,\"name\":\"PROFILE\",\"dose\":") +
           dose + ",\"grindSize\":" + size + ",\"grindUnit\":null,\"writable\":true,\"changed\":" + (changed ? "true" : "false") +
           ",\"stored\":" + (changed ? "true" : "false") + "}}}}";
}

std::string select_ok(int index) {
    return std::string("{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":{\"selectProfile\":") + std::to_string(index) +
           "}}}}";
}

std::string failure(const char* code) {
    return std::string("{\"response\":{\"grindProfile\":{\"result\":\"fail\",\"error\":{\"code\":\"") + code +
           "\",\"message\":\"x\"}}}}";
}

std::string result_body(uint32_t seq, const char* code, const char* final_weight, bool settled) {
    return std::string("{\"seq\":") + std::to_string(seq) + ",\"code\":\"" + code +
           "\",\"origin\":\"TOUCH\",\"profile\":3,\"mode\":\"WEIGHT\",\"target\":18.0,\"finalWeight\":" + final_weight +
           ",\"weightSettled\":" + (settled ? "true" : "false") + ",\"pulses\":2,\"durationMs\":14230,\"motorMs\":11800}";
}
std::string result_broadcast(uint32_t seq, const char* code, const char* w, bool settled) {
    return "{\"broadcast\":{\"grindResult\":" + result_body(seq, code, w, settled) + "}}";
}
std::string result_response(uint32_t seq, const char* code, const char* w, bool settled) {
    return "{\"response\":{\"grindResult\":{\"result\":\"success\",\"data\":" + result_body(seq, code, w, settled) + "}}}";
}

const char* const GET_ALL = "{\"request\":{\"grindProfile\":{\"op\":\"get\"}}}";
const char* const GET_RESULT = "{\"request\":{\"grindResult\":{\"op\":\"get\"}}}";
const char* const SELECT_3 = "{\"request\":{\"grindProfile\":{\"op\":\"select\",\"index\":3}}}";

struct F {
    FakeSink sink;
    GrinderCore core{sink};
    uint32_t now = 1000;
    uint16_t device_seq = 100;
    int proto_minor = 2;  // 0 = a 1.1 grinder: baseInfo without protoMinor

    void rx(const std::string& text, size_t packet = 20) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(text.data());
        const size_t count = glink_fragment_count(text.size(), packet);
        TEST_ASSERT_TRUE(count > 0);
        device_seq++;
        for (size_t i = 0; i < count; i++) {
            uint8_t buf[GLINK_MAX_PACKET_BYTES];
            const size_t n = glink_fragment_build(m, text.size(), device_seq, packet, i, buf, sizeof(buf));
            TEST_ASSERT_TRUE(n > 0);
            now += 5;
            core.on_packet(buf, n, now);
        }
    }
    void tick(uint32_t advance_ms = 1000) {
        now += advance_ms;
        core.update(now);
    }
    // connect and handshake (baseInfo + periodInfo); the periodInfo uses `dev_state`, `select` and the given counters
    void connect(uint32_t result_seq = 0, uint32_t rev = 0, const char* dev_state = "IDLE", int select = 1) {
        TEST_ASSERT_TRUE(core.on_connected(20, now));
        rx(proto_minor == 2 ? base_info() : proto_minor == 0 ? base_info_without_proto_minor() : base_info_with_proto_minor(proto_minor));
        rx(period(dev_state, result_seq, rev, select));
        TEST_ASSERT_TRUE(core.ready());
    }
    std::vector<std::string> msgs() const { return sink.messages(); }
    std::string last() const { return sink.messages().back(); }
    int count(const char* needle) const {
        int n = 0;
        for (const auto& m : msgs()) {
            if (m.find(needle) != std::string::npos) n++;
        }
        return n;
    }
    int updates() const { return count("\"op\":\"update\""); }
    int selects() const { return count("\"op\":\"select\""); }
    // first tick reads the stale profiles (one profile_get), the answer fills the cache
    void serve_profiles(const char* dose3 = nullptr, const char* size3 = "null") {
        tick();
        ASSERT_LAST(*this, GET_ALL);
        rx(profiles(dose3, size3));
    }
    void connect_with_rpm_profiles(const char* dose3, const char* size3, const char* rpm3, int select = 1) {
        connect(0, 0, "IDLE", select);
        tick();
        ASSERT_LAST(*this, GET_ALL);
        rx(profiles_rpm(dose3, size3, rpm3));
    }
    void connect_with_profiles(const char* dose3 = "18.0", const char* size3 = "14.5", uint32_t result_seq = 0, int select = 1) {
        connect(result_seq, 0, "IDLE", select);
        serve_profiles(dose3, size3);
    }
};

}  // namespace

// ---- handshake -------------------------------------------------------------------------------------------------------

void test_core_connect_writes_the_golden_hello() {
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    const GJson& golden = glink_vectors().at("frame_cases").a[0];
    TEST_ASSERT_EQUAL_STRING("handshake_20", golden.at("name").s.c_str());
    const auto& packets = golden.at("packets").a;
    TEST_ASSERT_EQUAL_INT(static_cast<long long>(packets.size()), static_cast<long long>(f.sink.packets.size()));
    for (size_t i = 0; i < packets.size(); i++) TEST_ASSERT_TRUE(f.sink.packets[i] == from_hex(packets[i].s));
    TEST_ASSERT_FALSE(f.core.ready());
    TEST_ASSERT_TRUE(f.core.link_state() == LinkState::CONNECTED);
}

void test_core_failed_hello_write_reports_false() {
    F f;
    f.sink.fail = true;
    TEST_ASSERT_FALSE(f.core.on_connected(20, f.now));
}

void test_core_burst_of_small_packets_reaches_ready_and_baseline_is_taken_once() {
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    f.rx(base_info());
    f.rx(period("IDLE", 5, 0));
    TEST_ASSERT_TRUE(f.core.ready());
    TEST_ASSERT_TRUE(f.core.has_period());
    // a later periodInfo with a higher resultSeq must not move the floor: result 6 is still newer than the baseline (5)
    f.rx(period("IDLE", 6, 0));
    f.rx(result_broadcast(6, "OK", "18.1", true));
    int32_t t = 0;
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(181, t);
}

void test_core_drain_before_tick_keeps_a_late_loop_alive() {
    F f;
    const uint32_t t0 = f.now;
    TEST_ASSERT_TRUE(f.core.on_connected(20, t0));
    f.now = t0 + 6000;  // the loop was stalled; the answers already sit in the ring
    f.rx(base_info());
    f.rx(period("IDLE", 0, 0));
    f.core.update(f.now);
    const char* why = nullptr;
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));
    TEST_ASSERT_TRUE(f.core.ready());
}

void test_core_no_period_for_5s_must_disconnect() {
    F f;
    const uint32_t t0 = f.now;
    TEST_ASSERT_TRUE(f.core.on_connected(20, t0));
    f.core.update(t0 + 4999);
    const char* why = nullptr;
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));
    f.core.update(t0 + 5000);
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    TEST_ASSERT_EQUAL_STRING("no periodInfo after the handshake", why);
    // the same stall without the answers having been drained first
    F g;
    TEST_ASSERT_TRUE(g.core.on_connected(20, g.now));
    g.core.update(g.now + 6000);
    TEST_ASSERT_TRUE(g.core.must_disconnect(&why));
}

void test_core_foreign_device_or_protocol_must_disconnect() {
    const char* why = nullptr;
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    f.rx("{\"broadcast\":{\"baseInfo\":{\"releaseVer\":1,\"snCode\":\"x\",\"wifiName\":null,\"device\":\"MT80\",\"proto\":1}}}");
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    TEST_ASSERT_EQUAL_STRING("not a GLink v1 grinder", why);
    F g;
    TEST_ASSERT_TRUE(g.core.on_connected(20, g.now));
    g.rx("{\"broadcast\":{\"baseInfo\":{\"releaseVer\":1,\"snCode\":\"x\",\"wifiName\":null,\"device\":\"SGBW\",\"proto\":2}}}");
    TEST_ASSERT_TRUE(g.core.must_disconnect(&why));
}

void test_core_must_disconnect_latch_clears_on_the_next_connect_and_stops_requests() {
    F f;
    f.connect();
    f.core.on_disconnected();
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    f.core.update(f.now + 5000);
    const char* why = nullptr;
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    const size_t before = f.sink.packets.size();
    f.core.update(f.now + 6000);  // latched: nothing more is sent
    TEST_ASSERT_EQUAL_INT(static_cast<long long>(before), static_cast<long long>(f.sink.packets.size()));
    f.core.on_disconnected();
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now + 7000));
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));
}

void test_core_ten_bad_fragments_within_10s_must_disconnect() {
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    const uint8_t junk[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const char* why = nullptr;
    for (int i = 0; i < 9; i++) {
        f.now += 500;
        f.core.on_packet(junk, sizeof(junk), f.now);
    }
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));
    f.now += 500;
    f.core.on_packet(junk, sizeof(junk), f.now);
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    TEST_ASSERT_EQUAL_STRING("too many bad fragments", why);
}

// ---- order and exclusivity -------------------------------------------------------------------------------------------

void test_core_order_profiles_then_push_then_result_fetch_one_at_a_time() {
    F f;
    f.connect(5, 0);  // resultSeq 5: a result fetch is due too
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, static_cast<long long>(f.msgs().size()));  // hello + profile_get only
    ASSERT_LAST(f, GET_ALL);
    f.tick();
    f.tick();  // still pending: nothing else goes out
    TEST_ASSERT_EQUAL_INT(2, static_cast<long long>(f.msgs().size()));
    f.rx(profiles("18.0", "14.5"));
    f.tick();  // the push comes before the result fetch
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_EQUAL_INT(0, f.count("grindResult"));
    f.tick();  // update still pending
    TEST_ASSERT_EQUAL_INT(0, f.count("grindResult"));
    f.rx(update_ok("19.0", "14.5", true));
    f.tick();  // the select is not due yet (1.1 s), so the result fetch goes
    ASSERT_LAST(f, GET_RESULT);
    TEST_ASSERT_EQUAL_INT(0, f.selects());
}

void test_core_response_timeout_frees_the_slot() {
    F f;
    f.connect();
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, static_cast<long long>(f.msgs().size()));
    const uint32_t sent_at = f.now;
    f.core.update(sent_at + 4999);
    TEST_ASSERT_EQUAL_INT(2, static_cast<long long>(f.msgs().size()));
    f.now = sent_at + 5000;
    f.core.update(f.now);  // the timeout frees the slot, the still-stale profiles are asked again in the same pass
    TEST_ASSERT_EQUAL_INT(3, static_cast<long long>(f.msgs().size()));
    ASSERT_LAST(f, GET_ALL);
    const char* why = nullptr;
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));
}

// ---- push scenarios --------------------------------------------------------------------------------------------------

void test_core_push_equal_cache_sends_nothing_and_no_select() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 180, true, 145, false, 0);
    for (int i = 0; i < 4; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.updates());
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);
    // dose only wanted while the entry has no size: equal, nothing to clear (Q1 mirror)
    F g;
    g.connect_with_profiles("18.0", "null");
    g.core.set_wanted(true, 180, false, 0, false, 0);
    for (int i = 0; i < 4; i++) g.tick();
    TEST_ASSERT_EQUAL_INT(0, g.updates());
    TEST_ASSERT_EQUAL_INT(0, g.selects());
}

void test_core_push_differs_sends_one_update_with_exact_bytes_and_no_unit() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
    TEST_ASSERT_EQUAL_INT(0, f.count("grindUnit"));
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());  // in flight: not repeated

    F g;  // dose only, entry without a size: dose only on the wire
    g.connect_with_profiles("18.0", "null");
    g.core.set_wanted(true, 195, false, 0, false, 0);
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.5}}}");

    F h;  // dose only, entry holds a size: Q1 mirror clears it
    h.connect_with_profiles("18.0", "14.5");
    h.core.set_wanted(true, 195, false, 0, false, 0);
    h.tick();
    ASSERT_LAST(h, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.5,\"grindSize\":null}}}");
}

void test_core_push_creates_profile_3_when_absent() {
    F f;
    f.connect_with_profiles(nullptr);
    f.core.set_wanted(true, 180, true, 145, false, 0);
    f.tick();
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5}}}");
}

void test_core_out_of_range_dose_sends_nothing_and_sets_the_note() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 49, true, 145, false, 0);  // 4.9 g
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::OUT_OF_RANGE_DOSE);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);  // once
    for (int i = 0; i < 3; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.updates());
    f.core.set_wanted(true, 601, false, 0, false, 0);  // 60.1 g
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::OUT_OF_RANGE_DOSE);
    f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.updates());
    f.core.set_wanted(true, 50, false, 0, false, 0);  // 5.0 g is valid
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
}

void test_core_invalid_size_is_sent_without_size() {
    F f;
    f.connect_with_profiles("18.0", "null");
    f.core.set_wanted(true, 190, true, 10000, false, 0);  // 1000.0 is above 999.9: treated as absent
    f.tick();
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0}}}");
    F g;  // the entry holds a size: an invalid wanted size counts as none, so the mirror clears it
    g.connect_with_profiles("18.0", "14.5");
    g.core.set_wanted(true, 190, true, 10000, false, 0);
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":null}}}");
}

void test_core_rate_limited_then_not_ready_then_idle_is_retried() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.rx(failure("RATE_LIMITED"));
    f.tick(500);
    f.tick(500);  // about 1.0 s after the answer: not yet
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.tick(200);  // 1.2 s: second attempt
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    f.rx(failure("NOT_READY"));
    f.rx(period("SETTING", 0, 0));
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());  // waits while the grinder is in its menu
    f.rx(period("IDLE", 0, 0));
    f.tick();
    TEST_ASSERT_EQUAL_INT(3, f.updates());
    f.rx(update_ok("19.0", "14.5", true));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::SELECT_PENDING);
}

void test_core_hard_error_gives_up_once() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(failure("PROFILE_READ_ONLY"));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::GAVE_UP);
    TEST_ASSERT_EQUAL_STRING("PROFILE_READ_ONLY", f.core.sync_gave_up_code());
    for (int i = 0; i < 4; i++) f.tick(2000);
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_EQUAL_INT(0, f.selects());
}

void test_core_writes_wait_in_setting_grinding_updating_and_go_in_idle_and_warning() {
    const char* blocked[] = {"SETTING", "GRINDING", "UPDATING", "STARTING", "DONE"};
    for (const char* dev : blocked) {
        F f;
        f.connect_with_profiles("18.0", "14.5");
        f.rx(period(dev, 0, 0));
        f.core.set_wanted(true, 190, true, 145, false, 0);
        for (int i = 0; i < 3; i++) f.tick();
        TEST_ASSERT_EQUAL_INT(0, f.updates());
        f.rx(period("IDLE", 0, 0));
        f.tick();
        TEST_ASSERT_EQUAL_INT(1, f.updates());
    }
    F w;
    w.connect_with_profiles("18.0", "14.5");
    w.rx(period("WARNING", 0, 0));
    w.core.set_wanted(true, 190, true, 145, false, 0);
    w.tick();
    TEST_ASSERT_EQUAL_INT(1, w.updates());
}

void test_core_same_wanted_again_does_not_reset_the_attempt_cap() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    for (int i = 1; i <= 2; i++) {
        f.tick(1200);
        TEST_ASSERT_EQUAL_INT(i, f.updates());
        f.rx(failure("RATE_LIMITED"));
        f.core.set_wanted(true, 190, true, 145, false, 0);  // unchanged: ignored
    }
    f.tick(1200);
    TEST_ASSERT_EQUAL_INT(3, f.updates());
    f.rx(failure("RATE_LIMITED"));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::GAVE_UP);
    for (int i = 0; i < 3; i++) f.tick(2000);
    TEST_ASSERT_EQUAL_INT(3, f.updates());
}

void test_core_newer_wanted_replaces_the_older_unsent_one() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.core.set_wanted(true, 200, true, 150, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":20.0,\"grindSize\":15.0}}}");
}

// ---- Q1: the select after a changed update ---------------------------------------------------------------------------

void test_core_select_3_goes_out_after_a_changed_update_not_before_1100ms() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(500);
    f.tick(500);  // ~1.0 s after the answer
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    f.tick(200);
    TEST_ASSERT_EQUAL_INT(1, f.selects());
    ASSERT_LAST(f, SELECT_3);
    f.rx(select_ok(3));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);
    for (int i = 0; i < 3; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_EQUAL_INT(1, f.selects());  // exactly once
}

void test_core_first_creation_of_profile_3_is_followed_by_the_select() {
    F f;
    f.connect_with_profiles(nullptr);
    f.core.set_wanted(true, 180, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("18.0", "14.5", true));
    f.tick(1200);
    ASSERT_LAST(f, SELECT_3);
}

void test_core_no_select_when_the_update_changed_nothing() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", false));  // the grinder already held it
    for (int i = 0; i < 3; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);
}

void test_core_select_waits_for_a_writable_state() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.rx(period("SETTING", 0, 0));  // the user moved to the menu
    for (int i = 0; i < 3; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    f.rx(period("IDLE", 0, 0));
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.selects());
}

// ---- reconnect: set_wanted re-sent on every ready --------------------------------------------------------------------

void test_core_satisfied_value_is_pushed_again_after_a_reconnect_when_the_cache_differs() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    f.rx(select_ok(3));
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("18.0", "14.5");  // the grinder lost it (reboot): no new set_wanted call was made
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());
}

void test_core_satisfied_value_sends_no_request_after_a_reconnect_when_the_cache_is_equal() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    f.rx(select_ok(3));
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("19.0", "14.5");
    for (int i = 0; i < 3; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());  // compare-first: nothing re-sent
    TEST_ASSERT_EQUAL_INT(1, f.selects());  // and no re-select
}

void test_core_gave_up_value_is_retried_on_the_next_link() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(failure("PROFILE_READ_ONLY"));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::GAVE_UP);
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());
}

void test_core_pending_select_is_dropped_on_a_link_drop() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("19.0", "14.5");
    for (int i = 0; i < 3; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(0, f.selects());
}

// ---- timeouts of the push --------------------------------------------------------------------------------------------

void test_core_update_timeout_frees_settings_sync_and_the_push_is_retried() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    const uint32_t sent_at = f.now;
    f.now = sent_at + 5000;
    f.core.update(f.now);  // timeout: not stuck in flight, waits for a state change or 5 s
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::UPDATE_PENDING);
    f.now += 5000;
    f.core.update(f.now);
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    TEST_ASSERT_EQUAL_INT(1, f.selects());
}

void test_core_three_update_timeouts_give_up() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    for (int i = 0; i < 2; i++) {
        f.now += 5000;
        f.core.update(f.now);  // timeout
        f.now += 5000;
        f.core.update(f.now);  // retry after the 5 s fallback
    }
    TEST_ASSERT_EQUAL_INT(3, f.updates());
    f.now += 5000;
    f.core.update(f.now);
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::GAVE_UP);
    TEST_ASSERT_EQUAL_STRING("TIMEOUT", f.core.sync_gave_up_code());
}

void test_core_select_timeout_frees_the_slot_too() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    TEST_ASSERT_EQUAL_INT(1, f.selects());
    f.now += 5000;
    f.core.update(f.now);
    f.now += 5000;
    f.core.update(f.now);
    TEST_ASSERT_EQUAL_INT(2, f.selects());
}

// ---- result path -----------------------------------------------------------------------------------------------------

void test_core_result_at_connect_is_not_reported_and_newer_ones_once() {
    F f;
    f.connect(5, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    ASSERT_LAST(f, GET_RESULT);
    f.rx(result_response(5, "OK", "18.1", true));
    int32_t t = 0;
    TEST_ASSERT_FALSE(f.core.take_finished(&t));  // before the link: baseline

    f.rx(result_broadcast(6, "OK", "18.1", true));
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(181, t);
    TEST_ASSERT_FALSE(f.core.take_finished(&t));  // once
    f.rx(result_broadcast(6, "OK", "18.1", true));
    TEST_ASSERT_FALSE(f.core.take_finished(&t));  // the same seq again
}

void test_core_stopped_then_ok_reports_only_the_ok() {
    F f;
    f.connect(5, 0);
    f.rx(result_broadcast(7, "STOPPED", "9.4", false));
    int32_t t = 0;
    TEST_ASSERT_FALSE(f.core.take_finished(&t));
    f.rx(result_broadcast(8, "OK", "17.9", true));
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(179, t);
    TEST_ASSERT_FALSE(f.core.take_finished(&t));
}

void test_core_finished_slot_is_latest_wins_and_survives_a_link_drop() {
    F f;
    f.connect(5, 0);
    f.rx(result_broadcast(6, "OK", "18.1", true));
    f.rx(result_broadcast(7, "OK", "18.4", true));
    f.core.on_disconnected();
    int32_t t = 0;
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(184, t);
    TEST_ASSERT_FALSE(f.core.take_finished(&t));
}

void test_core_result_missed_as_broadcast_is_fetched_through_result_seq() {
    F f;
    f.connect(5, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    f.rx(result_response(5, "OK", "18.1", true));
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.count("grindResult"));  // nothing further to fetch
    f.rx(period("IDLE", 6, 0));  // the broadcast was lost
    f.tick();
    ASSERT_LAST(f, GET_RESULT);
    TEST_ASSERT_EQUAL_INT(2, f.count("grindResult"));
    f.rx(result_response(6, "OK", "18.1", true));
    int32_t t = 0;
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(181, t);
}

void test_core_result_in_a_link_gap_is_not_reported() {
    F f;
    f.connect(5, 0);
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(6, 0);  // a grind finished while the link was down (seq 6): the new baseline
    f.serve_profiles("18.0", "14.5");
    f.tick();
    f.rx(result_response(6, "OK", "18.1", true));
    int32_t t = 0;
    TEST_ASSERT_FALSE(f.core.take_finished(&t));
}

void test_core_grinder_reboot_rebaselines_and_the_first_new_grind_is_reported() {
    F f;
    f.connect(9, 0);
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(1, 0);  // rebooted: the counter restarted
    int32_t t = 0;
    f.rx(result_broadcast(1, "OK", "18.1", true));  // not newer than the new baseline
    TEST_ASSERT_FALSE(f.core.take_finished(&t));
    f.rx(result_broadcast(2, "OK", "18.2", true));
    TEST_ASSERT_TRUE(f.core.take_finished(&t));
    TEST_ASSERT_EQUAL_INT(182, t);
}

// ---- live data, classes, reported settings ---------------------------------------------------------------------------

void test_core_period_holds_the_live_data() {
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    f.rx(base_info());
    f.rx(period("GRINDING", 3, 7, 3, "\"phase\":\"PREDICTIVE\",\"target\":18.0,\"mode\":\"WEIGHT\",\"leadInMs\":2000,", "12.3",
                "\"NOT_READY\""));
    const PeriodInfo& p = f.core.period();
    TEST_ASSERT_TRUE(p.weight_valid);
    TEST_ASSERT_EQUAL_INT(123, p.weight_tenths);
    TEST_ASSERT_EQUAL_STRING("GRINDING", p.dev_state);
    TEST_ASSERT_EQUAL_STRING("PREDICTIVE", p.phase);
    TEST_ASSERT_TRUE(p.has_target);
    TEST_ASSERT_EQUAL_INT(180, p.target_tenths);
    TEST_ASSERT_EQUAL_STRING("WEIGHT", p.mode);
    TEST_ASSERT_TRUE(p.has_lead_in);
    TEST_ASSERT_EQUAL_INT(2000, p.lead_in_ms);
    TEST_ASSERT_EQUAL_STRING("NOT_READY", p.blocked);
    TEST_ASSERT_EQUAL_INT(7, p.profile_rev);
    TEST_ASSERT_EQUAL_INT(3, p.select_profile);
    f.rx(period("IDLE", 3, 8));  // updated by the next one
    TEST_ASSERT_EQUAL_INT(8, f.core.period().profile_rev);
}

void test_core_unknown_dev_state_and_phase_do_not_break_parsing() {
    F f;
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    f.rx(base_info());
    f.rx(period("FROBNICATING", 0, 0, 1, "\"phase\":\"WARP\","));
    TEST_ASSERT_TRUE(f.core.ready());
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::UNKNOWN);
    TEST_ASSERT_EQUAL_STRING("WARP", f.core.period().phase);
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.updates());  // an unknown state accepts no writes
    f.rx(period("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::IDLE);
}

void test_core_dev_class_follows_the_dev_state() {
    F f;
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::UNKNOWN);  // no period yet
    f.connect();
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::IDLE);
    f.rx(period("GRINDING", 0, 0));
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::GRINDING);
    f.rx(period("STARTING", 0, 0));
    TEST_ASSERT_TRUE(f.core.dev_class() == DevClass::BUSY);
}

void test_core_selected_profile_dose_and_size_come_from_the_cache() {
    F f;
    int32_t v = 0;
    f.connect(0, 0, "IDLE", 3);
    TEST_ASSERT_FALSE(f.core.selected_profile_dose(&v));  // not cached yet
    f.serve_profiles("18.0", "14.5");
    TEST_ASSERT_TRUE(f.core.selected_profile_dose(&v));
    TEST_ASSERT_EQUAL_INT(180, v);
    TEST_ASSERT_TRUE(f.core.selected_profile_size(&v));
    TEST_ASSERT_EQUAL_INT(145, v);
    f.rx(period("IDLE", 0, 0, 2));  // CUSTOM, no size
    TEST_ASSERT_TRUE(f.core.selected_profile_dose(&v));
    TEST_ASSERT_EQUAL_INT(215, v);
    TEST_ASSERT_FALSE(f.core.selected_profile_size(&v));

    F g;  // profile 3 selected but not set
    g.connect(0, 0, "IDLE", 3);
    g.serve_profiles(nullptr);
    TEST_ASSERT_FALSE(g.core.selected_profile_dose(&v));
    TEST_ASSERT_FALSE(g.core.selected_profile_size(&v));
}

void test_core_never_sends_start_or_stop() {
    F f;
    f.connect_with_profiles("18.0", "14.5", 5);
    f.core.set_wanted(true, 190, true, 145, false, 0);
    for (int i = 0; i < 6; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(0, f.count("grindControl"));
}

// ---- hardening (chg025 step 3): the latch blocks everything in the same drain; a reconnect starts clean ---------------------

namespace {
// a proto 2 baseInfo from an SGBW: the Client latches "not a GLink v1 grinder" when it arrives after the handshake
const char* const PROTO2_BASE_INFO = "{\"broadcast\":{\"baseInfo\":{\"releaseVer\":1,\"snCode\":\"x\",\"wifiName\":null,\"device\":\"SGBW\",\"proto\":2}}}";
}  // namespace

void test_core_latched_session_yields_no_result_and_no_cache_in_the_same_drain() {
    // control: the same drain without the foreign baseInfo reports the grind, so the latched case below is not vacuous
    F c;
    c.connect_with_profiles("18.0", "14.5", 5);
    c.rx(period("IDLE", 6, 0));
    c.rx(result_broadcast(6, "OK", "18.1", true));
    int32_t v = 0;
    TEST_ASSERT_TRUE(c.core.take_finished(&v));
    TEST_ASSERT_EQUAL_INT(181, v);

    F f;
    f.connect_with_profiles("18.0", "14.5", 5);  // baseline 5, profile cache filled
    TEST_ASSERT_TRUE(f.core.selected_profile_dose(&v));
    TEST_ASSERT_EQUAL_INT(180, v);
    f.rx(PROTO2_BASE_INFO);
    const char* why = nullptr;
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    TEST_ASSERT_EQUAL_STRING("not a GLink v1 grinder", why);
    // the rest of the same drain: a periodInfo and a settled, newer grindResult
    f.rx(period("IDLE", 6, 0));
    f.rx(result_broadcast(6, "OK", "18.1", true));
    TEST_ASSERT_FALSE(f.core.take_finished(&v));
    TEST_ASSERT_FALSE(f.core.selected_profile_dose(&v));
    TEST_ASSERT_FALSE(f.core.selected_profile_size(&v));
    // a later drain and tick: still nothing, still reported, nothing sent
    const size_t before = f.sink.packets.size();
    f.tick();
    f.rx(period("IDLE", 7, 0));
    f.rx(result_broadcast(7, "OK", "18.2", true));
    TEST_ASSERT_FALSE(f.core.take_finished(&v));
    TEST_ASSERT_EQUAL_INT(static_cast<long long>(before), static_cast<long long>(f.sink.packets.size()));
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    TEST_ASSERT_EQUAL_STRING("not a GLink v1 grinder", why);
}

void test_core_reconnect_after_a_latch_clears_it_and_takes_a_fresh_baseline() {
    F f;
    f.connect(5, 0);
    f.rx(result_broadcast(9, "OK", "18.1", true));  // the old session's floor becomes 9
    int32_t v = 0;
    TEST_ASSERT_TRUE(f.core.take_finished(&v));
    TEST_ASSERT_EQUAL_INT(181, v);
    f.rx(PROTO2_BASE_INFO);
    const char* why = nullptr;
    TEST_ASSERT_TRUE(f.core.must_disconnect(&why));
    f.core.on_disconnected();
    f.now += 3000;

    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));  // the latch is gone
    f.rx(result_broadcast(9, "OK", "19.9", true));  // before any periodInfo: no baseline yet, nothing reported
    f.rx(base_info());
    f.rx(period("IDLE", 7, 0));  // the new baseline is 7 (the old floor 9 is not carried over)
    TEST_ASSERT_TRUE(f.core.ready());
    f.rx(result_broadcast(7, "OK", "18.5", true));  // at the new baseline: not reported
    TEST_ASSERT_FALSE(f.core.take_finished(&v));
    f.rx(result_broadcast(8, "OK", "18.6", true));  // newer than the new baseline, below the old floor: reported
    TEST_ASSERT_TRUE(f.core.take_finished(&v));
    TEST_ASSERT_EQUAL_INT(186, v);
    TEST_ASSERT_FALSE(f.core.must_disconnect(&why));

    f.serve_profiles("18.0", "14.5");  // the getters work again on the new link
    TEST_ASSERT_TRUE(f.core.selected_profile_dose(&v));
    TEST_ASSERT_EQUAL_INT(180, v);
}

void test_core_reconnect_drops_the_in_flight_update_and_sends_a_fresh_one() {
    F f;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::UPDATE_PENDING);
    f.core.on_disconnected();  // the answer to the update never comes
    f.now += 3000;
    f.connect(0, 0);
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::UPDATE_PENDING);  // the wanted value stays, nothing stays in flight
    f.serve_profiles("18.0", "14.5");
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());  // not blocked by the old in-flight slot
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
}

// ---- chg026 step 4: rpm (spec 1.2), mirror, old grinders ---------------------------------------------------------------------

void test_core_supports_rpm_only_on_a_ready_protoMinor_2_grinder() {
    F f;
    TEST_ASSERT_FALSE(f.core.rpm_supported());  // before anything
    TEST_ASSERT_TRUE(f.core.on_connected(20, f.now));
    TEST_ASSERT_FALSE(f.core.rpm_supported());  // connected, no baseInfo yet
    f.rx(base_info());
    TEST_ASSERT_FALSE(f.core.rpm_supported());  // baseInfo seen but no periodInfo: not ready yet
    f.rx(period("IDLE", 0, 0));
    TEST_ASSERT_TRUE(f.core.ready());
    TEST_ASSERT_TRUE(f.core.rpm_supported());
    f.core.on_disconnected();
    TEST_ASSERT_FALSE(f.core.rpm_supported());  // link gone

    F old;  // a 1.1 grinder: no protoMinor
    old.proto_minor = 0;
    old.connect();
    TEST_ASSERT_TRUE(old.core.ready());
    TEST_ASSERT_FALSE(old.core.rpm_supported());

    F one;  // protoMinor 1 is still not enough
    one.proto_minor = 1;
    one.connect();
    TEST_ASSERT_FALSE(one.core.rpm_supported());

    F larger;  // a larger minor is supported
    larger.proto_minor = 3;
    larger.connect();
    TEST_ASSERT_TRUE(larger.core.rpm_supported());

    // the next session replaces the answer: new grinder after an old one, and the other way round
    F g;
    g.proto_minor = 0;
    g.connect();
    TEST_ASSERT_FALSE(g.core.rpm_supported());
    g.core.on_disconnected();
    g.now += 3000;
    g.proto_minor = 2;
    g.connect();
    TEST_ASSERT_TRUE(g.core.rpm_supported());
}

void test_core_push_with_rpm_sends_exact_bytes_then_select_and_updates_the_cache() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", nullptr);
    f.core.set_wanted(true, 190, true, 145, true, 600);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5,\"rpm\":600}}}");
    TEST_ASSERT_EQUAL_INT(0, f.count("grindUnit"));
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());  // in flight: not repeated
    f.rx(update_ok_rpm("19.0", "14.5", "600", true));
    f.tick(500);
    f.tick(500);
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    f.tick(200);
    TEST_ASSERT_EQUAL_INT(1, f.selects());  // select after the stored change
    ASSERT_LAST(f, SELECT_3);
    f.rx(select_ok(3));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);
    for (int i = 0; i < 3; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    TEST_ASSERT_EQUAL_INT(1, f.selects());

    // rpm 0 is a value and rpm 9999 is allowed
    F z;
    z.connect_with_rpm_profiles("18.0", "14.5", "600");
    z.core.set_wanted(true, 180, true, 145, true, 0);
    z.tick();
    ASSERT_LAST(z, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":0}}}");
    F m;
    m.connect_with_rpm_profiles("18.0", "14.5", "600");
    m.core.set_wanted(true, 180, true, 145, true, 9999);
    m.tick();
    ASSERT_LAST(m, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":9999}}}");
}

void test_core_compare_first_with_rpm_and_only_the_rpm_differs() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 180, true, 145, true, 600);  // equal in dose, size and rpm
    for (int i = 0; i < 4; i++) f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.updates());
    TEST_ASSERT_EQUAL_INT(0, f.selects());
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);

    F g;  // only the rpm differs
    g.connect_with_rpm_profiles("18.0", "14.5", "600");
    g.core.set_wanted(true, 180, true, 145, true, 650);
    g.tick();
    TEST_ASSERT_EQUAL_INT(1, g.updates());
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":650}}}");

    F h;  // the entry has no rpm, one is wanted
    h.connect_with_rpm_profiles("18.0", "14.5", nullptr);
    h.core.set_wanted(true, 180, true, 145, true, 600);
    h.tick();
    TEST_ASSERT_EQUAL_INT(1, h.updates());
}

void test_core_mirror_clears_a_stale_size_and_rpm_in_a_single_update() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 180, false, 0, false, 0);  // GaggiMate profile with a dose only
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":null,\"rpm\":null}}}");
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.rx(update_ok_rpm("18.0", "null", nullptr, true));  // the grinder confirms: entry now has neither
    f.tick(1200);
    TEST_ASSERT_EQUAL_INT(1, f.selects());  // select follows
    f.rx(select_ok(3));
    for (int i = 0; i < 3; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(1, f.updates());  // and nothing more: the cleared cache equals the wanted value
    int32_t v = 0;
    f.rx(period("IDLE", 0, 0, 3));
    TEST_ASSERT_FALSE(f.core.selected_profile_size(&v));
    TEST_ASSERT_FALSE(f.core.selected_profile_rpm(&v));

    // entry with an rpm only: rpm:null alone
    F g;
    g.connect_with_rpm_profiles("18.0", "null", "600");
    g.core.set_wanted(true, 180, false, 0, false, 0);
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"rpm\":null}}}");

    // entry with neither: no request at all
    F h;
    h.connect_with_rpm_profiles("18.0", "null", nullptr);
    h.core.set_wanted(true, 180, false, 0, false, 0);
    for (int i = 0; i < 4; i++) h.tick();
    TEST_ASSERT_EQUAL_INT(0, h.updates());
    TEST_ASSERT_EQUAL_INT(0, h.selects());

    // no entry 3: create with what is wanted only
    F n;
    n.connect_with_rpm_profiles(nullptr, "null", nullptr);
    n.core.set_wanted(true, 180, false, 0, false, 0);
    n.tick();
    ASSERT_LAST(n, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0}}}");
    F c;
    c.connect_with_rpm_profiles(nullptr, "null", nullptr);
    c.core.set_wanted(true, 180, true, 145, true, 600);
    c.tick();
    ASSERT_LAST(c, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":600}}}");

    // a wanted rpm without a size clears a stale size
    F r;
    r.connect_with_rpm_profiles("18.0", "14.5", "600");
    r.core.set_wanted(true, 180, false, 0, true, 600);
    r.tick();
    ASSERT_LAST(r, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":null,\"rpm\":600}}}");
}

void test_core_old_grinder_never_gets_an_rpm_and_the_note_comes_once() {
    F f;
    f.proto_minor = 0;  // no protoMinor in baseInfo
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, true, 600);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::RPM_NOT_SUPPORTED);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);  // once
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    ASSERT_LAST(f, SELECT_3);
    f.rx(select_ok(3));
    for (int i = 0; i < 3; i++) f.tick(1200);
    TEST_ASSERT_EQUAL_INT(0, f.count("rpm"));  // no request in the whole session mentions it
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);

    // a different rpm for the same session is a new wanted value: noted again, still never sent
    f.core.set_wanted(true, 190, true, 145, true, 650);
    f.tick();
    TEST_ASSERT_EQUAL_INT(0, f.count("rpm"));
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::RPM_NOT_SUPPORTED);

    // an old grinder holding no rpm and none wanted: identical to chg025 (dose and size only, no note)
    F g;
    g.proto_minor = 1;
    g.connect_with_profiles("18.0", "14.5");
    g.core.set_wanted(true, 190, true, 145, false, 0);
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
    TEST_ASSERT_TRUE(g.core.wanted_note() == SyncNote::NONE);

    // an old grinder with an equal dose and size entry: satisfied, nothing sent
    F h;
    h.proto_minor = 0;
    h.connect_with_profiles("18.0", "14.5");
    h.core.set_wanted(true, 180, true, 145, true, 600);
    for (int i = 0; i < 4; i++) h.tick();
    TEST_ASSERT_EQUAL_INT(0, h.updates());
    TEST_ASSERT_EQUAL_INT(0, h.count("rpm"));
    TEST_ASSERT_TRUE(h.core.sync_stage() == SyncStage::IDLE);
}

void test_core_old_grinder_session_replays_the_chg025_push_scenarios_unchanged() {
    // The chg025 scenarios on a grinder without protoMinor, with the 1.1 wanted value (dose and size, no rpm): same bytes, same
    // select, same retry, same give-up.
    F f;
    f.proto_minor = 0;
    f.connect_with_profiles("18.0", "14.5");
    f.core.set_wanted(true, 190, true, 145, false, 0);
    f.tick();
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
    f.rx(failure("RATE_LIMITED"));
    f.tick(500);
    f.tick(500);
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.tick(200);
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    f.rx(update_ok("19.0", "14.5", true));
    f.tick(1200);
    ASSERT_LAST(f, SELECT_3);
    f.rx(select_ok(3));
    TEST_ASSERT_TRUE(f.core.sync_stage() == SyncStage::IDLE);
    TEST_ASSERT_EQUAL_INT(0, f.count("rpm"));
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);
    int32_t v = 0;
    f.rx(period("IDLE", 0, 0, 3));
    TEST_ASSERT_FALSE(f.core.selected_profile_rpm(&v));

    // a reconnect to the same old grinder after it lost the value: pushed again
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    TEST_ASSERT_EQUAL_INT(3, f.updates());
    TEST_ASSERT_EQUAL_INT(0, f.count("rpm"));

    // hard error gives up once, as before
    F g;
    g.proto_minor = 0;
    g.connect_with_profiles("18.0", "14.5");
    g.core.set_wanted(true, 190, true, 145, false, 0);
    g.tick();
    g.rx(failure("PROFILE_READ_ONLY"));
    TEST_ASSERT_TRUE(g.core.sync_stage() == SyncStage::GAVE_UP);
    for (int i = 0; i < 4; i++) g.tick(2000);
    TEST_ASSERT_EQUAL_INT(1, g.updates());
}

void test_core_rpm_out_of_range_is_noted_treated_as_absent_and_pinned_to_the_ready_replay() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 190, true, 145, true, 10000);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::RPM_OUT_OF_RANGE);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);
    f.tick();
    // treated as absent, so the stale rpm of the entry is cleared (mirror)
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5,\"rpm\":null}}}");
    // Pinned (Auditor note, non-blocking): the same value handed over again is dropped by the core (no new note); only the
    // hand-over at every ready raises the note again.
    f.core.set_wanted(true, 190, true, 145, true, 10000);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);
    f.core.on_disconnected();
    f.now += 3000;
    f.connect(0, 0);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::RPM_OUT_OF_RANGE);
    TEST_ASSERT_TRUE(f.core.wanted_note() == SyncNote::NONE);

    // a negative rpm is the same
    F g;
    g.connect_with_rpm_profiles("18.0", "14.5", nullptr);
    g.core.set_wanted(true, 190, true, 145, true, -1);
    TEST_ASSERT_TRUE(g.core.wanted_note() == SyncNote::RPM_OUT_OF_RANGE);
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
}

void test_core_selected_profile_rpm_follows_the_selected_entry() {
    F f;
    int32_t v = -1;
    f.connect(0, 0, "IDLE", 3);
    TEST_ASSERT_FALSE(f.core.selected_profile_rpm(&v));  // not cached yet
    f.tick();
    f.rx(profiles_rpm("18.0", "14.5", "600"));
    TEST_ASSERT_TRUE(f.core.selected_profile_rpm(&v));
    TEST_ASSERT_EQUAL_INT(600, v);
    TEST_ASSERT_TRUE(f.core.selected_profile_rpm(nullptr));  // the out pointer is optional
    f.rx(period("IDLE", 0, 0, 2));  // CUSTOM: no rpm
    TEST_ASSERT_FALSE(f.core.selected_profile_rpm(&v));
    f.rx(period("IDLE", 0, 0, 3));
    TEST_ASSERT_TRUE(f.core.selected_profile_rpm(&v));
    TEST_ASSERT_EQUAL_INT(600, v);
    f.core.on_disconnected();
    TEST_ASSERT_FALSE(f.core.selected_profile_rpm(&v));  // no link, no values

    F z;  // rpm 0 is a value
    z.connect(0, 0, "IDLE", 3);
    z.tick();
    z.rx(profiles_rpm("18.0", "null", "0"));
    TEST_ASSERT_TRUE(z.core.selected_profile_rpm(&v));
    TEST_ASSERT_EQUAL_INT(0, v);

    F g;  // profile 3 selected but no rpm on it
    g.connect(0, 0, "IDLE", 3);
    g.tick();
    g.rx(profiles_rpm("18.0", "14.5", nullptr));
    TEST_ASSERT_FALSE(g.core.selected_profile_rpm(&v));
    TEST_ASSERT_TRUE(g.core.selected_profile_size(&v));  // size and rpm are independent

    F n;  // profile 3 selected but not set
    n.connect(0, 0, "IDLE", 3);
    n.tick();
    n.rx(profiles_rpm(nullptr, "null", nullptr));
    TEST_ASSERT_FALSE(n.core.selected_profile_rpm(&v));

    // an update answer carrying the rpm refreshes the cached entry the driver reports from
    F u;
    u.connect_with_rpm_profiles("18.0", "14.5", nullptr, 3);
    TEST_ASSERT_FALSE(u.core.selected_profile_rpm(&v));
    u.core.set_wanted(true, 180, true, 145, true, 700);
    u.tick();
    u.rx(update_ok_rpm("18.0", "14.5", "700", true));
    TEST_ASSERT_TRUE(u.core.selected_profile_rpm(&v));
    TEST_ASSERT_EQUAL_INT(700, v);
}

void test_core_wanted_rpm_survives_a_link_drop_and_follows_the_next_grinder() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 190, true, 145, true, 650);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.core.on_disconnected();  // the answer never comes
    f.now += 3000;
    f.connect(0, 0);
    f.serve_profiles("18.0", "14.5");
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5,\"rpm\":650}}}");

    // the next link is a 1.1 grinder: the same wanted value goes out without the rpm
    F g;
    g.connect_with_rpm_profiles("18.0", "14.5", "600");
    g.core.set_wanted(true, 190, true, 145, true, 650);
    g.tick();
    g.core.on_disconnected();
    g.now += 3000;
    g.proto_minor = 0;
    g.connect(0, 0);
    g.serve_profiles("18.0", "14.5");
    g.tick();
    ASSERT_LAST(g, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5}}}");
    TEST_ASSERT_TRUE(g.core.wanted_note() == SyncNote::RPM_NOT_SUPPORTED);
}

void test_core_rate_limited_then_not_ready_retries_carry_the_rpm() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", nullptr);
    f.core.set_wanted(true, 190, true, 145, true, 600);
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    f.rx(failure("RATE_LIMITED"));
    f.tick(500);
    f.tick(500);
    TEST_ASSERT_EQUAL_INT(1, f.updates());  // 1.0 s after the answer: not yet
    f.tick(200);
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5,\"rpm\":600}}}");
    f.rx(failure("NOT_READY"));
    f.rx(period("SETTING", 0, 0));
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());  // waits while the grinder is in its menu
    f.rx(period("IDLE", 0, 0));
    f.tick();
    TEST_ASSERT_EQUAL_INT(3, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":19.0,\"grindSize\":14.5,\"rpm\":600}}}");
}

void test_core_latest_wanted_wins_when_only_the_rpm_changes() {
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 180, true, 145, true, 650);
    f.core.set_wanted(true, 180, true, 145, true, 700);  // replaces the unsent 650
    f.tick();
    TEST_ASSERT_EQUAL_INT(1, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":700}}}");
    // changing only the rpm while the update is in flight: the old answer is ignored, the new value goes out
    f.core.set_wanted(true, 180, true, 145, true, 750);
    f.rx(update_ok_rpm("18.0", "14.5", "700", true));
    f.tick();
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":750}}}");
    // the same wanted value again while pending does not reset the attempt cap (rpm is part of "same")
    f.core.set_wanted(true, 180, true, 145, true, 750);
    TEST_ASSERT_EQUAL_INT(2, f.updates());
    // dropping the rpm later is a change of its own: rpm:null is sent once the entry holds one
    f.rx(update_ok_rpm("18.0", "14.5", "750", true));
    f.tick(1200);
    f.rx(select_ok(3));
    f.core.set_wanted(true, 180, true, 145, false, 0);
    f.tick();
    ASSERT_LAST(f, "{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":18.0,\"grindSize\":14.5,\"rpm\":null}}}");
}

void test_core_never_sends_a_feed_rpm_or_any_other_unknown_member() {
    // every request of a busy session only has the members the spec 1.2 names
    F f;
    f.connect_with_rpm_profiles("18.0", "14.5", "600");
    f.core.set_wanted(true, 190, true, 145, true, 650);
    for (int i = 0; i < 4; i++) f.tick(1200);
    for (const std::string& m : f.msgs()) {
        TEST_ASSERT_TRUE(m.find("feed") == std::string::npos);
        TEST_ASSERT_TRUE(m.find("grindControl") == std::string::npos);
    }
}

void run_glink_plugin_core_tests() {
    RUN_TEST(test_core_connect_writes_the_golden_hello);
    RUN_TEST(test_core_failed_hello_write_reports_false);
    RUN_TEST(test_core_burst_of_small_packets_reaches_ready_and_baseline_is_taken_once);
    RUN_TEST(test_core_drain_before_tick_keeps_a_late_loop_alive);
    RUN_TEST(test_core_no_period_for_5s_must_disconnect);
    RUN_TEST(test_core_foreign_device_or_protocol_must_disconnect);
    RUN_TEST(test_core_must_disconnect_latch_clears_on_the_next_connect_and_stops_requests);
    RUN_TEST(test_core_ten_bad_fragments_within_10s_must_disconnect);
    RUN_TEST(test_core_order_profiles_then_push_then_result_fetch_one_at_a_time);
    RUN_TEST(test_core_response_timeout_frees_the_slot);
    RUN_TEST(test_core_push_equal_cache_sends_nothing_and_no_select);
    RUN_TEST(test_core_push_differs_sends_one_update_with_exact_bytes_and_no_unit);
    RUN_TEST(test_core_push_creates_profile_3_when_absent);
    RUN_TEST(test_core_out_of_range_dose_sends_nothing_and_sets_the_note);
    RUN_TEST(test_core_invalid_size_is_sent_without_size);
    RUN_TEST(test_core_rate_limited_then_not_ready_then_idle_is_retried);
    RUN_TEST(test_core_hard_error_gives_up_once);
    RUN_TEST(test_core_writes_wait_in_setting_grinding_updating_and_go_in_idle_and_warning);
    RUN_TEST(test_core_same_wanted_again_does_not_reset_the_attempt_cap);
    RUN_TEST(test_core_newer_wanted_replaces_the_older_unsent_one);
    RUN_TEST(test_core_select_3_goes_out_after_a_changed_update_not_before_1100ms);
    RUN_TEST(test_core_first_creation_of_profile_3_is_followed_by_the_select);
    RUN_TEST(test_core_no_select_when_the_update_changed_nothing);
    RUN_TEST(test_core_select_waits_for_a_writable_state);
    RUN_TEST(test_core_satisfied_value_is_pushed_again_after_a_reconnect_when_the_cache_differs);
    RUN_TEST(test_core_satisfied_value_sends_no_request_after_a_reconnect_when_the_cache_is_equal);
    RUN_TEST(test_core_gave_up_value_is_retried_on_the_next_link);
    RUN_TEST(test_core_pending_select_is_dropped_on_a_link_drop);
    RUN_TEST(test_core_update_timeout_frees_settings_sync_and_the_push_is_retried);
    RUN_TEST(test_core_three_update_timeouts_give_up);
    RUN_TEST(test_core_select_timeout_frees_the_slot_too);
    RUN_TEST(test_core_result_at_connect_is_not_reported_and_newer_ones_once);
    RUN_TEST(test_core_stopped_then_ok_reports_only_the_ok);
    RUN_TEST(test_core_finished_slot_is_latest_wins_and_survives_a_link_drop);
    RUN_TEST(test_core_result_missed_as_broadcast_is_fetched_through_result_seq);
    RUN_TEST(test_core_result_in_a_link_gap_is_not_reported);
    RUN_TEST(test_core_grinder_reboot_rebaselines_and_the_first_new_grind_is_reported);
    RUN_TEST(test_core_period_holds_the_live_data);
    RUN_TEST(test_core_unknown_dev_state_and_phase_do_not_break_parsing);
    RUN_TEST(test_core_dev_class_follows_the_dev_state);
    RUN_TEST(test_core_selected_profile_dose_and_size_come_from_the_cache);
    RUN_TEST(test_core_never_sends_start_or_stop);
    RUN_TEST(test_core_latched_session_yields_no_result_and_no_cache_in_the_same_drain);
    RUN_TEST(test_core_reconnect_after_a_latch_clears_it_and_takes_a_fresh_baseline);
    RUN_TEST(test_core_reconnect_drops_the_in_flight_update_and_sends_a_fresh_one);
    RUN_TEST(test_core_supports_rpm_only_on_a_ready_protoMinor_2_grinder);
    RUN_TEST(test_core_push_with_rpm_sends_exact_bytes_then_select_and_updates_the_cache);
    RUN_TEST(test_core_compare_first_with_rpm_and_only_the_rpm_differs);
    RUN_TEST(test_core_mirror_clears_a_stale_size_and_rpm_in_a_single_update);
    RUN_TEST(test_core_old_grinder_never_gets_an_rpm_and_the_note_comes_once);
    RUN_TEST(test_core_old_grinder_session_replays_the_chg025_push_scenarios_unchanged);
    RUN_TEST(test_core_rpm_out_of_range_is_noted_treated_as_absent_and_pinned_to_the_ready_replay);
    RUN_TEST(test_core_selected_profile_rpm_follows_the_selected_entry);
    RUN_TEST(test_core_wanted_rpm_survives_a_link_drop_and_follows_the_next_grinder);
    RUN_TEST(test_core_rate_limited_then_not_ready_retries_carry_the_rpm);
    RUN_TEST(test_core_latest_wanted_wins_when_only_the_rpm_changes);
    RUN_TEST(test_core_never_sends_a_feed_rpm_or_any_other_unknown_member);
}

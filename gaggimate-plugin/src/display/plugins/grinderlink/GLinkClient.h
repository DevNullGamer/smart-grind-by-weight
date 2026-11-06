#pragma once
#include <stddef.h>
#include <stdint.h>
#include "GLinkMessages.h"
#include "bluetooth/glink_frame.h"

// GLink client session (docs/GLINK-PROTOCOL.md sections 2, 3, 5.5 and 7.5): fragmenting, reassembling, the handshake,
// one request at a time with the 5 s timeout, the "did the start happen" decision, profile and result tracking.
// Pure: no Arduino/ESP/NimBLE dependencies. The NimBLE glue (GLinkPlugin) owns the connection and feeds this class:
//   on_connected()  after the TX subscription is in place (it sends appHello),
//   on_notify()     for every TX notification,
//   on_disconnected(), tick() (about every 100 ms; the clock is injected, milliseconds, wrap-safe).
// and gives it a sink that writes one packet to RX. Results come back through GLinkListener, always from inside one of
// those calls (never from another thread).

namespace glinkc {

class Sink {
public:
    virtual ~Sink() = default;
    // Writes one packet (header included) to RX, in order. false = the write failed.
    virtual bool write_packet(const uint8_t* packet, size_t len) = 0;
};

enum class LinkState : uint8_t {
    DISCONNECTED,
    CONNECTED,  // subscribed, appHello not sent yet or sent, no periodInfo yet
    READY       // the first periodInfo arrived: the channel is usable
};

enum class RequestKind : uint8_t { NONE, HELLO, PROFILE_GET, PROFILE_SELECT, PROFILE_UPDATE, START, RESULT_GET };

class Listener {
public:
    virtual ~Listener() = default;
    virtual void on_ready() {}                                    // first periodInfo of a connection
    virtual void on_base_info(const BaseInfo&) {}
    virtual void on_period(const PeriodInfo&, bool /*profile_rev_changed*/) {}
    virtual void on_result(const ResultInfo&) {}                  // broadcast or `grindResult get`
    virtual void on_profiles(const ProfileEntry* /*entries*/, int /*count*/) {}  // the cache changed
    // The response of the pending request (also failures); `request` is what was asked.
    virtual void on_response(RequestKind /*request*/, const Response&) {}
    virtual void on_request_timeout(RequestKind /*request*/) {}       // no response within 5 s
    virtual void on_stop_reply(bool /*accepted*/) {}              // false: the stop was refused
    virtual void on_stop_timeout() {}                             // no stop reply within 5 s
    // After a successful start: started = the first periodInfo past STARTING says GRINDING (or the wait ran out: false).
    virtual void on_start_outcome(bool /*started*/, const char* /*dev_state*/, const char* /*blocked*/) {}
    // The session cannot continue: the glue should disconnect. reason is a short fixed text.
    virtual void on_must_disconnect(const char* /*reason*/) {}
};

class Client {
public:
    Client(Sink& sink, Listener& listener);

    // packet_len: min(ATT MTU - 3, 512), or GLINK_DEFAULT_PACKET_BYTES when the MTU is unknown.
    // Sends appHello; false if that write failed (the glue then disconnects).
    bool on_connected(size_t packet_len, uint32_t now_ms);
    void on_disconnected();
    void on_notify(const uint8_t* packet, size_t len, uint32_t now_ms);
    void tick(uint32_t now_ms);

    // One request at a time (BUSY otherwise): these return false when not connected, when a request is pending, or when
    // the arguments are invalid (the same limits as the grinder; see GLinkMessages.h).
    bool profile_get(int index, uint32_t now_ms);  // index < 0: all
    bool profile_select(int index, uint32_t now_ms);
    bool profile_update(int index, const ProfileUpdate& update, uint32_t now_ms);
    // Always sent with expectDose (the dose the caller just wrote or read), so a stale or racing value is refused by the
    // grinder. Refused until a periodInfo has arrived, and again after a start timeout until a new periodInfo has
    // arrived (spec 7.5: look at periodInfo before sending start again).
    bool start(int profile, int32_t expect_tenths, uint32_t now_ms);
    bool result_get(uint32_t now_ms);
    // Stop is sent at any time while connected (also before the handshake and while a request is pending).
    bool stop(uint32_t now_ms);

    LinkState state() const { return state_; }
    RequestKind pending() const { return pending_; }
    bool start_outcome_pending() const { return start_wait_; }
    bool has_base() const { return has_base_; }
    const BaseInfo& base() const { return base_; }
    bool has_period() const { return has_period_; }
    const PeriodInfo& period() const { return period_; }
    bool has_result() const { return has_result_; }
    const ResultInfo& last_result() const { return result_; }
    const ProfileEntry* profiles() const { return profiles_; }
    int profile_count() const { return profile_count_; }
    // Spec 11 item 7: the profiles are re-read when profileRev differs from the value seen (the first periodInfo of a
    // connection counts as changed).
    bool needs_profile_refresh() const { return needs_profiles_; }
    // True when periodInfo.resultSeq is newer than the last result seen.
    bool needs_result_fetch() const { return has_period_ && period_.result_seq > result_seq_seen_; }

    static constexpr uint32_t RESPONSE_TIMEOUT_MS = 5000;
    static constexpr uint32_t READY_TIMEOUT_MS = 5000;
    static constexpr uint32_t START_CONFIRM_EXTRA_MS = 2000;

private:
    bool send_message(const char* text, size_t len);
    bool begin_request(RequestKind kind, const char* text, size_t len, uint32_t now_ms);
    void handle_message(const char* text, size_t len, uint32_t now_ms);
    void handle_period(const PeriodInfo& period, uint32_t now_ms);
    void handle_response(const Response& response, uint32_t now_ms);
    void note_result(const ResultInfo& result);
    void reset_connection_state();

    Sink& sink_;
    Listener& listener_;
    LinkState state_ = LinkState::DISCONNECTED;
    size_t packet_len_ = GLINK_DEFAULT_PACKET_BYTES;
    uint16_t sequence_ = 0;
    uint32_t hello_sent_ms_ = 0;
    bool ready_timeout_fired_ = false;

    uint8_t rx_storage_[GLINK_MAX_REPLY_BYTES];
    GlinkReassembler reassembler_;
    GlinkBadFrameWindow bad_frames_;

    RequestKind pending_ = RequestKind::NONE;
    uint32_t pending_since_ms_ = 0;
    uint32_t rev_at_request_ = 0;
    bool stop_pending_ = false;
    uint32_t stop_since_ms_ = 0;
    bool start_blocked_ = false;  // a start timed out and no periodInfo has arrived since

    bool start_wait_ = false;
    uint32_t start_deadline_ms_ = 0;

    bool has_base_ = false;
    bool has_period_ = false;
    bool has_result_ = false;
    BaseInfo base_;
    PeriodInfo period_;
    ResultInfo result_;
    uint32_t result_seq_seen_ = 0;
    bool needs_profiles_ = false;
    ProfileEntry profiles_[PROFILES_MAX];
    int profile_count_ = 0;

    Message msg_;  // parse target, kept off the stack
};

}  // namespace glinkc

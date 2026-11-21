#pragma once
#include <stddef.h>
#include <stdint.h>
#include "GLinkClient.h"
#include "GLinkDevState.h"
#include "GLinkMessages.h"
#include "GLinkResultGate.h"
#include "GLinkSettingsSync.h"

// GLink grinder core (chg025): the pure session orchestration behind GLinkGrinder (spec 3.3). Owns the Client, the
// SettingsSync and the ResultGate and implements the Client's Listener. Pure: no Arduino/ESP/NimBLE dependencies, no heap;
// the clock is injected (milliseconds, wrap-safe through the pieces it uses).
//
// The glue calls, all on one thread (the loop task):
//   on_connected()  after the TX subscription is in place (writes appHello through the Sink),
//   on_packet()     for every TX notification, drained from the byte ring BEFORE update() (a stalled loop must not fire the
//                   5 s timeouts while the answer already waits in the ring),
//   update()        about once a second: tick, then at most ONE request: stale profiles, else the settings push, else a
//                   result fetch (profiles first: compare-first needs a fresh entry 3),
//   on_disconnected().
// It never calls Client::start or Client::stop.

namespace glinkc {

class GrinderCore : public Listener {
public:
    // The GaggiMate profile index of the Profile Dose (the only one written).
    static constexpr int PROFILE_DOSE_INDEX = 3;

    explicit GrinderCore(Sink& sink);
    GrinderCore(const GrinderCore&) = delete;
    GrinderCore& operator=(const GrinderCore&) = delete;

    // false: the hello write failed (the glue then disconnects). Starts a fresh session: clears the must-disconnect latch
    // and resets the result gate and SettingsSync link state, so an aborted earlier connect leaves nothing behind.
    // While the latch is set, results, ready events, take_finished and the selected_profile_* getters yield nothing.
    bool on_connected(size_t packet_len, uint32_t now_ms);
    void on_disconnected();
    void on_packet(const uint8_t* data, size_t len, uint32_t now_ms);
    void update(uint32_t now_ms);

    // The wanted Profile Dose settings (tenths). has_dose false clears the wanted value. A dose outside 5.0 to 60.0 g
    // queues nothing (wanted_note() then says OUT_OF_RANGE_DOSE once). Passed to SettingsSync only when the value
    // changed (a re-queue resets its attempt cap) and again at every ready (a link drop does not re-queue a satisfied value).
    void set_wanted(bool has_dose, int32_t dose_tenths, bool has_size, int32_t size_tenths);
    SyncNote wanted_note() { return sync_.take_note(); }
    SyncStage sync_stage() const { return sync_.stage(); }
    const char* sync_gave_up_code() const { return sync_.gave_up_code(); }

    bool ready() const { return client_.state() == LinkState::READY; }
    DevClass dev_class() const;
    bool has_period() const { return client_.has_period(); }
    const PeriodInfo& period() const { return client_.period(); }
    LinkState link_state() const { return client_.state(); }
    // Dose / size of the profile selected on the grinder (from the cache). false: unknown (no period, not cached,
    // or, for the size, none defined).
    bool selected_profile_dose(int32_t* tenths) const;
    bool selected_profile_size(int32_t* tenths) const;
    // The weight (tenths) of a finished, settled grind, once. One slot, latest wins; survives a link drop until taken.
    bool take_finished(int32_t* tenths);
    // true once the session cannot continue; *reason is a fixed text. Latched until the next on_connected().
    bool must_disconnect(const char** reason) const;

    const Client& client() const { return client_; }

    // Listener (called by the Client)
    void on_ready() override;
    void on_result(const ResultInfo& result) override;
    void on_response(RequestKind request, const Response& response) override;
    void on_request_timeout(RequestKind request) override;
    void on_must_disconnect(const char* reason) override;

private:
    const ProfileEntry* find_entry(int index) const;
    SyncView make_view() const;
    void send_next(uint32_t now_ms);

    Client client_;
    SettingsSync sync_;
    ResultGate gate_;
    uint32_t now_ = 0;  // the time of the call being handled, for the Listener callbacks

    bool wanted_ = false;
    int32_t wanted_dose_ = 0;
    bool wanted_has_size_ = false;
    int32_t wanted_size_ = 0;

    bool finished_ = false;
    int32_t finished_tenths_ = 0;
    bool disconnect_ = false;
    const char* disconnect_reason_ = "";
};

}  // namespace glinkc

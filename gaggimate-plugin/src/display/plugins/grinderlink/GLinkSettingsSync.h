#pragma once
#include <stdint.h>
#include "GLinkMessages.h"

// GLink settings sync (chg025): decides WHEN a `grindProfile update` of profile 3 (the Profile Dose) goes out, and, after an
// actual stored change, when a single `grindProfile select 3` follows (so the grinder shows its PROFILE page).
// Pure: no Arduino/ESP/NimBLE dependencies, no heap; the clock is injected (milliseconds, wrap-safe via the signed
// difference). The owner (GLinkGrinderCore) calls next(), sends the request through the Client, then reports on_sent(),
// and later on_response() or on_timeout().
//
// Rules (spec chg025 3.4 and section 0 Q1/Q2):
//  * set_wanted(): dose outside 5.0 to 60.0 g (dose_ok) -> nothing is queued, an older unsent value is dropped, note
//    OUT_OF_RANGE_DOSE. The size is sent only when 0 to 999.9 (tenths 0 to 9999); otherwise the update carries the dose
//    only. No grindUnit, ever. Latest wins and resets the attempts; the same value again while still pending is ignored;
//    the same value after satisfied or given up is queued again (compare-first then decides).
//  * rpm (chg026, spec 1.2): wanted 0 to 9999, else note RPM_OUT_OF_RANGE and it counts as absent. It is sent only when the
//    grinder advertises it (SyncView::rpm_supported); otherwise it is dropped and note RPM_NOT_SUPPORTED is raised once.
//  * Mirror (chg026): no size wanted and the cached entry 3 has one -> the update carries grindSize null; no rpm wanted
//    (or dropped as out of range) and the entry has one and rpm is supported -> rpm null. No cached entry 3: nothing is
//    cleared (the create carries what is wanted only).
//  * Compare-first: cached entry 3 exists with the same dose, size and rpm after the mirror rule (nothing to send, nothing
//    to clear) -> no request, satisfied.
//  * Writes happen only when ready, the client is idle and accepts_profile_writes(devState) holds.
//  * Responses: success -> select pending when the update changed something, else satisfied; select success -> satisfied.
//    RATE_LIMITED -> retry no earlier than 1.1 s later. NOT_READY, EDIT_IN_PROGRESS, COMMAND_IN_FLIGHT, UPDATING, UI_BUSY,
//    BUSY and timeouts -> wait for the writes-accepted state to go away and return (or 5 s, whichever is first). At most
//    3 attempts per stage (update, select); then give up. Any other failure gives up at once. gave_up_code() has the code.
//  * The select is sent at least 1.1 s after the update response.
//  * on_link_down(): attempts, in-flight and retry timing cleared; the wanted value stays (also after a give-up, which is
//    re-queued); a pending select is dropped.

namespace glinkc {

struct SyncLimits {
    static constexpr int32_t SIZE_MAX_TENTHS = 9999;      // 999.9
    static constexpr uint32_t STORE_SPACING_MS = 1100;    // spec 7.3: stored changes at least 1 s apart, plus margin
    static constexpr uint32_t WAIT_FALLBACK_MS = 5000;    // wait-for-state retry when the state never changes
    static constexpr int MAX_ATTEMPTS = 3;
};

// What the owner should do next.
enum class SyncAction : uint8_t { NONE, UPDATE, SELECT };
enum class SyncStage : uint8_t { IDLE, UPDATE_PENDING, SELECT_PENDING, GAVE_UP };  // IDLE = nothing wanted or satisfied
enum class SyncNote : uint8_t { NONE, OUT_OF_RANGE_DOSE, RPM_NOT_SUPPORTED, RPM_OUT_OF_RANGE };

// Snapshot of the session the owner passes to next().
struct SyncView {
    bool ready = false;          // Client::state() == READY
    bool client_idle = false;    // Client::pending() == NONE
    bool writes_ok = false;      // accepts_profile_writes(devState)
    bool has_entry3 = false;     // profile cache holds index 3
    int32_t entry_dose_tenths = 0;
    bool entry_has_size = false;
    int32_t entry_size_tenths = 0;
    bool entry_has_rpm = false;
    int32_t entry_rpm = 0;
    bool rpm_supported = false;  // baseInfo.protoMinor >= 2
};

class SettingsSync {
public:
    SettingsSync() = default;

    void set_wanted(int32_t dose_tenths, bool has_size, int32_t size_tenths, bool has_rpm, int32_t rpm);
    void clear_wanted();

    // UPDATE: *out is filled (dose, size or its clearing, rpm or its clearing, no unit) for Client::profile_update(3, ...). SELECT: send
    // Client::profile_select(3). NONE: nothing now. Call on_sent() only when the Client accepted the request.
    SyncAction next(const SyncView& view, uint32_t now, ProfileUpdate* out);
    void on_sent(uint32_t now);
    // `changed`: the update stored something (pass true when the response carries no flag). Ignored for a select.
    void on_response(bool success, const char* code, bool changed, uint32_t now);
    void on_timeout(uint32_t now);
    void on_link_down();

    SyncStage stage() const { return stage_; }
    bool has_wanted() const { return wanted_; }
    SyncNote take_note();                                       // the latest note once, then NONE
    const char* gave_up_code() const { return gave_up_code_; }  // "" unless stage() == GAVE_UP

private:
    enum class Inflight : uint8_t { NONE, UPDATE, SELECT };

    void fail(const char* code, uint32_t now);
    void give_up(const char* code);
    void wait_for_state(uint32_t now);

    bool wanted_ = false;
    int32_t dose_ = 0;
    bool has_size_ = false;
    int32_t size_ = 0;
    bool has_rpm_ = false;  // a valid rpm (0 to 9999) is wanted
    int32_t rpm_ = 0;
    bool rpm_noted_ = false;  // RPM_NOT_SUPPORTED already raised for this wanted rpm

    SyncStage stage_ = SyncStage::IDLE;
    Inflight inflight_ = Inflight::NONE;
    bool dirty_ = false;  // wanted changed while a request was in flight: ignore that response
    int attempts_ = 0;
    bool retry_set_ = false;
    uint32_t retry_at_ = 0;
    bool wait_state_ = false;
    bool seen_blocked_ = false;
    uint32_t wait_since_ = 0;
    SyncNote note_ = SyncNote::NONE;
    char gave_up_code_[STR] = {};
};

}  // namespace glinkc

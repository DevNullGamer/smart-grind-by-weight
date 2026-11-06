#pragma once
#include <stdint.h>
#include "glink_messages.h"

// Mapping of a finished grind session onto the GLink result code (docs/GLINK-PROTOCOL.md 6.3). Pure. The enums
// below mirror GrindController::GrindSessionResult and the abort reasons of the grind controller, kept here so this
// module has no controller dependency; the glue code that publishes results maps the controller's values onto them
// (bluetooth/glink_session_map.*).
enum class GlinkOutcome : uint8_t { UNKNOWN = 0, SUCCESS, OVERSHOOT, MAX_PULSES, TIMEOUT, ERROR };

enum class GlinkAbortReason : uint8_t {
    NONE = 0,
    SENSOR,
    TARE,
    PRIME_FLOW,
    PREDICTIVE_FLOW,
    STALL,
    MOTOR_FAULT,
    NEG_WEIGHT,
    UPDATE
};

// outcome / reason: what the controller recorded; stopped: the session ended by a stop request (touch, web or BLE).
// A stop explains the end only when the grind has no outcome of its own (UNKNOWN, or the TIMEOUT a stopped grind
// falls into): a normal completion and a real error are reported as they are. A grind ending in ERROR without a
// known reason (NONE) is ERR_SENSOR.
GlinkResultCode glink_result_code(GlinkOutcome outcome, GlinkAbortReason reason, bool stopped);

// True for the codes whose final weight is the settled weight (OK, OVERSHOOT, MAX_PULSES).
bool glink_result_weight_settled(GlinkResultCode code);

// Decides when the Bluetooth task broadcasts a grindResult (docs/GLINK-PROTOCOL.md 5.6): once per finished grind, to the
// client that is connected and handshaken at that moment. A result that already existed when the client handshook is
// not broadcast (it was not "finished while connected"; periodInfo.resultSeq and `get` cover it). Pure, one instance per
// connection owner (the Bluetooth task); reset() on every connect/disconnect and on_handshake() at appHello.
class GlinkResultFeed {
public:
    void reset() { baseline_pending_ = false; have_sent_ = false; sent_seq_ = 0; }
    // appHello processed: the next fresh snapshot only records what already exists
    void on_handshake() { baseline_pending_ = true; have_sent_ = false; }
    // Called with each fresh snapshot while handshaken. true = broadcast the snapshot's result now (a failed attempt
    // may simply call again; mark_sent() ends it).
    bool due(bool has_result, uint32_t seq) {
        if (baseline_pending_) {
            baseline_pending_ = false;
            have_sent_ = has_result;
            sent_seq_ = seq;
            return false;
        }
        return has_result && (!have_sent_ || seq != sent_seq_);
    }
    void mark_sent(uint32_t seq) { have_sent_ = true; sent_seq_ = seq; }

private:
    bool baseline_pending_ = false;
    bool have_sent_ = false;
    uint32_t sent_seq_ = 0;
};

#include "GLinkReconnect.h"

namespace glinkc {

constexpr uint32_t ReconnectTiming::BACKOFF_MS[ReconnectTiming::BACKOFF_STEPS];

Action Reconnect::poll(const Inputs& inputs, uint32_t now) {
    if (settling_ && reached(now, settle_until_)) settling_ = false;  // never let a stale settle time wrap
    if (inputs.gaggimate == GaggimateState::STANDBY) {
        user_ = false;
        armed_ = false;
        immediate_ = false;
        settling_ = false;
        busy_seen_ = false;
        in_standby_ = true;
        return Action::WAIT;
    }
    if (in_standby_) {  // woke up: one immediate attempt
        in_standby_ = false;
        schedule_now();
    }
    if (inputs.gaggimate == GaggimateState::BUSY) {
        busy_seen_ = true;
        settling_ = false;
        return Action::WAIT;
    }
    if (busy_seen_) {  // BUSY just ended
        busy_seen_ = false;
        settling_ = true;
        settle_until_ = now + ReconnectTiming::IDLE_SETTLE_MS;
    }
    if (!inputs.grinder_stored || inputs.link_up || inputs.scanning || user_) return Action::WAIT;
    if (!armed_ || settling_) return Action::WAIT;
    if (!immediate_ && !reached(now, deadline_)) return Action::WAIT;
    // Safety deadline in case the caller forgets to report the outcome; the real backoff replaces it.
    immediate_ = false;
    deadline_ = now + ReconnectTiming::BACKOFF_MS[step_];
    return Action::ATTEMPT;
}

void Reconnect::note_user_disconnect() { user_ = true; }

void Reconnect::note_link_lost(uint32_t now) {
    if (up_known_ && (now - up_since_) >= ReconnectTiming::STABLE_MS) step_ = 0;
    up_known_ = false;
    schedule_backoff(now);
}

void Reconnect::note_attempt(uint32_t now, bool linked_ok) {
    if (linked_ok) {
        note_connected(now);
        return;
    }
    schedule_backoff(now);
}

void Reconnect::note_grinder_changed(uint32_t /*now*/) {
    user_ = false;
    step_ = 0;
    up_known_ = false;
    schedule_now();
}

void Reconnect::note_connected(uint32_t now) {
    up_known_ = true;
    up_since_ = now;
    armed_ = false;
    immediate_ = false;
}

void Reconnect::schedule_backoff(uint32_t now) {
    armed_ = true;
    immediate_ = false;
    deadline_ = now + ReconnectTiming::BACKOFF_MS[step_];
    if (step_ < ReconnectTiming::BACKOFF_STEPS - 1) step_++;
}

void Reconnect::schedule_now() {
    armed_ = true;
    immediate_ = true;
}

}  // namespace glinkc

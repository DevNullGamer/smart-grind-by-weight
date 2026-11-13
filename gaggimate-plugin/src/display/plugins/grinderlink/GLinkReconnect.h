#pragma once
#include <stdint.h>

// GLink auto-reconnect policy (chg023 part 2): decides WHEN the GaggiMate screen may try to connect to its stored grinder
// again. Pure: no Arduino/ESP/NimBLE dependencies, no heap, no pointers; the clock is injected (milliseconds, wrap-safe
// via static_cast<int32_t>(now - t) >= 0). The glue (GLinkPlugin::loop) calls poll() and, on ATTEMPT, connects and then
// reports the outcome with note_attempt(); it reports the other events as they happen.
//
// Rules:
//  1. ATTEMPT only when a grinder is stored, the link is down, no scan is running, the GaggiMate is IDLE, the user flag is
//     clear and the deadline has been reached. Anything else gives WAIT.
//  2. Boot: a fresh object is due immediately, so the first poll that is eligible attempts at once.
//  3. Backoff after a link loss (note_link_lost) or a failed attempt (note_attempt(false)): 3 s, 10 s, 30 s, then 30 s
//     forever (BACKOFF_MS). Each loss or failure uses the next step.
//  4. A link that stayed up less than STABLE_MS (30 s, from note_connected to note_link_lost) does not reset the step
//     (flapping keeps backing off). A link up for STABLE_MS or more resets the step to the first one at note_link_lost.
//     A successful connect by itself never resets the step; only stability does.
//  5. User flag: note_user_disconnect() sets it (RAM only, a fresh object has it clear); while set there are no
//     attempts. It is cleared by note_grinder_changed() (select, forget, manual Connect; these also reset the backoff
//     step and schedule an attempt now) and by STANDBY.
//  6. STANDBY: no attempts; it clears the user flag and drops the schedule. Leaving STANDBY arms an immediate attempt.
//  7. BUSY (a shot or a non-ready system): the deadline is kept, so an attempt that came due during a shot runs right
//     after it ends, once the GaggiMate has been IDLE for IDLE_SETTLE_MS.
//  8. A successful connect (note_connected, or note_attempt(true), which also calls note_connected) drops the schedule;
//     the next one starts at the next note_link_lost().
//  9. When poll() returns ATTEMPT it arms a safety deadline of one backoff interval (step not advanced). A caller that
//     forgets to report the outcome therefore falls back to one attempt per backoff interval; note_attempt(false) or
//     note_link_lost replace the deadline with the real backoff and advance the step.
//
// Reporting: report exactly ONE of note_attempt(false) / note_link_lost per failed connect (a failed NimBLE connect may
// also fire onDisconnect). note_user_disconnect during STANDBY is cleared by the next poll.

namespace glinkc {

enum class GaggimateState : uint8_t { IDLE, BUSY, STANDBY };

struct Inputs {
    bool grinder_stored;
    bool link_up;
    bool scanning;
    GaggimateState gaggimate;
};

enum class Action : uint8_t { WAIT, ATTEMPT };

struct ReconnectTiming {
    static constexpr int BACKOFF_STEPS = 3;
    static constexpr uint32_t BACKOFF_MS[BACKOFF_STEPS] = {3000, 10000, 30000};  // the last step repeats
    static constexpr uint32_t STABLE_MS = 30000;       // link up at least this long: backoff step resets
    static constexpr uint32_t IDLE_SETTLE_MS = 1000;   // idle time required after BUSY ends before an attempt
};

class Reconnect {
public:
    Reconnect() = default;

    // Cheap: the glue calls it on every loop iteration.
    Action poll(const Inputs& inputs, uint32_t now);

    void note_user_disconnect();
    void note_link_lost(uint32_t now);
    void note_attempt(uint32_t now, bool linked_ok);
    void note_grinder_changed(uint32_t now);
    void note_connected(uint32_t now);

    bool user_disconnected() const { return user_; }
    int backoff_step() const { return step_; }  // the step the next loss or failure will use

private:
    static bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }
    void schedule_backoff(uint32_t now);
    void schedule_now();

    bool armed_ = true;        // an attempt is scheduled (boot: yes)
    bool immediate_ = true;    // armed and due at once, deadline_ ignored
    uint32_t deadline_ = 0;
    int step_ = 0;
    bool user_ = false;
    bool in_standby_ = false;
    bool busy_seen_ = false;   // BUSY seen since the last settle
    bool settling_ = false;
    uint32_t settle_until_ = 0;
    bool up_known_ = false;    // note_connected seen for the current link
    uint32_t up_since_ = 0;
};

}  // namespace glinkc

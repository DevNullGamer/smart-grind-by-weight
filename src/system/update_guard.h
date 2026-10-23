#pragma once

#include <atomic>
#include <stdint.h>

/**
 * UpdateGuard - grinder-safety envelope around a WiFi (raw image) firmware update.
 *
 * Deliberately a small copy of the rules in OTAHandler::start_ota_locked()/cleanup_failed_ota() (the BLE
 * OTA is not refactored onto it yet, see chg008 notes A2). acquire() mirrors the BLE sequence:
 *   1. refuse if another update (BLE OTA or another guard) is running
 *   2. side-effect-free busy check (grind / autotune / motor)
 *   3. grinder lock_out(), then re-check; if busy now: unlock() and refuse
 *   4. extend the task watchdog, suspend the hardware tasks, disable touch
 * release() undoes it in the same order as the BLE cleanup: resume tasks first, then restore the watchdog,
 * re-enable touch, unlock the grinder. Idempotent. The guard never restarts the device; after a successful
 * update the caller restarts and simply never releases.
 *
 * Threading: acquire()/release() may be called from any task; one guard at a time system-wide
 * (is_held()). The caller serialises its own sink and calls note_activity() on each received chunk;
 * a periodic tick calls check_inactivity_timeout() and, when it returns true, aborts its sink.
 */
class UpdateGuard {
public:
    enum class Result : uint8_t {
        OK,
        ALREADY_HELD,      // this or another guard is held
        BLE_UPDATE_ACTIVE, // a BLE OTA is in progress
        GRINDER_BUSY       // grind, autotune or motor running
    };

    UpdateGuard() = default;
    ~UpdateGuard() { release(); }
    UpdateGuard(const UpdateGuard&) = delete;
    UpdateGuard& operator=(const UpdateGuard&) = delete;

    Result acquire();
    // Idempotent; safe if acquire() failed or was never called.
    void release();

    // True while this guard owns the update envelope
    bool held() const { return owns_.load(); }
    // True while ANY guard is held (used to keep the BLE OTA from starting)
    static bool is_held() { return any_held_.load(); }

    // Short reason for the last refused acquire(), suitable for UI / HTTP responses
    const char* refusal_reason() const { return refusal_reason_; }
    static const char* result_text(Result result);

    // Call whenever update data arrives (and after long operations such as the initial flash erase)
    void note_activity();
    // Releases the guard and returns true when no activity was noted for UPDATE_INACTIVITY_TIMEOUT_MS.
    // The caller must then abort its sink. False when not held or still active.
    bool check_inactivity_timeout();

private:
    static std::atomic<bool> any_held_;
    std::atomic<bool> owns_{false};
    std::atomic<uint32_t> last_activity_ms_{0};
    const char* refusal_reason_ = "";
};

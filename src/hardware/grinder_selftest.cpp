#include "grinder.h"
#include "../config/constants.h"

#if DEBUG_RMT_PULSE_SELFTEST

#include <esp_timer.h>

// NOTE on what is measured: every span below starts just before start_pulse_rmt() is called,
// so it INCLUDES encoder creation/transmit setup, and it ends at the end of the transmission,
// which INCLUDES the trailing 1 us LOW symbol. Three end times are reported:
//   first  = first on_trans_done callback
//   last   = last on_trans_done callback (used as "measured")
//   pin    = first time the motor pin was polled LOW after being seen HIGH (~1 ms resolution)
// cb=N is the number of callbacks seen per pulse; it is unknown whether the IDF fires
// on_trans_done once at the end of a looped transmission or once per loop iteration, so
// rows with N>1 are marked UNRELIABLE.

volatile int64_t Grinder::selftest_done_us = 0;
volatile int64_t Grinder::selftest_first_us = 0;
volatile uint32_t Grinder::selftest_done_count = 0;

bool IRAM_ATTR Grinder::selftest_on_trans_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void*) {
    const int64_t now = esp_timer_get_time();
    if (selftest_first_us == 0) {
        selftest_first_us = now;
    }
    selftest_done_us = now;
    selftest_done_count = selftest_done_count + 1;
    return false;  // No higher-priority task woken
}

namespace {

struct PulseResult {
    uint32_t req_ms;
    bool completed;
    uint32_t callbacks;
    int64_t first_us;   // Relative to pulse start
    int64_t last_us;    // Relative to pulse start
    int64_t pin_low_us; // Relative to pulse start, 0 = not seen
};

float error_pct(int64_t measured_us, int64_t requested_us) {
    return 100.0f * (float)(measured_us - requested_us) / (float)requested_us;
}

void print_summary(const PulseResult* results, size_t count) {
    LOG_BLE("[RMT-SELFTEST] ---- summary (spans include encoder setup and the 1us LOW tail) ----\n");
    LOG_BLE("[RMT-SELFTEST] req_ms  first_us  last_us  err_pct  pin_low_us  cb\n");
    for (size_t i = 0; i < count; i++) {
        const PulseResult& r = results[i];
        if (!r.completed) {
            LOG_BLE("[RMT-SELFTEST] %6lu  TIMEOUT\n", (unsigned long)r.req_ms);
            continue;
        }
        LOG_BLE("[RMT-SELFTEST] %6lu  %8lld  %7lld  %+7.1f  %10lld  %lu%s\n", (unsigned long)r.req_ms,
                (long long)r.first_us, (long long)r.last_us, error_pct(r.last_us, (int64_t)r.req_ms * 1000),
                (long long)r.pin_low_us, (unsigned long)r.callbacks,
                r.callbacks > 1 ? "  UNRELIABLE (cb>1)" : "");
    }
}

}  // namespace

void Grinder::run_pulse_selftest() {
    static const uint32_t durations_ms[] = {20, 50, 65, 70, 80, 100, 150, 200, 300, 550, 1000};
    constexpr size_t kCount = sizeof(durations_ms) / sizeof(durations_ms[0]);
    constexpr uint32_t kCountdownMs = 10000;
    constexpr uint32_t kGapMs = 500;
    constexpr uint32_t kExtraCallbackWaitMs = 150;
    constexpr int64_t kPinLowGraceUs = 50000;
    constexpr int kSummaryRepeats = 3;
    constexpr uint32_t kSummaryGapMs = 5000;

    LOG_BLE("[RMT-SELFTEST] RMT SELF-TEST in 10 s: DISCONNECT THE GRINDER FROM MAINS (OPEN SERIAL MONITOR BEFORE RESET)\n");
    LOG_BLE("[RMT-SELFTEST] Measured spans include encoder setup and the 1us LOW tail\n");
    delay(kCountdownMs);

    if (!initialized || !rmt_initialized) {
        LOG_BLE("[RMT-SELFTEST] Grinder not initialized - aborting\n");
        return;
    }

    PulseResult results[kCount] = {};

    for (size_t i = 0; i < kCount; i++) {
        const uint32_t d_ms = durations_ms[i];
        PulseResult& r = results[i];
        r.req_ms = d_ms;

        selftest_first_us = 0;
        selftest_done_us = 0;
        selftest_done_count = 0;

        const int64_t start_us = esp_timer_get_time();
        start_pulse_rmt(d_ms);

        const int64_t timeout_us = ((int64_t)2 * d_ms + 500) * 1000;
        bool pin_seen_high = false;
        while ((esp_timer_get_time() - start_us) < timeout_us) {
            const int64_t now = esp_timer_get_time();
            const int pin_level = digitalRead(motor_pin);
            if (pin_level == HIGH) {
                pin_seen_high = true;
            } else if (pin_seen_high && r.pin_low_us == 0) {
                r.pin_low_us = now - start_us;
            }
            if (selftest_done_count > 0 &&
                (r.pin_low_us != 0 || (now - selftest_first_us) > kPinLowGraceUs)) {
                break;
            }
            delay(1);
        }

        if (selftest_done_count > 0) {
            // Give extra callbacks (if any) time to arrive before taking the final count
            delay(kExtraCallbackWaitMs);
            r.completed = true;
            r.callbacks = selftest_done_count;
            r.first_us = selftest_first_us - start_us;
            r.last_us = selftest_done_us - start_us;

            const int64_t requested_us = (int64_t)d_ms * 1000;
            LOG_BLE("[RMT-SELFTEST] req=%lldus measured=%lldus err=%+.1f%% first=%lldus pin_low=%lldus cb=%lu%s\n",
                    (long long)requested_us, (long long)r.last_us, error_pct(r.last_us, requested_us),
                    (long long)r.first_us, (long long)r.pin_low_us, (unsigned long)r.callbacks,
                    r.callbacks > 1 ? " UNRELIABLE (cb>1)" : "");
        } else {
            LOG_BLE("[RMT-SELFTEST] req=%lldus TIMEOUT (no completion callback)\n", (long long)d_ms * 1000);
        }

        stop();  // Clear pulse state (not part of the measurement)
        delay(kGapMs);
    }

    for (int n = 0; n < kSummaryRepeats; n++) {
        print_summary(results, kCount);
        if (n < kSummaryRepeats - 1) {
            delay(kSummaryGapMs);
        }
    }
    LOG_BLE("[RMT-SELFTEST] done\n");
}

#endif  // DEBUG_RMT_PULSE_SELFTEST

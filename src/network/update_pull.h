#pragma once

#include <Arduino.h>
#include <atomic>
#include "pull_logic.h"
#include "../system/update_guard.h"
#include "../system/update_sink_image.h"

// What the last successful check found (valid when the check parsed a release)
struct PullOffer {
    char version[SEMVER_PRE_MAX + 1] = {0};  // without leading "v"
    uint32_t size = 0;
    bool prerelease = false;
};

/**
 * UpdatePull - checks a release channel and installs a release over HTTPS (the grinder pulls, nobody pushes).
 *
 * request_check() / request_install() never block and may be called from any task (UI, Bluetooth, the autorun
 * test task). Each starts the worker task "UpdatePull", which runs the job inside a TlsPsramScope (mbedTLS buffers
 * in PSRAM) and deletes itself when done. The job state is a small state machine (pull_logic.h); the UI only reads
 * state(), failure(), offer() and UpdateStatus. No LVGL call is ever made from the worker.
 *
 * Install sequence (worker): open the HTTPS connection and validate the reply (status 200, Content-Length ==
 * the size promised by the index) BEFORE anything else changes -> UpdateGuard::acquire() (the same envelope as a
 * push update; exclusion against push and BLE is UpdateGuard::is_held()) -> expected-version marker -> ImageSink
 * begin -> read 4 KB / sink write ... -> sink finish (SHA-256, esp_ota_end, set boot partition) -> restart after
 * UPDATE_RESTART_DELAY_MS. Every failure closes the connection, aborts the sink, releases the guard, clears the
 * marker and publishes FAILED; the running image and the boot partition are untouched until finish() succeeds.
 *
 * Refusals (precondition failures, see pull_refusal) leave the state alone and are reported by refusal().
 */
class UpdatePull {
public:
    // Starts a check of the stable channel, or the beta channel (which also offers prereleases) when
    // include_prereleases. False if refused: refusal() says why.
    bool request_check(bool include_prereleases);
    // Starts the install of the release found by the last check (state AVAILABLE). False if refused.
    bool request_install();
    // Finished states (UP_TO_DATE, AVAILABLE, CHECK_FAILED, FAILED) back to IDLE; no effect otherwise
    void reset();
    // Why request_install() would be refused right now (NONE if it would start); no side effects. Lets the UI
    // decide before it turns Bluetooth off.
    PullFailure install_refusal() const;
    // millis() when the last job ended (the age of an AVAILABLE result)
    uint32_t result_at_ms() const { return result_ms_.load(); }

    PullState state() const { return static_cast<PullState>(state_.load()); }
    // Why the last CHECK_FAILED / FAILED happened (NONE otherwise)
    PullFailure failure() const { return static_cast<PullFailure>(failure_.load()); }
    // Why the last request_*() call was refused (NONE after an accepted request)
    PullFailure refusal() const { return static_cast<PullFailure>(refusal_.load()); }
    // The channel had no release at all (UP_TO_DATE only)
    bool no_release() const { return no_release_.load(); }
    // Latest release known from the last check; false if the last check found none
    bool offer(PullOffer* out) const;

    // Entry point of the worker task
    static void task_entry(void* arg);

private:
    enum class Job : uint8_t { CHECK, INSTALL };

    std::atomic<uint8_t> state_{static_cast<uint8_t>(PullState::IDLE)};
    std::atomic<uint8_t> failure_{static_cast<uint8_t>(PullFailure::NONE)};
    std::atomic<uint8_t> refusal_{static_cast<uint8_t>(PullFailure::NONE)};
    std::atomic<bool> no_release_{false};
    std::atomic<uint32_t> result_ms_{0};
    Job job_ = Job::CHECK;
    bool beta_ = false;

    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    ReleaseInfo info_;          // guarded by mux_; written by the check job
    bool info_valid_ = false;   // guarded by mux_

    UpdateGuard guard_;         // worker only
    ImageSink sink_;            // worker only
    bool marker_owned_ = false; // worker only: the guard was granted, so the expected-version marker is ours to clear

    bool refuse(PullFailure why, const char* what);
    bool start(PullState from, PullState to, Job job, bool include_prereleases);
    bool set_state(PullState from, PullState to);
    void finish_job(PullState to, PullFailure why);

    void run();
    void run_check();
    void run_install();
    PullFailure check_index(bool beta, PullIndexDecision* decision);
    PullFailure install_image(const ReleaseInfo& info);
    void abort_install(PullFailure why);
};

extern UpdatePull update_pull;

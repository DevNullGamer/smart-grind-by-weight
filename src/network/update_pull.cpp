#include "update_pull.h"
#include "release_client.h"
#include "update_approval_gate.h"
#include "update_status.h"
#include "wifi_manager.h"
#include "../system/boot_health.h"
#include "../system/mbedtls_psram_override.h"
#include "../system/ota_marker.h"
#include "../bluetooth/manager.h"
#include "../config/constants.h"
#include "../controllers/grind_controller.h"
#include "../hardware/hardware_manager.h"
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

UpdatePull update_pull;
extern HardwareManager hardware_manager;
extern GrindController grind_controller;

namespace {

// PSRAM buffer with internal-RAM fallback, freed when the job step ends
struct Buffer {
    uint8_t* data = nullptr;
    explicit Buffer(size_t size) {
        data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data) data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    ~Buffer() { heap_caps_free(data); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

void log_heap(const char* what) {
    LOG_BLE("[HEAP] %s: internal free %u, largest block %u, lowest ever %u, PSRAM free %u\n", what,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Same test as the push endpoint and the guard: a grind, autotune or any motor activity
bool grinder_busy() {
    return grind_controller.is_grind_running() || hardware_manager.get_autotune_controller()->is_active() ||
           hardware_manager.get_grinder()->is_grinding();
}

PullPreconditions gather_preconditions() {
    PullPreconditions facts;
    facts.wifi_connected = wifi_manager.is_connected();
    facts.update_active = UpdateGuard::is_held() || update_approval.in_use() || UpdateStatus::active();
    facts.boot_pending = boot_health_pending();
    facts.grinder_busy = grinder_busy();
    facts.ble_connected = g_bluetooth_manager.is_connected() || g_bluetooth_manager.is_data_export_active();
    facts.internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return facts;
}

PullFailure failure_for_sink(SinkError error) {
    switch (error) {
        case SinkError::OK: return PullFailure::NONE;
        case SinkError::SHA_MISMATCH: return PullFailure::CHECKSUM_MISMATCH;
        case SinkError::BAD_HEADER: return PullFailure::NOT_FIRMWARE;
        case SinkError::BAD_ARGUMENT:
        case SinkError::BAD_SIZE:
        case SinkError::TOO_MUCH_DATA:
        case SinkError::SIZE_MISMATCH:
        case SinkError::IMAGE_INVALID: return PullFailure::IMAGE_REJECTED;
        case SinkError::NO_PARTITION:
        case SinkError::BEGIN_FAILED:
        case SinkError::WRITE_FAILED:
        case SinkError::HASH_FAILED:
        case SinkError::BOOT_SET_FAILED: return PullFailure::FLASH_FAILED;
        case SinkError::ALREADY_ACTIVE:
        case SinkError::NOT_ACTIVE: return PullFailure::INTERNAL;
    }
    return PullFailure::INTERNAL;
}

PullFailure failure_for_guard(UpdateGuard::Result result) {
    switch (result) {
        case UpdateGuard::Result::OK: return PullFailure::NONE;
        case UpdateGuard::Result::GRINDER_BUSY: return PullFailure::GRINDER_BUSY;
        case UpdateGuard::Result::ALREADY_HELD:
        case UpdateGuard::Result::BLE_UPDATE_ACTIVE: return PullFailure::UPDATE_RUNNING;
    }
    return PullFailure::INTERNAL;
}

}  // namespace

//------------------------------------------------------------------------------
// Requests (any task)
//------------------------------------------------------------------------------
bool UpdatePull::refuse(PullFailure why, const char* what) {
    refusal_.store(static_cast<uint8_t>(why));
    LOG_BLE("[PULL] %s refused: %s\n", what, pull_failure_text(why));
    return false;
}

bool UpdatePull::set_state(PullState from, PullState to) {
    if (!pull_transition_allowed(from, to)) return false;
    uint8_t expected = static_cast<uint8_t>(from);
    return state_.compare_exchange_strong(expected, static_cast<uint8_t>(to));
}

bool UpdatePull::start(PullState from, PullState to, Job job, bool include_prereleases) {
    if (!set_state(from, to)) return refuse(PullFailure::BUSY, job == Job::CHECK ? "check" : "install");
    job_ = job;  // the worker starts below, after these writes
    beta_ = include_prereleases;
    failure_.store(static_cast<uint8_t>(PullFailure::NONE));
    refusal_.store(static_cast<uint8_t>(PullFailure::NONE));
    if (xTaskCreatePinnedToCore(task_entry, "UpdatePull", UPDATE_PULL_TASK_STACK, this, UPDATE_PULL_TASK_PRIORITY, nullptr,
                                UPDATE_PULL_TASK_CORE) != pdPASS) {
        LOG_BLE("[PULL] worker task could not be created (free internal %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        refusal_.store(static_cast<uint8_t>(PullFailure::LOW_MEMORY));
        finish_job(job == Job::CHECK ? PullState::CHECK_FAILED : PullState::FAILED, PullFailure::LOW_MEMORY);
        return false;
    }
    return true;
}

bool UpdatePull::request_check(bool include_prereleases) {
    const PullState current = state();
    const PullFailure why = pull_refusal(current, false, gather_preconditions());
    if (why != PullFailure::NONE) return refuse(why, "check");
    return start(current, PullState::CHECKING, Job::CHECK, include_prereleases);
}

bool UpdatePull::request_install() {
    const PullState current = state();
    const PullFailure why = pull_refusal(current, true, gather_preconditions());
    if (why != PullFailure::NONE) return refuse(why, "install");
    return start(current, PullState::DOWNLOADING, Job::INSTALL, false);
}

PullFailure UpdatePull::install_refusal() const {
    return pull_refusal(state(), true, gather_preconditions());
}

void UpdatePull::reset() {
    const PullState current = state();
    if (set_state(current, PullState::IDLE)) {
        failure_.store(static_cast<uint8_t>(PullFailure::NONE));
        no_release_.store(false);
    }
}

bool UpdatePull::offer(PullOffer* out) const {
    bool valid;
    portENTER_CRITICAL(&mux_);
    valid = info_valid_;
    if (valid && out) {
        strncpy(out->version, info_.version, sizeof(out->version) - 1);
        out->version[sizeof(out->version) - 1] = '\0';
        out->size = info_.size;
        out->prerelease = info_.prerelease;
    }
    portEXIT_CRITICAL(&mux_);
    return valid;
}

// Publishes the end of a job. The failure is stored first so a reader that sees the new state also sees why.
void UpdatePull::finish_job(PullState to, PullFailure why) {
    failure_.store(static_cast<uint8_t>(why));
    result_ms_.store(millis());
    if (!set_state(state(), to)) {
        LOG_BLE("[PULL] ERROR: unexpected state change %s -> %s\n", pull_state_name(state()), pull_state_name(to));
        state_.store(static_cast<uint8_t>(to));  // never leave the job in a running state
    }
}

//------------------------------------------------------------------------------
// Worker
//------------------------------------------------------------------------------
void UpdatePull::task_entry(void* arg) {
    static_cast<UpdatePull*>(arg)->run();
    LOG_BLE("[PULL] worker stack high-water: %u bytes free of %u\n",
            (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)), (unsigned)UPDATE_PULL_TASK_STACK);
    vTaskDelete(nullptr);
}

void UpdatePull::run() {
    log_heap(job_ == Job::CHECK ? "pull job start (check)" : "pull job start (install)");
    if (job_ == Job::CHECK) {
        run_check();
    } else {
        run_install();
    }
    log_heap("pull job end");
    LOG_BLE("[PULL] job ended in state %s%s%s\n", pull_state_name(state()), failure() == PullFailure::NONE ? "" : ", ",
            pull_failure_text(failure()));
    if (state() == PullState::INSTALLED) {
        // The guard stays held (tasks suspended, grinder locked out) until the restart, like a push update
        vTaskDelay(pdMS_TO_TICKS(UPDATE_RESTART_DELAY_MS));
        LOG_BLE("[PULL] restarting into the new firmware\n");
        Serial.flush();
        esp_restart();
    }
}

//------------------------------------------------------------------------------
// Check
//------------------------------------------------------------------------------
void UpdatePull::run_check() {
    PullIndexDecision decision;
    PullFailure why;
    {
        TlsPsramScope tls_scope;
        why = check_index(beta_, &decision);
    }
    if (why != PullFailure::NONE) {
        finish_job(PullState::CHECK_FAILED, why);
    } else {
        finish_job(decision.state, decision.failure);
    }
}

PullFailure UpdatePull::check_index(bool beta, PullIndexDecision* decision) {
    constexpr size_t kCapacity = UPDATE_PULL_INDEX_MAX_BYTES + 1;
    Buffer body(kCapacity);
    if (!body.data) return PullFailure::LOW_MEMORY;

    ReleaseClient client;
    size_t length = 0;
    const PullFailure fetched = client.fetch_index(beta, reinterpret_cast<char*>(body.data), kCapacity, &length);
    if (fetched != PullFailure::NONE) return fetched;

    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    if (!slot) return PullFailure::INTERNAL;
    ReleaseInfo info;
    const ReleaseParseResult parsed = release_parse_index(reinterpret_cast<const char*>(body.data), length, slot->size,
                                                          beta ? "beta" : "stable", &info);
    *decision = pull_evaluate_index(parsed, &info, BUILD_FIRMWARE_VERSION);

    const bool have_release = parsed == ReleaseParseResult::OK;
    portENTER_CRITICAL(&mux_);
    info_valid_ = have_release;
    if (have_release) info_ = info;
    portEXIT_CRITICAL(&mux_);
    no_release_.store(decision->no_release);

    if (have_release) {
        LOG_BLE("[PULL] check (%s): installed %s, latest %s (%lu bytes) -> %s\n", beta ? "beta" : "stable",
                BUILD_FIRMWARE_VERSION, info.version, (unsigned long)info.size, pull_state_name(decision->state));
    } else {
        LOG_BLE("[PULL] check (%s): %s -> %s\n", beta ? "beta" : "stable", release_parse_result_text(parsed),
                pull_state_name(decision->state));
    }
    return PullFailure::NONE;
}

//------------------------------------------------------------------------------
// Install
//------------------------------------------------------------------------------
void UpdatePull::run_install() {
    ReleaseInfo info;
    bool valid;
    portENTER_CRITICAL(&mux_);
    valid = info_valid_;
    if (valid) info = info_;
    portEXIT_CRITICAL(&mux_);

    PullFailure why = PullFailure::NOT_AVAILABLE;
    if (valid) {
        TlsPsramScope tls_scope;
        why = install_image(info);
    }
    if (why != PullFailure::NONE) {
        abort_install(why);
        finish_job(PullState::FAILED, why);
        return;
    }
    UpdateStatus::set_installed();
    finish_job(PullState::INSTALLED, PullFailure::NONE);
}

// Everything that must happen on any install failure, in the order of UpdateServer::abort_session_locked(): sink,
// then the guard (tasks run again), then the marker. UpdateStatus is only touched when this job owned the guard,
// so a refusal never overwrites the status of a push update that holds it.
void UpdatePull::abort_install(PullFailure why) {
    const bool owned = guard_.held();
    sink_.abort();
    guard_.release();
    if (marker_owned_) ota_marker_clear();
    marker_owned_ = false;
    if (owned) UpdateStatus::set_failed(pull_failure_text(why));
    LOG_BLE("[PULL] install failed: %s\n", pull_failure_text(why));
}

PullFailure UpdatePull::install_image(const ReleaseInfo& info) {
    Buffer buffer(UPDATE_PULL_CHUNK_BYTES);
    if (!buffer.data) return PullFailure::LOW_MEMORY;

    // 1. Connect and validate the reply while nothing else has changed: a 404, a redirect, a missing Content-Length
    //    or a size that differs from the index ends here, before the grinder is locked or a flash byte is written.
    ReleaseClient client;
    const PullFailure opened = client.open(info.url, info.size, info.size);
    if (opened != PullFailure::NONE) return opened;

    // 2. The update envelope (same rules and the same system-wide exclusion as a push or BLE update). A Bluetooth
    //    client that connected since the request is refused here; later connections are refused by onConnect().
    if (g_bluetooth_manager.is_connected() || g_bluetooth_manager.is_data_export_active()) return PullFailure::BLE_CONNECTED;
    const PullFailure guarded = failure_for_guard(guard_.acquire());
    if (guarded != PullFailure::NONE) return guarded;
    guard_.note_activity();
    marker_owned_ = true;  // from here on every failure path removes the marker
    if (!ota_marker_store(-1, info.version)) {
        LOG_BLE("[PULL] WARNING: expected-version marker not stored, a failed boot will not be reported\n");
    }
    guard_.note_activity();

    const SinkError begun = sink_.begin(info.size, info.sha256);
    guard_.note_activity();
    if (begun != SinkError::OK) {
        LOG_BLE("[PULL] sink refused: %s\n", sink_.reason());
        return failure_for_sink(begun);
    }
    UpdateStatus::set_receiving(UpdateSource::WIFI_DOWNLOAD, info.size);

    // 3. Download
    const uint32_t total = info.size;
    uint32_t received = 0;
    uint32_t next_log = UPDATE_PULL_PROGRESS_LOG_STEP_BYTES;
    const uint32_t started = millis();
    LOG_BLE("[PULL] downloading %lu bytes\n", (unsigned long)total);
    while (received < total) {
        if (pull_download_deadline_exceeded(started, millis(), UPDATE_PULL_DOWNLOAD_MAX_MS)) {
            LOG_BLE("[PULL] download deadline passed after %lu of %lu bytes\n", (unsigned long)received,
                    (unsigned long)total);
            return PullFailure::DOWNLOAD_STALLED;
        }
        const uint32_t want = total - received < UPDATE_PULL_CHUNK_BYTES ? total - received : UPDATE_PULL_CHUNK_BYTES;
        const int n = client.read(buffer.data, want);
        const PullReadOutcome outcome = pull_classify_read(n, received, total);
        if (outcome != PullReadOutcome::DATA) {  // DONE cannot happen while received < total
            const PullFailure failure = pull_failure_for_read(outcome);
            LOG_BLE("[PULL] read ended after %lu of %lu bytes (read returned %d)\n", (unsigned long)received,
                    (unsigned long)total, n);
            return failure == PullFailure::NONE ? PullFailure::INTERNAL : failure;
        }
        const SinkError written = sink_.write(buffer.data, static_cast<size_t>(n));
        if (written != SinkError::OK) {
            LOG_BLE("[PULL] sink write failed: %s\n", sink_.reason());
            return failure_for_sink(written);
        }
        guard_.note_activity();
        received += static_cast<uint32_t>(n);
        UpdateStatus::set_received(received);
        if (received >= next_log) {
            LOG_BLE("[PULL] received %lu / %lu bytes (internal free %u)\n", (unsigned long)received, (unsigned long)total,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            next_log += UPDATE_PULL_PROGRESS_LOG_STEP_BYTES;
        }
    }
    const uint32_t elapsed_ms = millis() - started;
    LOG_BLE("[PULL] all %lu bytes received in %lu ms, verifying\n", (unsigned long)received, (unsigned long)elapsed_ms);
    client.close();  // frees the TLS buffers before the (slow) verification

    // 4. Verify and select the new image
    if (!set_state(PullState::DOWNLOADING, PullState::VERIFYING)) return PullFailure::INTERNAL;
    UpdateStatus::set_verifying();
    const SinkError finished = sink_.finish();
    guard_.note_activity();
    if (finished != SinkError::OK) {
        LOG_BLE("[PULL] sink finish failed: %s\n", sink_.reason());
        return failure_for_sink(finished);
    }
    LOG_BLE("[PULL] installed version %s\n", info.version);
    return PullFailure::NONE;
}

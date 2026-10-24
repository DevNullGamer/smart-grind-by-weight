#include "update_pull_autorun.h"

#if DEBUG_PULL_AUTORUN

#include "release_client.h"
#include "update_pull.h"
#include "wifi_manager.h"
#include "../config/constants.h"
#include "../system/boot_health.h"
#include "../system/mbedtls_psram_override.h"
#include <esp_heap_caps.h>

namespace {

constexpr uint32_t kWifiWaitMs = 120000;
constexpr uint32_t kBootPendingWaitMs = 60000;
constexpr uint32_t kJobTimeoutMs = 20UL * 60UL * 1000UL;
constexpr uint32_t kSettleMs = 10000;
constexpr int kSmokeRuns = 3;
constexpr size_t kSmokeIndexCap = 16 * 1024;
constexpr uint32_t kSmokeDownloadCap = 3UL * 1024UL * 1024UL;
constexpr uint32_t kSmokeTimeLimitMs = 90000;

void log_heap(const char* what) {
    LOG_BLE("[HEAP] %s: internal free %u, largest block %u, lowest ever %u\n", what,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

bool wait_until(bool (*done)(), uint32_t timeout_ms) {
    const uint32_t started = millis();
    while (!done()) {
        if (millis() - started > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return true;
}

bool wifi_up() { return wifi_manager.is_connected(); }

#if DEBUG_PULL_AUTORUN != 2
//------------------------------------------------------------------------------
// Mode 1: Check + Install against the PC test server
//------------------------------------------------------------------------------
bool image_confirmed() { return !boot_health_pending(); }
bool job_finished() { return !pull_state_is_running(update_pull.state()); }

void run_pull() {
    LOG_BLE("[PULL][AUTORUN] start: installed %s (build %d), channel %s\n", BUILD_FIRMWARE_VERSION, BUILD_NUMBER,
            DEBUG_PULL_AUTORUN_BETA ? "beta" : "stable");
    if (!wait_until(wifi_up, kWifiWaitMs)) {
        LOG_BLE("[PULL][AUTORUN] WiFi did not come up, giving up\n");
        return;
    }
    if (!wait_until(image_confirmed, kBootPendingWaitMs)) {
        LOG_BLE("[PULL][AUTORUN] image still unconfirmed, the install will be refused\n");
    }

    if (!update_pull.request_check(DEBUG_PULL_AUTORUN_BETA != 0)) {
        LOG_BLE("[PULL][AUTORUN] check refused: %s\n", pull_failure_text(update_pull.refusal()));
        return;
    }
    if (!wait_until(job_finished, kJobTimeoutMs)) {
        LOG_BLE("[PULL][AUTORUN] check did not finish\n");
        return;
    }
    PullOffer offer;
    const bool have_offer = update_pull.offer(&offer);
    LOG_BLE("[PULL][AUTORUN] check result: %s%s%s, latest %s\n", pull_state_name(update_pull.state()),
            update_pull.failure() == PullFailure::NONE ? "" : " reason: ", pull_failure_text(update_pull.failure()),
            have_offer ? offer.version : "none");

    if (update_pull.state() == PullState::AVAILABLE) {
        if (!update_pull.request_install()) {
            LOG_BLE("[PULL][AUTORUN] install refused: %s\n", pull_failure_text(update_pull.refusal()));
        } else if (wait_until(job_finished, kJobTimeoutMs)) {
            // INSTALLED never gets here: the worker restarts the device
            LOG_BLE("[PULL][AUTORUN] install result: %s reason: %s\n", pull_state_name(update_pull.state()),
                    pull_failure_text(update_pull.failure()));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    log_heap("[AUTORUN] 10 s after the job");
}

#else
//------------------------------------------------------------------------------
// Mode 2: HTTPS smoke test (index + discard download), as the spike did but with the production scope
//------------------------------------------------------------------------------
// firmware/index.json names the web-ota asset under "ota"; the raw image sits next to it without the suffix
bool find_bin_urls(const char* json, char* raw_url, char* ota_url, size_t len) {
    const char* key = strstr(json, "\"ota\"");
    if (!key) return false;
    const char* colon = strchr(key, ':');
    const char* start = colon ? strchr(colon, '"') : nullptr;
    if (!start) return false;
    start++;
    const char* end = strchr(start, '"');
    if (!end) return false;
    char path[200];
    const size_t n = static_cast<size_t>(end - start);
    if (n == 0 || n >= sizeof(path)) return false;
    memcpy(path, start, n);
    path[n] = '\0';
    const char* suffix = "-web-ota.bin";
    const size_t suffix_len = strlen(suffix);
    if (n < suffix_len || strcmp(path + n - suffix_len, suffix) != 0) return false;
    snprintf(ota_url, len, "%s%s", UPDATE_PULL_BASE_URL, path);
    path[n - suffix_len] = '\0';
    snprintf(raw_url, len, "%s%s.bin", UPDATE_PULL_BASE_URL, path);
    return true;
}

// Reads and discards one body; returns true when it arrived completely
bool discard_download(ReleaseClient& client, const char* url, uint8_t* chunk, size_t chunk_size, uint32_t* bytes,
                      uint32_t* elapsed_ms) {
    *bytes = 0;
    const uint32_t started = millis();
    PullFailure opened = client.open(url, -1, 16UL * 1024UL * 1024UL);
    if (opened != PullFailure::NONE) {
        LOG_BLE("[PULL][SMOKE] open failed: %s\n", pull_failure_text(opened));
        *elapsed_ms = millis() - started;
        return false;
    }
    const uint32_t total = static_cast<uint32_t>(client.content_length());
    bool complete = false;
    while (*bytes < kSmokeDownloadCap && millis() - started < kSmokeTimeLimitMs) {
        const int n = client.read(chunk, chunk_size);
        if (pull_classify_read(n, *bytes, total) != PullReadOutcome::DATA) {
            complete = n == 0 && *bytes == total;
            break;
        }
        *bytes += static_cast<uint32_t>(n);
    }
    *elapsed_ms = millis() - started;
    client.close();
    return complete || *bytes >= kSmokeDownloadCap;  // the cap is a deliberate early stop
}

bool smoke_once(int run) {
    log_heap("[SMOKE] before TLS");
    bool ok = false;
    {
        TlsPsramScope tls_scope;
        uint8_t* body = static_cast<uint8_t*>(heap_caps_malloc(kSmokeIndexCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(UPDATE_PULL_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ReleaseClient client;
        char url[UPDATE_PULL_URL_MAX];
        char raw_url[320] = {0};
        char ota_url[320] = {0};
        size_t length = 0;
        snprintf(url, sizeof(url), "%sfirmware/index.json", UPDATE_PULL_BASE_URL);
        if (body && chunk) {
            const PullFailure fetched = client.fetch(url, reinterpret_cast<char*>(body), kSmokeIndexCap, &length);
            LOG_BLE("[PULL][SMOKE] run %d index: %s, %u bytes\n", run, fetched == PullFailure::NONE ? "ok" : pull_failure_text(fetched),
                    (unsigned)length);
            if (fetched == PullFailure::NONE && find_bin_urls(reinterpret_cast<char*>(body), raw_url, ota_url, sizeof(raw_url))) {
                uint32_t bytes = 0, ms = 0;
                ok = discard_download(client, raw_url, chunk, UPDATE_PULL_CHUNK_BYTES, &bytes, &ms);
                if (!ok) ok = discard_download(client, ota_url, chunk, UPDATE_PULL_CHUNK_BYTES, &bytes, &ms);
                LOG_BLE("[PULL][SMOKE] run %d download: %s, %lu bytes in %lu ms (%lu KB/s)\n", run, ok ? "ok" : "FAILED",
                        (unsigned long)bytes, (unsigned long)ms, ms ? (unsigned long)(bytes / 1024.0f / (ms / 1000.0f)) : 0UL);
            }
        }
        heap_caps_free(body);
        heap_caps_free(chunk);
        LOG_BLE("[PULL][SMOKE] run %d TLS allocations in scope: PSRAM %u, internal fallback %u\n", run,
                tls_psram_scope_psram_allocs(), tls_psram_scope_fallback_allocs());
    }
    log_heap("[SMOKE] after TLS");
    return ok;
}

void run_smoke() {
    if (!wait_until(wifi_up, kWifiWaitMs)) {
        LOG_BLE("[PULL][SMOKE] WiFi did not come up, giving up\n");
        return;
    }
    int passed = 0;
    for (int run = 1; run <= kSmokeRuns; run++) {
        if (smoke_once(run)) passed++;
        vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    }
    log_heap("[SMOKE] 10 s after the last run");
    LOG_BLE("[PULL][SMOKE] RESULT %d of %d runs ok\n", passed, kSmokeRuns);
}
#endif  // DEBUG_PULL_AUTORUN != 2

void autorun_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(DEBUG_PULL_AUTORUN_DELAY_MS));
#if DEBUG_PULL_AUTORUN == 2
    run_smoke();
#else
    run_pull();
#endif
    LOG_BLE("[PULL][AUTORUN] task stack high-water: %u bytes free\n",
            (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    vTaskDelete(nullptr);
}

}  // namespace

void update_pull_autorun_start() {
    const BaseType_t ok = xTaskCreatePinnedToCore(autorun_task, "PullAutorun", UPDATE_PULL_TASK_STACK, nullptr, 1, nullptr, 0);
    LOG_BLE("[PULL][AUTORUN] mode %d, task %s\n", DEBUG_PULL_AUTORUN, ok == pdPASS ? "created" : "create FAILED");
}

#endif  // DEBUG_PULL_AUTORUN

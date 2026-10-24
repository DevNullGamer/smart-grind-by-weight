#include "release_client.h"
#include "../config/constants.h"
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <stdio.h>
#include <string.h>

PullFailure ReleaseClient::open(const char* url, int64_t expected_length, int64_t max_length) {
    close();
    if (!pull_url_scheme_allowed(url, DEBUG_PULL_ALLOW_HTTP != 0)) {
        LOG_BLE("[PULL] refused URL (not https)\n");
        return PullFailure::INTERNAL;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = UPDATE_PULL_READ_TIMEOUT_MS;  // per socket operation, so a stalled server aborts the job
    config.disable_auto_redirect = true;              // a redirect would leave the pinned base URL
    config.keep_alive_enable = false;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    if (strncmp(url, "https://", 8) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        LOG_BLE("[PULL] http client init failed (free internal %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return PullFailure::LOW_MEMORY;
    }
    handle_ = client;

    const esp_err_t opened = esp_http_client_open(client, 0);
    if (opened != ESP_OK) {
        // Non-secret facts only: error name, errno, mbedTLS error and certificate verification flags
        int tls_code = 0, tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        LOG_BLE("[PULL] connect failed: %s (errno %d, tls 0x%X, cert flags 0x%X)\n", esp_err_to_name(opened),
                esp_http_client_get_errno(client), (unsigned)tls_code, (unsigned)tls_flags);
        close();
        return PullFailure::NO_CONNECTION;
    }

    const int64_t fetched = esp_http_client_fetch_headers(client);
    status_ = esp_http_client_get_status_code(client);
    if (fetched < 0) {  // timed out or failed before the headers were complete
        LOG_BLE("[PULL] no reply headers (%d)\n", (int)fetched);
        close();
        return PullFailure::NO_CONNECTION;
    }
    content_length_ = fetched;

    const PullHttpCheck check = pull_check_response(status_, content_length_, expected_length, max_length);
    if (check != PullHttpCheck::OK) {
        LOG_BLE("[PULL] reply refused: status %d, content-length %lld (expected %lld, max %lld)\n", status_,
                (long long)content_length_, (long long)expected_length, (long long)max_length);
        const int refused_status = status_;  // close() resets status_
        close();
        return pull_failure_for_http_check(check, refused_status);
    }
    LOG_BLE("[HEAP] connection established (status %d, %lld bytes): internal free %u, largest block %u, lowest ever %u, "
            "PSRAM free %u\n",
            status_, (long long)content_length_, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return PullFailure::NONE;
}

int ReleaseClient::read(uint8_t* buffer, size_t length) {
    if (!handle_) return -1;
    return esp_http_client_read(static_cast<esp_http_client_handle_t>(handle_), reinterpret_cast<char*>(buffer),
                                static_cast<int>(length));
}

PullFailure ReleaseClient::fetch(const char* url, char* buffer, size_t cap, size_t* out_length) {
    if (out_length) *out_length = 0;
    if (!buffer || cap < 2) return PullFailure::INTERNAL;
    const PullFailure opened = open(url, -1, static_cast<int64_t>(cap) - 1);
    if (opened != PullFailure::NONE) return opened;

    const uint32_t total = static_cast<uint32_t>(content_length_);
    uint32_t received = 0;
    while (received < total) {
        const int n = read(reinterpret_cast<uint8_t*>(buffer) + received, total - received);
        const PullReadOutcome outcome = pull_classify_read(n, received, total);
        if (outcome != PullReadOutcome::DATA) {  // DONE cannot happen while received < total
            const PullFailure failure = pull_failure_for_read(outcome);
            close();
            return failure == PullFailure::NONE ? PullFailure::INTERNAL : failure;
        }
        received += static_cast<uint32_t>(n);
    }
    buffer[received] = '\0';
    if (out_length) *out_length = received;
    close();
    return PullFailure::NONE;
}

PullFailure ReleaseClient::fetch_index(bool beta, char* buffer, size_t cap, size_t* out_length) {
    char url[UPDATE_PULL_URL_MAX];
    const int n = snprintf(url, sizeof(url), "%s%s", UPDATE_PULL_BASE_URL,
                           beta ? UPDATE_PULL_BETA_FILE : UPDATE_PULL_STABLE_FILE);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return PullFailure::INTERNAL;
    return fetch(url, buffer, cap, out_length);
}

void ReleaseClient::close() {
    if (!handle_) return;
    esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(handle_);
    handle_ = nullptr;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    status_ = 0;
    content_length_ = -1;
}

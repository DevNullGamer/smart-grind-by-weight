#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pull_logic.h"

/**
 * ReleaseClient - thin esp_http_client wrapper for the pull update (GET only, one request at a time).
 *
 * Rules: https only (http:// only in dev builds with DEBUG_PULL_ALLOW_HTTP), certificates verified against the
 * ESP-IDF bundle (esp_crt_bundle_attach), no automatic redirects (a 3xx is a failed status), status must be 200,
 * Content-Length must be present (chunked replies are refused) and, when the caller knows the size, equal to it,
 * keep-alive off, UPDATE_PULL_READ_TIMEOUT_MS per socket read. The host is never taken from a server reply: callers
 * build URLs from UPDATE_PULL_BASE_URL. Certificate dates are not checked (no system time, see DESIGN-NOTES).
 *
 * Not thread safe; run it inside a TlsPsramScope so the TLS buffers come from PSRAM. close() is idempotent.
 */
class ReleaseClient {
public:
    ReleaseClient() = default;
    ~ReleaseClient() { close(); }
    ReleaseClient(const ReleaseClient&) = delete;
    ReleaseClient& operator=(const ReleaseClient&) = delete;

    // Connects, sends the GET and checks the reply headers. On OK the body (content_length() bytes) can be read.
    // expected_length < 0: any length up to max_length. Returns PullFailure::NONE on success.
    PullFailure open(const char* url, int64_t expected_length, int64_t max_length);
    // Raw esp_http_client_read() result (see pull_classify_read); only valid after a successful open()
    int read(uint8_t* buffer, size_t length);
    // Fetches a small document completely into buffer (NUL-terminated, capacity cap, length in *out_length).
    PullFailure fetch(const char* url, char* buffer, size_t cap, size_t* out_length);
    // Builds <base>/<update-stable|beta>.json and fetches it
    PullFailure fetch_index(bool beta, char* buffer, size_t cap, size_t* out_length);
    void close();

    int status() const { return status_; }
    int64_t content_length() const { return content_length_; }

private:
    void* handle_ = nullptr;  // esp_http_client_handle_t
    int status_ = 0;
    int64_t content_length_ = -1;
};

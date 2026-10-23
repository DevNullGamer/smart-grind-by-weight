#include "update_server.h"
#include "wifi_manager.h"
#include "wifi_credentials.h"
#include "update_approval_gate.h"
#include "update_status.h"
#include "../system/ota_marker.h"
#include "../config/constants.h"
#include "../hardware/board_detect.h"
#include "../hardware/hardware_manager.h"
#include "../controllers/grind_controller.h"
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <mbedtls/platform_util.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

UpdateServer update_server;
extern HardwareManager hardware_manager;
extern GrindController grind_controller;

namespace {

// Frees a heap_caps buffer when the request ends, whatever path it takes
struct ScopedBuffer {
    uint8_t* data = nullptr;
    ~ScopedBuffer() { heap_caps_free(data); }
};

bool get_header(httpd_req_t* req, const char* name, char* out, size_t capacity) {
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length == 0 || length >= capacity) return false;
    return httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK;
}

// Sends a JSON reply. Error replies return ESP_FAIL afterwards so the server closes the connection (an unread
// request body must not be parsed as the next request).
void send_json(httpd_req_t* req, const char* status, const char* json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Lingering close: after an early error reply the client may still be sending the body. Closing now would make
// the TCP stack answer with a reset that can destroy the reply before the client reads it, so keep discarding
// the body for a bounded amount of data and time first.
void discard_request_body(httpd_req_t* req) {
    if (req->content_len == 0) return;
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct timeval timeout = {1, 0};  // never wait long for a client that stopped sending
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    char scratch[256];
    size_t discarded = 0;
    const uint32_t started = millis();
    while (discarded < UPDATE_LINGER_MAX_BYTES && millis() - started < UPDATE_LINGER_MAX_MS) {
        const int n = httpd_req_recv(req, scratch, sizeof(scratch));
        if (n <= 0) break;  // body complete, closed or no more data
        discarded += static_cast<size_t>(n);
    }
    static bool stack_logged = false;  // once is enough to see how deep the HTTP task got on this path
    if (!stack_logged) {
        stack_logged = true;
        LOG_BLE("UPDATE: HTTP task stack %u bytes free after a lingering close\n",
                (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
}

// linger=false for replies to unauthenticated clients (401/429): they get no free time on the single HTTP task
esp_err_t send_error(httpd_req_t* req, const char* status, const char* error, const char* reason, bool linger = true) {
    char json[200];
    snprintf(json, sizeof(json), "{\"error\":\"%s\",\"reason\":\"%s\"}", error, reason);
    send_json(req, status, json);
    if (linger) discard_request_body(req);
    return ESP_FAIL;
}

enum class AuthResult : uint8_t { OK, LOCKED, DENIED };

// Bearer token check with the failure lockout. Called on the HTTP task only. matches(token) decides whether the
// token is right (NVS for the begin request, the RAM copy once a session runs). On success token_out (if given,
// WIFI_TOKEN_LEN + 1 bytes) receives the accepted token; the caller zeroizes it.
template <typename Matcher>
AuthResult authenticate(httpd_req_t* req, AuthLockout* lockout, Matcher matches, char* token_out = nullptr) {
    const uint32_t now = millis();
    if (auth_lockout_active(lockout, now)) return AuthResult::LOCKED;

    char header[UPDATE_BEARER_HEADER_MAX];
    char token[WIFI_TOKEN_LEN + 1];
    const bool parsed = get_header(req, "Authorization", header, sizeof(header)) && http_parse_bearer(header, token);
    const bool ok = parsed && matches(token);
    if (ok && token_out) memcpy(token_out, token, sizeof(token));
    mbedtls_platform_zeroize(token, sizeof(token));
    mbedtls_platform_zeroize(header, sizeof(header));
    if (ok) {
        auth_lockout_record_success(lockout);
        return AuthResult::OK;
    }
    if (auth_lockout_record_failure(lockout, now)) {
        LOG_BLE("UPDATE: 3 bad tokens, authentication locked for %lu s\n",
                (unsigned long)(auth_lockout_remaining_ms(lockout, now) / 1000));
    }
    return AuthResult::DENIED;
}

esp_err_t reject_auth(httpd_req_t* req, AuthResult result, AuthLockout* lockout) {
    if (result == AuthResult::LOCKED) {
        const uint32_t retry_s = (auth_lockout_remaining_ms(lockout, millis()) + 999) / 1000;
        char retry[12];
        snprintf(retry, sizeof(retry), "%lu", (unsigned long)retry_s);
        httpd_resp_set_hdr(req, "Retry-After", retry);
        char json[96];
        snprintf(json, sizeof(json), "{\"error\":\"locked\",\"reason\":\"Too many bad tokens\",\"retry_after_s\":%lu}",
                 (unsigned long)retry_s);
        send_json(req, "429 Too Many Requests", json);
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    return send_error(req, "401 Unauthorized", "unauthorized", "Missing or wrong token", false);
}

const char* status_for_sink_error(SinkError error) {
    switch (error) {
        case SinkError::BAD_ARGUMENT:
        case SinkError::TOO_MUCH_DATA: return "400 Bad Request";
        case SinkError::BAD_SIZE: return "413 Payload Too Large";
        case SinkError::ALREADY_ACTIVE: return "409 Conflict";
        case SinkError::NOT_ACTIVE: return "410 Gone";
        case SinkError::BAD_HEADER:
        case SinkError::SIZE_MISMATCH:
        case SinkError::SHA_MISMATCH:
        case SinkError::IMAGE_INVALID: return "422 Unprocessable Entity";
        default: return "500 Internal Server Error";
    }
}

const char* status_for_parse_error(BeginParseResult result) {
    switch (result) {
        case BeginParseResult::SIZE_TOO_LARGE:
        case BeginParseResult::BODY_TOO_LARGE: return "413 Payload Too Large";
        default: return "400 Bad Request";
    }
}

bool grinder_busy() {
    return grind_controller.is_grind_running() || hardware_manager.get_autotune_controller()->is_active() ||
           hardware_manager.get_grinder()->is_grinding();
}

// Dotted IPv4 of the client (empty if unknown or not IPv4)
void get_peer_ip(httpd_req_t* req, char* out, size_t out_len) {
    out[0] = '\0';
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;
    struct sockaddr_in6 addr = {};  // large enough for either family
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) return;
    if (reinterpret_cast<struct sockaddr*>(&addr)->sa_family != AF_INET) return;
    const struct sockaddr_in* v4 = reinterpret_cast<const struct sockaddr_in*>(&addr);
    inet_ntop(AF_INET, &v4->sin_addr, out, out_len);
}

// esp_http_server handlers are plain C callbacks: forward to the singleton
esp_err_t info_trampoline(httpd_req_t* req) { return update_server.handle_info(req); }
esp_err_t begin_trampoline(httpd_req_t* req) { return update_server.handle_begin(req); }
esp_err_t data_trampoline(httpd_req_t* req) { return update_server.handle_data(req); }

}  // namespace

//------------------------------------------------------------------------------
// Lifecycle
//------------------------------------------------------------------------------
void UpdateServer::begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    ready_ = (mutex_ != nullptr);
    if (!policy_) policy_ = production_accept_policy();
}

bool UpdateServer::lock(TickType_t wait) {
    return mutex_ && xSemaphoreTake(mutex_, wait) == pdTRUE;
}

void UpdateServer::unlock() {
    if (mutex_) xSemaphoreGive(mutex_);
}

// Caller holds mutex_. Drops the transfer, gives the grinder back, removes the marker and publishes FAILED.
void UpdateServer::abort_session_locked(const char* reason) {
    sink_.abort();
    guard_.release();
    ota_marker_clear();  // tasks run again: the expected-version marker of the failed update goes
    UpdateStatus::set_failed(reason);
    end_session_locked();
}

// Caller holds mutex_. Forgets the session id and the RAM copy of the token.
void UpdateServer::end_session_locked() {
    session_active_ = false;
    memset(session_, 0, sizeof(session_));
    mbedtls_platform_zeroize(token_cache_, sizeof(token_cache_));
}

void UpdateServer::start_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = UPDATE_HTTP_PORT;
    config.max_open_sockets = UPDATE_HTTP_MAX_OPEN_SOCKETS;
    config.stack_size = UPDATE_HTTP_STACK_SIZE;
    config.recv_wait_timeout = UPDATE_HTTP_RECV_TIMEOUT_S;
    config.send_wait_timeout = UPDATE_HTTP_RECV_TIMEOUT_S;
    config.lru_purge_enable = true;

    httpd_handle_t handle = nullptr;
    if (httpd_start(&handle, &config) != ESP_OK) {
        if (!start_failure_logged_) {
            LOG_BLE("UPDATE: HTTP server start failed (free internal heap %u)\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            start_failure_logged_ = true;
        }
        return;
    }
    start_failure_logged_ = false;

    struct Route { const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); };
    const Route routes[] = {
        {"/api/info", HTTP_GET, info_trampoline},
        {"/api/update/begin", HTTP_POST, begin_trampoline},
        {"/api/update/data", HTTP_PUT, data_trampoline},
    };
    for (const Route& route : routes) {
        httpd_uri_t uri = {};
        uri.uri = route.uri;
        uri.method = route.method;
        uri.handler = route.handler;
        uri.user_ctx = nullptr;
        httpd_register_uri_handler(handle, &uri);
    }

    server_ = handle;
    wifi_manager.register_http_service();
    LOG_BLE("UPDATE: push endpoint listening on port %d (free internal heap %u, Bluetooth task stack %u bytes free at its low point)\n",
            UPDATE_HTTP_PORT, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    if (!wifi_manager.accept_required()) {
        LOG_BLE("UPDATE: WARNING Confirm is off, any client with the token can update without a tap on the grinder\n");
    }
}

void UpdateServer::stop_server() {
    if (!server_) return;
    httpd_stop(static_cast<httpd_handle_t>(server_));
    server_ = nullptr;
    wifi_manager.unregister_http_service();
    LOG_BLE("UPDATE: push endpoint stopped\n");
}

void UpdateServer::tick() {
    if (!ready_) return;
    const uint32_t now = millis();

    if (restart_pending_ && static_cast<int32_t>(now - restart_at_ms_) >= 0) {
        LOG_BLE("UPDATE: restarting into the new firmware\n");
        Serial.flush();
        esp_restart();
    }

    // Inactivity: a request that is running right now counts as activity, so skip the round if we cannot lock
    bool busy = true;
    if (lock(0)) {
        if (session_active_ && !installed_ && guard_.check_inactivity_timeout()) {
            sink_.abort();  // check_inactivity_timeout() already released the guard
            ota_marker_clear();
            UpdateStatus::set_failed("Update timed out");
            end_session_locked();
            LOG_BLE("UPDATE: transfer timed out, aborted\n");
        }
        busy = session_active_ || installed_;
        unlock();
    }
    // While an update owns the flash and the tasks, stay away from NVS (a flash operation could block it)
    if (busy || UpdateGuard::is_held()) return;

    if (now - last_gate_check_ms_ < UPDATE_GATE_CHECK_INTERVAL_MS) return;
    last_gate_check_ms_ = now;

    const bool wanted = wifi_manager.is_connected() && wifi_manager.push_enabled();
    if (wanted && !server_) {
        start_server();
    } else if (!wanted && server_) {
        // Never pull the server out from under a running transfer; the request ends on its own. A handler that
        // waits for the user must not be stopped either (httpd_stop would wait for it): withdraw the prompt, the
        // handler returns, and the server stops on the next round.
        if (update_approval.in_use()) {
            if (update_approval.cancel()) LOG_BLE("UPDATE: gate closed, pending prompt withdrawn\n");
            return;
        }
        bool idle = false;
        if (lock(0)) {
            idle = !session_active_ && !installed_;
            unlock();
        }
        if (idle) stop_server();
    }
}

//------------------------------------------------------------------------------
// GET /api/info
//------------------------------------------------------------------------------
int UpdateServer::handle_info(httpd_req* req) {
    const bool busy = UpdateGuard::is_held() || grinder_busy();
    char json[288];
    snprintf(json, sizeof(json),
             "{\"name\":\"%s\",\"version\":\"%s\",\"build\":%d,\"board\":\"%s\",\"busy\":%s,\"confirm\":%s}",
             wifi_manager.hostname(), BUILD_FIRMWARE_VERSION, BUILD_NUMBER, board().name(), busy ? "true" : "false",
             wifi_manager.accept_required() ? "true" : "false");
    send_json(req, "200 OK", json);
    return ESP_OK;
}

//------------------------------------------------------------------------------
// POST /api/update/begin
//------------------------------------------------------------------------------
int UpdateServer::handle_begin(httpd_req* req) {
    // An update is already running or finishing: answer before authenticating (no NVS read, no lockout count)
    bool already_busy = UpdateGuard::is_held() || update_approval.in_use();
    if (lock(portMAX_DELAY)) {
        already_busy = already_busy || session_active_ || installed_;
        unlock();
    }
    if (already_busy) {
        return send_error(req, "409 Conflict", "busy", UpdateGuard::result_text(UpdateGuard::Result::ALREADY_HELD));
    }

    // The token is checked against NVS here, before the update takes over the flash; the data PUT uses a RAM copy
    char accepted_token[WIFI_TOKEN_LEN + 1] = {0};
    struct TokenWiper {
        char* data;
        ~TokenWiper() { mbedtls_platform_zeroize(data, WIFI_TOKEN_LEN + 1); }
    } wiper{accepted_token};
    const AuthResult auth = authenticate(
        req, &lockout_, [](const char* token) { return wifi_manager.token_matches(token); }, accepted_token);
    if (auth != AuthResult::OK) return reject_auth(req, auth, &lockout_);

    const size_t content_len = req->content_len;
    if (content_len == 0) return send_error(req, "400 Bad Request", "bad_request", begin_parse_result_text(BeginParseResult::BODY_EMPTY));
    if (content_len > UPDATE_BEGIN_BODY_MAX) {
        return send_error(req, "413 Payload Too Large", "bad_request", begin_parse_result_text(BeginParseResult::BODY_TOO_LARGE));
    }
    char body[UPDATE_BEGIN_BODY_MAX];
    size_t got = 0;
    while (got < content_len) {
        const int n = httpd_req_recv(req, body + got, content_len - got);
        if (n <= 0) return ESP_FAIL;  // timeout or closed: nothing to answer
        got += static_cast<size_t>(n);
    }

    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    if (!slot) return send_error(req, "500 Internal Server Error", "no_slot", "No update slot available");

    UpdateBeginRequest request;
    const BeginParseResult parsed = http_parse_begin_request(body, content_len, slot->size, &request);
    if (parsed != BeginParseResult::OK) {
        return send_error(req, status_for_parse_error(parsed), "bad_request", begin_parse_result_text(parsed));
    }

    // A busy grinder is refused before the user is bothered with a prompt (the guard checks again later)
    if (grinder_busy()) {
        return send_error(req, "409 Conflict", "busy", UpdateGuard::result_text(UpdateGuard::Result::GRINDER_BUSY));
    }

    char peer_ip[UPDATE_PEER_IP_BUF];
    get_peer_ip(req, peer_ip, sizeof(peer_ip));
    const AcceptDecision decision = policy_->decide(request, peer_ip);
    if (decision != AcceptDecision::ACCEPT) {
        const AcceptReply reply = accept_decision_reply(decision, update_approval.backoff_remaining_ms());
        LOG_BLE("UPDATE: begin refused: %s\n", reply.error);
        if (reply.retry_after_s > 0) {
            char retry[12];
            snprintf(retry, sizeof(retry), "%lu", (unsigned long)reply.retry_after_s);
            httpd_resp_set_hdr(req, "Retry-After", retry);
            char json[128];
            snprintf(json, sizeof(json), "{\"error\":\"%s\",\"reason\":\"%s\",\"retry_after_s\":%lu}", reply.error,
                     reply.reason, (unsigned long)reply.retry_after_s);
            send_json(req, reply.status, json);
            discard_request_body(req);
            return ESP_FAIL;
        }
        return send_error(req, reply.status, reply.error, reply.reason);
    }

    // Past this point every refusal must also drop the "waiting for approval" status
    auto refuse = [&](const char* status, const char* error, const char* reason) {
        UpdateStatus::clear_awaiting();
        return send_error(req, status, error, reason);
    };

    if (!lock(portMAX_DELAY)) return refuse("503 Service Unavailable", "internal", "Update unavailable");
    if (session_active_ || installed_) {
        unlock();
        return refuse("409 Conflict", "busy", UpdateGuard::result_text(UpdateGuard::Result::ALREADY_HELD));
    }
    if (guard_.acquire() != UpdateGuard::Result::OK) {
        const char* reason = guard_.refusal_reason();
        unlock();
        return refuse("409 Conflict", "busy", reason);
    }
    guard_.note_activity();
    // Expected-version marker for the post-reboot check. Only after the guard was granted (so a concurrent BLE
    // OTA's marker is never touched), like the BLE path does after suspending the tasks.
    const bool marker_stored = ota_marker_store(request.build, request.version);
    guard_.note_activity();
    const SinkError begun = sink_.begin(request.size, request.sha256);
    guard_.note_activity();
    if (begun != SinkError::OK) {
        const char* reason = sink_.reason();
        guard_.release();
        ota_marker_clear();  // unconditional: also drops a stale marker if storing the new one failed half-way
        unlock();
        return refuse(status_for_sink_error(begun), "rejected", reason);
    }
    UpdateStatus::set_receiving(UpdateSource::WIFI_PUSH, request.size);
    session_id_from_random(esp_random(), session_);
    session_active_ = true;
    announced_size_ = request.size;
    memcpy(token_cache_, accepted_token, sizeof(token_cache_));  // lets the data PUT authenticate without NVS
    char json[48];
    snprintf(json, sizeof(json), "{\"session\":\"%s\"}", session_);
    unlock();

    LOG_BLE("UPDATE: begin accepted, %lu bytes, build %ld, stack free %u\n", (unsigned long)request.size,
            (long)request.build, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    send_json(req, "200 OK", json);
    return ESP_OK;
}

//------------------------------------------------------------------------------
// PUT /api/update/data
//------------------------------------------------------------------------------
int UpdateServer::handle_data(httpd_req* req) {
    // No session means no RAM token to check against: answer 403 (not 401, and no lockout count) like any
    // other unknown or expired session
    bool have_token_copy = false;
    if (lock(portMAX_DELAY)) {
        have_token_copy = token_cache_[0] != '\0';
        unlock();
    }
    if (!have_token_copy) return send_error(req, "403 Forbidden", "session", "Unknown or expired session");

    // No NVS access while the update owns the flash: compare against the RAM copy taken at begin
    const AuthResult auth = authenticate(req, &lockout_, [this](const char* token) {
        bool equal = false;
        if (lock(portMAX_DELAY)) {
            equal = token_cache_[0] != '\0' && wifi_token_equal(token_cache_, token);
            unlock();
        }
        return equal;
    });
    if (auth != AuthResult::OK) return reject_auth(req, auth, &lockout_);

    // The session of the caller must be the one that is running; a stray request never aborts someone else's transfer
    char session[UPDATE_SESSION_ID_LEN + 1];
    const bool has_session = get_header(req, "X-Session", session, sizeof(session));
    uint32_t announced = 0;
    bool session_ok = false;
    if (has_session && lock(portMAX_DELAY)) {
        session_ok = session_active_ && session_id_equal(session_, session);
        announced = announced_size_;
        unlock();
    }
    if (!session_ok) return send_error(req, "403 Forbidden", "session", "Unknown or expired session");

    auto fail = [&](const char* status, const char* error, const char* reason) {
        if (lock(portMAX_DELAY)) {
            if (session_active_) abort_session_locked(reason);
            unlock();
        }
        LOG_BLE("UPDATE: failed: %s\n", reason);
        return send_error(req, status, error, reason);
    };

    const UploadLengthResult length_check = upload_length_check(req->content_len, announced);
    if (length_check == UploadLengthResult::MISSING) return fail("411 Length Required", "length", "Content-Length required");
    if (length_check == UploadLengthResult::MISMATCH) return fail("400 Bad Request", "length", "Content-Length differs from announced size");

    ScopedBuffer buffer;
    buffer.data = static_cast<uint8_t*>(heap_caps_malloc(UPDATE_HTTP_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer.data) buffer.data = static_cast<uint8_t*>(heap_caps_malloc(UPDATE_HTTP_CHUNK_BYTES, MALLOC_CAP_8BIT));
    if (!buffer.data) return fail("503 Service Unavailable", "memory", "Update failed: out of memory");

    LOG_BLE("UPDATE: receiving %lu bytes\n", (unsigned long)announced);
    size_t remaining = req->content_len;
    uint32_t received = 0;
    uint32_t next_log = UPDATE_PROGRESS_LOG_STEP_BYTES;
    while (remaining > 0) {
        const size_t want = remaining < UPDATE_HTTP_CHUNK_BYTES ? remaining : UPDATE_HTTP_CHUNK_BYTES;
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(buffer.data), want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) return fail("408 Request Timeout", "timeout", "Update aborted: upload stalled");
        if (n <= 0) return fail("400 Bad Request", "truncated", "Update aborted: connection lost");

        if (!lock(portMAX_DELAY)) return fail("503 Service Unavailable", "internal", "Update unavailable");
        if (!session_active_) {  // the inactivity tick ended it while we were waiting for data
            unlock();
            return send_error(req, "410 Gone", "session", "Update aborted: timed out");
        }
        const SinkError written = sink_.write(buffer.data, static_cast<size_t>(n));
        if (written != SinkError::OK) {
            const char* reason = sink_.reason();
            abort_session_locked(reason);
            unlock();
            LOG_BLE("UPDATE: failed: %s\n", reason);
            return send_error(req, status_for_sink_error(written), "rejected", reason);
        }
        guard_.note_activity();
        unlock();

        remaining -= static_cast<size_t>(n);
        received += static_cast<uint32_t>(n);
        UpdateStatus::set_received(received);
        if (received >= next_log) {
            LOG_BLE("UPDATE: received %lu / %lu bytes\n", (unsigned long)received, (unsigned long)announced);
            next_log += UPDATE_PROGRESS_LOG_STEP_BYTES;
        }
    }

    if (!lock(portMAX_DELAY)) return fail("503 Service Unavailable", "internal", "Update unavailable");
    if (!session_active_) {
        unlock();
        return send_error(req, "410 Gone", "session", "Update aborted: timed out");
    }
    LOG_BLE("UPDATE: all %lu bytes received, verifying\n", (unsigned long)received);
    UpdateStatus::set_verifying();
    const SinkError finished = sink_.finish();
    if (finished != SinkError::OK) {
        const char* reason = sink_.reason();
        abort_session_locked(reason);
        unlock();
        LOG_BLE("UPDATE: failed: %s\n", reason);
        return send_error(req, status_for_sink_error(finished), "rejected", reason);
    }
    // Installed: keep the guard (tasks stay suspended, grinder locked out) until the restart
    end_session_locked();  // wipes the session id and the token copy
    installed_ = true;
    UpdateStatus::set_installed();
    unlock();

    LOG_BLE("UPDATE: installed, stack free %u\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    send_json(req, "200 OK", "{\"status\":\"installed\"}");
    restart_at_ms_ = millis() + UPDATE_RESTART_DELAY_MS;
    restart_pending_ = true;
    return ESP_OK;
}

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "update_http_logic.h"
#include "../system/update_guard.h"
#include "../system/update_sink_image.h"

struct httpd_req;  // esp_http_server request, kept out of this header

/**
 * Decides whether an announced update may start (called from POST /api/update/begin after the token and the
 * request were validated, before the grinder is locked). The default policy accepts everything. A later UI
 * step plugs the on-device Accept prompt in here through UpdateServer::set_accept_policy().
 */
class UpdateAcceptPolicy {
public:
    virtual ~UpdateAcceptPolicy() = default;
    virtual bool accept(const UpdateBeginRequest& request) = 0;
};

/**
 * UpdateServer - plain-HTTP push endpoint for raw firmware images (port 80, esp_http_server).
 *
 * Runs only while WiFi is connected AND WiFiManager::push_enabled() is set; tick() starts/stops it (and the
 * mDNS "http" service) from the Bluetooth task loop. Contract:
 *   GET  /api/info                no auth; {"name","version","build","board","busy"}
 *   POST /api/update/begin        Bearer token; body {"size","sha256"[,"build","version"]};
 *                                 200 {"session":"<8hex>"}; errors carry {"error","reason"}
 *   PUT  /api/update/data         Bearer token + X-Session; Content-Length == announced size; raw image body;
 *                                 200 {"status":"installed"}, the device restarts ~800 ms later
 * One update at a time. Any error or inactivity aborts the sink and releases the UpdateGuard; after a
 * successful install the guard stays held until the restart. Tokens and session ids are never logged.
 *
 * Threading: the HTTP handlers run on the esp_http_server task, tick() on the Bluetooth task. mutex_ is the
 * single lock around EVERY ImageSink and UpdateGuard call and the session state; handlers take it per
 * chunk (never across a socket read), tick() only try-locks it.
 */
class UpdateServer {
public:
    // Creates the lock; call once from setup() (tick() does nothing before that).
    void begin();
    // Called periodically from the Bluetooth task loop.
    void tick();

    void set_accept_policy(UpdateAcceptPolicy* policy) { policy_ = policy ? policy : &default_policy_; }
    bool running() const { return server_ != nullptr; }

    // Handlers (public so the C trampolines in the .cpp can reach them)
    int handle_info(httpd_req* req);
    int handle_begin(httpd_req* req);
    int handle_data(httpd_req* req);

private:
    class AcceptAll : public UpdateAcceptPolicy {
    public:
        bool accept(const UpdateBeginRequest&) override { return true; }
    };

    volatile bool ready_ = false;
    SemaphoreHandle_t mutex_ = nullptr;
    void* server_ = nullptr;  // httpd_handle_t
    uint32_t last_gate_check_ms_ = 0;
    bool start_failure_logged_ = false;

    AcceptAll default_policy_;
    UpdateAcceptPolicy* policy_ = &default_policy_;

    // Session state, guarded by mutex_
    UpdateGuard guard_;
    ImageSink sink_;
    bool session_active_ = false;
    bool installed_ = false;
    char session_[UPDATE_SESSION_ID_LEN + 1] = {0};
    uint32_t announced_size_ = 0;

    // Lockout state: only touched by the HTTP task
    AuthLockout lockout_;

    volatile bool restart_pending_ = false;
    volatile uint32_t restart_at_ms_ = 0;

    void start_server();
    void stop_server();
    void abort_session_locked();
    bool lock(TickType_t wait);
    void unlock();
};

extern UpdateServer update_server;

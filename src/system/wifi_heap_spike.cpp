#include "wifi_heap_spike.h"
#include "../config/constants.h"

#if DEBUG_WIFI_HEAP_SPIKE

// Measurement-only spike for chg008 step 0. Never touches the grinder or motor.
// WiFi credentials come from include/spike_secrets.h (gitignored).

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_wifi.h>
#include "../bluetooth/manager.h"
#include "../hardware/board_detect.h"

#if __has_include("spike_secrets.h")
#include "spike_secrets.h"
#define SPIKE_HAVE_SECRETS 1
#else
#define SPIKE_HAVE_SECRETS 0
#endif

extern BluetoothManager g_bluetooth_manager;

namespace {

constexpr uint32_t kStartDelayMs = 30000;
constexpr uint32_t kIdleMs = 60000;
constexpr uint32_t kBleWindowMs = 30000;
constexpr uint32_t kWifiConnectTimeoutMs = 30000;
constexpr uint32_t kBleTimeoutMs = 30UL * 60UL * 1000UL;  // keep BLE up for the whole run
constexpr uint32_t kDownloadCapBytes = 3UL * 1024UL * 1024UL;
constexpr uint32_t kDownloadTimeLimitMs = 90000;
constexpr const char* kIndexUrl = "https://devnullgamer.github.io/smart-grind-by-weight/firmware/index.json";
constexpr const char* kSiteRoot = "https://devnullgamer.github.io/smart-grind-by-weight/";  // index paths are relative to this
constexpr uint32_t kMinValidDownloadBytes = 100 * 1024;
constexpr uint32_t kBleDisconnectWaitMs = 120000;
constexpr int kGateIdleFreeMin = 40 * 1024;
constexpr int kGateLargestMin = 24 * 1024;
constexpr int kGateTlsMinFree = 20 * 1024;
constexpr int kGateTeardownDelta = 4 * 1024;
constexpr uint32_t kE1IdleMs = 10000;
constexpr uint32_t kBleSettleMs = 3000;
constexpr uint32_t kFinalSettleMs = 20000;  // >= 20 s after the last TLS before the 'settled' measurement

struct HeapSnap {
    int free_b;
    int largest;
    int min_ever;
};

HeapSnap snap() {
    return {static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
            static_cast<int>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL))};
}

HeapSnap phase(const char* name) {
    HeapSnap s = snap();
    Serial.printf("[SPIKE] %s: free %d largest %d min %d\n", name, s.free_b, s.largest, s.min_ever);
    return s;
}

// Samples internal free/largest every 20 ms and keeps the lowest values seen. The sampler task itself
// costs ~3 KB of internal RAM (stack + TCB) while running, so PASS margins below 3-4 KB are marginal.
struct Sampler {
    volatile bool running = false;
    volatile bool stop = false;
    bool create_ok = true;  // false when the task could not be created: sampled minimums are meaningless
    volatile int min_free = INT32_MAX;
    volatile int min_largest = INT32_MAX;
    TaskHandle_t task = nullptr;

    static void run(void* arg) {
        Sampler* self = static_cast<Sampler*>(arg);
        while (!self->stop) {
            int f = static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            int l = static_cast<int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (f < self->min_free) self->min_free = f;
            if (l < self->min_largest) self->min_largest = l;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        self->running = false;
        self->task = nullptr;
        vTaskDelete(nullptr);
    }

    void reset() {
        min_free = INT32_MAX;
        min_largest = INT32_MAX;
    }

    void start() {
        reset();
        stop = false;
        running = true;
        create_ok = true;
        if (xTaskCreatePinnedToCore(run, "SpikeSampler", 3072, this, 6, &task, 1) != pdPASS) {
            running = false;
            create_ok = false;
            task = nullptr;
            Serial.println("[SPIKE] sampler task create FAILED: sampled minimums unavailable");
        }
    }

    void finish() {
        stop = true;
        while (running) vTaskDelay(pdMS_TO_TICKS(10));
    }
};

Sampler g_sampler;

// ---- WiFi connect diagnostics (non-secret facts only: never log SSID or password) ----
volatile int g_last_disc_reason = -1;

const char* disc_reason_name(int r) {
    switch (r) {
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        default: return "other";
    }
}

bool is_auth_reason(int r) {
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_ASSOC_FAIL;
}

const char* status_name(wl_status_t st) {
    switch (st) {
        case WL_NO_SHIELD: return "WL_NO_SHIELD";
        case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
        case WL_CONNECTED: return "WL_CONNECTED";
        case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED: return "WL_DISCONNECTED";
        default: return "unknown";
    }
}

const char* auth_name(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2-WPA3";
        default: return "other";
    }
}

void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        int r = info.wifi_sta_disconnected.reason;
        g_last_disc_reason = r;
        Serial.printf("[SPIKE] WiFi STA disconnected: reason %d (%s)\n", r, disc_reason_name(r));
    }
}

void scan_and_report() {
    Serial.println("[SPIKE] WiFi scan (blocking, max 10 s)");
    int n = WiFi.scanNetworks(false, true, false, 300, 0, nullptr, nullptr);
    if (n < 0) {
        Serial.printf("[SPIKE] WiFi scan FAILED (%d)\n", n);
        return;
    }
    int ghz24 = 0, hidden = 0;
    int found = -1;
    for (int i = 0; i < n; i++) {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 13) ghz24++;
        if (WiFi.SSID(i).length() == 0) hidden++;
        if (found < 0 && WiFi.SSID(i) == SPIKE_WIFI_SSID) found = i;
    }
    Serial.printf("[SPIKE] WiFi scan: %d APs total, %d on 2.4 GHz (ch 1-13), %d with hidden SSID\n", n, ghz24,
                  hidden);
    if (found >= 0) {
        Serial.printf("[SPIKE] WiFi scan: configured SSID present: yes, channel %d, RSSI %d, auth %s, hidden: no\n",
                      static_cast<int>(WiFi.channel(found)), static_cast<int>(WiFi.RSSI(found)),
                      auth_name(WiFi.encryptionType(found)));
    } else {
        Serial.println("[SPIKE] WiFi scan: configured SSID present: no (hidden APs counted above)");
    }
    WiFi.scanDelete();
}

// Waits for the connection, logging WiFi.status() every 2 s. Returns true when connected.
bool wait_for_connect(uint32_t timeout_ms) {
    uint32_t w0 = millis();
    uint32_t last_log = w0;
    while (WiFi.status() != WL_CONNECTED && millis() - w0 < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (millis() - last_log >= 2000) {
            last_log = millis();
            wl_status_t st = WiFi.status();
            Serial.printf("[SPIKE] WiFi status at %u ms: %d (%s)\n", static_cast<unsigned>(millis() - w0),
                          static_cast<int>(st), status_name(st));
        }
    }
    return WiFi.status() == WL_CONNECTED;
}

void teardown_step(const char* label) {
    HeapSnap b = snap();
    Serial.printf("[SPIKE] teardown before %s: free %d largest %d\n", label, b.free_b, b.largest);
}

void teardown_done(const char* label) {
    vTaskDelay(pdMS_TO_TICKS(500));
    HeapSnap a = snap();
    Serial.printf("[SPIKE] teardown after %s: free %d largest %d\n", label, a.free_b, a.largest);
}

void task_count(const char* label) {
    Serial.printf("[SPIKE] tasks %s: %u\n", label, static_cast<unsigned>(uxTaskGetNumberOfTasks()));
}

// vTaskList-style dump with stack high-water marks, to show tasks WiFi leaves behind.
void dump_tasks(const char* label) {
#if configUSE_TRACE_FACILITY
    UBaseType_t cap = uxTaskGetNumberOfTasks();
    TaskStatus_t* arr = static_cast<TaskStatus_t*>(heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    if (!arr) {
        Serial.printf("[SPIKE] task dump %s skipped (alloc failed)\n", label);
        return;
    }
    UBaseType_t n = uxTaskGetSystemState(arr, cap, nullptr);
    Serial.printf("[SPIKE] task dump %s: %u tasks (state R=run r=ready B=blocked S=suspended D=deleted)\n", label,
                  static_cast<unsigned>(n));
    const char* states = "RrBSD";
    for (UBaseType_t i = 0; i < n; i++) {
        int st = static_cast<int>(arr[i].eCurrentState);
        Serial.printf("[SPIKE] task %-16s state %c prio %u stack-hwm %u bytes\n", arr[i].pcTaskName,
                      (st >= 0 && st < 5) ? states[st] : '?', static_cast<unsigned>(arr[i].uxCurrentPriority),
                      static_cast<unsigned>(arr[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
    free(arr);
#else
    Serial.printf("[SPIKE] task dump %s unavailable (configUSE_TRACE_FACILITY off in this build)\n", label);
#endif
}

struct TlsResult {
    bool ran = false;
    int status = -1;
    int index_bytes = 0;
    int min_free_handshake = -1;
    int min_largest_handshake = -1;
    int largest_before = -1;
    bool download_valid = false;  // status 200 and >= kMinValidDownloadBytes
    bool sampler_ok = true;       // false: sampled minimums are not valid
};

struct DownloadResult {
    int status = -1;
    uint32_t bytes = 0;
    bool valid = false;
};

httpd_handle_t g_httpd = nullptr;

esp_err_t info_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"spike\":true}");
}

bool start_httpd() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 2;
    cfg.stack_size = 6144;
    if (httpd_start(&g_httpd, &cfg) != ESP_OK) {
        g_httpd = nullptr;
        return false;
    }
    httpd_uri_t uri = {};
    uri.uri = "/api/info";
    uri.method = HTTP_GET;
    uri.handler = info_handler;
    httpd_register_uri_handler(g_httpd, &uri);
    return true;
}

void stop_httpd() {
    if (g_httpd) {
        httpd_stop(g_httpd);
        g_httpd = nullptr;
    }
}

// Extracts the first "ota" path from the index (relative to the site root) and builds absolute URLs:
// the raw image (".bin", ~2.1 MB, preferred) and the web-ota asset (fallback).
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
    size_t n = static_cast<size_t>(end - start);
    if (n == 0 || n >= sizeof(path)) return false;
    memcpy(path, start, n);
    path[n] = '\0';
    const char* suffix = "-web-ota.bin";
    size_t sl = strlen(suffix);
    if (n < sl || strcmp(path + n - sl, suffix) != 0) return false;
    snprintf(ota_url, len, "%s%s", kSiteRoot, path);
    path[n - sl] = '\0';
    snprintf(raw_url, len, "%s%s.bin", kSiteRoot, path);
    return true;
}

// Streams one URL and discards the body (cap: kDownloadCapBytes / kDownloadTimeLimitMs).
DownloadResult stream_download(const char* tag, const esp_http_client_config_t& base, const char* url) {
    DownloadResult d;
    Serial.printf("[SPIKE] %s download: %s\n", tag, url);
    esp_http_client_config_t dcfg = base;
    dcfg.url = url;
    dcfg.buffer_size = 4096;
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    esp_http_client_handle_t dl = esp_http_client_init(&dcfg);
    uint32_t d0 = millis();
    if (dl && buf && esp_http_client_open(dl, 0) == ESP_OK) {
        esp_http_client_fetch_headers(dl);
        d.status = esp_http_client_get_status_code(dl);
        while (d.bytes < kDownloadCapBytes && millis() - d0 < kDownloadTimeLimitMs) {
            int n = esp_http_client_read(dl, reinterpret_cast<char*>(buf), 4096);
            if (n <= 0) break;
            d.bytes += static_cast<uint32_t>(n);
        }
    }
    uint32_t dt = millis() - d0;
    if (dl) {
        esp_http_client_close(dl);
        esp_http_client_cleanup(dl);
    }
    free(buf);
    d.valid = (d.status == 200 && d.bytes >= kMinValidDownloadBytes);
    Serial.printf("[SPIKE] %s download: status %d bytes %u in %u ms (%u KB/s)%s\n", tag, d.status,
                  static_cast<unsigned>(d.bytes), static_cast<unsigned>(dt),
                  dt ? static_cast<unsigned>(d.bytes / 1024.0f / (dt / 1000.0f)) : 0u,
                  d.valid ? "" : " DOWNLOAD INVALID");
    return d;
}

// One HTTPS pull: index.json, then a discarded streaming download of the first .bin it names.
TlsResult https_pull(const char* tag, bool with_download = true) {
    TlsResult r;
    r.ran = true;
    char name[64];

    snprintf(name, sizeof(name), "%s before TLS", tag);
    HeapSnap before = phase(name);
    r.largest_before = before.largest;

    g_sampler.start();
    r.sampler_ok = g_sampler.create_ok;

    esp_http_client_config_t cfg = {};
    cfg.url = kIndexUrl;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.keep_alive_enable = false;

    char* body = static_cast<char*>(heap_caps_calloc(1, 8193, MALLOC_CAP_SPIRAM));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    uint32_t t0 = millis();
    esp_err_t open_err = client ? esp_http_client_open(client, 0) : ESP_FAIL;
    if (open_err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        r.status = esp_http_client_get_status_code(client);
        int total = 0;
        while (body && total < 8192) {
            int n = esp_http_client_read(client, body + total, 8192 - total);
            if (n <= 0) break;
            total += n;
        }
        r.index_bytes = total;
    } else {
        HeapSnap hf = snap();
        Serial.printf("[SPIKE] %s: index open failed: %s (client %s), free %d largest %d\n", tag,
                      esp_err_to_name(open_err), client ? "ok" : "NULL", hf.free_b, hf.largest);
        if (client) {
            // Wraps esp_tls_get_and_clear_last_error() on the client's TLS error handle: code is the esp-tls /
            // mbedtls error, flags are the mbedtls cert-verify flags
            int tls_code = 0, tls_flags = 0;
            esp_err_t te = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
            Serial.printf("[SPIKE] %s: errno %d, tls last-error query %s, esp_tls code 0x%X, mbedtls/cert flags 0x%X\n",
                          tag, esp_http_client_get_errno(client), esp_err_to_name(te), static_cast<unsigned>(tls_code),
                          static_cast<unsigned>(tls_flags));
        }
    }
    uint32_t t_index = millis() - t0;
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    r.min_free_handshake = g_sampler.min_free;
    r.min_largest_handshake = g_sampler.min_largest;
    Serial.printf("[SPIKE] %s index: status %d bytes %d in %u ms, sampled min free %d min largest %d\n", tag,
                  r.status, r.index_bytes, static_cast<unsigned>(t_index), r.min_free_handshake,
                  r.min_largest_handshake);

    char raw_url[256] = {0};
    char ota_url[256] = {0};
    if (with_download && r.status == 200 && body && find_bin_urls(body, raw_url, ota_url, sizeof(raw_url))) {
        g_sampler.reset();
        DownloadResult d = stream_download(tag, cfg, raw_url);
        if (!d.valid) d = stream_download(tag, cfg, ota_url);
        r.download_valid = d.valid;
        Serial.printf("[SPIKE] %s download sampled min free %d min largest %d\n", tag,
                      static_cast<int>(g_sampler.min_free), static_cast<int>(g_sampler.min_largest));
        if (g_sampler.min_free < r.min_free_handshake) r.min_free_handshake = g_sampler.min_free;
        if (g_sampler.min_largest < r.min_largest_handshake) r.min_largest_handshake = g_sampler.min_largest;
    } else {
        if (with_download) {
            Serial.printf("[SPIKE] %s download skipped (no usable index) DOWNLOAD INVALID\n", tag);
        } else {
            Serial.printf("[SPIKE] %s download not requested (index-only pull)\n", tag);
        }
    }

    g_sampler.finish();
    free(body);
    snprintf(name, sizeof(name), "%s after TLS", tag);
    phase(name);
    return r;
}

#if SPIKE_HAVE_SECRETS
// E1: a second full WiFi cycle (BLE off for the index-only TLS) to tell one-time from per-cycle cost.
void run_e1_second_cycle(BluetoothManager& ble, const HeapSnap& c1_p0, const HeapSnap& c1_end) {
    Serial.println("[SPIKE] E1: second WiFi cycle, BLE off during TLS (index-only), compare with cycle 1");
    HeapSnap p0b = phase("E1 P0' baseline (BLE advertising)");
    task_count("E1 start");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    uint32_t w0 = millis();
    g_last_disc_reason = -1;
    WiFi.begin(SPIKE_WIFI_SSID, SPIKE_WIFI_PASSWORD);
    bool connected = wait_for_connect(kWifiConnectTimeoutMs);
    if (connected) {
        Serial.printf("[SPIKE] E1 WiFi connected in %u ms\n", static_cast<unsigned>(millis() - w0));
        phase("E1 WiFi connected");
        task_count("E1 after WiFi connect");
        vTaskDelay(pdMS_TO_TICKS(kE1IdleMs));
        phase("E1 idle 10 s");
        bool httpd_ok = start_httpd();
        bool mdns_ok = MDNS.begin("grindbyweight");
        if (mdns_ok) MDNS.addService("http", "tcp", 80);
        Serial.printf("[SPIKE] E1 httpd %s, mDNS %s\n", httpd_ok ? "ok" : "FAILED", mdns_ok ? "ok" : "FAILED");
        phase("E1 httpd+mDNS");
        task_count("E1 after httpd+mDNS");
        ble.disable();
        vTaskDelay(pdMS_TO_TICKS(500));
        phase("E1 BLE disabled");
        TlsResult t = https_pull("E1 (BLE off)", false);
        Serial.printf("[SPIKE] E1 TLS: status %d, sampled min free %d min largest %d%s\n", t.status,
                      t.min_free_handshake, t.min_largest_handshake, t.sampler_ok ? "" : " (sampler FAILED)");
        task_count("E1 after TLS");
    } else {
        Serial.println("[SPIKE] E1 WiFi connect FAILED, teardown only");
    }
    stop_httpd();
    MDNS.end();
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
    phase("E1 WiFi torn down");
    task_count("E1 after teardown");
    if (ble.is_enabled()) ble.disable();
    ble.enable(kBleTimeoutMs);
    HeapSnap end = phase("E1 after BLE disable/enable");
    task_count("E1 after BLE re-enable");
    Serial.printf("[SPIKE] E1 delta P0'-end (cycle 2 only): free %d largest %d | cycle 1 delta P0-P6: free %d "
                  "largest %d | cycle 2 vs cycle 1 start P0: free %d\n",
                  end.free_b - p0b.free_b, end.largest - p0b.largest, c1_end.free_b - c1_p0.free_b,
                  c1_end.largest - c1_p0.largest, p0b.free_b - c1_p0.free_b);
}
#endif

// E2: BLE disable/enable cycles with no WiFi, to measure BLE re-init drift on its own.
void run_e2_ble_control(BluetoothManager& ble) {
    Serial.println("[SPIKE] E2: BLE-only control, 2 disable/enable cycles, no WiFi");
    HeapSnap base = phase("E2 baseline");
    task_count("E2 baseline");
    for (int i = 1; i <= 2; i++) {
        if (ble.is_enabled()) ble.disable();
        ble.enable(kBleTimeoutMs);
        vTaskDelay(pdMS_TO_TICKS(kBleSettleMs));
        char name[32];
        snprintf(name, sizeof(name), "E2 after BLE cycle %d", i);
        HeapSnap s = phase(name);
        snprintf(name, sizeof(name), "E2 cycle %d", i);
        task_count(name);
        Serial.printf("[SPIKE] E2 cycle %d delta vs E2 baseline: free %d largest %d\n", i, s.free_b - base.free_b,
                      s.largest - base.largest);
    }
}

void spike_task(void*) {
    if (board().variant != BoardVariant::V2) {
        Serial.printf("[SPIKE] board is %s, not V2: spike not run\n", board().name());
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[SPIKE] waiting %u s before start (do not grind during the spike)\n",
                  static_cast<unsigned>(kStartDelayMs / 1000));
    vTaskDelay(pdMS_TO_TICKS(kStartDelayMs));

    BluetoothManager& ble = g_bluetooth_manager;
    // The 5 min boot timeout would switch BLE off mid-run: restart it with a long timeout
    if (ble.is_enabled()) ble.disable();
    ble.enable(kBleTimeoutMs);

    HeapSnap p0 = phase("P0 baseline (BLE advertising)");
    task_count("P0");
    bool ran_wifi = false;
    HeapSnap p4 = {-1, -1, -1};
    TlsResult tls_a, tls_a2, tls_b;
    bool tls_b_ran = false;
    int p5b_largest = -1;
    bool ble_seen = false;

#if !SPIKE_HAVE_SECRETS
    Serial.println("[SPIKE] SPIKE: create include/spike_secrets.h (WiFi phases skipped)");
#else
    // P1: WiFi up
    WiFi.persistent(false);
    WiFi.onEvent(on_wifi_event);
    phase("P1 before WiFi.mode(STA)");
    WiFi.mode(WIFI_STA);
    phase("P1 after WiFi.mode(STA)");
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    scan_and_report();
    phase("P1 after scan");
    uint32_t w0 = millis();
    g_last_disc_reason = -1;
    WiFi.begin(SPIKE_WIFI_SSID, SPIKE_WIFI_PASSWORD);
    phase("P1 after WiFi.begin");
    bool connected = wait_for_connect(kWifiConnectTimeoutMs);
    if (!connected) {
        int reason = g_last_disc_reason;
        Serial.printf("[SPIKE] P1 first attempt failed, status %s, last disconnect reason %d (%s)\n",
                      status_name(WiFi.status()), reason, disc_reason_name(reason));
        if (is_auth_reason(reason)) {
            Serial.println("[SPIKE] P1 retrying with setMinSecurity(WIFI_AUTH_WPA_PSK)");
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
            g_last_disc_reason = -1;
            w0 = millis();
            WiFi.begin(SPIKE_WIFI_SSID, SPIKE_WIFI_PASSWORD);
            connected = wait_for_connect(kWifiConnectTimeoutMs);
            if (!connected) {
                int r2 = g_last_disc_reason;
                Serial.printf("[SPIKE] P1 retry failed, status %s, last disconnect reason %d (%s)\n",
                              status_name(WiFi.status()), r2, disc_reason_name(r2));
            }
        }
    }
    if (connected) {
        ran_wifi = true;
        Serial.printf("[SPIKE] P1 WiFi connected in %u ms, RSSI %d\n", static_cast<unsigned>(millis() - w0),
                      static_cast<int>(WiFi.RSSI()));
        phase("P1 WiFi connected");
        task_count("after WiFi connect");

        // P2: idle
        vTaskDelay(pdMS_TO_TICKS(kIdleMs));
        phase("P2 idle 60 s");

        // P3: httpd + mDNS
        bool httpd_ok = start_httpd();
        bool mdns_ok = MDNS.begin("grindbyweight");
        if (mdns_ok) MDNS.addService("http", "tcp", 80);
        Serial.printf("[SPIKE] P3 httpd %s, mDNS %s\n", httpd_ok ? "ok" : "FAILED", mdns_ok ? "ok" : "FAILED");
        phase("P3 httpd+mDNS");
        task_count("after httpd+mDNS");

        // P4: BLE client window
        Serial.println("[SPIKE] P4: connect over BLE now (grinder.py info), 30 s window");
        for (uint32_t i = 0; i < kBleWindowMs / 1000; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (ble.is_connected() && !ble_seen) {
                ble_seen = true;
                phase("P4 BLE client connected");
            }
        }
        p4 = phase("P4 end of BLE window");
        Serial.printf("[SPIKE] P4 BLE client connected during window: %s\n", ble_seen ? "yes" : "no");
        // A client still connected would block the BLE disable below
        uint32_t wait_start = millis();
        while (ble.is_connected() && millis() - wait_start < kBleDisconnectWaitMs) {
            Serial.println("[SPIKE] P4: waiting for BLE client to disconnect");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (ble.is_connected()) {
            Serial.println("[SPIKE] P4: BLE client still connected after 120 s, continuing (BLE disable will drop it)");
        }

        // P5a: HTTPS pull with BLE on
        task_count("before P5a");
        tls_a = https_pull("P5a (BLE on)");
        // Repeat once (index-only) to separate first-TLS one-time init cost from the BLE-on cost
        tls_a2 = https_pull("P5a-repeat (BLE on)", false);
        Serial.printf("[SPIKE] P5a first vs repeat (BLE on): status %d/%d, sampled min free %d/%d, "
                      "min largest %d/%d\n",
                      tls_a.status, tls_a2.status, tls_a.min_free_handshake, tls_a2.min_free_handshake,
                      tls_a.min_largest_handshake, tls_a2.min_largest_handshake);
        task_count("after P5a");

        // P5b: HTTPS pull with BLE off
        ble.disable();
        vTaskDelay(pdMS_TO_TICKS(500));
        p5b_largest = phase("P5b BLE disabled").largest;
        tls_b = https_pull("P5b (BLE off)");
        tls_b_ran = true;
        task_count("after P5b");
    } else {
        Serial.println("[SPIKE] P1 WiFi connect FAILED (check include/spike_secrets.h)");
    }

    // P6: teardown
    stop_httpd();
    MDNS.end();
    teardown_step("WiFi.disconnect(true)");
    WiFi.disconnect(true);
    teardown_done("WiFi.disconnect(true)");
    teardown_step("WiFi.mode(WIFI_OFF)");
    WiFi.mode(WIFI_OFF);
    teardown_done("WiFi.mode(WIFI_OFF)");
    phase("P6 WiFi torn down (BLE state as left)");
    task_count("after teardown");
    dump_tasks("after first teardown");
    Serial.println("[SPIKE] P6: no fuller deinit step; Arduino already deinitialises WiFi "
                   "(esp_wifi_deinit returns 0x3001 ESP_ERR_WIFI_NOT_INIT)");
#endif

    if (ble.is_enabled()) ble.disable();
    ble.enable(kBleTimeoutMs);
    HeapSnap p6 = phase("P6 after BLE disable/enable");
    task_count("after BLE re-enable");
    Serial.printf("[SPIKE] P6 delta vs P0: free %d largest %d\n", p6.free_b - p0.free_b, p6.largest - p0.largest);

    // P7: gates
    const char* g1 = "SKIP";
    const char* g2 = "SKIP";
    const char* g3 = "SKIP";
    if (ran_wifi) {
        g1 = (p4.free_b >= kGateIdleFreeMin && p4.largest >= kGateLargestMin) ? "PASS" : "FAIL";
        if (!tls_b_ran || tls_b.status <= 0) {
            g2 = "FAIL (TLS did not complete)";
        } else if (!tls_b.sampler_ok) {
            g2 = "INCONCLUSIVE (sampler task create failed)";
        } else if (!tls_b.download_valid) {
            g2 = "INCONCLUSIVE (handshake ok, download invalid)";
        } else {
            g2 = (tls_b.min_free_handshake >= kGateTlsMinFree && p5b_largest >= kGateLargestMin) ? "PASS" : "FAIL";
        }
        g3 = ((p0.free_b - p6.free_b) <= kGateTeardownDelta) ? "PASS" : "FAIL";
    }
    Serial.printf("[SPIKE] RESULT G1 %s (idle+httpd+mDNS+BLE free %d largest %d; need >= %d/%d) | "
                  "G2 %s (BLE-off TLS min free %d largest before %d; need >= %d/%d) | "
                  "G3 %s (P0-P6 free %d; need <= %d) | P5a (BLE on) min free %d | BLE client seen: %s | "
                  "margins < 3-4 KB are marginal (sampler costs ~3 KB)\n",
                  g1, p4.free_b, p4.largest, kGateIdleFreeMin, kGateLargestMin, g2, tls_b.min_free_handshake,
                  p5b_largest, kGateTlsMinFree, kGateLargestMin, g3, p0.free_b - p6.free_b, kGateTeardownDelta,
                  tls_a.min_free_handshake, ble_seen ? "yes" : "no");

    // Experiments after the RESULT line so the sequence and gates above stay comparable with earlier runs
#if SPIKE_HAVE_SECRETS
    if (ran_wifi) run_e1_second_cycle(ble, p0, p6);
#endif
    run_e2_ble_control(ble);
    Serial.printf("[SPIKE] waiting %u s after the last TLS before the settled measurement\n",
                  static_cast<unsigned>(kFinalSettleMs / 1000));
    vTaskDelay(pdMS_TO_TICKS(kFinalSettleMs));
    HeapSnap settled = phase("SETTLED");
    task_count("settled");
    dump_tasks("settled");
    Serial.printf("[SPIKE] SETTLED delta vs P0: free %d largest %d\n", settled.free_b - p0.free_b,
                  settled.largest - p0.largest);
    Serial.printf("[SPIKE] spike task stack high-water: %u bytes free of 12288\n",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    vTaskDelete(nullptr);
}

}  // namespace

void wifi_heap_spike_start() {
    // 12 KB stack from internal RAM
    BaseType_t ok = xTaskCreatePinnedToCore(spike_task, "WifiSpike", 12288, nullptr, 3, nullptr, 1);
    Serial.printf("[SPIKE] task %s\n", ok == pdPASS ? "created" : "create FAILED");
}

#endif  // DEBUG_WIFI_HEAP_SPIKE

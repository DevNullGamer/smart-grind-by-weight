#include "update_status.h"
#include <Arduino.h>
#include <atomic>
#include <string.h>

namespace {

constexpr size_t kReasonLen = 48;

std::atomic<uint8_t> g_phase{static_cast<uint8_t>(UpdatePhase::IDLE)};
std::atomic<uint8_t> g_source{static_cast<uint8_t>(UpdateSource::NONE)};
std::atomic<uint32_t> g_received{0};
std::atomic<uint32_t> g_total{0};
std::atomic<uint32_t> g_revision{0};
char g_reason[kReasonLen] = {0};
portMUX_TYPE g_reason_mux = portMUX_INITIALIZER_UNLOCKED;

void set_phase(UpdatePhase next) {
    g_phase.store(static_cast<uint8_t>(next));
    g_revision.fetch_add(1);
}

}  // namespace

namespace UpdateStatus {

UpdatePhase phase() { return static_cast<UpdatePhase>(g_phase.load()); }
UpdateSource source() { return static_cast<UpdateSource>(g_source.load()); }
uint32_t received() { return g_received.load(); }
uint32_t total() { return g_total.load(); }
uint32_t revision() { return g_revision.load(); }

void reason(char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    portENTER_CRITICAL(&g_reason_mux);
    strncpy(out, g_reason, out_len - 1);
    out[out_len - 1] = '\0';
    portEXIT_CRITICAL(&g_reason_mux);
}

bool active() {
    const UpdatePhase p = phase();
    return p == UpdatePhase::RECEIVING || p == UpdatePhase::VERIFYING || p == UpdatePhase::INSTALLED;
}

void set_awaiting_approval() {
    portENTER_CRITICAL(&g_reason_mux);
    g_reason[0] = '\0';
    portEXIT_CRITICAL(&g_reason_mux);
    g_received.store(0);
    g_total.store(0);
    g_source.store(static_cast<uint8_t>(UpdateSource::NONE));
    set_phase(UpdatePhase::AWAITING_APPROVAL);
}

void set_receiving(UpdateSource source, uint32_t total_bytes) {
    portENTER_CRITICAL(&g_reason_mux);
    g_reason[0] = '\0';
    portEXIT_CRITICAL(&g_reason_mux);
    g_received.store(0);
    g_total.store(total_bytes);
    g_source.store(static_cast<uint8_t>(source));
    set_phase(UpdatePhase::RECEIVING);
}

void set_received(uint32_t received_bytes) { g_received.store(received_bytes); }

void set_verifying() { set_phase(UpdatePhase::VERIFYING); }

void set_installed() {
    g_received.store(g_total.load());
    set_phase(UpdatePhase::INSTALLED);
}

void set_failed(const char* reason_text) {
    portENTER_CRITICAL(&g_reason_mux);
    strncpy(g_reason, reason_text ? reason_text : "", kReasonLen - 1);
    g_reason[kReasonLen - 1] = '\0';
    portEXIT_CRITICAL(&g_reason_mux);
    set_phase(UpdatePhase::FAILED);
}

void clear_awaiting() {
    uint8_t expected = static_cast<uint8_t>(UpdatePhase::AWAITING_APPROVAL);
    if (g_phase.compare_exchange_strong(expected, static_cast<uint8_t>(UpdatePhase::IDLE))) g_revision.fetch_add(1);
}

void acknowledge() {
    uint8_t expected = static_cast<uint8_t>(UpdatePhase::FAILED);
    if (g_phase.compare_exchange_strong(expected, static_cast<uint8_t>(UpdatePhase::IDLE))) g_revision.fetch_add(1);
}

}  // namespace UpdateStatus

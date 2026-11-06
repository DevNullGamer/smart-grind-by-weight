#include "glink_session.h"
#include <string.h>

//------------------------------------------------------------------------------
// Modes and filter
//------------------------------------------------------------------------------
const char* glink_link_mode_name(GlinkLinkMode mode) {
    switch (mode) {
        case GlinkLinkMode::OFF: return "OFF";
        case GlinkLinkMode::FULL: return "FULL";
        case GlinkLinkMode::LINK_ONLY: return "LINK_ONLY";
    }
    return "?";
}

GlinkLinkMode glink_link_mode_resolve(bool active, bool full_enabled) {
    if (!active) return GlinkLinkMode::OFF;
    return full_enabled ? GlinkLinkMode::FULL : GlinkLinkMode::LINK_ONLY;
}

const char* ble_char_name(BleChar ch) {
    switch (ch) {
        case BleChar::OTA_DATA: return "ota_data";
        case BleChar::OTA_CONTROL: return "ota_control";
        case BleChar::OTA_STATUS: return "ota_status";
        case BleChar::OTA_BUILD_NUMBER: return "ota_build_number";
        case BleChar::DATA_CONTROL: return "data_control";
        case BleChar::DATA_TRANSFER: return "data_transfer";
        case BleChar::DATA_STATUS: return "data_status";
        case BleChar::DEBUG_RX: return "debug_rx";
        case BleChar::DEBUG_TX: return "debug_tx";
        case BleChar::SYSINFO_SYSTEM: return "sysinfo_system";
        case BleChar::SYSINFO_PERFORMANCE: return "sysinfo_performance";
        case BleChar::SYSINFO_HARDWARE: return "sysinfo_hardware";
        case BleChar::SYSINFO_SESSIONS: return "sysinfo_sessions";
        case BleChar::SYSINFO_DIAGNOSTICS: return "sysinfo_diagnostics";
        case BleChar::GLINK_RX: return "glink_rx";
        case BleChar::GLINK_TX: return "glink_tx";
        case BleChar::COUNT: break;
    }
    return "?";
}

bool ble_char_access_allowed(GlinkLinkMode mode, BleChar ch, BleAccess access) {
    if (static_cast<uint8_t>(ch) >= static_cast<uint8_t>(BleChar::COUNT)) return false;
    switch (mode) {
        case GlinkLinkMode::OFF:
            return false;
        case GlinkLinkMode::FULL:
            return true;
        case GlinkLinkMode::LINK_ONLY:
            if (ch == BleChar::GLINK_RX) return access == BleAccess::WRITE;
            if (ch == BleChar::GLINK_TX) return access == BleAccess::READ || access == BleAccess::NOTIFY;
            return false;
    }
    return false;
}

//------------------------------------------------------------------------------
// RX
//------------------------------------------------------------------------------
GlinkRx::GlinkRx() : reasm_(reasm_buf_, sizeof(reasm_buf_)) {}

void GlinkRx::on_fragment(const uint8_t* packet, size_t len, uint32_t now_ms) {
    switch (reasm_.feed(packet, len, now_ms)) {
        case GlinkFragmentResult::COMPLETE:
            if (pending_len_ != 0) {
                overrun_ = true;
            } else {
                pending_len_ = reasm_.message_length();
                memcpy(pending_, reasm_.message(), pending_len_);
            }
            break;
        case GlinkFragmentResult::BAD_FRAGMENT:
            if (bad_window_.record(now_ms)) disconnect_ = true;
            break;
        case GlinkFragmentResult::PAYLOAD_TOO_LARGE:
            too_large_ = true;
            break;
        case GlinkFragmentResult::PARTIAL:
        case GlinkFragmentResult::DISCARDED:
            break;
    }
}

void GlinkRx::reset() {
    reasm_.reset();
    bad_window_.reset();
    pending_len_ = 0;
    too_large_ = false;
    overrun_ = false;
    disconnect_ = false;
}

bool GlinkRx::take_too_large() {
    const bool value = too_large_;
    too_large_ = false;
    return value;
}

bool GlinkRx::take_overrun() {
    const bool value = overrun_;
    overrun_ = false;
    return value;
}

//------------------------------------------------------------------------------
// TX
//------------------------------------------------------------------------------
GlinkTx::GlinkTx(uint8_t* storage, size_t capacity) : buf_(storage), cap_(capacity) {}

bool GlinkTx::begin(const char* message, size_t len, uint16_t sequence) {
    if (len_ != 0 || !message || len == 0 || len > cap_) return false;
    memcpy(buf_, message, len);
    return commit(len, sequence);
}

bool GlinkTx::commit(size_t len, uint16_t sequence) {
    if (len_ != 0 || len == 0 || len > cap_ || len > GLINK_MAX_MESSAGE_BYTES) return false;
    len_ = len;
    offset_ = 0;
    peek_payload_ = 0;
    sequence_ = sequence;
    return true;
}

size_t GlinkTx::peek(size_t packet_len, uint8_t* out, size_t out_cap) {
    if (len_ == 0) return 0;
    const size_t packet = glink_packet_size(packet_len);
    if (packet == 0) return 0;
    size_t payload = packet - GLINK_FRAME_HEADER_BYTES;
    if (payload > len_ - offset_) payload = len_ - offset_;
    const size_t total = GLINK_FRAME_HEADER_BYTES + payload;
    if (!out || out_cap < total) return 0;
    glink_frame_header_write(out, sequence_, static_cast<uint16_t>(len_), static_cast<uint16_t>(offset_));
    memcpy(out + GLINK_FRAME_HEADER_BYTES, buf_ + offset_, payload);
    peek_payload_ = payload;
    return total;
}

void GlinkTx::advance() {
    if (len_ == 0) return;
    offset_ += peek_payload_;
    peek_payload_ = 0;
    if (offset_ >= len_) abort();
}

void GlinkTx::abort() {
    len_ = 0;
    offset_ = 0;
    peek_payload_ = 0;
}

bool GlinkTxPacer::window_open(uint32_t now_ms) const {
    return started_ && static_cast<uint32_t>(now_ms - window_start_) < BLE_GLINK_TX_PACE_MS;
}

uint8_t GlinkTxPacer::allowance(uint32_t now_ms) const {
    if (!window_open(now_ms)) return BLE_GLINK_TX_FRAGMENTS_PER_PACE;
    return count_ >= BLE_GLINK_TX_FRAGMENTS_PER_PACE ? 0 : static_cast<uint8_t>(BLE_GLINK_TX_FRAGMENTS_PER_PACE - count_);
}

void GlinkTxPacer::note_sent(uint32_t now_ms, uint8_t fragments) {
    if (!window_open(now_ms)) {
        started_ = true;
        window_start_ = now_ms;
        count_ = 0;
    }
    const unsigned total = static_cast<unsigned>(count_) + fragments;
    count_ = total > 255 ? 255 : static_cast<uint8_t>(total);
}

//------------------------------------------------------------------------------
// Cadence
//------------------------------------------------------------------------------
bool GlinkPeriodCadence::due(uint32_t now_ms, const GlinkPeriodKey& key, bool weight_changed, uint16_t att_mtu) const {
    if (!have_sent_) return true;
    const uint32_t elapsed = now_ms - last_ms_;
    if (elapsed >= GLINK_PERIOD_HEARTBEAT_MS) return true;
    if (key.dev_state != last_key_.dev_state) return true;  // at once, also while GRINDING

    const bool grinding = key.dev_state == GlinkDevState::GRINDING;
    const uint32_t min_interval =
        !grinding ? 0 : (att_mtu >= GLINK_FAST_MTU_MIN ? GLINK_PERIOD_GRINDING_FAST_MS : GLINK_PERIOD_GRINDING_SLOW_MS);
    if (key != last_key_ || (grinding && weight_changed)) return elapsed >= min_interval;
    return false;
}

void GlinkPeriodCadence::mark_sent(uint32_t now_ms, const GlinkPeriodKey& key) {
    have_sent_ = true;
    last_ms_ = now_ms;
    last_key_ = key;
}

//------------------------------------------------------------------------------
// Session
//------------------------------------------------------------------------------
void GlinkSession::reset() {
    connected_ = false;
    subscribed_ = false;
    handshaken_ = false;
    hello_pending_ = false;
    mtu_ = 0;
    connect_ms_ = 0;
    clock_running_ = false;
    cadence_.reset();
}

void GlinkSession::on_connect(uint32_t now_ms, bool clock_from_connect) {
    reset();
    connected_ = true;
    connect_ms_ = now_ms;
    clock_running_ = clock_from_connect;
}

void GlinkSession::note_activity(uint32_t now_ms) {
    if (!connected_ || clock_running_) return;
    clock_running_ = true;
    connect_ms_ = now_ms;
}

size_t GlinkSession::packet_len() const {
    if (mtu_ <= 3) return GLINK_DEFAULT_PACKET_BYTES;
    const size_t value = static_cast<size_t>(mtu_) - 3;
    return value > GLINK_MAX_PACKET_BYTES ? GLINK_MAX_PACKET_BYTES : value;
}

void GlinkSession::on_handshake() {
    if (!connected_) return;
    handshaken_ = true;
    hello_pending_ = true;
}

bool GlinkSession::handshake_timed_out(uint32_t now_ms) const {
    return connected_ && clock_running_ && !handshaken_ && static_cast<uint32_t>(now_ms - connect_ms_) >= GLINK_HANDSHAKE_TIMEOUT_MS;
}

bool GlinkSession::period_due(uint32_t now_ms, const GlinkPeriodKey& key, bool weight_changed) const {
    if (!connected_ || !handshaken_ || !subscribed_ || hello_pending_) return false;
    return cadence_.due(now_ms, key, weight_changed, mtu_);
}

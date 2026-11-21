#include "GLinkGrinderCore.h"

namespace glinkc {

constexpr int GrinderCore::PROFILE_DOSE_INDEX;

GrinderCore::GrinderCore(Sink& sink) : client_(sink, *this) {}

bool GrinderCore::on_connected(size_t packet_len, uint32_t now_ms) {
    now_ = now_ms;
    disconnect_ = false;
    disconnect_reason_ = "";
    // A fresh session never inherits a baseline or sync state from an aborted earlier connect.
    gate_.reset();
    sync_.on_link_down();
    return client_.on_connected(packet_len, now_ms);
}

void GrinderCore::on_disconnected() {
    client_.on_disconnected();
    gate_.reset();
    sync_.on_link_down();
}

void GrinderCore::on_packet(const uint8_t* data, size_t len, uint32_t now_ms) {
    now_ = now_ms;
    client_.on_notify(data, len, now_ms);
}

void GrinderCore::update(uint32_t now_ms) {
    now_ = now_ms;
    client_.tick(now_ms);
    if (disconnect_ || client_.state() != LinkState::READY) return;
    send_next(now_ms);
}

void GrinderCore::send_next(uint32_t now_ms) {
    if (client_.pending() != RequestKind::NONE) return;  // one request in flight at a time
    // 1. Stale profiles: compare-first needs a fresh entry 3, so nothing else is asked (not even SettingsSync) until read.
    // By design (spec order) a persistently failing profile_get starves the push and the result fetch.
    if (client_.needs_profile_refresh()) {
        client_.profile_get(-1, now_ms);
        return;
    }
    // 2. The settings push (an update, or the select after a stored change).
    ProfileUpdate update;
    switch (sync_.next(make_view(), now_ms, &update)) {
        case SyncAction::UPDATE:
            if (client_.profile_update(PROFILE_DOSE_INDEX, update, now_ms)) sync_.on_sent(now_ms);
            return;
        case SyncAction::SELECT:
            if (client_.profile_select(PROFILE_DOSE_INDEX, now_ms)) sync_.on_sent(now_ms);
            return;
        case SyncAction::NONE: break;
    }
    // 3. A result missed as a broadcast but visible through resultSeq.
    if (client_.needs_result_fetch()) client_.result_get(now_ms);
}

const ProfileEntry* GrinderCore::find_entry(int index) const {
    const ProfileEntry* entries = client_.profiles();
    for (int i = 0; i < client_.profile_count(); i++) {
        if (entries[i].index == index) return &entries[i];
    }
    return nullptr;
}

SyncView GrinderCore::make_view() const {
    SyncView v;
    v.ready = client_.state() == LinkState::READY;
    v.client_idle = client_.pending() == RequestKind::NONE;
    v.writes_ok = client_.has_period() && accepts_profile_writes(client_.period().dev_state);
    const ProfileEntry* e = find_entry(PROFILE_DOSE_INDEX);
    if (e) {
        v.has_entry3 = true;
        v.entry_dose_tenths = e->dose_tenths;
        v.entry_has_size = e->has_grind_size;
        v.entry_size_tenths = e->grind_size_tenths;
        v.entry_has_rpm = e->has_rpm;
        v.entry_rpm = e->rpm;
    }
    v.rpm_supported = client_.has_base() && client_.base().rpm_supported();
    return v;
}

void GrinderCore::set_wanted(bool has_dose, int32_t dose_tenths, bool has_size, int32_t size_tenths, bool has_rpm,
                             int32_t rpm) {
    if (!has_dose) {
        wanted_ = false;
        sync_.clear_wanted();
        return;
    }
    const int32_t size = has_size ? size_tenths : 0;
    const int32_t rpm_value = has_rpm ? rpm : 0;
    const bool same = wanted_ && wanted_dose_ == dose_tenths && wanted_has_size_ == has_size && wanted_size_ == size &&
                      wanted_has_rpm_ == has_rpm && wanted_rpm_ == rpm_value;
    if (same) return;
    wanted_ = true;
    wanted_dose_ = dose_tenths;
    wanted_has_size_ = has_size;
    wanted_size_ = size;
    wanted_has_rpm_ = has_rpm;
    wanted_rpm_ = rpm_value;
    sync_.set_wanted(wanted_dose_, wanted_has_size_, wanted_size_, wanted_has_rpm_, wanted_rpm_);
}

DevClass GrinderCore::dev_class() const {
    return client_.has_period() ? classify_dev_state(client_.period().dev_state) : DevClass::UNKNOWN;
}

bool GrinderCore::selected_profile_dose(int32_t* tenths) const {
    if (disconnect_ || !client_.has_period()) return false;
    const ProfileEntry* e = find_entry(client_.period().select_profile);
    if (!e) return false;
    if (tenths) *tenths = e->dose_tenths;
    return true;
}

bool GrinderCore::selected_profile_size(int32_t* tenths) const {
    if (disconnect_ || !client_.has_period()) return false;
    const ProfileEntry* e = find_entry(client_.period().select_profile);
    if (!e || !e->has_grind_size) return false;
    if (tenths) *tenths = e->grind_size_tenths;
    return true;
}

bool GrinderCore::selected_profile_rpm(int32_t* rpm) const {
    if (disconnect_ || !client_.has_period()) return false;
    const ProfileEntry* e = find_entry(client_.period().select_profile);
    if (!e || !e->has_rpm) return false;
    if (rpm) *rpm = e->rpm;
    return true;
}

bool GrinderCore::take_finished(int32_t* tenths) {
    if (disconnect_ || !finished_) return false;
    finished_ = false;
    if (tenths) *tenths = finished_tenths_;
    return true;
}

bool GrinderCore::must_disconnect(const char** reason) const {
    if (!disconnect_) return false;
    if (reason) *reason = disconnect_reason_;
    return true;
}

void GrinderCore::on_ready() {
    if (disconnect_) return;
    gate_.on_ready(client_.period().result_seq);
    // on_link_down does not re-queue a satisfied value: hand the wanted value over again (an unchanged one that is
    // still pending is ignored by SettingsSync).
    if (wanted_) sync_.set_wanted(wanted_dose_, wanted_has_size_, wanted_size_, wanted_has_rpm_, wanted_rpm_);
}

void GrinderCore::on_result(const ResultInfo& r) {
    if (disconnect_) return;  // a latched session (foreign device, wrong protocol) must not produce a finished grind
    int32_t tenths = 0;
    if (gate_.on_result(r.seq, r.weight_settled, r.final_weight_tenths, &tenths)) {
        finished_ = true;
        finished_tenths_ = tenths;
    }
}

void GrinderCore::on_response(RequestKind request, const Response& r) {
    // Only an update or a select can be in flight in SettingsSync, so no other completion can matter to it.
    if (request != RequestKind::PROFILE_UPDATE && request != RequestKind::PROFILE_SELECT) return;
    sync_.on_response(r.success, r.failure.code, !r.has_update_flags || r.changed, now_);
}

void GrinderCore::on_request_timeout(RequestKind request) {
    if (request != RequestKind::PROFILE_UPDATE && request != RequestKind::PROFILE_SELECT) return;
    sync_.on_timeout(now_);
}

void GrinderCore::on_must_disconnect(const char* reason) {
    disconnect_ = true;
    disconnect_reason_ = reason ? reason : "";
}

}  // namespace glinkc

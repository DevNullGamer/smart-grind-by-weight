#include "GLinkClient.h"
#include <string.h>

namespace glinkc {

namespace {

bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) { return static_cast<uint32_t>(now_ms - since_ms) >= span_ms; }

bool answers(RequestKind request, const char* object) {
    switch (request) {
        case RequestKind::PROFILE_GET:
        case RequestKind::PROFILE_SELECT:
        case RequestKind::PROFILE_UPDATE: return strcmp(object, "grindProfile") == 0;
        case RequestKind::START: return strcmp(object, "grindControl") == 0;
        case RequestKind::RESULT_GET: return strcmp(object, "grindResult") == 0;
        case RequestKind::HELLO: return strcmp(object, "appHello") == 0;
        case RequestKind::NONE: break;
    }
    return false;
}

}  // namespace

Client::Client(Sink& sink, Listener& listener) : sink_(sink), listener_(listener), reassembler_(rx_storage_, sizeof(rx_storage_)) {}

void Client::reset_connection_state() {
    reassembler_.reset();
    bad_frames_.reset();
    pending_ = RequestKind::NONE;
    stop_pending_ = false;
    start_blocked_ = false;
    start_wait_ = false;
    has_base_ = false;
    has_period_ = false;
    ready_timeout_fired_ = false;
}

bool Client::send_message(const char* text, size_t len) {
    const uint8_t* message = reinterpret_cast<const uint8_t*>(text);
    const size_t count = glink_fragment_count(len, packet_len_);
    if (count == 0) return false;
    uint8_t packet[GLINK_MAX_PACKET_BYTES];
    sequence_ = static_cast<uint16_t>(sequence_ % 0xFFFF + 1);  // 1 to 65535, like tools/ble/glink_client.py
    const uint16_t sequence = sequence_;
    for (size_t i = 0; i < count; i++) {
        const size_t n = glink_fragment_build(message, len, sequence, packet_len_, i, packet, sizeof(packet));
        if (n == 0 || !sink_.write_packet(packet, n)) return false;
    }
    return true;
}

bool Client::on_connected(size_t packet_len, uint32_t now_ms) {
    reset_connection_state();
    packet_len_ = glink_packet_size(packet_len);
    if (packet_len_ == 0) packet_len_ = GLINK_DEFAULT_PACKET_BYTES;
    state_ = LinkState::CONNECTED;
    char text[64];
    const size_t len = build_hello(text, sizeof(text));
    hello_sent_ms_ = now_ms;
    return len > 0 && send_message(text, len);
}

void Client::on_disconnected() {
    reset_connection_state();
    state_ = LinkState::DISCONNECTED;
}

bool Client::begin_request(RequestKind kind, const char* text, size_t len, uint32_t now_ms) {
    if (state_ == LinkState::DISCONNECTED || pending_ != RequestKind::NONE || len == 0) return false;
    if (kind == RequestKind::START && (state_ != LinkState::READY || start_blocked_)) return false;
    rev_at_request_ = has_period_ ? period_.profile_rev : 0;
    if (!send_message(text, len)) return false;
    pending_ = kind;
    pending_since_ms_ = now_ms;
    return true;
}

bool Client::profile_get(int index, uint32_t now_ms) {
    char text[GLINK_MAX_REQUEST_BYTES];
    return begin_request(RequestKind::PROFILE_GET, text, build_profile_get(text, sizeof(text), index), now_ms);
}

bool Client::profile_select(int index, uint32_t now_ms) {
    char text[GLINK_MAX_REQUEST_BYTES];
    return begin_request(RequestKind::PROFILE_SELECT, text, build_profile_select(text, sizeof(text), index), now_ms);
}

bool Client::profile_update(int index, const ProfileUpdate& update, uint32_t now_ms) {
    char text[GLINK_MAX_REQUEST_BYTES];
    return begin_request(RequestKind::PROFILE_UPDATE, text, build_profile_update(text, sizeof(text), index, update), now_ms);
}

bool Client::start(int profile, int32_t expect_tenths, uint32_t now_ms) {
    char text[GLINK_MAX_REQUEST_BYTES];
    return begin_request(RequestKind::START, text, build_start(text, sizeof(text), profile, true, expect_tenths), now_ms);
}

bool Client::result_get(uint32_t now_ms) {
    char text[GLINK_MAX_REQUEST_BYTES];
    return begin_request(RequestKind::RESULT_GET, text, build_result_get(text, sizeof(text)), now_ms);
}

bool Client::stop(uint32_t now_ms) {
    if (state_ == LinkState::DISCONNECTED) return false;
    char text[GLINK_MAX_REQUEST_BYTES];
    const size_t len = build_stop(text, sizeof(text));
    if (len == 0 || !send_message(text, len)) return false;
    stop_pending_ = true;
    stop_since_ms_ = now_ms;
    return true;
}

void Client::on_notify(const uint8_t* packet, size_t len, uint32_t now_ms) {
    if (state_ == LinkState::DISCONNECTED) return;
    switch (reassembler_.feed(packet, len, now_ms)) {
        case GlinkFragmentResult::COMPLETE:
            handle_message(reinterpret_cast<const char*>(reassembler_.message()), reassembler_.message_length(), now_ms);
            break;
        case GlinkFragmentResult::BAD_FRAGMENT:
            if (bad_frames_.record(now_ms)) listener_.on_must_disconnect("too many bad fragments");
            break;
        case GlinkFragmentResult::PARTIAL:
        case GlinkFragmentResult::PAYLOAD_TOO_LARGE:
        case GlinkFragmentResult::DISCARDED: break;
    }
}

void Client::handle_message(const char* text, size_t len, uint32_t now_ms) {
    if (!parse_message(text, len, msg_)) return;  // not something this client uses (spec 9: ignore what is unknown)
    switch (msg_.kind) {
        case Kind::BASE_INFO:
            base_ = msg_.base;
            has_base_ = true;
            if (strcmp(base_.device, "SGBW") != 0 || base_.proto != GLINK_PROTO_VERSION) {
                listener_.on_must_disconnect("not a GLink v1 grinder");
                return;
            }
            listener_.on_base_info(base_);
            break;
        case Kind::PERIOD_INFO: handle_period(msg_.period, now_ms); break;
        case Kind::GRIND_RESULT:
            note_result(msg_.result);
            listener_.on_result(msg_.result);
            break;
        case Kind::RESPONSE: handle_response(msg_.response, now_ms); break;
        case Kind::NONE: break;
    }
}

void Client::handle_period(const PeriodInfo& p, uint32_t now_ms) {
    (void)now_ms;
    const bool first = !has_period_;
    start_blocked_ = false;
    const bool rev_changed = first || p.profile_rev != period_.profile_rev;
    if (first && p.result_seq < result_seq_seen_) {  // a rebooted grinder restarts the counter
        result_seq_seen_ = p.result_seq;
        has_result_ = false;
    }
    period_ = p;
    has_period_ = true;
    if (rev_changed) needs_profiles_ = true;

    if (first) {
        state_ = LinkState::READY;
        listener_.on_ready();
    }
    listener_.on_period(period_, rev_changed);

    if (start_wait_ && strcmp(p.dev_state, "STARTING") != 0) {
        start_wait_ = false;
        listener_.on_start_outcome(strcmp(p.dev_state, "GRINDING") == 0, p.dev_state, p.blocked);
    }
}

void Client::note_result(const ResultInfo& r) {
    if (r.seq >= result_seq_seen_) {
        result_ = r;
        has_result_ = true;
        result_seq_seen_ = r.seq;
    }
}

void Client::handle_response(const Response& r, uint32_t now_ms) {
    // The stop reply is told apart by its shape; it is never the answer of a pending request.
    if (r.success && r.has_accepted) {
        stop_pending_ = false;
        listener_.on_stop_reply(r.accepted);
        return;
    }
    // A failed stop reply is the stop's, even while a start is pending (the grinder answers stop at once and never
    // fails it in v1, so a failure under grindControl with a stop outstanding is attributed to the stop).
    if (!r.success && stop_pending_ && strcmp(r.object, "grindControl") == 0) {
        stop_pending_ = false;
        listener_.on_stop_reply(false);
        return;
    }
    if (strcmp(r.object, "appHello") == 0) {  // only a failure is ever answered (spec 5.1)
        listener_.on_response(RequestKind::HELLO, r);
        return;
    }
    if (pending_ == RequestKind::NONE) return;
    if (strcmp(r.object, "error") != 0 && !answers(pending_, r.object)) return;  // spec 7.5: ignore a mismatching object

    const RequestKind kind = pending_;
    pending_ = RequestKind::NONE;
    if (r.success) {
        if (r.has_result) {
            note_result(r.result);
            listener_.on_result(r.result);
        }
        if (r.entry_count > 0 && kind != RequestKind::PROFILE_SELECT) {
            if (r.entries_are_list) {
                for (int i = 0; i < r.entry_count; i++) profiles_[i] = r.entries[i];
                profile_count_ = r.entry_count;
                needs_profiles_ = has_period_ && period_.profile_rev != rev_at_request_;
            } else {
                int slot = -1;
                for (int i = 0; i < profile_count_; i++) {
                    if (profiles_[i].index == r.entries[0].index) slot = i;
                }
                if (slot < 0 && profile_count_ < PROFILES_MAX) slot = profile_count_++;
                if (slot >= 0) profiles_[slot] = r.entries[0];
            }
            listener_.on_profiles(profiles_, profile_count_);
        }
        if (kind == RequestKind::START && r.has_lead_in) {
            start_wait_ = true;
            start_deadline_ms_ = now_ms + r.lead_in_ms + START_CONFIRM_EXTRA_MS;
        }
    }
    listener_.on_response(kind, r);
}

void Client::tick(uint32_t now_ms) {
    if (state_ == LinkState::DISCONNECTED) return;
    if (state_ == LinkState::CONNECTED && !ready_timeout_fired_ && elapsed(now_ms, hello_sent_ms_, READY_TIMEOUT_MS)) {
        ready_timeout_fired_ = true;
        listener_.on_must_disconnect("no periodInfo after the handshake");
    }
    if (pending_ != RequestKind::NONE && elapsed(now_ms, pending_since_ms_, RESPONSE_TIMEOUT_MS)) {
        const RequestKind kind = pending_;
        pending_ = RequestKind::NONE;
        if (kind == RequestKind::START) start_blocked_ = true;
        listener_.on_request_timeout(kind);
    }
    if (stop_pending_ && elapsed(now_ms, stop_since_ms_, RESPONSE_TIMEOUT_MS)) {
        stop_pending_ = false;
        listener_.on_stop_timeout();
    }
    if (start_wait_ && static_cast<int32_t>(now_ms - start_deadline_ms_) >= 0) {  // wrap-safe
        start_wait_ = false;
        listener_.on_start_outcome(false, has_period_ ? period_.dev_state : "", has_period_ ? period_.blocked : "");
    }
}

}  // namespace glinkc

#include "glink_frame.h"
#include <string.h>

bool glink_frame_header_parse(const uint8_t* packet, size_t len, GlinkFrameHeader& header) {
    if (packet == nullptr || len <= GLINK_FRAME_HEADER_BYTES) return false;
    if (packet[0] != GLINK_FRAME_MAGIC || packet[1] != GLINK_FRAME_VERSION) return false;
    header.sequence = static_cast<uint16_t>(packet[2] | (packet[3] << 8));
    header.total_length = static_cast<uint16_t>(packet[4] | (packet[5] << 8));
    header.offset = static_cast<uint16_t>(packet[6] | (packet[7] << 8));
    return true;
}

size_t glink_frame_header_write(uint8_t* out, uint16_t sequence, uint16_t total_length, uint16_t offset) {
    out[0] = GLINK_FRAME_MAGIC;
    out[1] = GLINK_FRAME_VERSION;
    out[2] = static_cast<uint8_t>(sequence & 0xFF);
    out[3] = static_cast<uint8_t>(sequence >> 8);
    out[4] = static_cast<uint8_t>(total_length & 0xFF);
    out[5] = static_cast<uint8_t>(total_length >> 8);
    out[6] = static_cast<uint8_t>(offset & 0xFF);
    out[7] = static_cast<uint8_t>(offset >> 8);
    return GLINK_FRAME_HEADER_BYTES;
}

size_t glink_packet_size(size_t packet_len) {
    if (packet_len > GLINK_MAX_PACKET_BYTES) packet_len = GLINK_MAX_PACKET_BYTES;
    return packet_len > GLINK_FRAME_HEADER_BYTES ? packet_len : 0;
}

size_t glink_fragment_count(size_t message_len, size_t packet_len) {
    const size_t packet = glink_packet_size(packet_len);
    if (packet == 0 || message_len == 0 || message_len > GLINK_MAX_MESSAGE_BYTES) return 0;
    const size_t per = packet - GLINK_FRAME_HEADER_BYTES;
    return (message_len + per - 1) / per;
}

size_t glink_fragment_build(const uint8_t* message, size_t message_len, uint16_t sequence, size_t packet_len,
                            size_t index, uint8_t* out, size_t out_cap) {
    const size_t count = glink_fragment_count(message_len, packet_len);
    if (message == nullptr || out == nullptr || index >= count) return 0;
    const size_t per = glink_packet_size(packet_len) - GLINK_FRAME_HEADER_BYTES;
    const size_t offset = index * per;
    size_t n = message_len - offset;
    if (n > per) n = per;
    if (out_cap < GLINK_FRAME_HEADER_BYTES + n) return 0;
    glink_frame_header_write(out, sequence, static_cast<uint16_t>(message_len), static_cast<uint16_t>(offset));
    memcpy(out + GLINK_FRAME_HEADER_BYTES, message + offset, n);
    return GLINK_FRAME_HEADER_BYTES + n;
}

const char* glink_fragment_result_name(GlinkFragmentResult result) {
    switch (result) {
        case GlinkFragmentResult::PARTIAL: return "partial";
        case GlinkFragmentResult::COMPLETE: return "complete";
        case GlinkFragmentResult::BAD_FRAGMENT: return "bad_fragment";
        case GlinkFragmentResult::PAYLOAD_TOO_LARGE: return "payload_too_large";
        case GlinkFragmentResult::DISCARDED: return "discarded";
    }
    return "bad_fragment";
}

GlinkReassembler::GlinkReassembler(uint8_t* storage, size_t capacity)
    : buf_(storage), capacity_(capacity > GLINK_MAX_MESSAGE_BYTES ? GLINK_MAX_MESSAGE_BYTES : capacity) {}

void GlinkReassembler::reset() {
    state_ = State::IDLE;
    received_ = 0;
    message_len_ = 0;
}

GlinkFragmentResult GlinkReassembler::bad() {
    state_ = State::IDLE;
    received_ = 0;
    return GlinkFragmentResult::BAD_FRAGMENT;
}

GlinkFragmentResult GlinkReassembler::feed(const uint8_t* packet, size_t len, uint32_t now_ms) {
    message_len_ = 0;
    GlinkFrameHeader h;
    if (!glink_frame_header_parse(packet, len, h)) return bad();
    const size_t n = len - GLINK_FRAME_HEADER_BYTES;
    if (h.total_length == 0 || h.total_length > GLINK_MAX_MESSAGE_BYTES || h.offset >= h.total_length ||
        static_cast<size_t>(h.offset) + n > h.total_length) {
        return bad();
    }
    const uint8_t* payload = packet + GLINK_FRAME_HEADER_BYTES;

    if (h.offset == 0) {  // starts a new message and drops an incomplete one
        state_ = State::IDLE;
        received_ = 0;
        sequence_ = h.sequence;
        total_ = h.total_length;
        last_ms_ = now_ms;
        if (h.total_length > capacity_) {
            state_ = State::DISCARDING;
            return GlinkFragmentResult::PAYLOAD_TOO_LARGE;
        }
        memcpy(buf_, payload, n);
        received_ = static_cast<uint16_t>(n);
        if (received_ == total_) {
            message_len_ = total_;
            received_ = 0;
            return GlinkFragmentResult::COMPLETE;
        }
        state_ = State::IN_PROGRESS;
        return GlinkFragmentResult::PARTIAL;
    }

    // A continuation is valid only inside a live (not expired) message with the same sequence and total length.
    const bool alive = state_ != State::IDLE && static_cast<uint32_t>(now_ms - last_ms_) <= GLINK_REASSEMBLY_TIMEOUT_MS;
    if (!alive || h.sequence != sequence_ || h.total_length != total_) return bad();

    if (state_ == State::DISCARDING) {
        last_ms_ = now_ms;
        if (static_cast<size_t>(h.offset) + n == total_) state_ = State::IDLE;
        return GlinkFragmentResult::DISCARDED;
    }

    if (h.offset != received_) return bad();
    memcpy(buf_ + h.offset, payload, n);
    received_ = static_cast<uint16_t>(received_ + n);
    last_ms_ = now_ms;
    if (received_ == total_) {
        state_ = State::IDLE;
        message_len_ = total_;
        received_ = 0;
        return GlinkFragmentResult::COMPLETE;
    }
    return GlinkFragmentResult::PARTIAL;
}

bool GlinkBadFrameWindow::record(uint32_t now_ms) {
    times_[head_] = now_ms;
    head_ = static_cast<uint8_t>((head_ + 1) % GLINK_BAD_FRAME_LIMIT);
    if (count_ < GLINK_BAD_FRAME_LIMIT) count_++;
    if (count_ < GLINK_BAD_FRAME_LIMIT) return false;
    const uint32_t oldest = times_[head_];  // head_ now points at the oldest of the last GLINK_BAD_FRAME_LIMIT
    return static_cast<uint32_t>(now_ms - oldest) < GLINK_BAD_FRAME_WINDOW_MS;
}

void GlinkBadFrameWindow::reset() {
    head_ = 0;
    count_ = 0;
}

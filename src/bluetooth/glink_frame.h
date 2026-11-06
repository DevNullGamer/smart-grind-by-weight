#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"

// GLink fragment frame v1 (docs/GLINK-PROTOCOL.md section 3): header codec, fragmenter, reassembler and the
// bad-fragment window. Pure: no Arduino/ESP dependencies, no heap (the caller owns the reassembly storage), the
// clock is injected (milliseconds, wrap-safe). Unit tested on the host against tools/tests/glink_vectors.json.

//------------------------------------------------------------------------------
// Header
//------------------------------------------------------------------------------
struct GlinkFrameHeader {
    uint16_t sequence;
    uint16_t total_length;
    uint16_t offset;
};

// True when the packet has the right magic and version and carries at least one payload byte (more than
// GLINK_FRAME_HEADER_BYTES bytes). The field values are NOT range checked here (the reassembler does).
bool glink_frame_header_parse(const uint8_t* packet, size_t len, GlinkFrameHeader& header);

// Writes the 8 header bytes (little-endian) to out; returns GLINK_FRAME_HEADER_BYTES.
size_t glink_frame_header_write(uint8_t* out, uint16_t sequence, uint16_t total_length, uint16_t offset);

//------------------------------------------------------------------------------
// Fragmenter (stateless)
//------------------------------------------------------------------------------
// Packet size actually used: packet_len clamped to GLINK_MAX_PACKET_BYTES; 0 if it is not above the header size.
size_t glink_packet_size(size_t packet_len);

// Number of fragments of a message (0 when the message is empty or above GLINK_MAX_MESSAGE_BYTES, or packet_len
// is unusable).
size_t glink_fragment_count(size_t message_len, size_t packet_len);

// Builds fragment number `index` (0-based) into out; returns its byte length, or 0 on bad arguments / small out.
size_t glink_fragment_build(const uint8_t* message, size_t message_len, uint16_t sequence, size_t packet_len,
                            size_t index, uint8_t* out, size_t out_cap);

//------------------------------------------------------------------------------
// Reassembler (rules 1 to 5 of section 3.3)
//------------------------------------------------------------------------------
enum class GlinkFragmentResult : uint8_t {
    PARTIAL,            // accepted, message not complete yet
    COMPLETE,           // message complete: message() / message_length() are valid until the next feed()
    BAD_FRAGMENT,       // dropped, and so is the message in progress
    PAYLOAD_TOO_LARGE,  // first fragment of a message above the capacity: answer once, discard the rest silently
    DISCARDED           // continuation of an oversize message, ignored
};

// Lower-case vector name ("partial", "complete", "bad_fragment", "payload_too_large", "discarded").
const char* glink_fragment_result_name(GlinkFragmentResult result);

class GlinkReassembler {
public:
    // storage: capacity bytes owned by the caller (the device passes GLINK_MAX_REQUEST_BYTES). A first fragment
    // announcing more than capacity bytes (within GLINK_MAX_MESSAGE_BYTES) is PAYLOAD_TOO_LARGE.
    GlinkReassembler(uint8_t* storage, size_t capacity);

    GlinkFragmentResult feed(const uint8_t* packet, size_t len, uint32_t now_ms);
    void reset();  // disconnect: forgets the message in progress and the oversize discard state

    const uint8_t* message() const { return buf_; }
    size_t message_length() const { return message_len_; }

private:
    enum class State : uint8_t { IDLE, IN_PROGRESS, DISCARDING };

    GlinkFragmentResult bad();

    uint8_t* buf_;
    size_t capacity_;
    State state_ = State::IDLE;
    uint16_t sequence_ = 0;
    uint16_t total_ = 0;
    uint16_t received_ = 0;
    uint32_t last_ms_ = 0;
    size_t message_len_ = 0;
};

//------------------------------------------------------------------------------
// Bad-fragment window (rule 6): GLINK_BAD_FRAME_LIMIT bad fragments within GLINK_BAD_FRAME_WINDOW_MS
//------------------------------------------------------------------------------
class GlinkBadFrameWindow {
public:
    // Records one bad fragment at now_ms; true when the client must be disconnected. A bad fragment counts together
    // with the earlier ones that are less than GLINK_BAD_FRAME_WINDOW_MS older.
    bool record(uint32_t now_ms);
    void reset();

private:
    uint32_t times_[GLINK_BAD_FRAME_LIMIT] = {};
    uint8_t head_ = 0;
    uint8_t count_ = 0;
};

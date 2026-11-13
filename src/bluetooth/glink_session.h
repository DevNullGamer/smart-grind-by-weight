#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"
#include "glink_frame.h"
#include "glink_messages.h"

// GLink link session (docs/GLINK-PROTOCOL.md sections 2, 3.3, 7.4 and 8, plan chg020 4.7): the pure state the
// BluetoothManager drives. No Arduino/ESP dependencies, no heap, no locking (the manager serialises access), the
// clock is injected (milliseconds, wrap-safe). Host-tested in test_native/test_glink_session.cpp.

//------------------------------------------------------------------------------
// Link modes and the per-characteristic filter
//------------------------------------------------------------------------------
// OFF: nothing is advertised and nothing is served. FULL: the Bluetooth "Enabled" mode (all services, 30/5 minute
// auto-disable). LINK_ONLY: the touch-only "GaggiMate link" is on and FULL is not: only the GLink service is usable
// and there is no auto-disable timer.
enum class GlinkLinkMode : uint8_t { OFF, FULL, LINK_ONLY };

const char* glink_link_mode_name(GlinkLinkMode mode);  // "OFF", "FULL", "LINK_ONLY"

// FULL wins; LINK_ONLY only when the stack is serving (`active`) without FULL.
GlinkLinkMode glink_link_mode_resolve(bool active, bool full_enabled);

// Every characteristic of the BluetoothManager
enum class BleChar : uint8_t {
    OTA_DATA,
    OTA_CONTROL,
    OTA_STATUS,
    OTA_BUILD_NUMBER,
    DATA_CONTROL,
    DATA_TRANSFER,
    DATA_STATUS,
    DEBUG_RX,
    DEBUG_TX,
    SYSINFO_SYSTEM,
    SYSINFO_PERFORMANCE,
    SYSINFO_HARDWARE,
    SYSINFO_SESSIONS,
    SYSINFO_DIAGNOSTICS,
    GLINK_RX,
    GLINK_TX,
    COUNT
};

enum class BleAccess : uint8_t { WRITE, READ, NOTIFY };

const char* ble_char_name(BleChar ch);

// May `access` to `ch` be served in `mode`? OFF refuses everything. FULL filters nothing (the stack's own
// characteristic properties still apply). LINK_ONLY allows only a write to GLink RX and read / notify of GLink TX:
// OTA, data export, debug and diagnostics writes are refused, and reads of every other characteristic return nothing.
bool ble_char_access_allowed(GlinkLinkMode mode, BleChar ch, BleAccess access);

//------------------------------------------------------------------------------
// RX: fragments in (host task), one complete request out (Bluetooth task)
//------------------------------------------------------------------------------
// on_fragment() runs on the BLE host task, everything else on the Bluetooth task; the caller wraps each call in one
// critical section. A complete request is copied into a second buffer, so the reassembler is free for the next
// message; a request that completes while the previous one is still unprocessed is dropped and reported as an
// overrun (answered with BUSY).
class GlinkRx {
public:
    GlinkRx();
    GlinkRx(const GlinkRx&) = delete;
    GlinkRx& operator=(const GlinkRx&) = delete;

    void on_fragment(const uint8_t* packet, size_t len, uint32_t now_ms);
    void reset();  // connect or disconnect: forgets everything of the previous client

    bool has_message() const { return pending_len_ != 0; }
    const char* message() const { return reinterpret_cast<const char*>(pending_); }
    size_t message_length() const { return pending_len_; }
    void release_message() { pending_len_ = 0; }

    bool take_too_large();             // true once per oversize first fragment (answer PAYLOAD_TOO_LARGE)
    bool take_overrun();               // true once per request dropped because one was still pending
    bool disconnect_requested() const { return disconnect_; }  // the bad-fragment limit was reached

private:
    uint8_t reasm_buf_[GLINK_MAX_REQUEST_BYTES];
    uint8_t pending_[GLINK_MAX_REQUEST_BYTES];
    GlinkReassembler reasm_;
    GlinkBadFrameWindow bad_window_;
    size_t pending_len_ = 0;
    bool too_large_ = false;
    bool overrun_ = false;
    bool disconnect_ = false;
};

//------------------------------------------------------------------------------
// TX: one message at a time, fragment by fragment
//------------------------------------------------------------------------------
class GlinkTx {
public:
    // storage: caller-owned buffer of `capacity` bytes (the device passes GLINK_MAX_REPLY_BYTES)
    GlinkTx(uint8_t* storage, size_t capacity);

    bool idle() const { return len_ == 0; }
    // Copies the message in; false when a message is still being sent, or it is empty or does not fit
    bool begin(const char* message, size_t len, uint16_t sequence);
    // Encoding straight into the buffer saves a copy: scratch() is the buffer while idle (nullptr while a message is
    // being sent), commit() queues the `len` bytes written there.
    char* scratch() { return len_ == 0 ? reinterpret_cast<char*>(buf_) : nullptr; }
    size_t scratch_capacity() const { return cap_; }
    bool commit(size_t len, uint16_t sequence);
    // The next fragment for a packet size of packet_len (re-evaluated for every fragment, so an MTU update in the
    // middle of a message is fine). Returns its length, 0 when idle or packet_len is unusable. Does not advance.
    size_t peek(size_t packet_len, uint8_t* out, size_t out_cap);
    void advance();  // the fragment from the last peek() was handed to the stack
    void abort();    // drop the rest of the message (failed notification, disconnect)

private:
    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    size_t offset_ = 0;
    size_t peek_payload_ = 0;
    uint16_t sequence_ = 0;
};

// At most BLE_GLINK_TX_FRAGMENTS_PER_PACE fragments per BLE_GLINK_TX_PACE_MS window
class GlinkTxPacer {
public:
    uint8_t allowance(uint32_t now_ms) const;  // fragments that may be sent now
    void note_sent(uint32_t now_ms, uint8_t fragments);
    void reset() { started_ = false; count_ = 0; }

private:
    bool window_open(uint32_t now_ms) const;  // the current window has not expired yet
    bool started_ = false;
    uint32_t window_start_ = 0;
    uint8_t count_ = 0;
};

//------------------------------------------------------------------------------
// periodInfo cadence (section 8)
//------------------------------------------------------------------------------
// The fields of periodInfo that trigger an immediate send when they change (the live weight is handled separately).
struct GlinkPeriodKey {
    GlinkDevState dev_state = GlinkDevState::UNKNOWN;
    GlinkPhase phase = GlinkPhase::NONE;
    uint8_t select_profile = 0;
    GlinkError blocked = GlinkError::NONE;
    uint32_t result_seq = 0;
    uint32_t profile_rev = 0;

    bool operator==(const GlinkPeriodKey& o) const {
        return dev_state == o.dev_state && phase == o.phase && select_profile == o.select_profile &&
               blocked == o.blocked && result_seq == o.result_seq && profile_rev == o.profile_rev;
    }
    bool operator!=(const GlinkPeriodKey& o) const { return !(*this == o); }
};

class GlinkPeriodCadence {
public:
    void reset() { have_sent_ = false; }
    void force_next() { have_sent_ = false; }  // the next due() is true (after a handshake)

    // May a periodInfo go out now? `weight_changed`: the live weight differs from the last one sent. While GRINDING
    // the immediate sends are limited to GLINK_PERIOD_GRINDING_FAST_MS (att_mtu >= GLINK_FAST_MTU_MIN) or
    // GLINK_PERIOD_GRINDING_SLOW_MS, except a devState change; a weight change alone only triggers while GRINDING;
    // otherwise a heartbeat every GLINK_PERIOD_HEARTBEAT_MS. att_mtu 0 = unknown (treated as 23).
    bool due(uint32_t now_ms, const GlinkPeriodKey& key, bool weight_changed, uint16_t att_mtu) const;
    void mark_sent(uint32_t now_ms, const GlinkPeriodKey& key);

private:
    bool have_sent_ = false;
    uint32_t last_ms_ = 0;
    GlinkPeriodKey last_key_;
};

//------------------------------------------------------------------------------
// Per-connection session
//------------------------------------------------------------------------------
// The ATT MTU to size packets from: what the MTU callback reported, else what the stack says (0 = unknown for both).
// The callback value can be missing although the MTU was negotiated; 20-byte packets would then be used on a link
// that carries 244 (docs/GLINK-PROTOCOL.md section 8).
uint16_t glink_effective_mtu(uint16_t reported_mtu, uint16_t stack_mtu);

class GlinkSession {
public:
    void reset();                        // no client: clears the handshake, subscription, MTU, cadence
    // A client connected (clears everything of the previous one). clock_from_connect: the handshake clock runs from the
    // connect (LINK_ONLY: every client must speak GLink). In FULL it is false: grinder.py and the web flasher never send
    // appHello, so the clock starts only at the first GLink activity (note_activity()).
    void on_connect(uint32_t now_ms, bool clock_from_connect = true);
    void note_activity(uint32_t now_ms);  // first GLink TX subscribe or RX write; starts a clock that is not running

    bool connected() const { return connected_; }
    void set_subscribed(bool subscribed) { subscribed_ = subscribed; }
    bool subscribed() const { return subscribed_; }
    void set_mtu(uint16_t att_mtu) { mtu_ = att_mtu; }
    uint16_t mtu() const { return mtu_; }
    // min(ATT MTU - 3, GLINK_MAX_PACKET_BYTES); GLINK_DEFAULT_PACKET_BYTES while the MTU is unknown
    size_t packet_len() const;

    // appHello processed: the handshake gate opens and baseInfo, then periodInfo, are due (also for a repeated one)
    void on_handshake();
    bool handshaken() const { return handshaken_; }
    // Connected without appHello for GLINK_HANDSHAKE_TIMEOUT_MS or longer: the manager disconnects the client
    bool handshake_timed_out(uint32_t now_ms) const;

    // baseInfo goes out once per handshake, as soon as the client is subscribed to TX
    bool base_info_due() const { return connected_ && handshaken_ && subscribed_ && hello_pending_; }
    void mark_base_info_sent() { hello_pending_ = false; cadence_.force_next(); }

    // periodInfo: only after baseInfo, to a subscribed handshaken client
    bool period_due(uint32_t now_ms, const GlinkPeriodKey& key, bool weight_changed) const;
    void mark_period_sent(uint32_t now_ms, const GlinkPeriodKey& key) { cadence_.mark_sent(now_ms, key); }

private:
    bool connected_ = false;
    bool subscribed_ = false;
    bool handshaken_ = false;
    bool hello_pending_ = false;
    uint16_t mtu_ = 0;
    uint32_t connect_ms_ = 0;  // start of the handshake clock
    bool clock_running_ = false;
    GlinkPeriodCadence cadence_;
};

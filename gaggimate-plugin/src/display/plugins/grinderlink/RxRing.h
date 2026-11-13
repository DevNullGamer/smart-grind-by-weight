#pragma once
// Byte ring for the notification packets between the NimBLE host task (push) and the loop task (pop).
// Pure: no heap, no locking and no Arduino; the caller owns the storage and serialises push/pop (the plugin holds
// rxMutex). A packet takes 2 + length bytes, so the ring holds many small packets (20-byte packets at the default MTU
// of 23) or a few large ones (up to 512 bytes at a large MTU) out of the same space.
#include <stddef.h>
#include <stdint.h>

namespace glinkc {

class RxRing {
  public:
    RxRing() = default;
    void init(uint8_t *storage, size_t capacity) {
        buf_ = storage;
        cap_ = capacity;
        clear();
    }
    void clear() { head_ = tail_ = used_ = count_ = 0; }

    // False when the packet is empty or does not fit (the ring stays as it was).
    bool push(const uint8_t *data, size_t len) {
        if (buf_ == nullptr || len == 0 || len > 0xFFFF || used_ + HEADER + len > cap_)
            return false;
        put(static_cast<uint8_t>(len & 0xFF));
        put(static_cast<uint8_t>(len >> 8));
        for (size_t i = 0; i < len; i++)
            put(data[i]);
        used_ += HEADER + len;
        count_++;
        return true;
    }

    // Length of the oldest packet, copied to out; 0 when the ring is empty or out is too small (that packet is then
    // discarded: a packet longer than the caller's buffer cannot be a GLink packet).
    size_t pop(uint8_t *out, size_t out_cap) {
        if (count_ == 0)
            return 0;
        const size_t len = static_cast<size_t>(take()) | (static_cast<size_t>(take()) << 8);
        const bool fits = len <= out_cap;
        for (size_t i = 0; i < len; i++) {
            const uint8_t b = take();
            if (fits)
                out[i] = b;
        }
        used_ -= HEADER + len;
        count_--;
        return fits ? len : 0;
    }

    size_t count() const { return count_; }

  private:
    static constexpr size_t HEADER = 2;
    void put(uint8_t b) {
        buf_[tail_] = b;
        tail_ = (tail_ + 1) % cap_;
    }
    uint8_t take() {
        const uint8_t b = buf_[head_];
        head_ = (head_ + 1) % cap_;
        return b;
    }

    uint8_t *buf_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t used_ = 0;
    size_t count_ = 0;
};

} // namespace glinkc

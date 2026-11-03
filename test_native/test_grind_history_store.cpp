// Persistent history store: codec, loader and the crash-safe append/rotation logic against a fake file system with fault
// injection (chg016 step 10). The fake can cut the power at any byte of a write, fail a write with or without effect,
// and writes a torn file the way a truncate-then-write replace would leave it.
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "logging/grind_history_store.h"

namespace {

constexpr uint32_t kEpoch = 0xA1B2C3D4u;

GrindHistoryEntry entry_n(uint32_t n) {
    GrindHistoryEntry e;
    e.session_id = n;
    e.ended_s = n * 10;
    e.time_ms = 10000 + n;
    e.motor_ms = 9000 + n;
    e.target = 18.0f;
    e.final_g = 18.0f + 0.1f * static_cast<float>(n % 5);
    e.profile = static_cast<uint8_t>(n % 3);
    e.mode = static_cast<uint8_t>(n % 2);
    e.pulses = static_cast<uint8_t>(n % 4);
    e.result = 0;
    return e;
}

// In-memory files with power-cut and failure injection
class FakeFiles : public HistoryFiles {
public:
    std::vector<uint8_t> bytes[2];
    bool present[2] = {false, false};
    int reads = 0, appends = 0, replaces = 0, removes = 0;
    size_t bytes_written = 0;

    // Power cut: budget = bytes of writes still allowed (-1 = none armed). The write that does not fit lands partially
    // (a replace truncates first), then the power is off and every call fails without effect.
    long budget = -1;
    bool dead = false;

    // Failure injection: the next fail_count write calls fail. CLEAN = no effect (open failed), SHORT = half of the bytes
    // land (a replace truncates first).
    enum Fail { NONE, CLEAN, SHORT } fail = NONE;
    int fail_count = 0;

    void revive() {
        dead = false;
        budget = -1;
    }

    int32_t read_all(uint8_t slot, uint8_t* buf, size_t cap) override {
        reads++;
        if (dead || slot >= 2 || !present[slot]) return -1;
        size_t n = bytes[slot].size();
        if (n > cap) n = cap;
        if (n) memcpy(buf, bytes[slot].data(), n);
        return static_cast<int32_t>(n);
    }

    bool append(uint8_t slot, const uint8_t* data, size_t len) override {
        appends++;
        return write(slot, data, len, false);
    }

    bool replace(uint8_t slot, const uint8_t* data, size_t len) override {
        replaces++;
        return write(slot, data, len, true);
    }

    bool remove(uint8_t slot) override {
        removes++;
        if (dead || slot >= 2) return false;
        bytes[slot].clear();
        present[slot] = false;
        return true;
    }

    void put(uint8_t slot, const std::vector<uint8_t>& content) {
        bytes[slot] = content;
        present[slot] = true;
    }

private:
    bool write(uint8_t slot, const uint8_t* data, size_t len, bool truncate) {
        if (dead || slot >= 2) return false;
        size_t allowed = len;
        bool ok = true;
        if (budget >= 0) {
            if (static_cast<size_t>(budget) < len) {
                allowed = static_cast<size_t>(budget);
                ok = false;
                dead = true;
                budget = 0;
            } else {
                budget -= static_cast<long>(len);
            }
        } else if (fail_count > 0) {
            fail_count--;
            if (fail == CLEAN) return false;
            if (fail == SHORT) {
                allowed = len / 2;
                ok = false;
            }
        }
        if (truncate) bytes[slot].clear();
        present[slot] = true;
        bytes[slot].insert(bytes[slot].end(), data, data + allowed);
        bytes_written += allowed;
        return ok;
    }
};

// A store as the firmware wires it: fake files, state, scratch, and the ring a boot load fills
struct Rig {
    FakeFiles files;
    HistoryStoreState state;
    uint8_t scratch[HISTORY_SCRATCH_BYTES];
    GrindHistoryRing ring;
    uint32_t epoch = kEpoch;

    HistoryLoadResult boot() {
        ring = GrindHistoryRing();
        return history_load(files, epoch, scratch, &state, &ring);
    }
    HistoryAppendOutcome add(uint32_t id) { return history_append(files, &state, scratch, entry_n(id)); }
    std::vector<uint32_t> ids() const {
        GrindHistoryEntry out[GRIND_HISTORY_RING_CAPACITY];
        const uint8_t n = ring.copy_newest_first(out, GRIND_HISTORY_RING_CAPACITY);
        std::vector<uint32_t> ids;
        for (uint8_t i = 0; i < n; i++) ids.push_back(out[i].session_id);
        return ids;
    }
};

// Newest `max` of an oldest-first list, newest first
std::vector<uint32_t> newest_first_tail(const std::vector<uint32_t>& oldest_first, size_t max = GRIND_HISTORY_RING_CAPACITY) {
    std::vector<uint32_t> out;
    for (size_t i = oldest_first.size(); i > 0 && out.size() < max; i--) out.push_back(oldest_first[i - 1]);
    return out;
}

std::vector<uint8_t> file_bytes(uint32_t generation, uint32_t epoch, const std::vector<uint32_t>& ids) {
    std::vector<uint8_t> out(HISTORY_HEADER_BYTES);
    HistoryHeader h;
    h.generation = generation;
    h.epoch = epoch;
    history_encode_header(h, out.data());
    for (uint32_t id : ids) {
        uint8_t rec[HISTORY_RECORD_BYTES];
        history_encode_record(entry_n(id), rec);
        out.insert(out.end(), rec, rec + HISTORY_RECORD_BYTES);
    }
    return out;
}

std::vector<uint32_t> range(uint32_t first, uint32_t last) {
    std::vector<uint32_t> v;
    for (uint32_t i = first; i <= last; i++) v.push_back(i);
    return v;
}

uint32_t generation_of(const std::vector<uint8_t>& file) {
    HistoryHeader h;
    return history_decode_header(file.data(), &h) ? h.generation : 0xDEADBEEFu;
}

}  // namespace

// ---- codec ----

static void test_crc32_known_vectors() {
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, history_crc32(digits, sizeof(digits)));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, history_crc32(nullptr, 0));
    const uint8_t zero4[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, history_crc32(zero4, 4));
}

static void test_record_layout_is_pinned() {
    GrindHistoryEntry e;
    e.session_id = 0x01020304u;
    e.ended_s = 0x05060708u;
    e.time_ms = 0x090A0B0Cu;
    e.motor_ms = 0x0D0E0F10u;
    e.target = 18.5f;
    e.final_g = -1.0f;
    e.profile = 2;
    e.mode = 1;
    e.pulses = 7;
    e.result = 3;
    uint8_t rec[HISTORY_RECORD_BYTES];
    history_encode_record(e, rec);
    // little-endian, offsets as documented
    TEST_ASSERT_EQUAL_HEX8(0x04, rec[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, rec[7]);
    TEST_ASSERT_EQUAL_HEX8(0x08, rec[8]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, rec[12]);
    TEST_ASSERT_EQUAL_HEX8(0x10, rec[16]);
    TEST_ASSERT_EQUAL_HEX8(0x41, rec[23]);  // 18.5f = 0x41940000
    TEST_ASSERT_EQUAL_HEX8(0x94, rec[22]);
    TEST_ASSERT_EQUAL_HEX8(0xBF, rec[27]);  // -1.0f = 0xBF800000
    TEST_ASSERT_EQUAL_HEX8(0x80, rec[26]);
    TEST_ASSERT_EQUAL_UINT8(2, rec[28]);
    TEST_ASSERT_EQUAL_UINT8(1, rec[29]);
    TEST_ASSERT_EQUAL_UINT8(7, rec[30]);
    TEST_ASSERT_EQUAL_UINT8(3, rec[31]);
    // the CRC covers every byte after it
    const uint32_t crc = history_crc32(rec + 4, HISTORY_RECORD_BYTES - 4);
    TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, rec[0]);
    TEST_ASSERT_EQUAL_HEX8((crc >> 24) & 0xFF, rec[3]);
    TEST_ASSERT_EQUAL(32, HISTORY_RECORD_BYTES);
    TEST_ASSERT_EQUAL(32, HISTORY_HEADER_BYTES);
    TEST_ASSERT_EQUAL(544, HISTORY_FILE_MAX_BYTES);
}

static void test_header_layout_is_pinned() {
    HistoryHeader h;
    h.generation = 0x11223344u;
    h.epoch = 0xA1B2C3D4u;
    uint8_t buf[HISTORY_HEADER_BYTES];
    history_encode_header(h, buf);
    TEST_ASSERT_EQUAL_UINT8('G', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('H', buf[1]);
    TEST_ASSERT_EQUAL_UINT8('S', buf[2]);
    TEST_ASSERT_EQUAL_UINT8('T', buf[3]);
    TEST_ASSERT_EQUAL_UINT8(1, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(32, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x44, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[11]);
    TEST_ASSERT_EQUAL_HEX8(0xD4, buf[12]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, buf[15]);
    for (int i = 16; i < 28; i++) TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
    const uint32_t crc = history_crc32(buf, 28);
    TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, buf[28]);
    HistoryHeader back;
    TEST_ASSERT_TRUE(history_decode_header(buf, &back));
    TEST_ASSERT_EQUAL_HEX32(0x11223344u, back.generation);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4u, back.epoch);
}

static void test_record_round_trip_extremes() {
    const uint32_t u32s[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    const float floats[] = {0.0f, -0.0f, 18.1f, -3.5f, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min()};
    for (uint32_t u : u32s) {
        for (float f : floats) {
            GrindHistoryEntry e;
            e.session_id = u;
            e.ended_s = u;
            e.time_ms = u;
            e.motor_ms = u;
            e.target = f;
            e.final_g = f;
            e.profile = GRIND_HISTORY_PROFILE_MAX;
            e.mode = GRIND_HISTORY_MODE_MAX;
            e.pulses = 255;
            e.result = 255;
            uint8_t rec[HISTORY_RECORD_BYTES];
            history_encode_record(e, rec);
            GrindHistoryEntry back;
            TEST_ASSERT_TRUE(history_decode_record(rec, &back));
            TEST_ASSERT_EQUAL_UINT32(u, back.session_id);
            TEST_ASSERT_EQUAL_UINT32(u, back.ended_s);
            TEST_ASSERT_EQUAL_UINT32(u, back.time_ms);
            TEST_ASSERT_EQUAL_UINT32(u, back.motor_ms);
            TEST_ASSERT_EQUAL_MEMORY(&f, &back.target, sizeof(float));  // bit for bit, also -0.0 and the denormal
            TEST_ASSERT_EQUAL_MEMORY(&f, &back.final_g, sizeof(float));
            TEST_ASSERT_EQUAL_UINT8(255, back.pulses);
            TEST_ASSERT_EQUAL_UINT8(255, back.result);
            TEST_ASSERT_FALSE(back.earlier_boot);
        }
    }
    // every result code
    for (uint8_t code : {0, 1, 2, 3, 255}) {
        GrindHistoryEntry e = entry_n(1);
        e.result = code;
        uint8_t rec[HISTORY_RECORD_BYTES];
        history_encode_record(e, rec);
        GrindHistoryEntry back;
        TEST_ASSERT_TRUE(history_decode_record(rec, &back));
        TEST_ASSERT_EQUAL_UINT8(code, back.result);
    }
}

static void test_non_finite_weights_are_encoded_but_rejected_on_load() {
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    for (float f : bad) {
        for (int which = 0; which < 2; which++) {
            GrindHistoryEntry e = entry_n(1);
            (which ? e.target : e.final_g) = f;
            uint8_t rec[HISTORY_RECORD_BYTES];
            history_encode_record(e, rec);  // the bits are stored as they are, with a valid CRC
            GrindHistoryEntry back = entry_n(99);
            TEST_ASSERT_FALSE(history_decode_record(rec, &back));
            TEST_ASSERT_EQUAL_UINT32(99, back.session_id);  // untouched on rejection
        }
    }
}

static void test_out_of_range_enums_are_rejected_even_with_a_good_crc() {
    GrindHistoryEntry e = entry_n(1);
    e.profile = 3;
    uint8_t rec[HISTORY_RECORD_BYTES];
    history_encode_record(e, rec);
    GrindHistoryEntry back;
    TEST_ASSERT_FALSE(history_decode_record(rec, &back));
    e = entry_n(1);
    e.mode = 2;
    history_encode_record(e, rec);
    TEST_ASSERT_FALSE(history_decode_record(rec, &back));
    e = entry_n(1);
    e.result = 4;
    history_encode_record(e, rec);
    TEST_ASSERT_FALSE(history_decode_record(rec, &back));
}

static void test_decode_rejects_null() {
    uint8_t rec[HISTORY_RECORD_BYTES] = {0};
    GrindHistoryEntry out;
    TEST_ASSERT_FALSE(history_decode_record(nullptr, &out));
    TEST_ASSERT_FALSE(history_decode_record(rec, nullptr));
    HistoryHeader h;
    TEST_ASSERT_FALSE(history_decode_header(nullptr, &h));
    TEST_ASSERT_FALSE(history_decode_header(rec, nullptr));
    TEST_ASSERT_FALSE(history_decode_header(rec, &h));  // all zeros: no magic
}

static void test_generation_order_wraps() {
    TEST_ASSERT_TRUE(history_generation_newer(2, 1));
    TEST_ASSERT_FALSE(history_generation_newer(1, 2));
    TEST_ASSERT_FALSE(history_generation_newer(5, 5));
    TEST_ASSERT_TRUE(history_generation_newer(0, 0xFFFFFFFFu));
    TEST_ASSERT_FALSE(history_generation_newer(0xFFFFFFFFu, 0));
    TEST_ASSERT_TRUE(history_generation_newer(3, 0xFFFFFFFEu));
}

// ---- epoch ----

static void test_epoch_plan() {
    HistoryEpochPlan p = history_epoch_plan(0x1234u, 0x9999u);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, p.epoch);
    TEST_ASSERT_FALSE(p.store);
    p = history_epoch_plan(0, 0x9999u);
    TEST_ASSERT_EQUAL_HEX32(0x9999u, p.epoch);
    TEST_ASSERT_TRUE(p.store);
    p = history_epoch_plan(0, 0);  // a random 0 must not become "no id"
    TEST_ASSERT_NOT_EQUAL(0u, p.epoch);
    TEST_ASSERT_TRUE(p.store);
}

static void test_factory_reset_retires_the_old_history() {
    Rig r;
    r.boot();
    for (uint32_t i = 1; i <= 20; i++) r.add(i);
    r.boot();
    TEST_ASSERT_EQUAL(10, r.ids().size());

    // Factory reset: NVS is erased, so the stored id is gone and a new random one is made
    const HistoryEpochPlan fresh = history_epoch_plan(0, 0x55AA55AAu);
    TEST_ASSERT_TRUE(fresh.store);
    r.epoch = fresh.epoch;
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL(0, r.ids().size());
    TEST_ASSERT_EQUAL_UINT8(2, result.foreign_removed);
    TEST_ASSERT_FALSE(r.files.present[0]);
    TEST_ASSERT_FALSE(r.files.present[1]);
    TEST_ASSERT_TRUE(r.state.persistent);
    // and the new history starts clean
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(1));
    r.boot();
    TEST_ASSERT_EQUAL(1, r.ids().size());
}

static void test_matching_epoch_keeps_the_files() {
    Rig r;
    r.boot();
    for (uint32_t i = 1; i <= 5; i++) r.add(i);
    const int removes_before = r.files.removes;
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL(removes_before, r.files.removes);
    TEST_ASSERT_EQUAL_UINT8(0, result.foreign_removed);
    TEST_ASSERT_EQUAL(5, r.ids().size());
}

static void test_one_foreign_file_goes_the_other_stays() {
    Rig r;
    r.files.put(0, file_bytes(3, 0x77777777u, range(1, 4)));  // another unit's file
    r.files.put(1, file_bytes(2, kEpoch, range(5, 7)));
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL_UINT8(1, result.foreign_removed);
    TEST_ASSERT_FALSE(r.files.present[0]);
    TEST_ASSERT_TRUE(r.files.present[1]);
    TEST_ASSERT_TRUE(newest_first_tail(range(5, 7)) == r.ids());
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
}

static void test_no_epoch_means_no_file_access() {
    Rig r;
    r.epoch = 0;
    r.files.put(0, file_bytes(1, kEpoch, range(1, 3)));
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL(0, r.files.reads);
    TEST_ASSERT_EQUAL(0, r.files.removes);
    TEST_ASSERT_EQUAL_UINT8(0, result.entries);
    TEST_ASSERT_TRUE(r.state.loaded);
    TEST_ASSERT_FALSE(r.state.persistent);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::NOT_AVAILABLE, r.add(1));
    TEST_ASSERT_EQUAL(0, r.files.appends + r.files.replaces);
}

static void test_append_before_load_is_not_available() {
    Rig r;  // state never loaded
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::NOT_AVAILABLE, r.add(1));
    TEST_ASSERT_EQUAL(0, r.files.appends + r.files.replaces);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::NOT_AVAILABLE, history_append(r.files, nullptr, r.scratch, entry_n(1)));
}

// ---- loader: torn tails, bit flips, header cases ----

static void test_torn_tail_at_every_length() {
    const std::vector<uint32_t> ids = {11, 12, 13};
    const std::vector<uint8_t> full = file_bytes(4, kEpoch, ids);
    for (size_t len = 0; len <= full.size(); len++) {
        Rig r;
        r.files.put(0, std::vector<uint8_t>(full.begin(), full.begin() + len));
        r.boot();
        size_t whole = len < HISTORY_HEADER_BYTES ? 0 : (len - HISTORY_HEADER_BYTES) / HISTORY_RECORD_BYTES;
        std::vector<uint32_t> expected(ids.begin(), ids.begin() + whole);
        char msg[48];
        snprintf(msg, sizeof(msg), "length %u", static_cast<unsigned>(len));
        TEST_ASSERT_TRUE_MESSAGE(newest_first_tail(expected) == r.ids(), msg);
        // a file with a header but a torn or missing record is usable but not appendable
        if (len >= HISTORY_HEADER_BYTES) {
            TEST_ASSERT_EQUAL_MESSAGE(len == HISTORY_HEADER_BYTES + whole * HISTORY_RECORD_BYTES, r.state.slot[0].clean, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(whole, r.state.slot[0].records, msg);
        } else {
            TEST_ASSERT_FALSE_MESSAGE(r.state.slot[0].valid, msg);
        }
    }
}

static void test_torn_tail_of_a_full_file_at_every_length() {
    const std::vector<uint8_t> full = file_bytes(9, kEpoch, range(1, 16));
    TEST_ASSERT_EQUAL(HISTORY_FILE_MAX_BYTES, full.size());
    for (size_t len = 0; len <= full.size(); len++) {
        Rig r;
        r.files.put(1, std::vector<uint8_t>(full.begin(), full.begin() + len));
        r.boot();
        const size_t whole = len < HISTORY_HEADER_BYTES ? 0 : (len - HISTORY_HEADER_BYTES) / HISTORY_RECORD_BYTES;
        std::vector<uint32_t> expected = range(1, static_cast<uint32_t>(whole));
        if (whole == 0) expected.clear();
        TEST_ASSERT_TRUE(newest_first_tail(expected) == r.ids());
    }
}

static void test_every_single_bit_flip_in_a_three_record_file() {
    const std::vector<uint32_t> ids = {21, 22, 23};
    const std::vector<uint8_t> good = file_bytes(6, kEpoch, ids);
    for (size_t bit = 0; bit < good.size() * 8; bit++) {
        std::vector<uint8_t> bad = good;
        bad[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        Rig r;
        r.files.put(0, bad);
        r.boot();
        size_t kept;  // records the loader may return
        if (bit / 8 < HISTORY_HEADER_BYTES) {
            kept = 0;  // header: the whole file is dropped
        } else {
            kept = (bit / 8 - HISTORY_HEADER_BYTES) / HISTORY_RECORD_BYTES;  // stops at the damaged record
        }
        std::vector<uint32_t> expected(ids.begin(), ids.begin() + kept);
        char msg[48];
        snprintf(msg, sizeof(msg), "bit %u", static_cast<unsigned>(bit));
        TEST_ASSERT_TRUE_MESSAGE(newest_first_tail(expected) == r.ids(), msg);
        if (kept < ids.size()) TEST_ASSERT_FALSE_MESSAGE(r.state.slot[0].clean, msg);  // not appendable
    }
}

static void test_a_record_after_a_bad_one_is_not_trusted() {
    std::vector<uint8_t> file = file_bytes(2, kEpoch, range(1, 6));
    file[HISTORY_HEADER_BYTES + 2 * HISTORY_RECORD_BYTES + 9] ^= 0x40;  // damage record 3 of 6
    Rig r;
    r.files.put(0, file);
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 2)) == r.ids());
}

static void test_header_variants_are_dropped() {
    auto with_header_field = [](size_t offset, uint16_t value) {
        std::vector<uint8_t> f = file_bytes(2, kEpoch, range(1, 3));
        f[offset] = static_cast<uint8_t>(value);
        f[offset + 1] = static_cast<uint8_t>(value >> 8);
        // recompute the header CRC so that only the field itself is wrong
        const uint32_t crc = history_crc32(f.data(), 28);
        for (int i = 0; i < 4; i++) f[28 + i] = static_cast<uint8_t>(crc >> (8 * i));
        return f;
    };
    std::vector<std::vector<uint8_t>> variants;
    variants.push_back(with_header_field(4, 2));    // version 2
    variants.push_back(with_header_field(4, 0));    // version 0
    variants.push_back(with_header_field(6, 16));   // record size 16
    variants.push_back(with_header_field(6, 64));   // record size 64
    variants.push_back(with_header_field(6, 0));
    std::vector<uint8_t> magic = file_bytes(2, kEpoch, range(1, 3));
    magic[0] = 'X';
    const uint32_t crc = history_crc32(magic.data(), 28);
    for (int i = 0; i < 4; i++) magic[28 + i] = static_cast<uint8_t>(crc >> (8 * i));
    variants.push_back(magic);                      // wrong magic with a matching CRC
    std::vector<uint8_t> zero_epoch = file_bytes(2, 0, range(1, 3));
    variants.push_back(zero_epoch);                 // epoch 0 is never valid
    for (size_t i = 0; i < variants.size(); i++) {
        Rig r;
        r.files.put(0, variants[i]);
        const HistoryLoadResult result = r.boot();
        TEST_ASSERT_EQUAL_MESSAGE(0, r.ids().size(), "variant must be dropped");
        TEST_ASSERT_EQUAL_UINT8(0, result.files_valid);
        TEST_ASSERT_EQUAL_UINT8(0, result.foreign_removed);  // damaged, not foreign: left for the next replace
        TEST_ASSERT_EQUAL_INT8(-1, r.state.active);
    }
}

static void test_both_files_invalid_starts_fresh() {
    Rig r;
    r.files.put(0, std::vector<uint8_t>(HISTORY_FILE_MAX_BYTES, 0xA5));
    r.files.put(1, std::vector<uint8_t>(10, 0x00));
    r.boot();
    TEST_ASSERT_EQUAL(0, r.ids().size());
    TEST_ASSERT_EQUAL_INT8(-1, r.state.active);
    TEST_ASSERT_TRUE(r.state.persistent);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(1));
    TEST_ASSERT_EQUAL(1, r.files.replaces);
    TEST_ASSERT_EQUAL_UINT32(1, generation_of(r.files.bytes[0]));
    r.boot();
    TEST_ASSERT_EQUAL(1, r.ids().size());
}

static void test_one_file_missing() {
    Rig r;
    r.files.put(1, file_bytes(5, kEpoch, range(1, 4)));  // slot 0 missing
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 4)) == r.ids());
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(5));
    TEST_ASSERT_EQUAL_UINT32(5, generation_of(r.files.bytes[1]));
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 5)) == r.ids());
}

static void test_generation_order_decides_the_order_of_the_files() {
    Rig r;
    r.files.put(0, file_bytes(7, kEpoch, range(1, 3)));   // older
    r.files.put(1, file_bytes(8, kEpoch, range(4, 6)));   // newer
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 6)) == r.ids());
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);

    Rig s;  // the same with the slots swapped
    s.files.put(1, file_bytes(7, kEpoch, range(1, 3)));
    s.files.put(0, file_bytes(8, kEpoch, range(4, 6)));
    s.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 6)) == s.ids());
    TEST_ASSERT_EQUAL_INT8(0, s.state.active);
}

static void test_generation_wrap_between_files() {
    Rig r;
    r.files.put(0, file_bytes(0xFFFFFFFFu, kEpoch, range(1, 16)));
    r.files.put(1, file_bytes(0, kEpoch, range(17, 18)));  // generation 0 follows 0xFFFFFFFF
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 18)) == r.ids());
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
    // the next rotation goes to slot 0 with generation 1 after the active file's 0 ... after it fills
    for (uint32_t id = 19; id <= 32; id++) TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(id));
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(33));
    TEST_ASSERT_EQUAL_UINT32(1, generation_of(r.files.bytes[0]));
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 33)) == r.ids());
}

static void test_equal_generations_are_deterministic() {
    Rig r;
    r.files.put(0, file_bytes(4, kEpoch, range(1, 3)));
    r.files.put(1, file_bytes(4, kEpoch, range(4, 6)));  // only reachable by damage
    r.boot();
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);  // a tie counts slot 1 as the newer
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 6)) == r.ids());
    // and the next rotation still produces a strictly newer generation than both
    for (uint32_t id = 7; id <= 19; id++) TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(id));  // slot 1: 3 + 13
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(20));
    TEST_ASSERT_TRUE(history_generation_newer(generation_of(r.files.bytes[0]), 4));
}

static void test_oversized_file_reads_only_sixteen_and_is_not_appendable() {
    std::vector<uint8_t> file = file_bytes(3, kEpoch, range(1, 20));  // 20 records: more than a file may hold
    TEST_ASSERT_TRUE(file.size() > HISTORY_FILE_MAX_BYTES);
    Rig r;
    r.files.put(0, file);
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 16)) == r.ids());
    TEST_ASSERT_EQUAL_UINT8(16, r.state.slot[0].records);
    TEST_ASSERT_FALSE(r.state.slot[0].clean);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(21));
}

static void test_full_file_with_trailing_garbage_is_not_appendable() {
    std::vector<uint8_t> file = file_bytes(3, kEpoch, range(1, 16));
    file.push_back(0x5A);
    Rig r;
    r.files.put(0, file);
    r.boot();
    TEST_ASSERT_EQUAL_UINT8(16, r.state.slot[0].records);
    TEST_ASSERT_FALSE(r.state.slot[0].clean);
}

static void test_records_beyond_the_ring_are_loaded_oldest_first() {
    Rig r;
    r.files.put(0, file_bytes(1, kEpoch, range(1, 16)));
    r.files.put(1, file_bytes(2, kEpoch, range(17, 30)));
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL_UINT8(30, result.entries);
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 30)) == r.ids());  // the newest ten of both files
    GrindHistoryEntry e[1];
    r.ring.copy_newest_first(e, 1);
    TEST_ASSERT_TRUE(e[0].earlier_boot);
}

static void test_duplicate_session_ids_keep_file_order() {
    Rig r;
    r.boot();
    for (uint32_t id : {5u, 5u, 6u, 5u}) r.add(id);
    r.boot();
    const std::vector<uint32_t> expected = {5, 6, 5, 5};
    TEST_ASSERT_TRUE(expected == r.ids());
}

static void test_session_id_wrap_does_not_reorder() {
    Rig r;
    r.boot();
    for (uint32_t id : {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u}) r.add(id);
    r.boot();
    const std::vector<uint32_t> expected = {1, 0, 0xFFFFFFFFu, 0xFFFFFFFEu};
    TEST_ASSERT_TRUE(expected == r.ids());
}

// ---- append and rotation ----

static void test_first_append_creates_slot_zero_generation_one() {
    Rig r;
    r.boot();
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(1));
    TEST_ASSERT_TRUE(r.files.present[0]);
    TEST_ASSERT_FALSE(r.files.present[1]);
    TEST_ASSERT_EQUAL(HISTORY_HEADER_BYTES + HISTORY_RECORD_BYTES, r.files.bytes[0].size());
    TEST_ASSERT_EQUAL_UINT32(1, generation_of(r.files.bytes[0]));
    TEST_ASSERT_TRUE(r.state.persistent);
}

static void test_sixteen_fill_a_file_the_seventeenth_rotates() {
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 16; id++) r.add(id);
    TEST_ASSERT_EQUAL(HISTORY_FILE_MAX_BYTES, r.files.bytes[0].size());
    TEST_ASSERT_FALSE(r.files.present[1]);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(17));
    TEST_ASSERT_EQUAL(HISTORY_FILE_MAX_BYTES, r.files.bytes[0].size());  // the full file is untouched
    TEST_ASSERT_EQUAL(HISTORY_HEADER_BYTES + HISTORY_RECORD_BYTES, r.files.bytes[1].size());
    TEST_ASSERT_EQUAL_UINT32(1, generation_of(r.files.bytes[0]));
    TEST_ASSERT_EQUAL_UINT32(2, generation_of(r.files.bytes[1]));
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
    // another 15 fill slot 1, the 33rd rotates back to slot 0 with generation 3
    for (uint32_t id = 18; id <= 32; id++) TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(id));
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(33));
    TEST_ASSERT_EQUAL_UINT32(3, generation_of(r.files.bytes[0]));
    TEST_ASSERT_EQUAL(HISTORY_HEADER_BYTES + HISTORY_RECORD_BYTES, r.files.bytes[0].size());
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 33)) == r.ids());
}

static void test_load_after_any_number_of_appends_reproduces_the_list() {
    for (uint32_t n = 0; n <= 100; n++) {
        Rig r;
        r.boot();
        for (uint32_t id = 1; id <= n; id++) r.add(id);
        // the list the running system would show ...
        GrindHistoryRing live;
        for (uint32_t id = 1; id <= n; id++) live.push(entry_n(id));
        GrindHistoryEntry a[GRIND_HISTORY_RING_CAPACITY];
        const uint8_t live_n = live.copy_newest_first(a, GRIND_HISTORY_RING_CAPACITY);
        // ... equals the list after a reboot
        r.boot();
        const std::vector<uint32_t> after = r.ids();
        TEST_ASSERT_EQUAL_UINT8(live_n, after.size());
        GrindHistoryEntry b[GRIND_HISTORY_RING_CAPACITY];
        r.ring.copy_newest_first(b, GRIND_HISTORY_RING_CAPACITY);
        for (uint8_t i = 0; i < live_n; i++) {
            TEST_ASSERT_EQUAL_UINT32(a[i].session_id, b[i].session_id);
            TEST_ASSERT_EQUAL_FLOAT(a[i].final_g, b[i].final_g);
            TEST_ASSERT_EQUAL_UINT8(a[i].profile, b[i].profile);
            TEST_ASSERT_EQUAL_UINT8(a[i].mode, b[i].mode);
            TEST_ASSERT_EQUAL_UINT32(a[i].time_ms, b[i].time_ms);
        }
    }
}

static void test_a_bad_tail_in_the_active_file_forces_a_rotation() {
    std::vector<uint8_t> file = file_bytes(5, kEpoch, range(1, 5));
    file.insert(file.end(), 13, 0xEE);  // 13 bytes of garbage after the 5th record
    const std::vector<uint8_t> before = file;
    Rig r;
    r.files.put(0, file);
    r.files.put(1, file_bytes(4, kEpoch, range(100, 110)));  // an older file with 11 records
    r.boot();
    TEST_ASSERT_EQUAL_INT8(0, r.state.active);
    TEST_ASSERT_FALSE(r.state.slot[0].clean);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(6));
    TEST_ASSERT_TRUE(before == r.files.bytes[0]);  // the damaged file is left as it is, its 5 records stay
    TEST_ASSERT_EQUAL_UINT32(6, generation_of(r.files.bytes[1]));
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 6)) == r.ids());
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
}

static void test_an_active_file_without_a_valid_record_is_replaced_in_place() {
    std::vector<uint8_t> torn = file_bytes(9, kEpoch, {});  // header only ...
    torn.insert(torn.end(), 20, 0x11);                      // ... and a torn first record
    const std::vector<uint8_t> older = file_bytes(8, kEpoch, range(1, 16));
    Rig r;
    r.files.put(0, older);
    r.files.put(1, torn);
    r.boot();
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);
    TEST_ASSERT_EQUAL_UINT8(0, r.state.slot[1].records);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(17));
    TEST_ASSERT_TRUE(older == r.files.bytes[0]);  // the full older file is never touched
    TEST_ASSERT_EQUAL_UINT32(9, generation_of(r.files.bytes[1]));  // same generation, so it stays the newer file
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 17)) == r.ids());
}

static void test_header_only_active_file_is_appended_to() {
    Rig r;
    r.files.put(0, file_bytes(2, kEpoch, {}));  // a valid, clean file with no record yet
    r.boot();
    TEST_ASSERT_TRUE(r.state.slot[0].clean);
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(1));
    r.boot();
    TEST_ASSERT_EQUAL(1, r.ids().size());
}

static void test_invalid_entries_are_rejected_without_a_write() {
    Rig r;
    r.boot();
    GrindHistoryEntry bad = entry_n(1);
    bad.final_g = std::numeric_limits<float>::quiet_NaN();
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REJECTED, history_append(r.files, &r.state, r.scratch, bad));
    TEST_ASSERT_EQUAL(0, r.files.appends + r.files.replaces);
    TEST_ASSERT_TRUE(r.state.persistent);  // a rejected entry says nothing about the files
}

// ---- power cut at every byte ----

// A scripted history: each step is one grind; cut >= 0 cuts the power after that many bytes of the step's write (then the
// system reboots: the state is rebuilt from the files). All ids attempted are listed in order with a flag for cut ones.
struct Step {
    uint32_t id;
    long cut;
};

// Checks the reloaded list against the candidates that a correct store may show: the completed grinds, each cut grind
// present or absent. Returns false with a message when none matches.
static bool check_script(const std::vector<Step>& script, char* msg, size_t msg_size) {
    Rig r;
    r.boot();
    for (const Step& step : script) {
        if (step.cut >= 0) r.files.budget = step.cut;
        r.add(step.id);
        if (step.cut >= 0) {
            r.files.revive();
            r.boot();  // power back: the store starts from what is on flash
        }
    }
    r.boot();

    std::vector<size_t> cut_steps;
    for (size_t i = 0; i < script.size(); i++) {
        if (script[i].cut >= 0) cut_steps.push_back(i);
    }
    for (uint32_t mask = 0; mask < (1u << cut_steps.size()); mask++) {
        std::vector<uint32_t> sequence;
        for (size_t i = 0; i < script.size(); i++) {
            bool included = true;
            for (size_t k = 0; k < cut_steps.size(); k++) {
                if (cut_steps[k] == i) included = (mask >> k) & 1u;
            }
            if (included) sequence.push_back(script[i].id);
        }
        if (newest_first_tail(sequence) == r.ids()) return true;
    }
    snprintf(msg, msg_size, "script of %u steps, first cut step %u at %ld bytes: list differs",
             static_cast<unsigned>(script.size()), static_cast<unsigned>(cut_steps.empty() ? 0 : cut_steps[0]),
             cut_steps.empty() ? 0L : script[cut_steps[0]].cut);
    return false;
}

static void test_power_cut_at_every_byte_of_every_write_up_to_the_third_rotation() {
    char msg[128];
    for (uint32_t completed = 0; completed <= 70; completed++) {
        for (long cut = 0; cut <= 64; cut++) {
            std::vector<Step> script;
            for (uint32_t id = 1; id <= completed; id++) script.push_back({id, -1});
            script.push_back({completed + 1, cut});
            TEST_ASSERT_TRUE_MESSAGE(check_script(script, msg, sizeof(msg)), msg);
        }
    }
}

static void test_the_newest_ten_survive_a_cut_during_any_rotation() {
    // Stronger than the candidate check above: with 16 or more grinds completed, a cut during the next write never takes
    // away any of the newest ten completed grinds, whatever the byte.
    for (uint32_t completed = 16; completed <= 80; completed++) {
        for (long cut = 0; cut <= 64; cut++) {
            Rig r;
            r.boot();
            for (uint32_t id = 1; id <= completed; id++) r.add(id);
            r.files.budget = cut;
            r.add(completed + 1);
            r.files.revive();
            r.boot();
            const std::vector<uint32_t> ids = r.ids();
            // the 9 newest completed grinds are there, in order, under the cut one if it landed (else the 10th is too)
            std::vector<uint32_t> tail = newest_first_tail(range(1, completed), 9);
            size_t offset = (!ids.empty() && ids[0] == completed + 1) ? 1 : 0;
            TEST_ASSERT_TRUE(ids.size() >= 9 + offset);
            for (size_t i = 0; i < 9; i++) TEST_ASSERT_EQUAL_UINT32(tail[i], ids[offset + i]);
        }
    }
}

static void test_two_power_cuts_in_a_row() {
    char msg[128];
    const uint32_t fills[] = {0, 1, 15, 16, 31, 32};
    for (uint32_t completed : fills) {
        for (long cut1 = 0; cut1 <= 64; cut1++) {
            for (uint32_t between = 0; between <= 2; between++) {
                for (long cut2 = 0; cut2 <= 64; cut2++) {
                    std::vector<Step> script;
                    uint32_t id = 1;
                    for (; id <= completed; id++) script.push_back({id, -1});
                    script.push_back({id++, cut1});
                    for (uint32_t b = 0; b < between; b++) script.push_back({id++, -1});
                    script.push_back({id, cut2});
                    TEST_ASSERT_TRUE_MESSAGE(check_script(script, msg, sizeof(msg)), msg);
                }
            }
        }
    }
}

static void test_a_torn_rotation_does_not_cost_the_old_file() {
    // 16 grinds fill slot 0. The 17th write is cut after the header of the new file only (32 bytes, the record missing).
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 16; id++) r.add(id);
    r.files.budget = 40;
    r.add(17);
    r.files.revive();
    r.boot();
    TEST_ASSERT_EQUAL_INT8(1, r.state.active);  // the torn file has the newer generation ...
    TEST_ASSERT_EQUAL_UINT8(0, r.state.slot[1].records);
    TEST_ASSERT_TRUE(newest_first_tail(range(1, 16)) == r.ids());  // ... and the full file is still all there
    // the next grind must not truncate the full file: it replaces the torn one
    const std::vector<uint8_t> full_before = r.files.bytes[0];
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(18));
    TEST_ASSERT_TRUE(full_before == r.files.bytes[0]);
    r.boot();
    std::vector<uint32_t> expected = range(1, 16);  // 17 was cut and is gone
    expected.push_back(18);
    TEST_ASSERT_TRUE(newest_first_tail(expected) == r.ids());
}

// ---- failure injection ----

static void test_failed_append_without_effect_then_recovery() {
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 3; id++) r.add(id);
    r.files.fail = FakeFiles::CLEAN;
    r.files.fail_count = 1;
    const int writes_before = r.files.appends + r.files.replaces;
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::FAILED, r.add(4));
    TEST_ASSERT_FALSE(r.state.persistent);
    TEST_ASSERT_EQUAL(1, r.files.appends + r.files.replaces - writes_before);  // one attempt, no retry loop
    TEST_ASSERT_TRUE(r.state.slot[0].clean);                                    // re-read: nothing was written
    // the next grind appends normally and persistence is back
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::APPENDED, r.add(5));
    TEST_ASSERT_TRUE(r.state.persistent);
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail({1, 2, 3, 5}) == r.ids());
}

static void test_short_append_leaves_a_torn_tail_that_is_never_appended_to() {
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 3; id++) r.add(id);
    r.files.fail = FakeFiles::SHORT;
    r.files.fail_count = 1;
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::FAILED, r.add(4));
    TEST_ASSERT_FALSE(r.state.persistent);
    TEST_ASSERT_EQUAL(HISTORY_HEADER_BYTES + 3 * HISTORY_RECORD_BYTES + 16, r.files.bytes[0].size());
    TEST_ASSERT_FALSE(r.state.slot[0].clean);
    TEST_ASSERT_EQUAL_UINT8(3, r.state.slot[0].records);
    // the next grind does not write behind the torn record: it rotates to the other file
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::ROTATED, r.add(5));
    TEST_ASSERT_TRUE(r.state.persistent);
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail({1, 2, 3, 5}) == r.ids());
}

static void test_failed_rotation_taints_the_target_and_the_next_write_restarts_it() {
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 16; id++) r.add(id);
    r.files.fail = FakeFiles::SHORT;  // the replace truncates, then only half of header + record lands
    r.files.fail_count = 1;
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::FAILED, r.add(17));
    TEST_ASSERT_FALSE(r.state.persistent);
    TEST_ASSERT_EQUAL_INT8(1, r.state.taint);
    TEST_ASSERT_EQUAL_INT8(0, r.state.active);
    const std::vector<uint8_t> full = r.files.bytes[0];
    TEST_ASSERT_EQUAL(HistoryAppendOutcome::REPLACED, r.add(18));
    TEST_ASSERT_TRUE(r.state.persistent);
    TEST_ASSERT_EQUAL_INT8(-1, r.state.taint);
    TEST_ASSERT_TRUE(full == r.files.bytes[0]);  // the full file was never touched
    TEST_ASSERT_TRUE(history_generation_newer(generation_of(r.files.bytes[1]), generation_of(r.files.bytes[0])));
    r.boot();
    std::vector<uint32_t> expected = range(1, 16);  // 17 failed: it is missing, 16 and older are intact
    expected.push_back(18);
    TEST_ASSERT_TRUE(newest_first_tail(expected) == r.ids());
}

static void test_failing_every_write_never_loops_or_crashes() {
    Rig r;
    r.boot();
    r.files.fail = FakeFiles::CLEAN;
    r.files.fail_count = 1000000;
    for (uint32_t id = 1; id <= 50; id++) {
        const int before = r.files.appends + r.files.replaces;
        TEST_ASSERT_EQUAL(HistoryAppendOutcome::FAILED, r.add(id));
        TEST_ASSERT_FALSE(r.state.persistent);
        TEST_ASSERT_TRUE(r.files.appends + r.files.replaces - before <= 1);  // at most one write per grind
    }
    r.files.fail_count = 0;  // the flash works again
    TEST_ASSERT_NOT_EQUAL(HistoryAppendOutcome::FAILED, r.add(51));
    TEST_ASSERT_TRUE(r.state.persistent);
    r.boot();
    TEST_ASSERT_TRUE(newest_first_tail({51}) == r.ids());
}

static void test_unreadable_files_at_boot_are_an_empty_history() {
    Rig r;
    r.files.put(0, file_bytes(1, kEpoch, range(1, 3)));
    r.files.dead = true;  // every read fails
    const HistoryLoadResult result = r.boot();
    TEST_ASSERT_EQUAL_UINT8(0, result.files_valid);
    TEST_ASSERT_EQUAL(0, r.ids().size());
    TEST_ASSERT_TRUE(r.state.loaded);
}

// ---- wear (the plan's numbers, section 3.5) ----

static void test_wear_one_write_per_grind_and_the_plans_erase_budget() {
    Rig r;
    r.boot();
    const int reads_after_boot = r.files.reads;
    constexpr int kGrinds = 160;
    for (uint32_t id = 1; id <= kGrinds; id++) r.add(id);
    // exactly one storage write per grind, a replace only for the first file and every 16th grind after the first 16
    TEST_ASSERT_EQUAL(kGrinds, r.files.appends + r.files.replaces);
    TEST_ASSERT_EQUAL(10, r.files.replaces);   // 160 / 16
    TEST_ASSERT_EQUAL(150, r.files.appends);
    TEST_ASSERT_EQUAL(0, r.files.removes);     // steady state deletes nothing
    TEST_ASSERT_EQUAL(reads_after_boot, r.files.reads);  // and reads nothing
    TEST_ASSERT_EQUAL(150 * 32 + 10 * 64, static_cast<int>(r.files.bytes_written));

    // The plan's estimate, in tenths of an erase: an append costs at most 1.5 block erases, a replace about 2
    const int erases_x10 = r.files.appends * 15 + r.files.replaces * 20;
    TEST_ASSERT_TRUE(erases_x10 <= kGrinds * 20);       // at most 2.0 erases per grind on average
    TEST_ASSERT_EQUAL(2450, erases_x10);                // 1.53 per grind for this pattern
    // 20 grinds a day: about 40 erases a day (plan: 15 000 a year)
    const int per_day_x10 = 20 * 20;                    // 2.0 erases per grind * 20 grinds, in tenths
    TEST_ASSERT_TRUE(per_day_x10 / 10 <= 40);
    TEST_ASSERT_TRUE((per_day_x10 / 10) * 365 <= 15000);
    // Even if wear levelling failed and only 16 blocks took everything: erases per block and year at 20 and 100 grinds a
    // day, against a 100 000 cycle rating (plan: over 100 years and over 20 years)
    const long per_block_20 = (20L * 2 * 365 + 15) / 16;
    const long per_block_100 = (100L * 2 * 365 + 15) / 16;
    TEST_ASSERT_TRUE(100000L / per_block_20 >= 100);
    TEST_ASSERT_TRUE(100000L / per_block_100 >= 20);
}

static void test_a_file_never_outgrows_the_documented_size() {
    Rig r;
    r.boot();
    for (uint32_t id = 1; id <= 500; id++) {
        r.add(id);
        TEST_ASSERT_TRUE(r.files.bytes[0].size() <= HISTORY_FILE_MAX_BYTES);
        TEST_ASSERT_TRUE(r.files.bytes[1].size() <= HISTORY_FILE_MAX_BYTES);
    }
}

void run_grind_history_store_tests() {
    RUN_TEST(test_crc32_known_vectors);
    RUN_TEST(test_record_layout_is_pinned);
    RUN_TEST(test_header_layout_is_pinned);
    RUN_TEST(test_record_round_trip_extremes);
    RUN_TEST(test_non_finite_weights_are_encoded_but_rejected_on_load);
    RUN_TEST(test_out_of_range_enums_are_rejected_even_with_a_good_crc);
    RUN_TEST(test_decode_rejects_null);
    RUN_TEST(test_generation_order_wraps);
    RUN_TEST(test_epoch_plan);
    RUN_TEST(test_factory_reset_retires_the_old_history);
    RUN_TEST(test_matching_epoch_keeps_the_files);
    RUN_TEST(test_one_foreign_file_goes_the_other_stays);
    RUN_TEST(test_no_epoch_means_no_file_access);
    RUN_TEST(test_append_before_load_is_not_available);
    RUN_TEST(test_torn_tail_at_every_length);
    RUN_TEST(test_torn_tail_of_a_full_file_at_every_length);
    RUN_TEST(test_every_single_bit_flip_in_a_three_record_file);
    RUN_TEST(test_a_record_after_a_bad_one_is_not_trusted);
    RUN_TEST(test_header_variants_are_dropped);
    RUN_TEST(test_both_files_invalid_starts_fresh);
    RUN_TEST(test_one_file_missing);
    RUN_TEST(test_generation_order_decides_the_order_of_the_files);
    RUN_TEST(test_generation_wrap_between_files);
    RUN_TEST(test_equal_generations_are_deterministic);
    RUN_TEST(test_oversized_file_reads_only_sixteen_and_is_not_appendable);
    RUN_TEST(test_full_file_with_trailing_garbage_is_not_appendable);
    RUN_TEST(test_records_beyond_the_ring_are_loaded_oldest_first);
    RUN_TEST(test_duplicate_session_ids_keep_file_order);
    RUN_TEST(test_session_id_wrap_does_not_reorder);
    RUN_TEST(test_first_append_creates_slot_zero_generation_one);
    RUN_TEST(test_sixteen_fill_a_file_the_seventeenth_rotates);
    RUN_TEST(test_load_after_any_number_of_appends_reproduces_the_list);
    RUN_TEST(test_a_bad_tail_in_the_active_file_forces_a_rotation);
    RUN_TEST(test_an_active_file_without_a_valid_record_is_replaced_in_place);
    RUN_TEST(test_header_only_active_file_is_appended_to);
    RUN_TEST(test_invalid_entries_are_rejected_without_a_write);
    RUN_TEST(test_power_cut_at_every_byte_of_every_write_up_to_the_third_rotation);
    RUN_TEST(test_the_newest_ten_survive_a_cut_during_any_rotation);
    RUN_TEST(test_two_power_cuts_in_a_row);
    RUN_TEST(test_a_torn_rotation_does_not_cost_the_old_file);
    RUN_TEST(test_failed_append_without_effect_then_recovery);
    RUN_TEST(test_short_append_leaves_a_torn_tail_that_is_never_appended_to);
    RUN_TEST(test_failed_rotation_taints_the_target_and_the_next_write_restarts_it);
    RUN_TEST(test_failing_every_write_never_loops_or_crashes);
    RUN_TEST(test_unreadable_files_at_boot_are_an_empty_history);
    RUN_TEST(test_wear_one_write_per_grind_and_the_plans_erase_budget);
    RUN_TEST(test_a_file_never_outgrows_the_documented_size);
}

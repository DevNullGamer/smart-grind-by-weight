#include "grind_history_store.h"
#include <string.h>

namespace {

constexpr uint8_t kMagic[4] = {'G', 'H', 'S', 'T'};
constexpr uint32_t kFallbackEpoch = 0x47485354u;  // used if the random candidate happens to be 0

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void put_f32(uint8_t* p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

float get_f32(const uint8_t* p) {
    const uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// What a file holds, judged from its bytes alone
struct ParsedFile {
    bool header_ok = false;  // right header, our epoch
    bool foreign = false;    // a good header of another epoch
    HistoryHeader header;
    uint8_t records = 0;     // whole valid records from the start (at most HISTORY_FILE_RECORDS)
    bool clean = false;      // nothing after those records
};

// n comes from the storage adapter and is checked against the buffer size here; no length stored in the file is used.
// With a ring, the valid records are pushed oldest first, flagged earlier_boot.
ParsedFile parse_file(const uint8_t* buf, int32_t n, uint32_t epoch, GrindHistoryRing* ring) {
    ParsedFile parsed;
    if (!buf || n < static_cast<int32_t>(HISTORY_HEADER_BYTES) || static_cast<size_t>(n) > HISTORY_SCRATCH_BYTES) {
        return parsed;
    }
    HistoryHeader header;
    if (!history_decode_header(buf, &header)) return parsed;
    if (header.epoch != epoch) {
        parsed.foreign = true;
        return parsed;
    }
    parsed.header_ok = true;
    parsed.header = header;

    size_t whole = (static_cast<size_t>(n) - HISTORY_HEADER_BYTES) / HISTORY_RECORD_BYTES;
    if (whole > HISTORY_FILE_RECORDS) whole = HISTORY_FILE_RECORDS;
    size_t good = 0;
    while (good < whole) {
        GrindHistoryEntry entry;
        if (!history_decode_record(buf + HISTORY_HEADER_BYTES + good * HISTORY_RECORD_BYTES, &entry)) break;
        if (ring) {
            entry.earlier_boot = true;
            ring->push(entry);
        }
        good++;
    }
    parsed.records = static_cast<uint8_t>(good);
    parsed.clean = static_cast<size_t>(n) == HISTORY_HEADER_BYTES + good * HISTORY_RECORD_BYTES;
    return parsed;
}

HistorySlotInfo slot_info(const ParsedFile& parsed) {
    HistorySlotInfo info;
    info.valid = parsed.header_ok;
    info.clean = parsed.header_ok && parsed.clean;
    info.records = parsed.header_ok ? parsed.records : 0;
    info.generation = parsed.header_ok ? parsed.header.generation : 0;
    return info;
}

// The valid slot with the newest generation (a tie counts slot 1 as newer: deterministic, and only reachable by damage)
int8_t newest_valid_slot(const HistoryStoreState& state) {
    const HistorySlotInfo& a = state.slot[0];
    const HistorySlotInfo& b = state.slot[1];
    if (a.valid && b.valid) return history_generation_newer(a.generation, b.generation) ? 0 : 1;
    if (a.valid) return 0;
    if (b.valid) return 1;
    return -1;
}

// A generation newer than any a file of ours could carry: the valid slots and a tainted slot count (its header may be
// on flash even though the replace did not finish)
uint32_t generation_above_all(const HistoryStoreState& state) {
    bool any = false;
    uint32_t top = 0;
    for (uint8_t s = 0; s < HISTORY_SLOT_COUNT; s++) {
        if (!state.slot[s].valid && state.taint != static_cast<int8_t>(s)) continue;
        if (!any || history_generation_newer(state.slot[s].generation, top)) top = state.slot[s].generation;
        any = true;
    }
    return any ? top + 1u : 1u;
}

// Header + one record in scratch, written with replace(). On success the slot is a clean one-record file.
bool replace_with_first_record(HistoryFiles& files, HistoryStoreState* state, uint8_t* scratch, uint8_t slot,
                               uint32_t generation, const uint8_t* record) {
    HistoryHeader header;
    header.generation = generation;
    header.epoch = state->epoch;
    history_encode_header(header, scratch);
    memcpy(scratch + HISTORY_HEADER_BYTES, record, HISTORY_RECORD_BYTES);
    const bool ok = files.replace(slot, scratch, HISTORY_HEADER_BYTES + HISTORY_RECORD_BYTES);

    HistorySlotInfo& info = state->slot[slot];
    info.generation = generation;  // also after a failure: a torn file may carry it
    if (ok) {
        info.valid = true;
        info.clean = true;
        info.records = 1;
        if (state->taint == static_cast<int8_t>(slot)) state->taint = -1;
    } else {
        info.valid = false;
        info.clean = false;
        info.records = 0;
        state->taint = static_cast<int8_t>(slot);
    }
    state->active = newest_valid_slot(*state);
    return ok;
}

}  // namespace

uint32_t history_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; data && i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void history_encode_record(const GrindHistoryEntry& entry, uint8_t out[HISTORY_RECORD_BYTES]) {
    put_u32(out + 4, entry.session_id);
    put_u32(out + 8, entry.ended_s);
    put_u32(out + 12, entry.time_ms);
    put_u32(out + 16, entry.motor_ms);
    put_f32(out + 20, entry.target);
    put_f32(out + 24, entry.final_g);
    out[28] = entry.profile;
    out[29] = entry.mode;
    out[30] = entry.pulses;
    out[31] = entry.result;
    put_u32(out, history_crc32(out + 4, HISTORY_RECORD_BYTES - 4));
}

bool history_decode_record(const uint8_t in[HISTORY_RECORD_BYTES], GrindHistoryEntry* out) {
    if (!in || !out) return false;
    if (get_u32(in) != history_crc32(in + 4, HISTORY_RECORD_BYTES - 4)) return false;
    GrindHistoryEntry entry;
    entry.session_id = get_u32(in + 4);
    entry.ended_s = get_u32(in + 8);
    entry.time_ms = get_u32(in + 12);
    entry.motor_ms = get_u32(in + 16);
    entry.target = get_f32(in + 20);
    entry.final_g = get_f32(in + 24);
    entry.profile = in[28];
    entry.mode = in[29];
    entry.pulses = in[30];
    entry.result = in[31];
    if (!grind_history_entry_valid(entry)) return false;
    *out = entry;
    return true;
}

void history_encode_header(const HistoryHeader& header, uint8_t out[HISTORY_HEADER_BYTES]) {
    memset(out, 0, HISTORY_HEADER_BYTES);
    memcpy(out, kMagic, sizeof(kMagic));
    put_u16(out + 4, HISTORY_FORMAT_VERSION);
    put_u16(out + 6, static_cast<uint16_t>(HISTORY_RECORD_BYTES));
    put_u32(out + 8, header.generation);
    put_u32(out + 12, header.epoch);
    put_u32(out + 28, history_crc32(out, HISTORY_HEADER_BYTES - 4));
}

bool history_decode_header(const uint8_t in[HISTORY_HEADER_BYTES], HistoryHeader* out) {
    if (!in || !out) return false;
    if (memcmp(in, kMagic, sizeof(kMagic)) != 0) return false;
    if (get_u16(in + 4) != HISTORY_FORMAT_VERSION || get_u16(in + 6) != HISTORY_RECORD_BYTES) return false;
    if (get_u32(in + 28) != history_crc32(in, HISTORY_HEADER_BYTES - 4)) return false;
    HistoryHeader header;
    header.generation = get_u32(in + 8);
    header.epoch = get_u32(in + 12);
    if (header.epoch == 0) return false;
    *out = header;
    return true;
}

bool history_generation_newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

HistoryEpochPlan history_epoch_plan(uint32_t stored, uint32_t random_candidate) {
    HistoryEpochPlan plan;
    if (stored != 0) {
        plan.epoch = stored;
        return plan;
    }
    plan.epoch = random_candidate != 0 ? random_candidate : kFallbackEpoch;
    plan.store = true;
    return plan;
}

HistoryLoadResult history_load(HistoryFiles& files, uint32_t epoch, uint8_t* scratch, HistoryStoreState* state,
                               GrindHistoryRing* ring) {
    HistoryLoadResult result;
    if (!state) return result;
    *state = HistoryStoreState();
    state->epoch = epoch;
    state->loaded = true;
    if (epoch == 0 || !scratch) return result;  // no usable id: RAM only
    state->persistent = true;

    // Pass 1: what each file is (headers and record counts); a file of another epoch is deleted
    for (uint8_t s = 0; s < HISTORY_SLOT_COUNT; s++) {
        const int32_t n = files.read_all(s, scratch, HISTORY_SCRATCH_BYTES);
        const ParsedFile parsed = parse_file(scratch, n, epoch, nullptr);
        if (parsed.foreign && files.remove(s)) result.foreign_removed++;
        state->slot[s] = slot_info(parsed);
    }
    state->active = newest_valid_slot(*state);

    // Pass 2: the older file's records first, then the newest file's
    if (state->active >= 0) {
        const uint8_t newest = static_cast<uint8_t>(state->active);
        const uint8_t order[HISTORY_SLOT_COUNT] = {static_cast<uint8_t>(1 - newest), newest};
        for (uint8_t i = 0; i < HISTORY_SLOT_COUNT; i++) {
            const uint8_t s = order[i];
            if (!state->slot[s].valid) continue;
            const int32_t n = files.read_all(s, scratch, HISTORY_SCRATCH_BYTES);
            const ParsedFile parsed = parse_file(scratch, n, epoch, ring);
            state->slot[s] = slot_info(parsed);
            result.entries = static_cast<uint8_t>(result.entries + state->slot[s].records);
        }
        state->active = newest_valid_slot(*state);
    }
    for (uint8_t s = 0; s < HISTORY_SLOT_COUNT; s++) {
        if (state->slot[s].valid) result.files_valid++;
    }
    return result;
}

HistoryAppendOutcome history_append(HistoryFiles& files, HistoryStoreState* state, uint8_t* scratch,
                                    const GrindHistoryEntry& entry) {
    if (!state || !scratch || !state->loaded || state->epoch == 0) return HistoryAppendOutcome::NOT_AVAILABLE;
    if (!grind_history_entry_valid(entry)) return HistoryAppendOutcome::REJECTED;

    uint8_t record[HISTORY_RECORD_BYTES];
    history_encode_record(entry, record);

    bool ok = false;
    HistoryAppendOutcome outcome = HistoryAppendOutcome::REPLACED;

    if (state->taint >= 0) {
        // The last replace of this slot did not finish: whatever it holds is unknown, start it over (newest generation)
        const uint8_t slot = static_cast<uint8_t>(state->taint);
        ok = replace_with_first_record(files, state, scratch, slot, generation_above_all(*state), record);
    } else if (state->active < 0) {
        // No usable file at all: nothing to protect
        ok = replace_with_first_record(files, state, scratch, 0, generation_above_all(*state), record);
    } else {
        const uint8_t active = static_cast<uint8_t>(state->active);
        const HistorySlotInfo info = state->slot[active];
        if (info.clean && info.records < HISTORY_FILE_RECORDS) {
            outcome = HistoryAppendOutcome::APPENDED;
            ok = files.append(active, record, HISTORY_RECORD_BYTES);
            if (ok) {
                state->slot[active].records = static_cast<uint8_t>(info.records + 1);
            } else {
                // Unknown how much of the record reached the file: look at it again instead of guessing
                const int32_t n = files.read_all(active, scratch, HISTORY_SCRATCH_BYTES);
                const ParsedFile parsed = parse_file(scratch, n, state->epoch, nullptr);
                if (parsed.header_ok && parsed.header.generation == info.generation) {
                    state->slot[active] = slot_info(parsed);
                } else {
                    state->slot[active].valid = false;
                    state->slot[active].clean = false;
                    state->slot[active].records = 0;
                    state->taint = static_cast<int8_t>(active);
                    state->active = newest_valid_slot(*state);
                }
            }
        } else if (info.records == 0) {
            // Full is impossible here; this file has no valid record (an interrupted replace): nothing in it to lose
            outcome = HistoryAppendOutcome::REPLACED;
            ok = replace_with_first_record(files, state, scratch, active, info.generation, record);
        } else {
            // Full, or damaged after some valid records: leave this file alone and replace the other (the older)
            // one, so the newest records stay on flash while it is being rewritten
            outcome = HistoryAppendOutcome::ROTATED;
            const uint8_t other = static_cast<uint8_t>(1 - active);
            ok = replace_with_first_record(files, state, scratch, other, info.generation + 1u, record);
        }
    }

    state->persistent = ok;
    return ok ? outcome : HistoryAppendOutcome::FAILED;
}

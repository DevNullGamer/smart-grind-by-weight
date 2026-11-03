#pragma once
#include <stddef.h>
#include <stdint.h>
#include "grind_history.h"

// Crash-safe history store (chg016): a two-file rotating record log, written against an abstract file interface so the
// logic is host-tested with injected faults. Pure: no Arduino, no filesystem, no heap, no logging. The design, the
// crash-safety argument and the wear estimate are in docs/DESIGN-NOTES.md "Persistent history (chg016)".
//
// File = 32-byte header + up to GRIND_HISTORY_FILE_RECORDS records of 32 bytes, all integers little-endian.
//   header: "GHST" | u16 version=1 | u16 record_size=32 | u32 generation | u32 epoch | 12 zero bytes | u32 crc32(bytes 0..27)
//   record: u32 crc32(bytes 4..31) | u32 session_id | u32 ended_s | u32 time_ms | u32 motor_ms | f32 target |
//           f32 final_g | u8 profile | u8 mode | u8 pulses | u8 result
// A file is trusted up to its first bad record; nothing after a bad record is read.

constexpr size_t HISTORY_HEADER_BYTES = 32;
constexpr size_t HISTORY_RECORD_BYTES = 32;
constexpr size_t HISTORY_FILE_RECORDS = GRIND_HISTORY_FILE_RECORDS;
constexpr size_t HISTORY_FILE_MAX_BYTES = HISTORY_HEADER_BYTES + HISTORY_FILE_RECORDS * HISTORY_RECORD_BYTES;  // 544
// Buffer every store call needs: one byte more than the largest valid file, so an oversized file is noticed
constexpr size_t HISTORY_SCRATCH_BYTES = HISTORY_FILE_MAX_BYTES + 1;
constexpr uint16_t HISTORY_FORMAT_VERSION = 1;
constexpr uint8_t HISTORY_SLOT_COUNT = 2;
static_assert(HISTORY_FILE_MAX_BYTES == 544, "history file size changed: update docs/DESIGN-NOTES.md and the wear estimate");

// CRC-32 (IEEE 802.3, the zlib one); crc32("123456789") == 0xCBF43926
uint32_t history_crc32(const uint8_t* data, size_t len);

// ---- codec ----
void history_encode_record(const GrindHistoryEntry& entry, uint8_t out[HISTORY_RECORD_BYTES]);
// True only when the CRC matches and the fields are in range (grind_history_entry_valid); *out is then the entry
// (earlier_boot false). *out is untouched on false.
bool history_decode_record(const uint8_t in[HISTORY_RECORD_BYTES], GrindHistoryEntry* out);

struct HistoryHeader {
    uint32_t generation = 0;
    uint32_t epoch = 0;
};
void history_encode_header(const HistoryHeader& header, uint8_t out[HISTORY_HEADER_BYTES]);
// True only for magic, version, record size and CRC all right and a non-zero epoch
bool history_decode_header(const uint8_t in[HISTORY_HEADER_BYTES], HistoryHeader* out);

// Generation order with wrap-around (0xFFFFFFFF is followed by 0): is a newer than b?
bool history_generation_newer(uint32_t a, uint32_t b);

// ---- epoch ----
// The history is tied to a random id kept in NVS. A factory reset erases NVS, so after it the files carry a foreign
// id and are discarded. stored 0 = no id yet: use the candidate (a random number; 0 is replaced by a constant) and
// store it. A stored id is kept as it is.
struct HistoryEpochPlan {
    uint32_t epoch = 0;
    bool store = false;  // the epoch is new and has to be written to NVS
};
HistoryEpochPlan history_epoch_plan(uint32_t stored, uint32_t random_candidate);

// ---- storage interface (the firmware adapter is the only code that touches LittleFS) ----
class HistoryFiles {
public:
    virtual ~HistoryFiles() {}
    // Reads file `slot` (0 or 1) into buf, at most cap bytes. Returns the number of bytes read, -1 when the file is
    // missing or unreadable.
    virtual int32_t read_all(uint8_t slot, uint8_t* buf, size_t cap) = 0;
    // Appends len bytes to the file (created if missing). True only when all of them were written.
    virtual bool append(uint8_t slot, const uint8_t* data, size_t len) = 0;
    // Replaces the file's content (truncate, then write). True only when all of it was written.
    virtual bool replace(uint8_t slot, const uint8_t* data, size_t len) = 0;
    // Deletes the file. True when it is gone afterwards (also when it did not exist).
    virtual bool remove(uint8_t slot) = 0;
};

// ---- store state (RAM, owned by the file-IO task) ----
struct HistorySlotInfo {
    bool valid = false;        // header trusted (right magic/version/size/CRC and our epoch)
    bool clean = false;        // exactly header + `records` whole records, nothing after them
    uint8_t records = 0;       // whole valid records from the start
    uint32_t generation = 0;
};

struct HistoryStoreState {
    uint32_t epoch = 0;        // 0 = no usable id: nothing is read or written
    bool loaded = false;
    bool persistent = false;   // the last file operation worked (or none was needed yet)
    HistorySlotInfo slot[HISTORY_SLOT_COUNT];
    int8_t active = -1;        // slot appended to: the valid one with the newest generation, -1 = none
    int8_t taint = -1;         // slot whose last write failed: its content is unknown, the next write replaces it
    // The taint comes from a failed append whose re-read also failed: the slot may still hold valid records, so the next
    // write goes to the other file when that is valid (older) instead of truncating this one
    bool taint_from_append = false;
};

struct HistoryLoadResult {
    uint8_t files_valid = 0;
    uint8_t foreign_removed = 0;
    uint8_t entries = 0;       // records pushed into the ring (the ring itself keeps the newest ones)
};

// Boot load. Reads both files (scratch: HISTORY_SCRATCH_BYTES), drops foreign-epoch files (removes them), trusts each
// file up to its first bad record, and pushes the records oldest first into *ring (a fresh ring: its entries are
// flagged earlier_boot). Never fails: unreadable or corrupt files simply give an empty history. epoch 0 does no file
// access at all and leaves persistent false.
HistoryLoadResult history_load(HistoryFiles& files, uint32_t epoch, uint8_t* scratch, HistoryStoreState* state,
                               GrindHistoryRing* ring);

enum class HistoryAppendOutcome : uint8_t {
    NOT_AVAILABLE,  // not loaded or no epoch: nothing written
    REJECTED,       // the entry is not storable (grind_history_entry_valid): nothing written
    APPENDED,       // 32 bytes appended to the active file
    ROTATED,        // the other file was replaced by header + this record
    REPLACED,       // an unusable file (empty, torn or after a failed replace) was replaced by header + this record
    FAILED,         // the write did not complete; state->persistent is false
};

// Stores one entry. At most one write call per invocation (plus one read to re-check the active file after a failed
// append); no retry. Rules (see DESIGN-NOTES): append while the active file is clean and has room; a full or untidy
// active file is never touched, the OTHER file is replaced instead (so the newest records stay safe); only an active
// file without a single valid record is replaced in place. state->persistent follows the outcome.
HistoryAppendOutcome history_append(HistoryFiles& files, HistoryStoreState* state, uint8_t* scratch,
                                    const GrindHistoryEntry& entry);

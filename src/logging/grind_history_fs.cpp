#include "grind_history_fs.h"
#include <LittleFS.h>
#include "../config/constants.h"

GrindHistoryService grind_history;

namespace {

const char* const kHistoryPaths[HISTORY_SLOT_COUNT] = {"/hist0.bin", "/hist1.bin"};

// RAII spinlock section
struct Section {
    portMUX_TYPE* mux;
    explicit Section(portMUX_TYPE* m) : mux(m) { portENTER_CRITICAL(mux); }
    ~Section() { portEXIT_CRITICAL(mux); }
};

// HistoryFiles over LittleFS. A file is opened, used once and closed; LittleFS commits on close.
class LittleFsHistoryFiles : public HistoryFiles {
public:
    int32_t read_all(uint8_t slot, uint8_t* buf, size_t cap) override {
        const char* path = path_of(slot);
        if (!path || !buf || !LittleFS.exists(path)) return -1;
        File file = LittleFS.open(path, "r");
        if (!file) return -1;
        if (file.isDirectory()) {
            file.close();
            return -1;
        }
        const size_t n = file.read(buf, cap);
        file.close();
        return static_cast<int32_t>(n);
    }

    bool append(uint8_t slot, const uint8_t* data, size_t len) override {
        // A missing file (e.g. the filesystem was reformatted by a recovery) must not be created here: "a" would make a
        // file without a header. Failing sends the store through its re-read, taint and replace path, which writes one.
        const char* path = path_of(slot);
        if (!path || !LittleFS.exists(path)) return false;
        return write_file(slot, "a", data, len);
    }
    bool replace(uint8_t slot, const uint8_t* data, size_t len) override { return write_file(slot, "w", data, len); }

    bool remove(uint8_t slot) override {
        const char* path = path_of(slot);
        if (!path) return false;
        return !LittleFS.exists(path) || LittleFS.remove(path);
    }

private:
    static const char* path_of(uint8_t slot) { return slot < HISTORY_SLOT_COUNT ? kHistoryPaths[slot] : nullptr; }

    // Size on flash, -1 when the file cannot be opened
    static int32_t size_on_flash(const char* path) {
        File file = LittleFS.open(path, "r");
        if (!file) return -1;
        const int32_t size = file.isDirectory() ? -1 : static_cast<int32_t>(file.size());
        file.close();
        return size;
    }

    // write() is buffered and flush()/close() return void, so a full or failing filesystem is not reported by the write
    // call. The result is therefore checked by size: the file must have grown (or been replaced) by exactly len bytes.
    static bool write_file(uint8_t slot, const char* mode, const uint8_t* data, size_t len) {
        const char* path = path_of(slot);
        if (!path || !data) return false;
        const bool appending = mode[0] == 'a';
        int32_t expected = static_cast<int32_t>(len);
        if (appending) {
            const int32_t before = size_on_flash(path);
            if (before < 0) return false;
            expected += before;
        }
        File file = LittleFS.open(path, mode);
        if (!file) return false;
        const size_t written = file.write(data, len);
        file.flush();
        file.close();
        return written == len && size_on_flash(path) == expected;
    }
};

LittleFsHistoryFiles g_files;

const char* outcome_name(HistoryAppendOutcome outcome) {
    switch (outcome) {
        case HistoryAppendOutcome::APPENDED: return "append";
        case HistoryAppendOutcome::ROTATED: return "rotate";
        case HistoryAppendOutcome::REPLACED: return "replace";
        case HistoryAppendOutcome::FAILED: return "FAILED";
        case HistoryAppendOutcome::REJECTED: return "rejected";
        default: return "not stored";
    }
}

}  // namespace

void GrindHistoryService::begin(uint32_t epoch) {
    GrindHistoryRing loaded_ring;  // about 400 bytes; the ring is swapped in under the lock below
    const bool fs_available = LittleFS.totalBytes() > 0;  // 0 when the filesystem is not mounted
    const uint32_t use_epoch = fs_available ? epoch : 0;
    const HistoryLoadResult result = history_load(g_files, use_epoch, scratch_, &state_, &loaded_ring);
    {
        Section section(&mux_);
        ring_ = loaded_ring;
        loaded_ = true;
        persistent_ = state_.persistent;
    }
    if (use_epoch == 0) {
        LOG_BLE("[HISTORY] no usable store (%s): history stays in RAM\n", fs_available ? "no id" : "no filesystem");
    } else {
        LOG_BLE("[HISTORY] loaded %u record(s) from %u file(s), %u foreign file(s) removed\n",
                static_cast<unsigned>(result.entries), static_cast<unsigned>(result.files_valid),
                static_cast<unsigned>(result.foreign_removed));
    }
}

void GrindHistoryService::record(GrindHistoryEntry entry) {
    grind_history_entry_sanitize(&entry);
    entry.earlier_boot = false;
    {
        Section section(&mux_);
        ring_.push(entry);
    }
    const bool was_persistent = state_.persistent;
    const HistoryAppendOutcome outcome = history_append(g_files, &state_, scratch_, entry);
    {
        Section section(&mux_);
        persistent_ = state_.persistent;
    }
    // One line per grind (the flash write is one append, or one replace every 16th grind); a failure or its end is
    // said once, not once per retry
    if (outcome == HistoryAppendOutcome::FAILED && was_persistent) {
        LOG_BLE("[HISTORY] ERROR: write failed, history is not being saved\n");
    } else if (outcome != HistoryAppendOutcome::FAILED && !was_persistent && state_.persistent) {
        LOG_BLE("[HISTORY] saving again\n");
    }
    LOG_BLE("[HISTORY] grind %lu recorded: %s\n", static_cast<unsigned long>(entry.session_id), outcome_name(outcome));
}

void GrindHistoryService::snapshot(GrindHistorySnapshot* out) const {
    if (!out) return;
    Section section(&mux_);  // at most GRIND_HISTORY_RING_CAPACITY entry copies, no call out
    out->sequence = ring_.sequence();
    out->loaded = loaded_;
    out->persistent = persistent_;
    out->count = ring_.copy_newest_first(out->entries, GRIND_HISTORY_RING_CAPACITY);
}

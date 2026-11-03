#pragma once
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "grind_history.h"
#include "grind_history_store.h"

// Firmware side of the recent-grinds history (chg016): the RAM ring behind a spinlock, the store state, and (in the
// .cpp) the flash file adapter for the two history files, the only code of this feature that touches the filesystem.
//
// Threads:
//  - begin() and record() run on the file-IO task only (begin() before the first record(); record() from
//    GrindLogger::end_grind_session). They are the only code that touches the files or the store state.
//  - snapshot() may be called from any task (the UI task does): it copies at most GRIND_HISTORY_RING_CAPACITY entries
//    under the spinlock; nothing else happens inside the lock (no logging, no allocation, no file access).
// The HTTP task never calls this; it reads the copy the UI task publishes through PortalCommandGate.
class GrindHistoryService {
public:
    // Loads the files into the ring. epoch is the id kept in NVS (GrindLogger::history_epoch(); 0 = none, then nothing is
    // read or written and the history stays in RAM). The same when the filesystem is not mounted (main.cpp mounts it
    // before any task exists, so this is checked here and not through FileIOTask::filesystem_available, which
    // FileIOTask::init() sets only after the task has been created).
    void begin(uint32_t epoch);
    // Adds a finished grind: always to the ring, and to the files when the store is usable. One flash write at most.
    void record(GrindHistoryEntry entry);
    // A consistent copy of the ring and the flags
    void snapshot(GrindHistorySnapshot* out) const;

private:
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    GrindHistoryRing ring_;     // guarded by mux_
    bool loaded_ = false;       // guarded by mux_
    bool persistent_ = false;   // guarded by mux_
    HistoryStoreState state_;   // file-IO task only
    uint8_t scratch_[HISTORY_SCRATCH_BYTES];  // file-IO task only
};

extern GrindHistoryService grind_history;

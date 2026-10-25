#include "grind_start_check.h"

GrindStartBlock grind_start_block(bool motor_available, bool locked_out, GrindStartFault fault) {
    if (!motor_available) return GrindStartBlock::MOTOR_DISABLED;
    if (locked_out) return GrindStartBlock::UPDATE_IN_PROGRESS;
    switch (fault) {
        case GrindStartFault::NOT_CONNECTED: return GrindStartBlock::LOAD_CELL_NOT_CONNECTED;
        case GrindStartFault::NO_DATA: return GrindStartBlock::LOAD_CELL_NO_DATA;
        case GrindStartFault::INVALID_SAMPLE_RATE: return GrindStartBlock::LOAD_CELL_RATE_INVALID;
        case GrindStartFault::NONE: break;
    }
    return GrindStartBlock::NONE;
}

const char* grind_start_block_reason(GrindStartBlock block) {
    switch (block) {
        case GrindStartBlock::MOTOR_DISABLED: return "motor disabled (board not confirmed)";
        case GrindStartBlock::UPDATE_IN_PROGRESS: return "update in progress (grinder locked out)";
        case GrindStartBlock::LOAD_CELL_NOT_CONNECTED: return "load cell not connected";
        case GrindStartBlock::LOAD_CELL_NO_DATA: return "load cell sends no data";
        case GrindStartBlock::LOAD_CELL_RATE_INVALID: return "load cell sample rate invalid";
        case GrindStartBlock::NONE: break;
    }
    return "";
}

const char* grind_start_block_title(GrindStartBlock block) {
    switch (block) {
        case GrindStartBlock::UPDATE_IN_PROGRESS:
        case GrindStartBlock::LOAD_CELL_NOT_CONNECTED:
        case GrindStartBlock::LOAD_CELL_NO_DATA:
        case GrindStartBlock::LOAD_CELL_RATE_INVALID:
            return "CAN'T START";
        case GrindStartBlock::NONE:
        case GrindStartBlock::MOTOR_DISABLED:
            break;
    }
    return nullptr;
}

const char* grind_start_block_message(GrindStartBlock block) {
    switch (block) {
        case GrindStartBlock::UPDATE_IN_PROGRESS: return "Update in progress\n\nTry again when it is done.";
        case GrindStartBlock::LOAD_CELL_NOT_CONNECTED: return "Load cell not connected\n\nCheck the wiring and restart.";
        case GrindStartBlock::LOAD_CELL_NO_DATA: return "Load cell sends no data\n\nCheck the cable and restart.";
        case GrindStartBlock::LOAD_CELL_RATE_INVALID: return "Load cell rate invalid\n\nTie the RATE pin to GND.";
        case GrindStartBlock::NONE:
        case GrindStartBlock::MOTOR_DISABLED:
            break;
    }
    return nullptr;
}

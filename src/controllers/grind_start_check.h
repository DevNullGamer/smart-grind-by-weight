#pragma once
#include <stdint.h>

// Pure "may a grind start now?" decision and its texts. No Arduino/ESP dependencies so it can be
// unit tested on the host. GrindController gathers the inputs and acts on the answer.

enum class GrindStartBlock : uint8_t {
    NONE = 0,
    MOTOR_DISABLED,           // Board not confirmed or grinder not initialised
    UPDATE_IN_PROGRESS,       // Grinder locked out (firmware update running)
    LOAD_CELL_NOT_CONNECTED,
    LOAD_CELL_NO_DATA,
    LOAD_CELL_RATE_INVALID
};

// Load cell boot fault as the start check sees it (mirror of WeightSensor::HardwareFault).
enum class GrindStartFault : uint8_t { NONE = 0, NOT_CONNECTED, NO_DATA, INVALID_SAMPLE_RATE };

// First applicable block, in this order: motor, lock-out, load cell fault.
GrindStartBlock grind_start_block(bool motor_available, bool locked_out, GrindStartFault fault);

// Short lower-case reason for the "[GRIND_START] Refused - <reason>" log line. "" for NONE.
const char* grind_start_block_reason(GrindStartBlock block);

// Title and body of the on-screen notice. nullptr for NONE and MOTOR_DISABLED (that one has its
// own dialog, UIManager::show_motor_disabled_notice).
const char* grind_start_block_title(GrindStartBlock block);
const char* grind_start_block_message(GrindStartBlock block);

#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure decisions and texts for Menu -> Motor Test (and the autotune start refusal). No Arduino/ESP
// dependencies so it can be unit tested on the host; MenuUIController gathers the inputs and acts on
// the answers. The firmware cannot see the motor, only the output pin, so the result texts only ever
// state what the output did, never that the motor works.

enum class MotorTestBlock : uint8_t {
    NONE = 0,
    MOTOR_DISABLED,       // Board not confirmed or grinder not initialised
    UPDATE_IN_PROGRESS,   // Grinder locked out (firmware update running)
    BUSY,                 // A grind or autotune is running, or the output is active
    MOTOR_FAULT,          // The motor output could not be stopped cleanly earlier (latched until reboot)
    START_FAILED          // Never returned by motor_test_block(): the grinder refused the pulse
};

// First applicable block, in this order: fault latched, motor disabled, lock-out, busy.
MotorTestBlock motor_test_block(bool motor_available, bool locked_out, bool busy, bool fault_latched);

// Short lower-case reason for log lines. "" for NONE.
const char* motor_test_block_reason(MotorTestBlock block);

// Title and body of the refusal notice. Never nullptr ("" for NONE). MOTOR_DISABLED carries the same
// text as UIManager::show_motor_disabled_notice; UPDATE_IN_PROGRESS reuses the grind start texts.
const char* motor_test_block_title(MotorTestBlock block);
const char* motor_test_block_message(MotorTestBlock block);

enum class MotorTestOutcome : uint8_t {
    OK = 0,            // Output went low after about the requested time
    ENDED_EARLY,       // Output went low after less than 90 % of the requested time
    NOT_STOPPED,       // Output was still high when the safety stop had to be issued
    STOPPED_BY_USER
};

// How the run went. stopped_by_user wins; otherwise a pulse that is not complete (the caller only
// asks once the pulse is done or the safety stop at requested + grace is due) is NOT_STOPPED;
// otherwise OK, or ENDED_EARLY when elapsed_ms is below 90 % of requested_ms.
MotorTestOutcome motor_test_finish(uint32_t requested_ms, uint32_t elapsed_ms, bool completed, bool stopped_by_user);

// How the result notice should be coloured.
enum class MotorTestSeverity : uint8_t { SUCCESS = 0, WARNING, ERROR };
MotorTestSeverity motor_test_outcome_severity(MotorTestOutcome outcome);

// Title of the result notice. Never nullptr.
const char* motor_test_outcome_title(MotorTestOutcome outcome);

// Body of the result notice, written into buf (always NUL-terminated when len > 0). Never empty for
// a valid buffer. Returns the number of characters written (excluding the NUL), 0 for buf == nullptr
// or len == 0.
size_t motor_test_outcome_message(MotorTestOutcome outcome, uint32_t requested_ms, uint32_t elapsed_ms,
                                  char* buf, size_t len);

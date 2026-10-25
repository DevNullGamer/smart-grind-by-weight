#include "motor_test_check.h"
#include "grind_start_check.h"
#include <stdio.h>

namespace {

// A completed pulse shorter than this share of the request is reported as ended early
constexpr uint64_t kEarlyEndPercent = 90;

// Writes "<seconds>.<tenth>" rounded to a tenth, e.g. 1000 -> "1.0", 449 -> "0.4"
void format_seconds(uint32_t ms, char* out, size_t len) {
    const unsigned long tenths = (static_cast<unsigned long>(ms) + 50UL) / 100UL;
    snprintf(out, len, "%lu.%lu", tenths / 10UL, tenths % 10UL);
}

}  // namespace

MotorTestBlock motor_test_block(bool motor_available, bool locked_out, bool busy, bool fault_latched) {
    // A latched fault first: a failed output halt also clears "available", and the user must restart
    if (fault_latched) return MotorTestBlock::MOTOR_FAULT;
    if (!motor_available) return MotorTestBlock::MOTOR_DISABLED;
    if (locked_out) return MotorTestBlock::UPDATE_IN_PROGRESS;
    if (busy) return MotorTestBlock::BUSY;
    return MotorTestBlock::NONE;
}

const char* motor_test_block_reason(MotorTestBlock block) {
    switch (block) {
        case MotorTestBlock::MOTOR_DISABLED: return "motor disabled (board not confirmed)";
        case MotorTestBlock::UPDATE_IN_PROGRESS: return "update in progress (grinder locked out)";
        case MotorTestBlock::BUSY: return "grinder busy";
        case MotorTestBlock::MOTOR_FAULT: return "motor fault latched";
        case MotorTestBlock::START_FAILED: return "output did not start";
        case MotorTestBlock::NONE: break;
    }
    return "";
}

const char* motor_test_block_title(MotorTestBlock block) {
    switch (block) {
        case MotorTestBlock::MOTOR_DISABLED: return "MOTOR DISABLED";
        case MotorTestBlock::UPDATE_IN_PROGRESS: return grind_start_block_title(GrindStartBlock::UPDATE_IN_PROGRESS);
        case MotorTestBlock::BUSY:
        case MotorTestBlock::START_FAILED:
            return "CAN'T START";
        case MotorTestBlock::MOTOR_FAULT: return "MOTOR FAULT";
        case MotorTestBlock::NONE: break;
    }
    return "";
}

const char* motor_test_block_message(MotorTestBlock block) {
    switch (block) {
        case MotorTestBlock::MOTOR_DISABLED:
            return "Board type not confirmed.\n\nThe grinder output is off.\n\nSet the board under Menu > Hardware board.";
        case MotorTestBlock::UPDATE_IN_PROGRESS: return grind_start_block_message(GrindStartBlock::UPDATE_IN_PROGRESS);
        case MotorTestBlock::BUSY: return "Grinder is busy\n\nTry again when it has stopped.";
        case MotorTestBlock::START_FAILED: return "Motor output did not start\n\nTry again.";
        case MotorTestBlock::MOTOR_FAULT: return "Motor output was cut off.\n\nRestart the device.";
        case MotorTestBlock::NONE: break;
    }
    return "";
}

MotorTestOutcome motor_test_finish(uint32_t requested_ms, uint32_t elapsed_ms, bool completed, bool stopped_by_user) {
    if (stopped_by_user) return MotorTestOutcome::STOPPED_BY_USER;
    if (!completed) return MotorTestOutcome::NOT_STOPPED;
    if (static_cast<uint64_t>(elapsed_ms) * 100 < static_cast<uint64_t>(requested_ms) * kEarlyEndPercent) {
        return MotorTestOutcome::ENDED_EARLY;
    }
    return MotorTestOutcome::OK;
}

MotorTestSeverity motor_test_outcome_severity(MotorTestOutcome outcome) {
    switch (outcome) {
        case MotorTestOutcome::OK: return MotorTestSeverity::SUCCESS;
        case MotorTestOutcome::NOT_STOPPED: return MotorTestSeverity::ERROR;
        case MotorTestOutcome::ENDED_EARLY:
        case MotorTestOutcome::STOPPED_BY_USER:
            break;
    }
    return MotorTestSeverity::WARNING;
}

const char* motor_test_outcome_title(MotorTestOutcome) {
    return "MOTOR TEST";
}

size_t motor_test_outcome_message(MotorTestOutcome outcome, uint32_t requested_ms, uint32_t elapsed_ms,
                                  char* buf, size_t len) {
    if (!buf || len == 0) return 0;

    char elapsed_text[16];
    char requested_text[16];
    format_seconds(elapsed_ms, elapsed_text, sizeof(elapsed_text));
    format_seconds(requested_ms, requested_text, sizeof(requested_text));

    int written = 0;
    switch (outcome) {
        case MotorTestOutcome::OK:
            written = snprintf(buf, len, "Output ran %s s.\n\nIf the motor did not turn, check the motor wiring.", elapsed_text);
            break;
        case MotorTestOutcome::ENDED_EARLY:
            written = snprintf(buf, len, "Output ended after %s s instead of %s s.", elapsed_text, requested_text);
            break;
        case MotorTestOutcome::NOT_STOPPED:
            written = snprintf(buf, len, "Output did not stop in time. Stopped.\n\nRestart the grinder.");
            break;
        case MotorTestOutcome::STOPPED_BY_USER:
            written = snprintf(buf, len, "Stopped.");
            break;
    }
    if (written < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (static_cast<size_t>(written) < len) ? static_cast<size_t>(written) : len - 1;
}

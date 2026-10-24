#include "boot_health_logic.h"

bool boot_health_should_confirm(bool pending_verify, bool setup_done, uint32_t uptime_ms, uint32_t confirm_after_ms) {
    return pending_verify && setup_done && uptime_ms >= confirm_after_ms;
}

bool boot_health_crash_due(int mode, bool pending_verify, bool setup_done, uint32_t uptime_ms, uint32_t delay_ms) {
    return mode != 0 && pending_verify && setup_done && uptime_ms >= delay_ms;
}

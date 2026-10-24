#pragma once

#include <stdint.h>

// Pure decisions of the boot-health rollback (no Arduino/IDF dependencies, host-tested).
// The IDF side lives in system/boot_health.*.

// Should the running image be confirmed now? Only an image that is still PENDING_VERIFY needs it, only after setup()
// finished, and only once it ran confirm_after_ms. uptime_ms is millis(); it counts from boot, so a wrap (49 days)
// cannot matter: an image is confirmed long before.
bool boot_health_should_confirm(bool pending_verify, bool setup_done, uint32_t uptime_ms, uint32_t confirm_after_ms);

// Test builds only: is it time to crash on purpose? mode 0 never, otherwise only while the image is unconfirmed
// (installed by an update), setup() finished and uptime_ms >= delay_ms.
bool boot_health_crash_due(int mode, bool pending_verify, bool setup_done, uint32_t uptime_ms, uint32_t delay_ms);

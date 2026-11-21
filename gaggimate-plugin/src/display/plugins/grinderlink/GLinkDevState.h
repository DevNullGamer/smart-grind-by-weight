#pragma once
#include <stdint.h>

// GLink devState classification (docs/GLINK-PROTOCOL.md 6.1). Pure: no Arduino/ESP/NimBLE dependencies, no heap.
// The text is compared case-sensitively; an unknown, empty or null string is UNKNOWN (spec 6: unknown values are tolerated).

namespace glinkc {

enum class DevClass : uint8_t { UNKNOWN, IDLE, GRINDING, BUSY };

// IDLE = IDLE; GRINDING = GRINDING; STARTING, DONE, SETTING, UPDATING, WARNING = BUSY; anything else UNKNOWN.
DevClass classify_dev_state(const char* dev_state);

// True for IDLE and WARNING (the READY screen; spec 7.1 accepts `grindProfile update` and `select` there). SETTING is
// deliberately not written to, so a user in the grinder's menu is not disturbed.
bool accepts_profile_writes(const char* dev_state);

}  // namespace glinkc

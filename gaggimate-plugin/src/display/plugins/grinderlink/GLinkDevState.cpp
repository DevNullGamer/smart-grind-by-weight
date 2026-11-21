#include "GLinkDevState.h"

namespace glinkc {

static bool dev_state_same(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

DevClass classify_dev_state(const char* s) {
    if (!s) return DevClass::UNKNOWN;
    if (dev_state_same(s, "IDLE")) return DevClass::IDLE;
    if (dev_state_same(s, "GRINDING")) return DevClass::GRINDING;
    if (dev_state_same(s, "STARTING") || dev_state_same(s, "DONE") || dev_state_same(s, "SETTING") ||
        dev_state_same(s, "UPDATING") || dev_state_same(s, "WARNING"))
        return DevClass::BUSY;
    return DevClass::UNKNOWN;
}

bool accepts_profile_writes(const char* s) {
    if (!s) return false;
    return dev_state_same(s, "IDLE") || dev_state_same(s, "WARNING");
}

}  // namespace glinkc

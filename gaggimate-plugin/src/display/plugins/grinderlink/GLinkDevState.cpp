#include "GLinkDevState.h"

namespace glinkc {

static bool same(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

DevClass classify_dev_state(const char* s) {
    if (!s) return DevClass::UNKNOWN;
    if (same(s, "IDLE")) return DevClass::IDLE;
    if (same(s, "GRINDING")) return DevClass::GRINDING;
    if (same(s, "STARTING") || same(s, "DONE") || same(s, "SETTING") || same(s, "UPDATING") || same(s, "WARNING"))
        return DevClass::BUSY;
    return DevClass::UNKNOWN;
}

bool accepts_profile_writes(const char* s) {
    if (!s) return false;
    return same(s, "IDLE") || same(s, "WARNING");
}

}  // namespace glinkc

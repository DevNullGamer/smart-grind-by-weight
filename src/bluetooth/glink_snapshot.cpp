#include "glink_snapshot.h"

size_t glink_serve_get(const GlinkSnapshot& snapshot, const GlinkRequest& request, char* out, size_t cap,
                       GlinkFailure& failure) {
    const int object = static_cast<int>(GlinkObject::GRIND_PROFILE);
    if (!glink_state_check(snapshot.screen, GlinkOpClass::GET, object, failure)) return 0;

    size_t n = 0;
    if (!request.selector_index) {
        const size_t count = snapshot.profile3_exists ? GLINK_PROFILE_COUNT_MAX : GLINK_PROFILE_COUNT_MAX - 1;
        n = glink_encode_profile_list(out, cap, snapshot.profiles, count);
    } else {
        const uint8_t index = request.selector_value;
        if (index >= GLINK_PROFILE_COUNT_MAX) {
            glink_set_failure(failure, GlinkError::OUT_OF_RANGE, glink_msg::INDEX_RANGE, "selector.value", object);
            return 0;
        }
        if (index == GLINK_PROFILE_WRITABLE_INDEX && !snapshot.profile3_exists) {
            glink_set_failure(failure, GlinkError::NOT_FOUND, glink_msg::PROFILE_NOT_SET, "selector.value", object);
            return 0;
        }
        n = glink_encode_profile_entry(out, cap, snapshot.profiles[index]);
    }
    if (n == 0) {
        glink_set_failure(failure, GlinkError::UI_BUSY, glink_msg::UI_BUSY, "", object);
    }
    return n;
}

size_t glink_serve_result_get(const GlinkSnapshot& snapshot, char* out, size_t cap, GlinkFailure& failure) {
    const int object = static_cast<int>(GlinkObject::GRIND_RESULT);
    if (!glink_state_check(snapshot.screen, GlinkOpClass::GET, object, failure)) return 0;
    if (!snapshot.has_result) {
        glink_set_failure(failure, GlinkError::NOT_FOUND, glink_msg::NO_RESULT, "", object);
        return 0;
    }
    const size_t n = glink_encode_result_get(out, cap, snapshot.last_result);
    if (n == 0) glink_set_failure(failure, GlinkError::UI_BUSY, glink_msg::UI_BUSY, "", object);
    return n;
}

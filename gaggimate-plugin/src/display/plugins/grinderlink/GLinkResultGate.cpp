#include "GLinkResultGate.h"

namespace glinkc {

void ResultGate::reset() {
    baseline_ = false;
    floor_ = 0;
}

void ResultGate::on_ready(uint32_t result_seq) {
    baseline_ = true;
    floor_ = result_seq;
}

bool ResultGate::on_result(uint32_t seq, bool weight_settled, int32_t final_tenths, int32_t* out_tenths) {
    if (!baseline_ || static_cast<int32_t>(seq - floor_) <= 0) return false;
    floor_ = seq;
    if (!weight_settled) return false;
    if (out_tenths) *out_tenths = final_tenths;
    return true;
}

}  // namespace glinkc

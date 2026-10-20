#include "board_detect.h"

namespace {

// Probe was inconclusive or contradictory: fall back to the last known board for
// display purposes (no motor), otherwise UNKNOWN.
BoardDecision inconclusive(const BoardEvidence& evidence, const char* reason) {
    if (evidence.cached == BoardVariant::V1 || evidence.cached == BoardVariant::V2) {
        return {evidence.cached, BoardSource::CACHED, reason};
    }
    return {BoardVariant::UNKNOWN, BoardSource::NONE, reason};
}

}  // namespace

bool board_needs_te_probe(const BoardEvidence& evidence) {
    return evidence.override_setting == BoardOverride::AUTO && evidence.gpio45_pullup_level == 0;
}

BoardDecision decide_variant(const BoardEvidence& evidence) {
    if (evidence.override_setting == BoardOverride::FORCE_V1) {
        return {BoardVariant::V1, BoardSource::USER, "user override V1"};
    }
    if (evidence.override_setting == BoardOverride::FORCE_V2) {
        return {BoardVariant::V2, BoardSource::USER, "user override V2"};
    }

    if (evidence.gpio45_pullup_level != 0 && evidence.gpio45_pullup_level != 1) {
        return inconclusive(evidence, "GPIO45 not read");
    }

    if (evidence.gpio45_pullup_level == 1) {
        if (evidence.gpio18_pulldown_level == 1) {
            return inconclusive(evidence, "GPIO45 high (V1) but GPIO18 high (V2 hint)");
        }
        return {BoardVariant::V1, BoardSource::PROBE, "GPIO45 high"};
    }

    // GPIO45 low: V2 candidate, needs TE activity from the panel.
    if (evidence.te_edges < 0) {
        return inconclusive(evidence, "GPIO45 low but TE probe failed");
    }
    if (evidence.te_edges < BOARD_TE_MIN_EDGES) {
        return inconclusive(evidence, "GPIO45 low but no TE edges");
    }
    if (evidence.gpio18_pulldown_level == 0) {
        return inconclusive(evidence, "GPIO45 low and TE active (V2) but GPIO18 low");
    }
    return {BoardVariant::V2, BoardSource::PROBE, "GPIO45 low, TE active"};
}

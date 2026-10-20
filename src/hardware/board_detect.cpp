#include "board_detect.h"
#include "display_manager.h"
#include "../config/constants.h"
#include <Arduino.h>
#include <Preferences.h>

namespace {

constexpr const char* kNvsNamespace = "grinder";
constexpr const char* kNvsKeyOverride = "board_override";
constexpr const char* kNvsKeyLast = "board_last";

BoardVariant variant_from_nvs(uint8_t value) {
    switch (value) {
        case 1: return BoardVariant::V1;
        case 2: return BoardVariant::V2;
        default: return BoardVariant::UNKNOWN;
    }
}

BoardOverride override_from_nvs(uint8_t value) {
    switch (value) {
        case 1: return BoardOverride::FORCE_V1;
        case 2: return BoardOverride::FORCE_V2;
        default: return BoardOverride::AUTO;
    }
}

int8_t read_with_pull(uint8_t pin, uint8_t mode) {
    pinMode(pin, mode);
    delay(HW_BOARD_SETTLE_MS);
    return digitalRead(pin) ? 1 : 0;
}

}  // namespace

BoardOverride board_override_load() {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, true)) {
        return BoardOverride::AUTO;
    }
    const BoardOverride value = override_from_nvs(prefs.getUChar(kNvsKeyOverride, 0));
    prefs.end();
    return value;
}

bool board_override_store(BoardOverride value) {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return false;
    }
    const size_t written = prefs.putUChar(kNvsKeyOverride, static_cast<uint8_t>(value));
    prefs.end();
    return written > 0;
}

void board_park_pins() {
    // GPIO16 is the motor pin on every board: hold it low until the motor is allowed.
    // GPIO18 is the motor pin on V1 but a touch interrupt input on V2, so it must never
    // be driven before the board is known.
    pinMode(HW_BOARD_PARK_PIN, OUTPUT);
    digitalWrite(HW_BOARD_PARK_PIN, LOW);
    pinMode(HW_BOARD_HINT_PIN, INPUT_PULLDOWN);
}

BoardDecision detect_board() {
    BoardEvidence evidence = {BoardOverride::AUTO, BoardVariant::UNKNOWN, -1, -1, -1};

    Preferences prefs;
    if (prefs.begin(kNvsNamespace, true)) {
        evidence.override_setting = override_from_nvs(prefs.getUChar(kNvsKeyOverride, 0));
        evidence.cached = variant_from_nvs(prefs.getUChar(kNvsKeyLast, 0));
        prefs.end();
    } else {
        // Read-only open fails when the namespace does not exist yet (first boot)
        LOG_BLE("[BOARD] No stored board settings (first boot?)\n");
    }

    if (evidence.override_setting == BoardOverride::AUTO) {
        evidence.gpio45_pullup_level = read_with_pull(HW_BOARD_ID_PIN, INPUT_PULLUP);
        evidence.gpio18_pulldown_level = read_with_pull(HW_BOARD_HINT_PIN, INPUT_PULLDOWN);
        if (board_needs_te_probe(evidence)) {
            const BoardConfig v2 = board_config_for(BoardVariant::V2, BoardSource::PROBE);
            evidence.te_edges = static_cast<int16_t>(DisplayManager::probe_te_edges(
                v2.display_cs_pin, HW_BOARD_ID_PIN, HW_BOARD_TE_WINDOW_MS));
        }
    }

    const BoardDecision decision = decide_variant(evidence);
    LOG_BLE("[BOARD] %s (source %d): %s | override=%d cached=%d GPIO45=%d GPIO18=%d TE=%d\n",
            board_config_for(decision.variant, decision.source).name(),
            static_cast<int>(decision.source), decision.reason,
            static_cast<int>(evidence.override_setting), static_cast<int>(evidence.cached),
            evidence.gpio45_pullup_level, evidence.gpio18_pulldown_level, evidence.te_edges);

    if (decision.source == BoardSource::PROBE && decision.variant != evidence.cached) {
        if (prefs.begin(kNvsNamespace, false)) {
            prefs.putUChar(kNvsKeyLast, static_cast<uint8_t>(decision.variant));
            prefs.end();
        }
    }

    board_set(board_config_for(decision.variant, decision.source));
    if (decision.variant == BoardVariant::UNKNOWN || decision.source == BoardSource::CACHED) {
        LOG_BLE("[BOARD] WARNING: board not established by probe or user, motor disabled\n");
    }
    return decision;
}

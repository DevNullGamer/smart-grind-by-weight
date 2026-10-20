#pragma once

#include <stdint.h>
#include "../config/board_config.h"

//==============================================================================
// BOARD DETECTION
//==============================================================================
// Decides at boot whether the firmware runs on a Waveshare 1.64" V1 or V2 board.
//
// Evidence, strongest first:
//   1. GPIO45 read with internal pull-up: 0 => V2 (held low by the board), 1 => V1.
//   2. V2 confirmation: panel brought up on the V2 chip select; the panel's TE
//      output on GPIO45 must toggle (V1 has no TE signal).
//   3. GPIO18 read with internal pull-down: hint only (V2 holds it high through a
//      pull-up; on V1 it is the user's motor wire).
// A stored user override wins over everything. Contradicting or missing evidence
// gives UNKNOWN (or the last known board for display only); the motor stays
// disabled unless the board was established by PROBE or USER.
//
// board_decide.cpp (decide_variant) has no hardware dependencies and is unit tested
// on the host; board_detect.cpp holds the hardware access.

enum class BoardOverride : uint8_t { AUTO = 0, FORCE_V1 = 1, FORCE_V2 = 2 };

struct BoardEvidence {
    BoardOverride override_setting;  // NVS board_override
    BoardVariant cached;             // NVS board_last (UNKNOWN = none stored)
    int8_t gpio45_pullup_level;      // 0 / 1, -1 = not read
    int16_t te_edges;                // edges counted on GPIO45, -1 = not measured / bring-up failed
    int8_t gpio18_pulldown_level;    // 0 / 1, -1 = not read
};

struct BoardDecision {
    BoardVariant variant;
    BoardSource source;     // PROBE, USER, CACHED or NONE (UNKNOWN)
    const char* reason;     // static string for logging
};

// True when the TE confirmation measurement is needed to reach a decision.
bool board_needs_te_probe(const BoardEvidence& evidence);

// Pure decision function.
BoardDecision decide_variant(const BoardEvidence& evidence);

// Parks the motor-related pins in a safe state. Call first thing in setup(),
// before the board is known: GPIO16 driven LOW, GPIO18 input with pull-down.
void board_park_pins();

// Gathers evidence (NVS, pins, V2 panel probe), decides, stores board_last on a
// positive probe and calls board_set(). Must run before display init and before
// any task starts.
BoardDecision detect_board();

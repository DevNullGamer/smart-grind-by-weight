#pragma once

#include <stdint.h>

//==============================================================================
// RUNTIME BOARD CONFIGURATION (types only)
//==============================================================================
// Describes the pins and display parameters that differ between hardware
// revisions of the Waveshare 1.64" AMOLED board. The active configuration is
// read through board(); it starts as the compile-time default selected with
// -DBOARD_DEFAULT_VARIANT (1 = V1, 2 = V2) and may be replaced at runtime via
// board_set(). Implementation: src/hardware/board_config.cpp

enum class BoardVariant : uint8_t { UNKNOWN, V1, V2 };

enum class BoardSource : uint8_t { NONE, PROBE, USER, CACHED, COMPILE_DEFAULT };

struct BoardConfig {
    BoardVariant variant;
    BoardSource source;
    int8_t display_cs_pin;        // Display chip select
    int8_t loadcell_sck_pin;      // HX711 serial clock
    int8_t loadcell_sck_pin_alt;  // Alternative HX711 clock pin (-1 = none)
    int8_t motor_pin;             // Grinder relay output (-1 = disabled)
    int8_t motor_pin_alt;         // Alternative grinder relay pin (-1 = none)
    int16_t display_x_offset_px;  // Panel column offset
    uint32_t qspi_hz;             // Display QSPI clock

    bool motor_enabled() const { return motor_pin >= 0; }
    const char* name() const {
        switch (variant) {
            case BoardVariant::V1: return "V1";
            case BoardVariant::V2: return "V2";
            default: return "UNKNOWN";
        }
    }
};

// Active board configuration (never null, always safe to call).
const BoardConfig& board();

// Replace the active board configuration (used by board detection).
void board_set(const BoardConfig& config);

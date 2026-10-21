#pragma once

#include <stdint.h>

//==============================================================================
// RUNTIME BOARD CONFIGURATION (types only)
//==============================================================================
// Describes the pins and display parameters that differ between hardware
// revisions of the Waveshare 1.64" AMOLED board. The active configuration is
// read through board(); it starts as a motor-less default and is replaced by
// board detection via board_set(). Implementation: src/hardware/board_config.cpp

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
    const char* source_name() const {
        switch (source) {
            case BoardSource::PROBE: return "probe";
            case BoardSource::USER: return "user";
            case BoardSource::CACHED: return "cached";
            case BoardSource::COMPILE_DEFAULT: return "default";
            default: return "none";
        }
    }
};

// Minimum TE edges (both polarities) in the counting window to accept a V2 panel
// (~60 Hz TE seen on V2 = ~12 edges per 100 ms; 0 on V1).
#define BOARD_TE_MIN_EDGES 6

// Dual wiring: both known grinder wirings are driven at once.
//   HX711 clock: pulsed on GPIO1 and GPIO2 together (DOUT is GPIO3 on both boards).
//   Motor: GPIO16 always (when allowed); GPIO18 in addition ONLY on a V1 board
//   established by PROBE or USER, because GPIO18 is the touch interrupt input on V2.
#define BOARD_MOTOR_PIN 16
#define BOARD_MOTOR_PIN_V1_ALT 18

// True when `pin` may be driven as a grinder motor output on `variant`. Rejects
// UNKNOWN boards, GPIO18 on V2, and display / touch / IMU / HX711 / strapping /
// USB / UART / flash and PSRAM pins.
bool board_motor_pin_allowed(BoardVariant variant, int pin);

// Build the configuration for a variant. Motor pins are only populated when the
// variant was established by PROBE or USER and the pins pass
// board_motor_pin_allowed(); UNKNOWN/CACHED/default sources get no motor.
// UNKNOWN uses V1 display and load cell pins (display only).
BoardConfig board_config_for(BoardVariant variant, BoardSource source);

// Degraded display mode: while the board is UNKNOWN the panel is driven on both
// known chip-select pins so the on-screen board picker is visible on either board.
// Returns the second chip select to mirror to, or -1 when the board is known.
int8_t board_display_mirror_cs_pin(const BoardConfig& config);

// Active board configuration (never null, always safe to call).
const BoardConfig& board();

// Replace the active board configuration (used by board detection).
void board_set(const BoardConfig& config);

// True when this build can drive the grinder motor (mock builds simulate it).
inline bool motor_enabled() {
#if defined(DEBUG_ENABLE_LOADCELL_MOCK) && (DEBUG_ENABLE_LOADCELL_MOCK != 0)
    return true;
#else
    return board().motor_enabled();
#endif
}

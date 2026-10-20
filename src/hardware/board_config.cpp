#include "../config/board_config.h"

namespace {

// Motor pins here are the maximum a board may have; board_config_for() strips
// them unless the board was established by PROBE or USER. The HX711 clock is
// wired to GPIO2 (V1 convention) or GPIO1 (V2 convention); both are always driven.
constexpr BoardConfig kBoardV1 = {
    BoardVariant::V1, BoardSource::COMPILE_DEFAULT,
    /*display_cs_pin*/ 9,
    /*loadcell_sck_pin*/ 2, /*loadcell_sck_pin_alt*/ 1,
    /*motor_pin*/ BOARD_MOTOR_PIN, /*motor_pin_alt*/ BOARD_MOTOR_PIN_V1_ALT,
    /*display_x_offset_px*/ 20,
    /*qspi_hz*/ 40000000};

constexpr BoardConfig kBoardV2 = {
    BoardVariant::V2, BoardSource::COMPILE_DEFAULT,
    /*display_cs_pin*/ 46,
    /*loadcell_sck_pin*/ 1, /*loadcell_sck_pin_alt*/ 2,
    /*motor_pin*/ BOARD_MOTOR_PIN, /*motor_pin_alt*/ -1,
    /*display_x_offset_px*/ 20,
    /*qspi_hz*/ 40000000};

// Initial configuration until detect_board() calls board_set(): V1 display and
// load cell pins but never a motor pin. Built from constants (not
// board_config_for) so it does not depend on static initialisation order.
BoardConfig g_board = {
    BoardVariant::V1, BoardSource::COMPILE_DEFAULT,
    kBoardV1.display_cs_pin,
    kBoardV1.loadcell_sck_pin, kBoardV1.loadcell_sck_pin_alt,
    /*motor_pin*/ -1, /*motor_pin_alt*/ -1,
    kBoardV1.display_x_offset_px, kBoardV1.qspi_hz};

// Pins that must never be a motor output on any board.
bool is_reserved_pin(int pin) {
    switch (pin) {
        case 0:                              // Boot strapping
        case 1: case 2: case 3:              // HX711 clock pins and DOUT (GPIO3 is also strapping)
        case 9: case 10: case 11:            // Display CS (V1), SCK, D0 (IMU INT1 on V2 is GPIO9)
        case 12: case 13: case 14:           // Display D1..D3
        case 17:                             // IMU INT2 (V2)
        case 19: case 20:                    // USB
        case 21:                             // Display reset
        case 43: case 44:                    // UART0
        case 45: case 46:                    // Strapping, display TE (V2) / CS (V2) / IMU INT1 (V1)
        case 47: case 48:                    // Touch / IMU I2C
            return true;
        default:
            // 26..37: SPI flash and octal PSRAM
            return pin < 0 || pin > 48 || (pin >= 26 && pin <= 37);
    }
}

}  // namespace

bool board_motor_pin_allowed(BoardVariant variant, int pin) {
    if (variant == BoardVariant::UNKNOWN || is_reserved_pin(pin)) {
        return false;
    }
    // GPIO18 is the touch interrupt input (4.7k pull-up) on V2: never an output there.
    if (pin == BOARD_MOTOR_PIN_V1_ALT) {
        return variant == BoardVariant::V1;
    }
    return true;
}

BoardConfig board_config_for(BoardVariant variant, BoardSource source) {
    BoardConfig config = (variant == BoardVariant::V2) ? kBoardV2 : kBoardV1;
    config.variant = variant;
    config.source = source;
    const bool motor_established = variant != BoardVariant::UNKNOWN &&
                                   (source == BoardSource::PROBE || source == BoardSource::USER);
    if (!motor_established || !board_motor_pin_allowed(variant, config.motor_pin)) {
        config.motor_pin = -1;
        config.motor_pin_alt = -1;
    } else if (!board_motor_pin_allowed(variant, config.motor_pin_alt)) {
        config.motor_pin_alt = -1;
    }
    return config;
}

const BoardConfig& board() { return g_board; }

void board_set(const BoardConfig& config) { g_board = config; }

#include "../config/board_config.h"

namespace {

constexpr BoardConfig kBoardV1 = {
    BoardVariant::V1, BoardSource::COMPILE_DEFAULT,
    /*display_cs_pin*/ 9,
    /*loadcell_sck_pin*/ 2, /*loadcell_sck_pin_alt*/ -1,
    /*motor_pin*/ 18, /*motor_pin_alt*/ -1,
    /*display_x_offset_px*/ 20,
    /*qspi_hz*/ 40000000};

constexpr BoardConfig kBoardV2 = {
    BoardVariant::V2, BoardSource::COMPILE_DEFAULT,
    /*display_cs_pin*/ 46,
    /*loadcell_sck_pin*/ 1, /*loadcell_sck_pin_alt*/ -1,
    /*motor_pin*/ 16, /*motor_pin_alt*/ -1,
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

}  // namespace

BoardConfig board_config_for(BoardVariant variant, BoardSource source) {
    BoardConfig config = (variant == BoardVariant::V2) ? kBoardV2 : kBoardV1;
    config.variant = variant;
    config.source = source;
    const bool motor_allowed = variant != BoardVariant::UNKNOWN &&
                               (source == BoardSource::PROBE || source == BoardSource::USER);
    if (!motor_allowed) {
        config.motor_pin = -1;
        config.motor_pin_alt = -1;
    }
    return config;
}

const BoardConfig& board() { return g_board; }

void board_set(const BoardConfig& config) { g_board = config; }

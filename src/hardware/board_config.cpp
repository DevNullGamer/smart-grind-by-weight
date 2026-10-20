#include "../config/constants.h"

#ifndef BOARD_DEFAULT_VARIANT
#define BOARD_DEFAULT_VARIANT 1
#endif

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

#if BOARD_DEFAULT_VARIANT == 2
BoardConfig g_board = kBoardV2;
#else
BoardConfig g_board = kBoardV1;
#endif

}  // namespace

const BoardConfig& board() { return g_board; }

void board_set(const BoardConfig& config) { g_board = config; }

#pragma once

//==============================================================================
// HARDWARE CONFIGURATION CONSTANTS
//==============================================================================
// This file contains hardware-specific configuration values that define the
// physical characteristics and constraints of the ESP32-S3 coffee grinder system.
// These values are tied to the specific hardware implementation and should only
// be modified when changing hardware components.

//------------------------------------------------------------------------------
// GPIO PIN ASSIGNMENTS
//------------------------------------------------------------------------------
// Touch Controller (I2C)
#define HW_TOUCH_I2C_SDA_PIN 47                                                // I2C data pin for capacitive touch controller
#define HW_TOUCH_I2C_SCL_PIN 48                                                // I2C clock pin for capacitive touch controller
#define HW_TOUCH_I2C_ADDRESS 0x38                                              // I2C address of FT3168 touch controller

// Display Controller (QSPI)
// Chip select, load cell clock, motor pin and display offset/clock differ per
// board revision: see board() in board_config.h.
#define HW_DISPLAY_SCK_PIN 10                                                  // SPI clock for display controller
#define HW_DISPLAY_D0_PIN 11                                                   // SPI data line 0 (QSPI mode)
#define HW_DISPLAY_D1_PIN 12                                                   // SPI data line 1 (QSPI mode)
#define HW_DISPLAY_D2_PIN 13                                                   // SPI data line 2 (QSPI mode)
#define HW_DISPLAY_D3_PIN 14                                                   // SPI data line 3 (QSPI mode)
#define HW_DISPLAY_RESET_PIN 21                                                // Display reset pin

// Load Cell ADC Pins
#define HW_LOADCELL_DOUT_PIN 3                                                 // HX711 data output pin

// Motor Control (relay pin is board-specific, see board())
#define HW_GRINDER_SETTLING_TIME_MS 500                                        // Startup transient immunity (tune based on mechanical rigidity, 0 to disable)

//------------------------------------------------------------------------------
// LOAD CELL ADC SPECIFICATIONS
//------------------------------------------------------------------------------
// Sample rate configuration
#define HW_LOADCELL_SAMPLE_RATE_SPS 10                                         // Current sample rate setting
#define HW_LOADCELL_SAMPLE_INTERVAL_MS (1000 / HW_LOADCELL_SAMPLE_RATE_SPS)   // Calculated sample interval

// Calibration validation
#define HW_LOADCELL_CAL_MIN_ADC_VALUE 1000                                    // Minimum ADC value to confirm weight placed on scale

//------------------------------------------------------------------------------
// DISPLAY SPECIFICATIONS  
//------------------------------------------------------------------------------
#define HW_DISPLAY_WIDTH_PX 280                                                // LCD width in pixels
#define HW_DISPLAY_HEIGHT_PX 456                                               // LCD height in pixels
#define HW_DISPLAY_ROTATION_DEG 0                                              // Display rotation angle
#define HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT 15                               // Minimum brightness percentage (to avoid too dim to see)
#define HW_DISPLAY_DRAW_BUF_ROWS_PSRAM 40                                      // LVGL draw buffer rows when placed in PSRAM (22.4 KB, ~12 strips/full redraw)
#define HW_DISPLAY_DRAW_BUF_ROWS_INTERNAL 8                                    // Fallback rows in internal DMA RAM (4.5 KB, 57 strips/full redraw)
#define HW_DISPLAY_DRAW_BUF_ALIGN_BYTES 64                                     // PSRAM draw buffer alignment (>= S3 data cache line); size must be a multiple too
#define HW_DISPLAY_STAGING_ROWS 8                                              // Rows per internal DMA staging buffer (2 buffers x 4.5 KB), halves on alloc failure
#define HW_DISPLAY_STAGING_MIN_ROWS 4                                          // Smallest staging buffer accepted before falling back to the internal draw buffer
#define HW_DISPLAY_STAGING_TIMEOUT_MS 100                                      // Max wait for one staging chunk DMA transfer to complete

//------------------------------------------------------------------------------
// SERIAL COMMUNICATION
//------------------------------------------------------------------------------
#define HW_SERIAL_BAUD_RATE 115200                                             // UART baud rate for debug/logging output

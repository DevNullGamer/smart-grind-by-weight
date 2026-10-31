// MenuScreen: Diagnostics page.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../event_bridge_lvgl.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_diagnostics_page(lv_obj_t* parent) {
    init_column_page(parent);
    lv_obj_set_style_pad_all(parent, 0, 0);

    // Load Cell Status separator
    create_separator(parent, "Load Cell Status");

    // Status indicator
    create_data_label(parent, "Status:", &diag_status_label);

    // Calibration factor - stacked for long decimal values
    create_data_label(parent, "Cal. factor:", &diag_calibration_factor_label);

    // Info label (only shown when not calibrated)
    diag_info_label = lv_label_create(parent);
    lv_label_set_text(diag_info_label, "");
    lv_obj_set_style_text_font(diag_info_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(diag_info_label, lv_color_hex(THEME_COLOR_WARNING), 0);
    lv_obj_set_style_margin_top(diag_info_label, 10, 0);
    lv_obj_set_style_margin_bottom(diag_info_label, 10, 0);
    lv_label_set_long_mode(diag_info_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(diag_info_label, THEME_CONTENT_INNER_WIDTH_PX);
    lv_obj_set_hidden(diag_info_label, true); // Hidden by default

    diag_reset_button = create_button(parent, "Reset Diagnostics", lv_color_hex(THEME_COLOR_WARNING));
    lv_obj_set_style_margin_bottom(diag_reset_button, 10, 0);

    // Noise Floor separator
    create_separator(parent, "Noise Floor");

    // Std dev readings (moved from System Info) - stacked for precision values
    create_data_label(parent, "Std Dev (g):", &diag_std_dev_g_label);
    create_data_label(parent, "Std Dev (ADC):", &diag_std_dev_adc_label);
    create_data_label(parent, "Noise level:", &diag_noise_level_label);

    // Static info label about calibration dependency
    lv_obj_t* cal_info = lv_label_create(parent);
    lv_label_set_text(cal_info, "Noise level readings depend on proper calibration.");
    lv_obj_set_style_text_font(cal_info, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cal_info, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(cal_info, 10, 0);
    lv_label_set_long_mode(cal_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(cal_info, THEME_CONTENT_INNER_WIDTH_PX);

    // Motor Response separator
    create_separator(parent, "Motor Response");

    // Motor latency
    create_data_label(parent, "Motor Latency:", &diag_motor_latency_label, true);

    // Register event for the button (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(diag_reset_button, LV_EVENT_CLICKED, ET::MENU_DIAGNOSTIC_RESET);
}

void MenuScreen::update_diagnostics(WeightSensor* weight_sensor) {
    if (!visible || !diagnostics_controller) return;

    // Update standard deviations only every 1 second to reduce noise
    static unsigned long last_std_dev_update = 0;
    unsigned long now = millis();
    if (now - last_std_dev_update >= 1000) {  // Update every 1 second
        last_std_dev_update = now;

        // Get standard deviations using same window as grind control precision settling
        float std_dev_g = weight_sensor->get_standard_deviation_g(GRIND_SCALE_PRECISION_SETTLING_TIME_MS);  // 500ms window
        int32_t std_dev_adc = weight_sensor->get_standard_deviation_adc(GRIND_SCALE_PRECISION_SETTLING_TIME_MS);  // 500ms window

        char std_dev_g_text[32];
        snprintf(std_dev_g_text, sizeof(std_dev_g_text), "%.4f", std_dev_g);
        set_label_text_if_changed(diag_std_dev_g_label, std_dev_g_text);

        set_label_text_int(diag_std_dev_adc_label, std_dev_adc);

        // Check noise level using WeightSensor diagnostic method
        bool noise_acceptable = weight_sensor->noise_level_diagnostic();

        // Update noise level indicator
        // The colour follows the text, so it is only set when the text changes
        if (noise_acceptable) {
            if (set_label_text_if_changed(diag_noise_level_label, "OK")) {
                lv_obj_set_style_text_color(diag_noise_level_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
            }
        } else {
            if (set_label_text_if_changed(diag_noise_level_label, "Too High")) {
                lv_obj_set_style_text_color(diag_noise_level_label, lv_color_hex(THEME_COLOR_ERROR), 0);
            }
        }
    }

    // Update calibration factor
    float cal_factor = weight_sensor->get_calibration_factor();
    char cal_factor_text[32];
    snprintf(cal_factor_text, sizeof(cal_factor_text), "%.2f", cal_factor);
    set_label_text_if_changed(diag_calibration_factor_label, cal_factor_text);

    // Update motor latency
    if (grind_controller) {
        float motor_latency = grind_controller->get_motor_response_latency();
        char latency_text[32];
        snprintf(latency_text, sizeof(latency_text), "%.0f ms", motor_latency);
        set_label_text_if_changed(diag_motor_latency_label, latency_text);
    } else {
        set_label_text_if_changed(diag_motor_latency_label, "-- ms");
    }

    // Get highest priority diagnostic
    DiagnosticCode diagnostic = diagnostics_controller->get_highest_priority_warning();

    // Update status label
    if (diagnostic == DiagnosticCode::NONE) {
        if (set_label_text_if_changed(diag_status_label, "OK")) {
            lv_obj_set_style_text_color(diag_status_label, lv_color_hex(THEME_COLOR_SUCCESS), 0);
        }
        lv_obj_set_hidden(diag_info_label, true);
    } else {
        if (set_label_text_if_changed(diag_status_label, LV_SYMBOL_WARNING " Warning")) {
            lv_obj_set_style_text_color(diag_status_label, lv_color_hex(THEME_COLOR_WARNING), 0);
        }

        // Show appropriate warning message
        if (diagnostic == DiagnosticCode::LOAD_CELL_NOT_CALIBRATED) {
            set_label_text_if_changed(diag_info_label, "Loadcell not calibrated");
            lv_obj_set_hidden(diag_info_label, false);
        } else {
            // For future noise/mechanical warnings, show in info label
            const char* message = diagnostics_controller->get_diagnostic_message(diagnostic);
            set_label_text_if_changed(diag_info_label, message);
            lv_obj_set_hidden(diag_info_label, false);
        }
    }
}

LV_DEPRECATIONS_IGNORE_END

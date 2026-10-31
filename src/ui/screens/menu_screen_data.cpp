// MenuScreen: Logs & Data and Lifetime Stats pages.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../logging/grind_logging.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/statistics_manager.h"
#include "../event_bridge_lvgl.h"
#include "../components/blocking_overlay.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_data_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_description_label(parent, "Saved grind logs stored on the grinder for export/analysis.");

    create_toggle_row(parent, "Logging", &logging_toggle);

    // Log data section
    create_separator(parent, "Log Data");
    create_data_label(parent, "Sessions:", &sessions_label);
    create_data_label(parent, "Events:", &events_label);
    create_data_label(parent, "Metrics:", &measurements_label);

    // Reset separator
    create_separator(parent, "Reset");

    purge_button = create_button(parent, "Purge Logs", lv_color_hex(THEME_COLOR_WARNING));
    lv_obj_set_style_margin_bottom(purge_button, 10, 0);
    reset_button = create_button(parent, "Factory Reset", lv_color_hex(THEME_COLOR_ERROR));

    // Register events for the toggle and buttons (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(logging_toggle, LV_EVENT_VALUE_CHANGED, ET::LOGGING_TOGGLE);
    bind_event(purge_button, LV_EVENT_CLICKED, ET::MENU_PURGE);
    bind_event(reset_button, LV_EVENT_CLICKED, ET::MENU_RESET);
}

void MenuScreen::create_stats_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_description_label(parent, "Lifetime totals for the grinder.");

    create_separator(parent, "Lifetime Statistics");
    create_data_label(parent, "Total Grinds:", &stat_total_grinds_label, true);
    create_data_label(parent, "Shots (S/D/C):", &stat_shots_label, true);
    create_data_label(parent, "Motor Runtime:", &stat_motor_runtime_label, true);
    create_data_label(parent, "Device Uptime:", &stat_device_uptime_label, true);
    create_data_label(parent, "Total Weight:", &stat_total_weight_label, true);
    create_data_label(parent, "Mode (W/T):", &stat_mode_grinds_label, true);
    create_data_label(parent, "Avg Accuracy:", &stat_avg_accuracy_label, true);
    create_data_label(parent, "Total Pulses:", &stat_total_pulses_label, true);

    refresh_stats_button = create_button(parent, "Refresh Stats");
    lv_obj_set_style_margin_top(refresh_stats_button, 10, 0);

    // Register event for the button (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(refresh_stats_button, LV_EVENT_CLICKED, ET::MENU_REFRESH_STATS);
}

void MenuScreen::refresh_statistics(bool show_overlay) {
    if (!visible) return;

    // Define the statistics loading operation
    auto load_statistics_operation = [this]() {
        // Log data
        set_label_text_int(sessions_label, grind_logger.get_total_flash_sessions());
        set_label_text_int(events_label, grind_logger.count_total_events_in_flash());
        set_label_text_int(measurements_label, grind_logger.count_total_measurements_in_flash());

        // Lifetime statistics
        set_label_text_int(stat_total_grinds_label, statistics_manager.get_total_grinds());

        // Shot type breakdown (Single/Double/Custom)
        char shot_text[32];
        snprintf(shot_text, sizeof(shot_text), "%lu / %lu / %lu",
                 statistics_manager.get_single_shots(),
                 statistics_manager.get_double_shots(),
                 statistics_manager.get_custom_shots());
        lv_label_set_text(stat_shots_label, shot_text);

        // Motor runtime (convert seconds to hours:minutes)
        uint64_t runtime_ms = statistics_manager.get_motor_runtime_ms();
        uint32_t runtime_sec = static_cast<uint32_t>(runtime_ms / 1000ULL);
        uint32_t hours = runtime_sec / 3600;
        uint32_t minutes = (runtime_sec % 3600) / 60;
        uint32_t seconds = runtime_sec % 60;
        char runtime_text[32];
        if (runtime_sec >= 3600) {
            snprintf(runtime_text, sizeof(runtime_text), "%luh %lum", hours, minutes);
        } else if (runtime_sec >= 60) {
            snprintf(runtime_text, sizeof(runtime_text), "%lum %lus", minutes, seconds);
        } else if (runtime_ms >= 1000) {
            float seconds_float = static_cast<float>(runtime_ms) / 1000.0f;
            snprintf(runtime_text, sizeof(runtime_text), "%.1fs", seconds_float);
        } else {
            snprintf(runtime_text, sizeof(runtime_text), "%llums", static_cast<unsigned long long>(runtime_ms));
        }
        lv_label_set_text(stat_motor_runtime_label, runtime_text);

        // Device uptime
        char uptime_text[32];
        uint32_t uptime_hours = statistics_manager.get_device_uptime_hrs();
        uint32_t uptime_minutes = statistics_manager.get_device_uptime_min_remainder();
        snprintf(uptime_text, sizeof(uptime_text), "%luh %lum", uptime_hours, uptime_minutes);
        lv_label_set_text(stat_device_uptime_label, uptime_text);

        // Total weight
        char weight_text[32];
        snprintf(weight_text, sizeof(weight_text), "%.2f kg", statistics_manager.get_total_weight_kg());
        lv_label_set_text(stat_total_weight_label, weight_text);

        // Mode grinds (Weight/Time)
        char mode_text[32];
        snprintf(mode_text, sizeof(mode_text), "%lu / %lu",
                 statistics_manager.get_weight_mode_grinds(),
                 statistics_manager.get_time_mode_grinds());
        lv_label_set_text(stat_mode_grinds_label, mode_text);

        // Average accuracy
        char accuracy_text[32];
        snprintf(accuracy_text, sizeof(accuracy_text), "%.2fg", statistics_manager.get_avg_accuracy_g());
        lv_label_set_text(stat_avg_accuracy_label, accuracy_text);

        // Total pulses (also show average)
        char pulses_text[32];
        snprintf(pulses_text, sizeof(pulses_text), "%lu (avg: %.1f)",
                 statistics_manager.get_total_pulses(),
                 statistics_manager.get_avg_pulses());
        lv_label_set_text(stat_total_pulses_label, pulses_text);
    };

    // Used for when we reload the statistics after a data purge
    if (!show_overlay){
        load_statistics_operation();
        return;
    }

    // Show blocking overlay while loading statistics
    auto& overlay = BlockingOperationOverlay::getInstance();
    overlay.show_and_execute(BlockingOperation::LOADING_STATISTICS, load_statistics_operation);
}

void MenuScreen::update_logging_toggle() {
    if (!logging_toggle) return;

    bool logging_enabled = ui_settings.logging_enabled();

    // Update toggle state
    if (logging_enabled) {
        lv_obj_add_state(logging_toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(logging_toggle, LV_STATE_CHECKED);
    }
}

LV_DEPRECATIONS_IGNORE_END

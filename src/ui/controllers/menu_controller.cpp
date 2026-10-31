#include "menu_controller.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_err.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <cstdint>
#include "../../config/constants.h"
#include "../../controllers/grind_controller.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../hardware/board_detect.h"
#include "../../logging/grind_logging.h"
#include "../../system/diagnostics_controller.h"
#include "../../system/menu_setting_models.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/statistics_manager.h"
#include "../components/blocking_overlay.h"
#include "../components/ui_operations.h"
#include "../event_bridge_lvgl.h"
#include "../ui_helpers.h"
#include "../ui_manager.h"
#include "../screens/menu_screen.h"

MenuUIController::MenuUIController(UIManager* manager)
    : ui_manager_(manager) {}

void MenuUIController::register_events() {
    if (!ui_manager_) {
        return;
    }

    using ET = EventBridgeLVGL::EventType;

    EventBridgeLVGL::register_handler(ET::MENU_CALIBRATE, [this](lv_event_t*) { handle_calibrate(); });
    EventBridgeLVGL::register_handler(ET::MENU_RESET, [this](lv_event_t*) { handle_reset(); });
    EventBridgeLVGL::register_handler(ET::MENU_PURGE, [this](lv_event_t*) { handle_purge(); });
    EventBridgeLVGL::register_handler(ET::MENU_MOTOR_TEST, [this](lv_event_t*) { handle_motor_test(); });
    EventBridgeLVGL::register_handler(ET::MENU_SCALE_OPEN, [this](lv_event_t*) { handle_scale_open(); });
    EventBridgeLVGL::register_handler(ET::MENU_SCALE_TARE, [this](lv_event_t*) { handle_scale_tare(); });
    EventBridgeLVGL::register_handler(ET::MENU_AUTOTUNE, [this](lv_event_t*) { handle_autotune(); });
    EventBridgeLVGL::register_handler(ET::MENU_DIAGNOSTIC_RESET, [this](lv_event_t*) { handle_diagnostics_reset(); });
    EventBridgeLVGL::register_handler(ET::MENU_BACK, [this](lv_event_t*) { handle_back(); });
    EventBridgeLVGL::register_handler(ET::MENU_REFRESH_STATS, [this](lv_event_t*) { handle_refresh_stats(); });

    EventBridgeLVGL::register_handler(ET::BLE_TOGGLE, [this](lv_event_t*) { handle_ble_toggle(); });
    register_setting_toggle_events();
    register_setting_slider_events();

    register_setting_radio_events();

    EventBridgeLVGL::register_handler(ET::WIFI_FORGET, [this](lv_event_t*) { handle_wifi_forget(); });
    EventBridgeLVGL::register_handler(ET::WIFI_PUSH_TOGGLE, [this](lv_event_t*) { handle_wifi_push_toggle(); });
    EventBridgeLVGL::register_handler(ET::WIFI_CONFIRM_TOGGLE, [this](lv_event_t*) { handle_wifi_confirm_toggle(); });
    EventBridgeLVGL::register_handler(ET::WIFI_BETA_TOGGLE, [this](lv_event_t*) { handle_wifi_beta_toggle(); });
    EventBridgeLVGL::register_handler(ET::WIFI_CHECK_UPDATE, [this](lv_event_t*) { handle_wifi_check_update(); });
    EventBridgeLVGL::register_handler(ET::WIFI_INSTALL_UPDATE, [this](lv_event_t*) { handle_wifi_install_update(); });


    // Note: Event registration for menu widgets is done in the page creation functions
    // (menu_screen*.cpp): the widgets are created once in MenuScreen::create and only hidden afterwards.
}

void MenuUIController::update() {
    if (!ui_manager_) {
        return;
    }

    // Each page is refreshed only while it is on screen; explicit callers (toggle handlers,
    // show(), diagnostics reset, autotune completion) still refresh their page directly.
    auto& menu_screen = ui_manager_->menu_screen;
    WeightSensor* sensor = ui_manager_->hardware_manager->get_weight_sensor();

    if (menu_screen.is_info_page_current()) {
        menu_screen.update_info(sensor, millis(), ESP.getFreeHeap());
    }
    if (menu_screen.is_diagnostics_page_current()) {
        menu_screen.update_diagnostics(sensor);
    }
    if (menu_screen.is_ble_page_current()) {
        menu_screen.update_ble_status();
    }
    update_wifi_page();

    if (ui_manager_->menu_screen.is_scale_page_active()) {
        float display_weight = sensor ? sensor->get_display_weight() : 0.0f;
        ui_manager_->menu_screen.update_scale_weight(display_weight);
    }
}

void MenuUIController::handle_calibrate() {
    if (ui_manager_) {
        ui_manager_->switch_to_state(UIState::CALIBRATION);
    }
}

void MenuUIController::handle_reset() {
    if (!ui_manager_) return;

    ui_manager_->show_confirmation(
        "FACTORY RESET",
        "This will reset all settings to factory defaults:\n\n"
        "• Profile weights\n"
        "• Calibration data\n"
        "• Grind history\n"
        "• Lifetime statistics\n\n"
        "This action cannot be undone.",
        "RESET",
        lv_color_hex(THEME_COLOR_ERROR),
        [this]() { perform_factory_reset(); },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

void MenuUIController::handle_purge() {
    if (!ui_manager_) return;

    ui_manager_->show_confirmation(
        "PURGE LOGS",
        "This will remove all saved grind log files from flash.\n"
        "Lifetime statistics will be preserved."
        "\n\n"
        "This action cannot be undone.",
        "PURGE LOGS",
        lv_color_hex(THEME_COLOR_ERROR),
        [this]() { execute_purge_operation(); },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

void MenuUIController::handle_scale_open() {
    if (!ui_manager_) return;
    auto* hardware = ui_manager_->get_hardware_manager();
    if (!hardware) return;

    ui_manager_->menu_screen.reset_scale_display();

    UIOperations::execute_tare(hardware, [this]() {
        if (!ui_manager_) return;
        ui_manager_->refresh_auto_action_settings();

        auto* sensor = ui_manager_->hardware_manager->get_weight_sensor();
        float weight = sensor ? sensor->get_display_weight() : 0.0f;
        if (ui_manager_->menu_screen.is_scale_page_active()) {
            ui_manager_->menu_screen.update_scale_weight(weight);
        }
    });
}

void MenuUIController::handle_scale_tare() {
    if (!ui_manager_) return;
    auto* hardware = ui_manager_->get_hardware_manager();
    if (!hardware) return;

    UIOperations::execute_tare(hardware, [this]() {
        if (!ui_manager_) return;
        ui_manager_->refresh_auto_action_settings();

        auto* sensor = ui_manager_->hardware_manager->get_weight_sensor();
        float weight = sensor ? sensor->get_display_weight() : 0.0f;
        if (ui_manager_->menu_screen.is_scale_page_active()) {
            ui_manager_->menu_screen.update_scale_weight(weight);
        }
    });
}

void MenuUIController::handle_autotune() {
    if (!ui_manager_) return;

    const MotorTestBlock block = ui_manager_->motor_pulse_block();
    if (block != MotorTestBlock::NONE) {
        ui_manager_->show_motor_pulse_blocked_notice(block);
        return;
    }

    // Show confirmation screen with setup instructions
    auto autotune_controller = ui_manager_->autotune_controller_.get();
    if (autotune_controller) {
        ui_manager_->show_confirmation(
            "Auto-Tune Setup",
            "Before starting:\n\n"
            "- Beans loaded\n"
            "- Cup on scale\n\n"
            "Process takes ~1 min.",
            "START",
            lv_color_hex(THEME_COLOR_ACCENT),
            [autotune_controller]() { autotune_controller->confirm_and_begin(); },
            "CANCEL",
            [this]() { return_to_menu(); }
        );
    }
}

void MenuUIController::handle_back() {
    if (!ui_manager_) return;
    ui_manager_->set_current_tab(3);
    ui_manager_->switch_to_state(UIState::READY);
}

void MenuUIController::handle_refresh_stats() {
    if (!ui_manager_) return;
    ui_manager_->menu_screen.refresh_statistics();
}

void MenuUIController::handle_diagnostics_reset() {
    if (!ui_manager_) return;

    ui_manager_->show_confirmation(
        "Reset Diagnostics",
        "This will clear all active diagnostic warnings.\n\nContinue?",
        "RESET",
        lv_color_hex(THEME_COLOR_WARNING),
        [this]() { perform_diagnostics_reset(); },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

void MenuUIController::perform_diagnostics_reset() {
    if (!ui_manager_) return;

    auto* diagnostics = ui_manager_->diagnostics_controller_.get();
    if (diagnostics) {
        diagnostics->reset_diagnostic(DiagnosticCode::LOAD_CELL_NOISY_SUSTAINED);
        diagnostics->reset_diagnostic(DiagnosticCode::MECHANICAL_INSTABILITY);
        diagnostics->reset_noise_tracking();
    }

    auto* grind_controller = ui_manager_->get_grind_controller();
    if (grind_controller) {
        grind_controller->reset_mechanical_anomaly_count();
    }

    auto* hardware = ui_manager_->get_hardware_manager();
    auto* sensor = hardware ? hardware->get_weight_sensor() : nullptr;
    if (sensor) {
        ui_manager_->menu_screen.update_diagnostics(sensor);
    }
}

void MenuUIController::handle_ble_toggle() {
    if (!ui_manager_ || !ui_manager_->bluetooth_manager) return;

    auto* ble = ui_manager_->bluetooth_manager;
    if (ble->is_enabled()) {
        ble->disable();
        LOG_DEBUG_PRINTLN("Bluetooth disabled by user");
        ui_manager_->menu_screen.update_ble_status();
        return;
    }

    auto completion = [this]() {
        ui_manager_->menu_screen.update_ble_status();
    };

    auto operation = [ble]() {
        ble->enable();
        LOG_DEBUG_PRINTLN("Bluetooth enabled by user (30 minute timeout)");
    };

    auto& overlay = BlockingOperationOverlay::getInstance();
    overlay.show_and_execute(BlockingOperation::BLE_ENABLING, operation, completion);
}

void MenuUIController::perform_factory_reset() {
    if (!ui_manager_) return;

    LOG_DEBUG_PRINTLN("Factory reset: clearing NVS preferences and rebooting...");

    nvs_flash_deinit();
    esp_err_t erase_result = nvs_flash_erase();

    if (erase_result == ESP_OK) {
        LOG_DEBUG_PRINTLN("Factory reset: NVS erase successful. Restarting device...");
    } else {
        LOG_DEBUG_PRINTF("Factory reset: NVS erase failed (code %d). Forcing restart...\n",
                         static_cast<int>(erase_result));
    }

    delay(100);
    esp_restart();
}

void MenuUIController::execute_purge_operation() {
    if (!ui_manager_) return;

    auto completion = [this]() {
        return_to_menu();
        ui_manager_->menu_screen.refresh_statistics(false);
    };

    auto purge_task = []() {
        LOG_DEBUG_PRINTLN("\n=== PURGE GRIND LOGS INITIATED ===");
        extern GrindLogger grind_logger;
        bool success = grind_logger.clear_all_sessions_from_flash();
        if (success) {
            LOG_DEBUG_PRINTLN("Grind logs purged successfully - reinitializing logger...");
        } else {
            LOG_DEBUG_PRINTLN("ERROR: Failed to purge all grind log data!");
        }
    };

    auto& overlay = BlockingOperationOverlay::getInstance();
    overlay.show_and_execute(BlockingOperation::CUSTOM, purge_task, completion,
                             "PURGING LOGS...\nPlease wait");
}

void MenuUIController::return_to_menu() {
    if (!ui_manager_) return;
    ui_manager_->set_current_tab(3);
    ui_manager_->switch_to_state(UIState::MENU);
}

float MenuUIController::get_normal_brightness() const {
    if (!ui_manager_ || !ui_manager_->hardware_manager) {
        return USER_SCREEN_BRIGHTNESS_NORMAL;
    }

    return brightness_fraction_floor(ui_settings.brightness_normal());
}

float MenuUIController::get_screensaver_brightness() const {
    if (!ui_manager_ || !ui_manager_->hardware_manager) {
        return USER_SCREEN_BRIGHTNESS_DIMMED;
    }

    return brightness_fraction_floor(ui_settings.brightness_screensaver());
}


#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "../../controllers/motor_test_check.h"
#include "../../system/menu_setting_table.h"

class UIManager;

// Handles the interactive menu: setting toggles, sliders and radio groups, maintenance actions, and stats

class MenuUIController {
public:
    explicit MenuUIController(UIManager* manager);

    void register_events();
    void update();
    void handle_calibrate();
    void handle_reset();
    void handle_purge();
    void handle_motor_test();
    // Starts the motor test without the confirm dialog (web panel): re-checks motor_pulse_block(), starts the pulse,
    // shows the overlay and the poll timer. NONE = running; anything else = refused, nothing started and no notice
    // shown. UI task only.
    MotorTestBlock begin_motor_test();
    // True from a started Motor Test until finish_motor_test() (the poll timer is the single source)
    bool is_motor_test_active() const { return motor_timer_ != nullptr; }
    void handle_scale_open();
    void handle_scale_tare();
    void handle_autotune();
    void handle_back();
    void handle_refresh_stats();
    void handle_diagnostics_reset();
    void handle_ble_toggle();
    void handle_grind_mode_radio_button();
    void handle_setting_toggle(ToggleSetting id);
    void handle_setting_slider(SliderSetting id, bool released);
    void handle_grinder_purge_mode_radio_button();
    void handle_board_override_radio_button();
    void handle_wifi_forget();
    void handle_wifi_push_toggle();
    void handle_wifi_web_toggle();
    void handle_wifi_confirm_toggle();
    void handle_wifi_beta_toggle();
    void handle_wifi_check_update();
    void handle_wifi_install_update();

    float get_normal_brightness() const;
    float get_screensaver_brightness() const;

private:
    UIManager* ui_manager_;
    lv_timer_t* motor_timer_{};  // Motor Test poll timer; non-null exactly while a test runs
    uint32_t motor_test_start_ms_{};
    uint32_t last_wifi_refresh_ms_{};

    static constexpr uint32_t WIFI_PAGE_REFRESH_INTERVAL_MS = 1000;  // WiFi page runtime values refresh rate
    void update_wifi_page();
    void register_setting_toggle_events();  // menu_controller_settings.cpp
    void register_setting_slider_events();  // menu_controller_settings.cpp
    void register_setting_radio_events();   // menu_controller_settings.cpp
    void install_update_confirmed(const char* version);

    void perform_factory_reset();
    void execute_purge_operation();
    void run_motor_test();
    void stop_motor_timer();
    void motor_timer_cb(lv_timer_t* timer);
    void finish_motor_test(bool completed, bool stopped_by_user);
    void show_motor_test_result(MotorTestOutcome outcome, uint32_t ran_ms);
    static void static_motor_timer_cb(lv_timer_t* timer);
    void return_to_menu();
    void perform_diagnostics_reset();
};

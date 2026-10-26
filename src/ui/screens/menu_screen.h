#pragma once
#include <lvgl.h>
#include "../../config/constants.h"
#include "../../bluetooth/manager.h"
#include "../../controllers/grind_controller.h"
#include "../../system/diagnostics_controller.h"
#include "../../network/pull_ui_text.h"
#include "../../network/wifi_status_text.h"
#include "../ui_helpers.h"

class GrindingScreen;  // Forward declaration

class MenuScreen {
private:
    lv_obj_t* screen = nullptr;
    lv_obj_t* menu = nullptr;
    lv_obj_t* info_page = nullptr;
    lv_obj_t* bluetooth_page = nullptr;
    lv_obj_t* display_page = nullptr;
    lv_obj_t* grind_mode_page = nullptr;
    lv_obj_t* data_page = nullptr;
    lv_obj_t* stats_page = nullptr;
    lv_obj_t* diagnostics_page = nullptr;
    lv_obj_t* scale_page = nullptr;
    lv_obj_t* board_page = nullptr;
    lv_obj_t* wifi_page = nullptr;

    // Info tab elements
    lv_obj_t* uptime_label = nullptr;
    lv_obj_t* memory_label = nullptr;
    lv_obj_t* instant_label = nullptr;
    lv_obj_t* samples_label = nullptr;
    lv_obj_t* raw_label = nullptr;
    lv_obj_t* sessions_label = nullptr;
    lv_obj_t* events_label = nullptr;
    lv_obj_t* measurements_label = nullptr;
    lv_obj_t* refresh_stats_button = nullptr;

    // Lifetime statistics labels
    lv_obj_t* stat_total_grinds_label = nullptr;
    lv_obj_t* stat_shots_label = nullptr;
    lv_obj_t* stat_motor_runtime_label = nullptr;
    lv_obj_t* stat_device_uptime_label = nullptr;
    lv_obj_t* stat_total_weight_label = nullptr;
    lv_obj_t* stat_mode_grinds_label = nullptr;
    lv_obj_t* stat_avg_accuracy_label = nullptr;
    lv_obj_t* stat_total_pulses_label = nullptr;
    
    // Menu toggle elements
    lv_obj_t* ble_toggle = nullptr;
    lv_obj_t* ble_startup_toggle = nullptr;
    lv_obj_t* ble_status_label = nullptr;
    lv_obj_t* ble_timer_label = nullptr;
    lv_obj_t* logging_toggle = nullptr;
    lv_obj_t* brightness_normal_slider = nullptr;
    lv_obj_t* brightness_screensaver_slider = nullptr;
    lv_obj_t* brightness_normal_label = nullptr;
    lv_obj_t* brightness_screensaver_label = nullptr;
    lv_obj_t* purge_button = nullptr;
    lv_obj_t* reset_button = nullptr;
    
    // Grind mode tab elements
    lv_obj_t* grind_mode_radio_group = nullptr;
    lv_obj_t* grind_mode_swipe_toggle = nullptr;
    lv_obj_t* auto_start_toggle = nullptr;
    lv_obj_t* auto_return_toggle = nullptr;
    lv_obj_t* grinder_purge_mode_radio_group = nullptr;
    lv_obj_t* grinder_purge_amount_slider = nullptr;
    lv_obj_t* grinder_purge_amount_label = nullptr;
    lv_obj_t* grind_freshness_hours_slider = nullptr;
    lv_obj_t* grind_freshness_hours_label = nullptr;

    // Hardware board page elements
    lv_obj_t* board_status_label = nullptr;
    lv_obj_t* board_override_radio_group = nullptr;

    // WiFi page elements (menu_screen_wifi.cpp)
    lv_obj_t* wifi_status_label = nullptr;
    lv_obj_t* wifi_network_label = nullptr;
    lv_obj_t* wifi_ip_label = nullptr;
    lv_obj_t* wifi_signal_label = nullptr;
    lv_obj_t* wifi_name_label = nullptr;
    lv_obj_t* wifi_setup_block = nullptr;      // shown while no credentials are stored
    lv_obj_t* wifi_ble_warning_label = nullptr;
    lv_obj_t* wifi_manage_block = nullptr;     // shown while credentials are stored
    lv_obj_t* wifi_confirm_block = nullptr;    // Confirm description + toggle, hidden while Receive is off
    lv_obj_t* wifi_forget_button = nullptr;
    lv_obj_t* wifi_push_toggle = nullptr;      // "Receive": arms the push endpoint
    lv_obj_t* wifi_confirm_toggle = nullptr;   // "Confirm": every update needs a tap on INSTALL
    lv_obj_t* wifi_listen_label = nullptr;     // "Listening: <name>.local" while the endpoint runs
    // Firmware section (pull update)
    lv_obj_t* wifi_fw_latest_label = nullptr;  // "Latest:" value
    lv_obj_t* wifi_beta_toggle = nullptr;      // include prereleases
    lv_obj_t* wifi_check_button = nullptr;
    lv_obj_t* wifi_install_button = nullptr;   // only visible while a fresh AVAILABLE result exists
    lv_obj_t* wifi_fw_notice_label = nullptr;  // refusal / error text under the buttons
    PullFailure wifi_pull_notice = PullFailure::NONE;  // refusal of the user's last tap, expires after UPDATE_PULL_NOTICE_MS
    uint32_t wifi_pull_notice_ms = 0;
    char wifi_ssid_display[WIFI_STATUS_TEXT_BUF] = {0};  // shortened SSID, cached from NVS
    bool wifi_has_credentials = false;                   // cached from NVS
    uint32_t wifi_loaded_revision = 0;                   // WiFiManager::credentials_revision() at last reload
    bool wifi_page_active = false;                     // provisioning window opened by this page

    // Tools entries / scale page elements
    lv_obj_t* scale_item = nullptr;
    lv_obj_t* cal_button = nullptr;
    lv_obj_t* motor_test_button = nullptr;
    lv_obj_t* autotune_button = nullptr;
    lv_obj_t* scale_weight_label = nullptr;
    lv_obj_t* scale_tare_button = nullptr;

    // Diagnostics tab elements
    lv_obj_t* diag_status_label = nullptr;
    lv_obj_t* diag_calibration_factor_label = nullptr;
    lv_obj_t* diag_std_dev_g_label = nullptr;
    lv_obj_t* diag_std_dev_adc_label = nullptr;
    lv_obj_t* diag_noise_level_label = nullptr;
    lv_obj_t* diag_motor_latency_label = nullptr;
    lv_obj_t* diag_info_label = nullptr;
    lv_obj_t* diag_reset_button = nullptr;

    // Common elements
    bool visible = false;
    bool scale_active = false;
    
    BluetoothManager* bluetooth_manager = nullptr;
    GrindController* grind_controller = nullptr;
    GrindingScreen* grinding_screen = nullptr;
    class HardwareManager* hardware_manager = nullptr; // Forward declaration to access preferences
    DiagnosticsController* diagnostics_controller = nullptr;

public:
    static constexpr float kPurgeSliderScale = 10.0f; // Slider uses 0.1g increments

    void create(BluetoothManager* bluetooth, GrindController* grind_ctrl, GrindingScreen* grind_screen, class HardwareManager* hw_mgr, DiagnosticsController* diag_ctrl);
    void show();
    void hide();
    void update_info(const WeightSensor* weight_sensor, unsigned long uptime_ms, size_t free_heap);
    void update_diagnostics(WeightSensor* weight_sensor);
    void update_ble_status();
    void refresh_statistics(bool show_overlay = true);
    void update_brightness_labels(int normal_percent = -1, int screensaver_percent = -1); // Use negative value to leave unchanged
    void update_brightness_sliders();
    void update_bluetooth_startup_toggle();
    void update_logging_toggle();
    void update_grind_mode_toggles();
    void update_grinder_purge_amount_label(float amount_g);
    void update_grind_freshness_hours_label(float hours);
    void update_board_page();
    // Runtime values only (state, IP, signal, blocks); cheap, no NVS access
    void update_wifi_page();
    // Re-reads the stored SSID / credentials flag from NVS (page entry and after Forget), then update_wifi_page()
    void reload_wifi_page();
    bool is_wifi_page_current() const;
    // True while the menu is visible and that page is the one on screen (per-frame updates run only then)
    bool is_info_page_current() const;
    bool is_diagnostics_page_current() const;
    bool is_ble_page_current() const;
    // Shows why a tap on Check / Install was refused until it expires (PullFailure::NONE clears it)
    void set_wifi_pull_notice(PullFailure why);
    // True if credentials were stored/cleared since reload_wifi_page() last ran
    bool wifi_credentials_changed() const;
    void reset_scale_display();
    void update_scale_weight(float weight);

    bool is_visible() const { return visible; }
    lv_obj_t* get_screen() const { return screen; }
    lv_obj_t* get_tabview() const { return menu; }
    lv_obj_t* get_diagnostics_page() const { return diagnostics_page; }
    lv_obj_t* get_cal_button() const { return cal_button; }
    lv_obj_t* get_purge_button() const { return purge_button; }
    lv_obj_t* get_reset_button() const { return reset_button; }
    lv_obj_t* get_motor_test_button() const { return motor_test_button; }
    lv_obj_t* get_autotune_button() const { return autotune_button; }
    bool is_scale_page_active() const { return scale_active; }
    lv_obj_t* get_ble_toggle() const { return ble_toggle; }
    lv_obj_t* get_ble_startup_toggle() const { return ble_startup_toggle; }
    lv_obj_t* get_logging_toggle() const { return logging_toggle; }
    lv_obj_t* get_refresh_stats_button() const { return refresh_stats_button; }
    lv_obj_t* get_diag_reset_button() const { return diag_reset_button; }
    lv_obj_t* get_brightness_normal_slider() const { return brightness_normal_slider; }
    lv_obj_t* get_brightness_screensaver_slider() const { return brightness_screensaver_slider; }
    lv_obj_t* get_grind_mode_radio_group() const { return grind_mode_radio_group; }
    lv_obj_t* get_grind_mode_swipe_toggle() const { return grind_mode_swipe_toggle; }
    lv_obj_t* get_auto_start_toggle() const { return auto_start_toggle; }
    lv_obj_t* get_auto_return_toggle() const { return auto_return_toggle; }
    lv_obj_t* get_grinder_purge_mode_radio_group() const { return grinder_purge_mode_radio_group; }
    lv_obj_t* get_grinder_purge_amount_slider() const { return grinder_purge_amount_slider; }
    lv_obj_t* get_grind_freshness_hours_slider() const { return grind_freshness_hours_slider; }
    lv_obj_t* get_board_override_radio_group() const { return board_override_radio_group; }
    lv_obj_t* get_wifi_push_toggle() const { return wifi_push_toggle; }
    lv_obj_t* get_wifi_confirm_toggle() const { return wifi_confirm_toggle; }
    lv_obj_t* get_wifi_beta_toggle() const { return wifi_beta_toggle; }
    lv_obj_t* get_wifi_check_button() const { return wifi_check_button; }
    lv_obj_t* get_wifi_install_button() const { return wifi_install_button; }

private:
    void create_menu_ui();
    void create_info_page(lv_obj_t* parent);
    void create_bluetooth_page(lv_obj_t* parent);
    void create_display_page(lv_obj_t* parent);
    void create_grind_mode_page(lv_obj_t* parent);
    void create_scale_page(lv_obj_t* parent);
    void create_board_page(lv_obj_t* parent);
    void create_wifi_page(lv_obj_t* parent);
    void apply_wifi_update_toggles();  // Receive / Confirm switch states and greying (menu_screen_wifi.cpp)
    void update_wifi_firmware();       // Firmware section: Latest row, buttons, notice (menu_screen_wifi.cpp)
    // Opens/closes the BLE provisioning window to match "WiFi page is the current page of the visible menu"
    void sync_wifi_provisioning_window();
    void create_data_page(lv_obj_t* parent);
    void create_stats_page(lv_obj_t* parent);
    void create_diagnostics_page(lv_obj_t* parent);
    lv_obj_t* create_separator(lv_obj_t* parent, const char* text = nullptr);
    lv_obj_t* create_menu_item(lv_obj_t* parent, const char* text);
    lv_obj_t *create_toggle_row(lv_obj_t *parent, const char *text,lv_obj_t **out_toggle);
    lv_obj_t *create_slider_row(lv_obj_t *parent, const char *text,
                                lv_obj_t **label, lv_obj_t **slider,
                                lv_color_t slider_color = lv_color_hex(THEME_COLOR_ACCENT),
                                uint32_t min = 0, uint32_t max = 100);
    lv_obj_t *create_static_data_label(lv_obj_t *parent, const char *name,
                                       const char *value);
    lv_obj_t *create_data_label(lv_obj_t *parent, const char *name,
                                lv_obj_t **variable, bool stacked = false);
    lv_obj_t *create_description_label(lv_obj_t *parent, const char *text);
};

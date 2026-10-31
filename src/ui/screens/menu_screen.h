#pragma once
#include <lvgl.h>
#include "../../config/constants.h"
#include "../../bluetooth/manager.h"
#include "../../controllers/grind_controller.h"
#include "../../system/diagnostics_controller.h"
#include "../../network/pull_ui_text.h"
#include "../../network/wifi_status_text.h"
#include "../../system/menu_setting_table.h"
#include "../event_bridge_lvgl.h"
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
    lv_obj_t* ble_status_label = nullptr;
    lv_obj_t* ble_timer_label = nullptr;
    lv_obj_t* setting_toggles_[TOGGLE_SETTING_COUNT] = {};  // table-driven toggles (menu_screen_settings.cpp)
    lv_obj_t* setting_sliders_[SLIDER_SETTING_COUNT] = {};        // table-driven sliders (menu_screen_settings.cpp)
    lv_obj_t* setting_slider_labels_[SLIDER_SETTING_COUNT] = {};  // the label row above each slider
    lv_obj_t* purge_button = nullptr;
    lv_obj_t* reset_button = nullptr;
    
    lv_obj_t* setting_radios_[RADIO_SETTING_COUNT] = {};  // table-driven radio groups (menu_screen_settings.cpp)

    // Hardware board page elements
    lv_obj_t* board_status_label = nullptr;

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

    void create(BluetoothManager* bluetooth, GrindController* grind_ctrl, GrindingScreen* grind_screen, class HardwareManager* hw_mgr, DiagnosticsController* diag_ctrl);
    void show();
    void hide();
    void update_info(const WeightSensor* weight_sensor, unsigned long uptime_ms, size_t free_heap);
    void update_diagnostics(WeightSensor* weight_sensor);
    void update_ble_status();
    void refresh_statistics(bool show_overlay = true);
    // Sets all table-driven toggles to the stored values (menu_screen_settings.cpp)
    void refresh_setting_toggles();
    // Selects the stored button of every table-driven radio group (menu_screen_settings.cpp)
    void refresh_setting_radios();
    // Sets all table-driven sliders and their labels to the stored values (menu_screen_settings.cpp)
    void refresh_setting_sliders();
    // Shows the label text of a slider position
    void set_setting_slider_label(SliderSetting id, int pos);
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
    lv_obj_t* get_setting_toggle(ToggleSetting id) const { return setting_toggles_[static_cast<size_t>(id)]; }
    static EventBridgeLVGL::EventType setting_toggle_event(ToggleSetting id);
    lv_obj_t* get_refresh_stats_button() const { return refresh_stats_button; }
    lv_obj_t* get_diag_reset_button() const { return diag_reset_button; }
    lv_obj_t* get_setting_slider(SliderSetting id) const { return setting_sliders_[static_cast<size_t>(id)]; }
    static EventBridgeLVGL::EventType setting_slider_event(SliderSetting id, bool released);
    lv_obj_t* get_setting_radio(RadioSetting id) const { return setting_radios_[static_cast<size_t>(id)]; }
    static EventBridgeLVGL::EventType setting_radio_event(RadioSetting id);
    lv_obj_t* get_wifi_push_toggle() const { return wifi_push_toggle; }
    lv_obj_t* get_wifi_confirm_toggle() const { return wifi_confirm_toggle; }
    lv_obj_t* get_wifi_beta_toggle() const { return wifi_beta_toggle; }
    lv_obj_t* get_wifi_check_button() const { return wifi_check_button; }
    lv_obj_t* get_wifi_install_button() const { return wifi_install_button; }

private:
    void create_menu_ui();
    bool is_page_current(lv_obj_t* page) const;  // menu visible and `page` is the one on screen
    void create_info_page(lv_obj_t* parent);
    void create_bluetooth_page(lv_obj_t* parent);
    // Label + switch row of a table-driven setting, wired to its event (menu_screen_settings.cpp)
    void create_setting_toggle_row(lv_obj_t* parent, ToggleSetting id);
    // Label + slider row of a table-driven setting, wired to its events (menu_screen_settings.cpp)
    void create_setting_slider_row(lv_obj_t* parent, SliderSetting id);
    // Radio button group of a table-driven setting, wired to its event (menu_screen_settings.cpp)
    void create_setting_radio(lv_obj_t* parent, RadioSetting id);
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
};

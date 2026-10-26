#pragma once
#include <lvgl.h>
#include <functional>
#include <cstdint>
#include <memory>
#include "../controllers/grind_start_check.h"
#include "../controllers/motor_test_check.h"
#include "components/blocking_overlay.h"
#include "components/ui_operations.h"
#include "screens/ready_screen.h"
#include "screens/edit_screen.h"
#include "screens/grinding_screen.h"
#include "screens/menu_screen.h"
#include "screens/calibration_screen.h"
#include "screens/confirm_screen.h"
#include "screens/purge_confirm_screen.h"
#include "screens/ota_screen.h"
#include "screens/ota_update_failed_screen.h"
#include "screens/autotune_screen.h"
#include "event_bridge_lvgl.h"
#include "controllers/calibration_controller.h"
#include "controllers/autotune_controller.h"
#include "controllers/confirm_controller.h"
#include "controllers/edit_controller.h"
#include "controllers/grinding_controller.h"
#include "controllers/jog_adjust_controller.h"
#include "controllers/ota_data_export_controller.h"
#include "controllers/ready_controller.h"
#include "controllers/screen_timeout_controller.h"
#include "controllers/menu_controller.h"
#include "controllers/status_indicator_controller.h"
#include "controllers/wifi_update_controller.h"
#include "../system/state_machine.h"
#include "../system/diagnostics_controller.h"
#include "../controllers/profile_controller.h"
#include "../controllers/grind_controller.h"
#include "../controllers/grind_events.h"
#include "../controllers/grind_mode.h"
#include "../hardware/hardware_manager.h"
#include "../hardware/board_detect.h"
#include "../bluetooth/manager.h"

/*
 * AVAILABLE FONTS AND THEIR USAGE:
 * - lv_font_montserrat_24: Standard text and button labels
 * - lv_font_montserrat_32: Button symbols (OK, CLOSE, PLUS, MINUS)
 * - lv_font_montserrat_36: Screen titles
 * - lv_font_montserrat_56: Large weight displays
 * 
 * JOG ACCELERATION STAGES:
 * - Stage 1 (0-2s): 1.0g/s (100ms intervals, 1x multiplier)
 * - Stage 2 (2-4s): 4.7g/s (64ms intervals, 3x multiplier) 
 * - Stage 3 (4-6s): 9.4g/s (64ms intervals, 6x multiplier)
 * - Stage 4 (6s+): 20.3g/s (64ms intervals, 13x multiplier)
 */

class UIManager {
    friend class ReadyUIController;
    friend class EditUIController;
    friend class MenuUIController;
    friend class CalibrationUIController;
    friend class GrindingUIController;
    friend class ConfirmUIController;
    friend class StatusIndicatorController;
    friend class OtaDataExportController;
    friend class ScreenTimeoutController;
    friend class JogAdjustController;
    friend class WifiUpdateUIController;

private:
    HardwareManager* hardware_manager = nullptr;
    StateMachine* state_machine = nullptr;
    ProfileController* profile_controller = nullptr;
    GrindController* grind_controller = nullptr;
    BluetoothManager* bluetooth_manager = nullptr;
    
    lv_timer_t* jog_timer = nullptr;

    float edit_target = 0.0f;
    float original_target = 0.0f;
    int current_tab = 0;
    GrindMode current_mode = GrindMode::WEIGHT;
    bool initialized = false;
    unsigned long jog_start_time = 0;
    int jog_stage = 1;
    int jog_direction = 0;

    // Static instance pointer for grind event callback
    static UIManager* instance;

    // Controller instances (feature-focused)
    std::unique_ptr<ReadyUIController> ready_controller_;
    std::unique_ptr<EditUIController> edit_controller_;
    std::unique_ptr<GrindingUIController> grinding_controller_;
    std::unique_ptr<MenuUIController> menu_controller_;
    std::unique_ptr<StatusIndicatorController> status_indicator_controller_;
    std::unique_ptr<CalibrationUIController> calibration_controller_;
    std::unique_ptr<AutoTuneUIController> autotune_controller_;
    std::unique_ptr<ConfirmUIController> confirm_controller_;
    std::unique_ptr<OtaDataExportController> ota_data_export_controller_;
    std::unique_ptr<ScreenTimeoutController> screen_timeout_controller_;
    std::unique_ptr<JogAdjustController> jog_adjust_controller_;
    std::unique_ptr<WifiUpdateUIController> wifi_update_controller_;
    std::unique_ptr<DiagnosticsController> diagnostics_controller_;

public:
    ReadyScreen ready_screen;
    EditScreen edit_screen;
    GrindingScreen grinding_screen;
    MenuScreen menu_screen;
    CalibrationScreen calibration_screen;
    ConfirmScreen confirm_screen;
    PurgeConfirmScreen purge_confirm_screen;
    AutoTuneScreen autotune_screen;
    OTAScreen ota_screen;
    OtaUpdateFailedScreen ota_update_failed_screen;

    ~UIManager();
    void init(HardwareManager* hw_mgr, StateMachine* sm, 
              ProfileController* pc, GrindController* gc, BluetoothManager* bluetooth);
    void update();
    void switch_to_state(UIState new_state);
    // Helper method to show confirmation dialog
    void show_confirmation(const char* title, const char* message,
                           const char* confirm_text, lv_color_t confirm_color,
                           std::function<void()> on_confirm,
                           const char* cancel_text = "CANCEL",
                           std::function<void()> on_cancel = nullptr);
    
    // True when the grinder output can run: board allows it and the grinder initialised
    bool motor_available() const;

    // Tells the user why grinding / motor actions are unavailable (motor_available() is false)
    void show_motor_disabled_notice();

    // Tells the user why a grind did not start (block != NONE): the motor dialog for MOTOR_DISABLED,
    // otherwise a "CAN'T START" notice with the cause. A single OK button dismisses it.
    void show_start_blocked_notice(GrindStartBlock block);

    // Whether a finite motor pulse (Motor Test, Auto-Tune) may start now; NONE when it may.
    MotorTestBlock motor_pulse_block() const;

    // Refusal notice for a motor_pulse_block() answer (block != NONE): the motor dialog for
    // MOTOR_DISABLED, otherwise a single-OK notice with the cause. Not for use inside a confirm callback.
    void show_motor_pulse_blocked_notice(MotorTestBlock block);

    // Runs show_notice from a one-shot timer, after the confirm dialog that is handling a tap has
    // returned to its previous state (a dialog opened inside a confirm callback corrupts that state).
    // The notice is dropped unless the menu is the current state by then.
    void show_notice_after_dialog(std::function<void()> show_notice);

    // Degraded mode (board UNKNOWN): asks which board this is, stores the answer as
    // the board override and restarts so detection picks it up. Reuses the confirm
    // dialog, so nothing extra is allocated.
    void show_board_picker();

    // Static instance getter
    static UIManager* get_instance() { return instance; }
    
    // Public accessors for static callbacks
    ProfileController* get_profile_controller() { return profile_controller; }
    HardwareManager* get_hardware_manager() { return hardware_manager; }
    GrindController* get_grind_controller() { return grind_controller; }
    OtaDataExportController* get_ota_data_export_controller() { return ota_data_export_controller_.get(); }
    void set_current_tab(int tab) { current_tab = tab; }
    
    void set_background_active(bool active);
    void refresh_auto_action_settings();
    

private:
    void create_ui();
    void update_auto_actions();
    void apply_board_choice(BoardOverride choice);

    // State-specific update methods

    // Controller wiring helpers
    void init_controllers();
    void register_controller_events();
    struct AutoActionState {
        bool auto_start_enabled = false;
        bool auto_return_enabled = false;
        uint32_t last_auto_start_ms = 0;
        uint32_t last_auto_return_ms = 0;
    } auto_actions_;
};

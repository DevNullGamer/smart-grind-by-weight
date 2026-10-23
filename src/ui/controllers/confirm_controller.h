#pragma once
#include <functional>
#include <lvgl.h>

class UIManager;
enum class UIState;

// Provides confirmation dialogs with custom callbacks and button text

class ConfirmUIController {
public:
    explicit ConfirmUIController(UIManager* manager);

    void register_events();
    void update();

    void show(const char* title,
              const char* message,
              const char* confirm_text,
              lv_color_t confirm_color,
              std::function<void()> on_confirm,
              const char* cancel_text = "CANCEL",
              std::function<void()> on_cancel = nullptr);

    void handle_confirm();
    void handle_cancel();

    // Closes the dialog from code without running either callback (the request it belonged to is gone) and
    // returns to the state it was opened from. No effect unless the confirm screen is the current state.
    void dismiss();

    // Ignores INSTALL/DECLINE-style taps (dialog stays up, no callback) for this long from now; show() clears it
    void guard_taps(uint32_t duration_ms);

private:
    void reset_callbacks();

    UIManager* ui_manager_;
    std::function<void()> on_confirm_;
    std::function<void()> on_cancel_;
    UIState previous_state_;
    uint32_t tap_guard_until_ms_ = 0;
    bool tap_guard_active_ = false;
    bool taps_guarded() const;
};

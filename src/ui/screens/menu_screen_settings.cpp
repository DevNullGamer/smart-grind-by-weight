// MenuScreen: table-driven setting toggles and sliders (see system/menu_setting_table.h).
#include "menu_screen.h"
#include "../../config/constants.h"
#include "../../hardware/board_detect.h"
#include "../../system/menu_setting_models.h"
#include "../../system/nvs_settings_store.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

// Indexed by ToggleSetting
static const EventBridgeLVGL::EventType kSettingToggleEvents[TOGGLE_SETTING_COUNT] = {
    EventBridgeLVGL::EventType::BLE_STARTUP_TOGGLE,
    EventBridgeLVGL::EventType::LOGGING_TOGGLE,
    EventBridgeLVGL::EventType::GRIND_MODE_SWIPE_TOGGLE,
    EventBridgeLVGL::EventType::AUTO_START_TOGGLE,
    EventBridgeLVGL::EventType::AUTO_RETURN_TOGGLE,
};

EventBridgeLVGL::EventType MenuScreen::setting_toggle_event(ToggleSetting id) {
    return kSettingToggleEvents[static_cast<size_t>(id)];
}

void MenuScreen::create_setting_toggle_row(lv_obj_t* parent, ToggleSetting id) {
    lv_obj_t** toggle = &setting_toggles_[static_cast<size_t>(id)];
    create_toggle_row(parent, toggle_setting_def(id).label, toggle);
    bind_event(*toggle, LV_EVENT_VALUE_CHANGED, setting_toggle_event(id));
}

void MenuScreen::refresh_setting_toggles() {
    for (size_t i = 0; i < TOGGLE_SETTING_COUNT; i++) {
        lv_obj_t* toggle = setting_toggles_[i];
        if (!toggle) continue;

        if ((ui_settings.*toggle_setting_def(static_cast<ToggleSetting>(i)).get)()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
    }
}

// Indexed by SliderSetting: value-changed and released events of each slider
static const EventBridgeLVGL::EventType kSettingSliderEvents[SLIDER_SETTING_COUNT][2] = {
    {EventBridgeLVGL::EventType::BRIGHTNESS_NORMAL_SLIDER, EventBridgeLVGL::EventType::BRIGHTNESS_NORMAL_SLIDER_RELEASED},
    {EventBridgeLVGL::EventType::BRIGHTNESS_SCREENSAVER_SLIDER, EventBridgeLVGL::EventType::BRIGHTNESS_SCREENSAVER_SLIDER_RELEASED},
    {EventBridgeLVGL::EventType::GRINDER_PURGE_AMOUNT_SLIDER, EventBridgeLVGL::EventType::GRINDER_PURGE_AMOUNT_SLIDER_RELEASED},
    {EventBridgeLVGL::EventType::GRIND_FRESHNESS_HOURS_SLIDER, EventBridgeLVGL::EventType::GRIND_FRESHNESS_HOURS_SLIDER_RELEASED},
};

EventBridgeLVGL::EventType MenuScreen::setting_slider_event(SliderSetting id, bool released) {
    return kSettingSliderEvents[static_cast<size_t>(id)][released ? 1 : 0];
}

void MenuScreen::create_setting_slider_row(lv_obj_t* parent, SliderSetting id) {
    const size_t i = static_cast<size_t>(id);
    const SliderSettingDef& def = slider_setting_def(id);
    create_slider_row(parent, def.row_label, &setting_slider_labels_[i], &setting_sliders_[i],
                      lv_color_hex(def.color), static_cast<uint32_t>(def.min), static_cast<uint32_t>(def.max));
    bind_event(setting_sliders_[i], LV_EVENT_VALUE_CHANGED, setting_slider_event(id, false));
    bind_event(setting_sliders_[i], LV_EVENT_RELEASED, setting_slider_event(id, true));
}

void MenuScreen::set_setting_slider_label(SliderSetting id, int pos) {
    lv_obj_t* label = setting_slider_labels_[static_cast<size_t>(id)];
    if (!label) return;

    char buffer[32];
    lv_label_set_text(label, slider_setting_def(id).label(buffer, sizeof(buffer), pos));
}

void MenuScreen::refresh_setting_sliders() {
    for (size_t i = 0; i < SLIDER_SETTING_COUNT; i++) {
        lv_obj_t* slider = setting_sliders_[i];
        if (!slider) continue;

        const SliderSetting id = static_cast<SliderSetting>(i);
        const int pos = slider_setting_def(id).load_pos(ui_settings);
        lv_slider_set_value(slider, pos, LV_ANIM_OFF);
        set_setting_slider_label(id, pos);
    }
}

// Indexed by RadioSetting
static const EventBridgeLVGL::EventType kSettingRadioEvents[RADIO_SETTING_COUNT] = {
    EventBridgeLVGL::EventType::GRIND_MODE_RADIO_BUTTON,
    EventBridgeLVGL::EventType::GRINDER_PURGE_MODE_RADIO_BUTTON,
    EventBridgeLVGL::EventType::BOARD_OVERRIDE_RADIO_BUTTON,
};

EventBridgeLVGL::EventType MenuScreen::setting_radio_event(RadioSetting id) {
    return kSettingRadioEvents[static_cast<size_t>(id)];
}

// Shared by all radio groups: user_data carries the EventType, the controller handler reads the selection
static void setting_radio_callback(int selected_index, void* user_data) {
    EventBridgeLVGL::handle_event(
        static_cast<EventBridgeLVGL::EventType>(reinterpret_cast<uintptr_t>(user_data)), nullptr);
}

void MenuScreen::create_setting_radio(lv_obj_t* parent, RadioSetting id) {
    const RadioSettingDef& def = radio_setting_def(id);
    setting_radios_[static_cast<size_t>(id)] = create_radio_button_group(
        parent,
        def.labels,
        def.count,
        LV_FLEX_FLOW_ROW,
        def.initial_index,
        def.width, def.height,
        setting_radio_callback,
        reinterpret_cast<void*>(static_cast<uintptr_t>(setting_radio_event(id)))
    );
}

// Selected button index of a radio group as stored
static int stored_radio_index(RadioSetting id) {
    switch (id) {
        case RadioSetting::GRIND_MODE: return grind_mode_radio_index(ui_settings.grind_mode_raw());
        case RadioSetting::PURGE_MODE: return ui_settings.purge_mode();
        case RadioSetting::BOARD_OVERRIDE: return static_cast<int>(board_override_load());
        default: return 0;
    }
}

void MenuScreen::refresh_setting_radios() {
    for (size_t i = 0; i < RADIO_SETTING_COUNT; i++) {
        if (!setting_radios_[i]) continue;
        radio_button_group_set_selection(setting_radios_[i], stored_radio_index(static_cast<RadioSetting>(i)));
    }
}

LV_DEPRECATIONS_IGNORE_END

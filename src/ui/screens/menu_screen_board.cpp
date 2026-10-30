// MenuScreen: Hardware board page.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../hardware/board_detect.h"
#include "../event_bridge_lvgl.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

// Callback for hardware board override radio button selection
static void board_override_callback(int selected_index, void* user_data) {
    EventBridgeLVGL::handle_event(EventBridgeLVGL::EventType::BOARD_OVERRIDE_RADIO_BUTTON, nullptr);
}

void MenuScreen::create_board_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_description_label(parent, "Waveshare 1.64\" board revision this grinder runs on.");
    create_data_label(parent, "Detected:", &board_status_label);

    create_separator(parent, "Override");
    const char* board_options[] = {"Auto", "V1", "V2"};
    board_override_radio_group = create_radio_button_group(
        parent,
        board_options,
        3,
        LV_FLEX_FLOW_ROW,
        0,  // Auto initially selected
        -1, 100,  // Auto width, height
        board_override_callback,
        this
    );
    create_description_label(parent, "Use Auto unless the board is not recognised. A change applies after restarting the grinder.");
}

void MenuScreen::update_board_page() {
    if (board_status_label) {
        char text[32];
        snprintf(text, sizeof(text), "%s (%s)", board().name(), board().source_name());
        lv_label_set_text(board_status_label, text);
    }

    if (board_override_radio_group) {
        radio_button_group_set_selection(board_override_radio_group, static_cast<int>(board_override_load()));
    }
}

LV_DEPRECATIONS_IGNORE_END

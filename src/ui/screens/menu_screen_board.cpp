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

void MenuScreen::create_board_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_description_label(parent, "Waveshare 1.64\" board revision this grinder runs on.");
    create_data_label(parent, "Detected:", &board_status_label);

    create_separator(parent, "Override");
    create_setting_radio(parent, RadioSetting::BOARD_OVERRIDE);
    create_description_label(parent, "Use Auto unless the board is not recognised. A change applies after restarting the grinder.");
}

void MenuScreen::update_board_page() {
    if (board_status_label) {
        char text[32];
        snprintf(text, sizeof(text), "%s (%s)", board().name(), board().source_name());
        lv_label_set_text(board_status_label, text);
    }
}

LV_DEPRECATIONS_IGNORE_END

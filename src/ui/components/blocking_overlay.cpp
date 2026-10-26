#include "blocking_overlay.h"
#include "../../config/constants.h"
#include <lvgl.h>
#include <Arduino.h>
#include <utility>
#include "../ui_helpers.h"

// Static instance
BlockingOperationOverlay* BlockingOperationOverlay::g_instance = nullptr;

void BlockingOperationOverlay::init() {
    // Create overlay (initially hidden)
    overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(overlay, THEME_OPACITY_OVERLAY, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_center(overlay);
    
    // Ensure overlay is always on top by moving to foreground
    lv_obj_move_to_index(overlay, -1);

    lv_obj_t* content = lv_obj_create(overlay);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_gap(content, 5, 0);
    lv_obj_center(content);

    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Create main label
    label = lv_label_create(content);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t * wait_label = lv_label_create(content);
    lv_label_set_text(wait_label, "Please Wait...");
    lv_obj_set_style_text_font(wait_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wait_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(wait_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // STOP button (hidden unless show_with_stop() is used); a child of the overlay so it is
    // reachable while the overlay swallows all other touches
    stop_button = create_button(overlay, "STOP", lv_color_hex(THEME_COLOR_ERROR), 200, 70, &lv_font_montserrat_28);
    lv_obj_align(stop_button, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_event_cb(stop_button, stop_button_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(stop_button, true);
    stop_callback = nullptr;

    // Hide initially
    lv_obj_set_hidden(overlay, true);
    is_visible = false;
    operation_timer = nullptr;
}

void BlockingOperationOverlay::show_and_execute(BlockingOperation op_type, 
                                                OperationCallback operation_func,
                                                OperationCallback completion_func,
                                                const char* custom_message) {
    
    // Set appropriate message
    const char* message = get_operation_message(op_type, custom_message);
    lv_label_set_text(label, message);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, 240);
    
    set_stop_visible(false);

    // Store callbacks
    completion_callback = completion_func;
    operation_callback = operation_func;
    
    // Show overlay
    lv_obj_set_hidden(overlay, false);
    
    // Ensure overlay is on top when shown
    lv_obj_move_to_index(overlay, -1);
    
    is_visible = true;
    
    // Start operation after small delay to ensure overlay is visible
    if (operation_timer) {
        lv_timer_delete(operation_timer);
    }
    
    operation_timer = lv_timer_create(operation_timer_cb, 100, nullptr);
    lv_timer_set_repeat_count(operation_timer, 1);
}

void BlockingOperationOverlay::hide_and_complete() {
    // Hide overlay
    lv_obj_set_hidden(overlay, true);
    is_visible = false;
    
    // Clean up timer
    if (operation_timer) {
        lv_timer_delete(operation_timer);
        operation_timer = nullptr;
    }
    
    // Call completion callback if provided
    if (completion_callback) {
        completion_callback();
        completion_callback = nullptr;
    }
    
    // Clear operation callback
    operation_callback = nullptr;
}

const char* BlockingOperationOverlay::get_operation_message(BlockingOperation op_type, const char* custom_message) {
    if (custom_message) {
        return custom_message;
    }
    
    switch (op_type) {
        case BlockingOperation::TARING:
            return "TARING";
        case BlockingOperation::CALIBRATING:
            return "CALIBRATING";
        case BlockingOperation::SETTLING:
            return "SETTLING";
        case BlockingOperation::BLE_ENABLING:
            return "ENABLING BLUETOOTH";
        case BlockingOperation::LOADING_STATISTICS:
            return "LOADING STATISTICS";
        default:
            return "PROCESSING";
    }
}

void BlockingOperationOverlay::operation_timer_cb(lv_timer_t* timer) {
    auto* instance = &getInstance();
    
    // Execute the blocking operation
    if (instance->operation_callback) {
        instance->operation_callback();
    }
    
    // Hide overlay and call completion callback
    instance->hide_and_complete();
}

void BlockingOperationOverlay::show(const char* message) {
    set_stop_visible(false);

    // Set message
    lv_label_set_text(label, message);
    
    // Show overlay
    lv_obj_set_hidden(overlay, false);
    lv_obj_move_to_index(overlay, -1);
    is_visible = true;
}

void BlockingOperationOverlay::hide() {
    set_stop_visible(false);

    // Hide overlay
    lv_obj_set_hidden(overlay, true);
    is_visible = false;
}

void BlockingOperationOverlay::show_with_stop(const char* message, OperationCallback on_stop) {
    lv_label_set_text(label, message);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, 240);

    stop_callback = std::move(on_stop);
    set_stop_visible(true);

    lv_obj_set_hidden(overlay, false);
    lv_obj_move_to_index(overlay, -1);
    is_visible = true;
}

void BlockingOperationOverlay::set_message(const char* message) {
    lv_label_set_text(label, message);
}

void BlockingOperationOverlay::set_stop_visible(bool visible) {
    if (visible) {
        lv_obj_set_hidden(stop_button, false);
    } else {
        lv_obj_set_hidden(stop_button, true);
        stop_callback = nullptr;
    }
}

void BlockingOperationOverlay::stop_button_cb(lv_event_t* e) {
    auto* instance = static_cast<BlockingOperationOverlay*>(lv_event_get_user_data(e));
    if (!instance || !instance->stop_callback) {
        return;
    }
    // Move out first: the callback hides the overlay, which clears stop_callback
    auto callback = std::move(instance->stop_callback);
    instance->stop_callback = nullptr;
    callback();
}

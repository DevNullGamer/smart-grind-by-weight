#include "grinding_screen_chart.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include <lvgl.h>
#include <cstring>
#include "../ui_helpers.h"

void GrindingScreenChart::create() {
    screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(80));
    lv_obj_align(screen, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0); // Keep transparent
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_clickable(screen, true); // Make the parent screen container clickable

    // Use flex layout for centering
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(screen, 15, 0);

    // Profile name label
    profile_label = lv_label_create(screen);
    lv_label_set_text(profile_label, "DOUBLE");
    lv_obj_set_style_text_font(profile_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(profile_label, lv_color_hex(THEME_COLOR_SECONDARY), 0);

    // Create chart - use full screen width
    chart = lv_chart_create(screen);
    lv_obj_set_size(chart, LV_PCT(100), 140);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, MAX_CHART_POINTS);
    lv_chart_set_div_line_count(chart, 0, 0);  // No grid lines for clean look
    
    // Chart styling - dark background
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    
    // Initialize data tracking
    target_weight_value = 18.0f;
    predicted_chart_points = 0;
    predicted_time_override_ms = 0;
    // Same headroom update_target_weight() applies. GrindingScreen::set_mode() no longer calls it
    // on every event, so the initial axis must already match what that call used to set.
    max_y_value = target_weight_value + 1.2f;
    time_mode = false;
    target_time_seconds = 0.0f;
    current_weight_value = 0.0f;
    showing_tare = false;
    has_text_override = false;
    text_override[0] = '\0';
    
    // Set chart update mode (point count is finalized after series creation)
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT); // Slide data left when full
    
    // Set Y-axis ranges (scale by 10 to handle decimals)
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(max_y_value * 10)); // Weight axis
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, 25); // Flow rate axis: 0-2.5 g/s * 10
    
    // Add data series in z-order: weight (bottom/filled), flow rate, target (top)
    // Weight series (red filled area) - Use primary Y axis for weight
    weight_series = lv_chart_add_series(chart, lv_color_hex(THEME_COLOR_PRIMARY), LV_CHART_AXIS_PRIMARY_Y);
    
    // Flow rate series (green line) - Use secondary Y axis for flow rate
    flow_rate_series = lv_chart_add_series(chart, lv_color_hex(THEME_COLOR_SUCCESS), LV_CHART_AXIS_SECONDARY_Y);
    
    // Configure chart to show both lines and bars (for filled area effect)
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    
    // Style each series individually - remove data point markers
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_color(chart, lv_color_hex(THEME_COLOR_PRIMARY), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(chart, lv_color_hex(THEME_COLOR_PRIMARY), LV_PART_ITEMS);
    // lv_obj_set_style_bg_opa(chart, LV_OPA_20, LV_PART_ITEMS);
    
    // Remove data point markers/circles - set both width and height to 0
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(chart, 0, LV_PART_INDICATOR);
    
    // Flow rate and target series will inherit line styling
    
    // Configure the initial point count based on defaults
    update_chart_point_configuration();

    // Initialize all series with zero values
    reset_chart_data();

    // Current/Target weight display with mixed font sizes using spangroup
    weight_spangroup = lv_spangroup_create(screen);
    lv_obj_set_width(weight_spangroup, LV_PCT(100));
    lv_obj_set_style_text_align(weight_spangroup, LV_TEXT_ALIGN_CENTER, 0);
    lv_spangroup_set_overflow(weight_spangroup, LV_SPAN_OVERFLOW_CLIP);
    lv_spangroup_set_indent(weight_spangroup, 0);
    
    // Create initial spans using correct API
    lv_span_t* current_span = lv_spangroup_add_span(weight_spangroup);
    lv_style_set_text_font(lv_span_get_style(current_span), &lv_font_montserrat_56);
    lv_style_set_text_color(lv_span_get_style(current_span), lv_color_hex(THEME_COLOR_TEXT_PRIMARY));
    lv_span_set_text(current_span, "0.0g");
    
    lv_span_t* separator_span = lv_spangroup_add_span(weight_spangroup);
    lv_style_set_text_font(lv_span_get_style(separator_span), &lv_font_montserrat_24);
    lv_style_set_text_color(lv_span_get_style(separator_span), lv_color_hex(THEME_COLOR_TEXT_SECONDARY));
    lv_span_set_text(separator_span, " / 18.0g");
    
    lv_spangroup_refresh(weight_spangroup);
    
    // MODIFIED: Ensure all child widgets pass click events to the parent screen
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
        lv_obj_set_clickable(lv_obj_get_child(screen, i), false);
    }

    visible = false;
    lv_obj_set_hidden(screen, true);
}

void GrindingScreenChart::show() {
    lv_obj_set_hidden(screen, false);
    visible = true;
    // Texts stored while hidden are applied now (a hidden layout does no LVGL text work)
    render_weight_spans();
}

// Render both weight spans from the stored state. Idempotent: a span is rewritten only when its
// text differs, so repeated calls with unchanged state do no LVGL work. Skipped while hidden.
void GrindingScreenChart::render_weight_spans() {
    if (!visible || !weight_spangroup) {
        return;
    }

    lv_span_t* current_span = lv_spangroup_get_child(weight_spangroup, 0);
    lv_span_t* separator_span = lv_spangroup_get_child(weight_spangroup, 1);
    if (!current_span || !separator_span) {
        return;
    }

    char current_text[16];
    if (showing_tare) {
        snprintf(current_text, sizeof(current_text), "TARE");
    } else {
        snprintf(current_text, sizeof(current_text), SYS_WEIGHT_DISPLAY_FORMAT, current_weight_value);
    }

    char separator_text[48];
    if (has_text_override) {
        snprintf(separator_text, sizeof(separator_text), "%s", text_override);
    } else if (time_mode) {
        snprintf(separator_text, sizeof(separator_text), "\nTime: %.1fs", target_time_seconds);
    } else {
        snprintf(separator_text, sizeof(separator_text), " / " SYS_WEIGHT_DISPLAY_FORMAT, target_weight_value);
    }

    bool changed = false;
    const char* shown_current = lv_span_get_text(current_span);
    if (!shown_current || strcmp(shown_current, current_text) != 0) {
        lv_span_set_text(current_span, current_text);
        changed = true;
    }
    const char* shown_separator = lv_span_get_text(separator_span);
    if (!shown_separator || strcmp(shown_separator, separator_text) != 0) {
        lv_span_set_text(separator_span, separator_text);
        changed = true;
    }
    if (changed) {
        lv_spangroup_refresh(weight_spangroup);
    }
}

void GrindingScreenChart::hide() {
    lv_obj_set_hidden(screen, true);
    visible = false;
}

void GrindingScreenChart::update_profile_name(const char* name) {
    set_label_text_if_changed(profile_label, name);
}

void GrindingScreenChart::update_target_weight(float weight) {
    target_weight_value = weight;
    max_y_value = target_weight_value + 1.2f;
    update_chart_point_configuration();

    if (!time_mode) {
        // Weight mode shows "0.0g / <target>" until the next weight arrives
        current_weight_value = 0.0f;
        showing_tare = false;
        has_text_override = false;
        render_weight_spans();
    }

    // Update chart configuration
    lv_chart_set_point_count(chart, predicted_chart_points);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(max_y_value * 10)); // Weight axis
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, 25); // Flow rate axis: 0-2.5 g/s * 10

    if (visible) {
        lv_chart_refresh(chart);
    }
}

void GrindingScreenChart::update_target_weight_text(const char* text) {
    const char* target_text = text ? text : "";
    if (target_text[0] && target_text[0] != ' ' && target_text[0] != '/' && target_text[0] != '\n') {
        snprintf(text_override, sizeof(text_override), "\n%s", target_text);
    } else {
        snprintf(text_override, sizeof(text_override), "%s", target_text);
    }
    has_text_override = true;
    render_weight_spans();
}

void GrindingScreenChart::update_target_time(float seconds) {
    target_time_seconds = seconds;
    has_text_override = false;
    // Keep the current weight untouched; time is shown on a new line without slash
    render_weight_spans();

    uint32_t predicted_ms = (seconds > 0.0f) ? static_cast<uint32_t>(seconds * 1000.0f) : 0;
    set_chart_time_prediction(predicted_ms);
}

// The error/status override is deliberately not cleared here: it stays until the next
// target/tare/reset call, so the per-frame weight refresh in GRIND_TIMEOUT cannot flip the text.
void GrindingScreenChart::update_current_weight(float weight) {
    current_weight_value = weight;
    showing_tare = false;
    render_weight_spans();
}

void GrindingScreenChart::update_tare_display() {
    showing_tare = true;
    has_text_override = false;
    render_weight_spans();
}

void GrindingScreenChart::update_progress(int percent) {
    // Progress is now visualized through the chart data
    // This method is kept for compatibility but chart updates happen via add_chart_data_point
}

void GrindingScreenChart::add_chart_data_point(float current_weight, float flow_rate, uint32_t /*current_time_ms*/) {
    // Scale weight and flow rate by 10 to handle decimals in LVGL chart
    int32_t weight_value = (int32_t)(current_weight * 10);
    // Clamp flow rate to 0-2.5 g/s range then scale by 10
    float clamped_flow_rate = (flow_rate < 0.0f) ? 0.0f : ((flow_rate > 2.5f) ? 2.5f : flow_rate);
    int32_t flow_rate_value = (int32_t)(clamped_flow_rate * 10);
    
    // Add data points to series (LVGL will handle sliding window due to SHIFT mode)
    lv_chart_set_next_value(chart, weight_series, weight_value);
    lv_chart_set_next_value(chart, flow_rate_series, flow_rate_value);
    
    // Data keeps accumulating while hidden; only a visible chart needs the redraw request
    if (visible) {
        lv_chart_refresh(chart);
    }
}

void GrindingScreenChart::set_chart_time_prediction(uint32_t predicted_time_ms) {
    predicted_time_override_ms = predicted_time_ms;
    update_chart_point_configuration();
}

void GrindingScreenChart::reset_chart_data() {
    has_text_override = false;  // A new grind starts without a leftover error text

    if (!chart) {
        return;
    }

    if (weight_series) {
        lv_chart_set_all_values(chart, weight_series, 0);
    }
    if (flow_rate_series) {
        lv_chart_set_all_values(chart, flow_rate_series, 0);
    }

    predicted_chart_points = static_cast<uint16_t>(lv_chart_get_point_count(chart));
}

void GrindingScreenChart::set_time_mode(bool enabled) {
    if (time_mode == enabled) {
        return;
    }
    time_mode = enabled;
    if (time_mode) {
        update_target_time(target_time_seconds);
    } else {
        // Revert to weight display formatting using the last known target weight
        update_target_weight(target_weight_value);
    }
}

void GrindingScreenChart::update_chart_point_configuration() {
    if (!chart) {
        return;
    }

    uint32_t effective_time_ms = predicted_time_override_ms;
    if (effective_time_ms == 0) {
        float predicted_ms = 1000.0f + (target_weight_value / REFERENCE_FLOW_RATE_GPS) * 1000.0f;
        effective_time_ms = static_cast<uint32_t>(predicted_ms);
    }

    if (effective_time_ms < DATA_POINT_INTERVAL_MS) {
        effective_time_ms = DATA_POINT_INTERVAL_MS;
    }

    uint32_t new_count_32 = (effective_time_ms + DATA_POINT_INTERVAL_MS - 1) / DATA_POINT_INTERVAL_MS;
    if (new_count_32 < MIN_CHART_POINTS) {
        new_count_32 = MIN_CHART_POINTS;
    }
    if (new_count_32 > MAX_CHART_POINTS) {
        new_count_32 = MAX_CHART_POINTS;
    }

    uint16_t new_count = static_cast<uint16_t>(new_count_32);
    uint16_t current_count = static_cast<uint16_t>(lv_chart_get_point_count(chart));

    if (new_count != current_count) {
        lv_chart_set_point_count(chart, new_count);
        if (weight_series) {
            lv_chart_set_all_values(chart, weight_series, 0);
        }
        if (flow_rate_series) {
            lv_chart_set_all_values(chart, flow_rate_series, 0);
        }
        current_count = new_count;
    }

    predicted_chart_points = current_count;
}

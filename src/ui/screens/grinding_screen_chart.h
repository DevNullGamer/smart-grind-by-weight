#pragma once
#include <lvgl.h>
#include "grinding_screen_base.h"
#include "../../config/constants.h"

class GrindingScreenChart : public IGrindingScreen {
private:
    lv_obj_t* screen = nullptr;
    lv_obj_t* profile_label = nullptr;
    lv_obj_t* weight_spangroup = nullptr;
    lv_obj_t* chart = nullptr;
    lv_chart_series_t* weight_series = nullptr;
    lv_chart_series_t* flow_rate_series = nullptr;
    bool visible = false;
    bool time_mode = false;
    
    // Chart data management
    static const uint16_t MIN_CHART_POINTS = 32;
    static const uint16_t MAX_CHART_POINTS = 1000;
    static constexpr float REFERENCE_FLOW_RATE_GPS = 1.6f;  // Reference flow rate for time prediction
    static const uint32_t DATA_POINT_INTERVAL_MS = SYS_TASK_GRIND_CONTROL_INTERVAL_MS; // Match grind control loop (50Hz)
    uint16_t predicted_chart_points = 0;
    uint32_t predicted_time_override_ms = 0;
    float target_weight_value = 0.0f;
    float max_y_value = 0.0f;
    float target_time_seconds = 0.0f;

    // Stored state the weight spans are rendered from (see render_weight_spans)
    float current_weight_value = 0.0f;
    bool showing_tare = false;
    bool has_text_override = false;     // Error/status text replaces the target part
    char text_override[48] = "";

    void update_chart_point_configuration();
    void render_weight_spans();

public:
    void create() override;
    void show() override;
    void hide() override;
    void update_profile_name(const char* name) override;
    void update_target_weight(float weight) override;
    void update_target_weight_text(const char* text) override;
    void update_target_time(float seconds);
    void update_current_weight(float weight) override;
    void update_tare_display() override;
    void update_progress(int percent) override;
    void add_chart_data_point(float current_weight, float flow_rate, uint32_t current_time_ms) override;
    void set_chart_time_prediction(uint32_t predicted_time_ms);
    void reset_chart_data();
    void set_time_mode(bool enabled);

    bool is_visible() const override { return visible; }
    lv_obj_t* get_screen() const override { return screen; }
};

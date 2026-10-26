#include "hardware_manager.h"
#include "../controllers/grind_controller.h"
#include <Arduino.h>
#include "../config/constants.h"

void HardwareManager::init() {
    preferences.begin("grinder", false);
    display_manager.init();
    weight_sensor.init(&preferences);
    grinder.init(board().motor_pin, board().motor_pin_alt);

    grind_controller = nullptr; // Will be set later

    initialized = true;
}

void HardwareManager::update() {
    if (!initialized) return;
    
    // All hardware components now updated independently by the FreeRTOS tasks (TaskManager):
    // - weight_sensor.update() in the WeightSampling task
    // - display_manager.update() in the UIRender task
    // No need for grinding mode switching - load cell runs at constant high speed
}



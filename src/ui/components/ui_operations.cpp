#include "ui_operations.h"
#include <Arduino.h>

void UIOperations::execute_tare(HardwareManager* hw_manager, OperationCallback completion) {
    auto& overlay = BlockingOperationOverlay::getInstance();
    
    auto tare_operation = [hw_manager]() {
        // This will now block and wait for settlement internally
        hw_manager->get_load_cell()->tare();
        LOG_BLE("Scale tared successfully\n");
    };
    
    overlay.show_and_execute(BlockingOperation::TARING, tare_operation, completion);
}

void UIOperations::execute_calibration(HardwareManager* hw_manager, float cal_weight,
                                      CalibrationCompletion completion) {
    auto& overlay = BlockingOperationOverlay::getInstance();

    // The operation callback has no return value, so the result travels in shared state
    auto result = std::make_shared<CalibrationResult>(CalibrationResult::HARDWARE_FAULT);

    auto calibration_operation = [hw_manager, cal_weight, result]() {
        // Blocks and waits for settlement internally
        *result = hw_manager->get_load_cell()->calibrate(cal_weight);
        if (*result == CalibrationResult::OK) {
            LOG_BLE("Scale calibrated with %.2fg weight\n", cal_weight);
        }
    };

    overlay.show_and_execute(BlockingOperation::CALIBRATING, calibration_operation,
                             [completion, result]() {
                                 if (completion) {
                                     completion(*result);
                                 }
                             });
}

void UIOperations::execute_grind_tare(GrindController* grind_controller, OperationCallback completion) {
    // No blocking overlay needed - tare is now non-blocking in GrindController
    // The GrindController will handle the tare operation in its update loop
    grind_controller->user_tare_request();
    LOG_BLE("Grind tare initiated (non-blocking)\n");
    
    // Call completion callback immediately since we're not blocking
    if (completion) {
        completion();
    }
}

void UIOperations::execute_custom_operation(const char* message, 
                                           OperationCallback operation,
                                           OperationCallback completion) {
    auto& overlay = BlockingOperationOverlay::getInstance();
    overlay.show_and_execute(BlockingOperation::CUSTOM, operation, completion, message);
}

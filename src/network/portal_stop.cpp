#include "portal_stop.h"
#include "portal_command_gate.h"
#include "../config/constants.h"
#include "../controllers/grind_controller.h"
#include "../hardware/hardware_manager.h"

extern HardwareManager hardware_manager;
extern GrindController grind_controller;

void portal_stop_now() {
    // Order matters: the flag first, so a control cycle that runs between the two lines cannot find the motor
    // stopped and restart it without also seeing the stop request.
    grind_controller.request_stop();
    Grinder* grinder = hardware_manager.get_grinder();
    if (grinder) grinder->stop();
    portal_command_gate.request_stop();
    LOG_BLE("[PORTAL] stop requested\n");
}

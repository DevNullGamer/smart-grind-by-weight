#pragma once

/**
 * Stop path of the web panel (POST /api/command {"cmd":"stop"}), docs/PORTAL-API.md "Stop".
 *
 * The one command that must not depend on the UI task (it may be busy in a blocking screen operation). It runs on
 * the HTTP task and touches only two things, both built for being called from any task:
 *  - GrindController::request_stop(): an atomic flag the grind control task (Core 0, its own task, independent of
 *    the UI task) consumes within one control cycle (SYS_TASK_GRIND_CONTROL_INTERVAL_MS) and answers with the same
 *    stop_grind() as the STOP button, so the control loop cannot restart the motor behind the stop.
 *  - Grinder::stop(): cuts the motor output at once (mutex-serialised, continues without the lock if it is busy,
 *    like the deadman). This also ends a Motor Test pulse and a pulse of the autotune.
 * The UI task is told through portal_command_gate.request_stop() (a counter), so it can cancel an autotune run it
 * drives itself; the mailbox is never used.
 *
 * No LVGL, no profiles, no NVS here. Keep this file the only HTTP-task code that includes the controllers.
 */
void portal_stop_now();

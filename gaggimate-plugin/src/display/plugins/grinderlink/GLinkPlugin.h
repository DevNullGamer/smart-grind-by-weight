#ifndef GAGGIMATE_SIM
#pragma once

#include <display/core/Plugin.h>
#include <stdint.h>

class Controller;
class PluginManager;

// GLink consumer plugin (chg025, spec 3.5). GaggiMate's BluetoothDevicesPlugin does scan, connect and reconnect; this plugin
// only (1) registers the GLinkGrinder driver and the busy probe it needs, and (2) applies the weight of a grind the grinder
// finished as the dose of the selected profile (event "grinder:grind:finished"). Device code only: the whole file is empty in
// the simulator build. Deliberately includes no GLink header: config/bluetooth.h defines BLE_* macros that must not reach
// Controller.cpp.
//
// All of it runs on the loop task: the event is fired from BluetoothDevicesPlugin::update(), loop() applies the dose.
class GLinkPlugin : public Plugin {
  public:
    GLinkPlugin() = default;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    // The only writer of the profile dose (memory only, like Smart Dose; last writer wins).
    void applyPendingDose(uint32_t now);

    Controller *controller = nullptr;
};

#endif // GAGGIMATE_SIM

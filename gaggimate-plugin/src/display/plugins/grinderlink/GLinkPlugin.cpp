#ifndef GAGGIMATE_SIM

#include "GLinkPlugin.h"

#include "GLinkDoseSave.h"
#include "GLinkGrinder.h"
#include <cmath>
#include <display/core/Controller.h>
#include <display/core/constants.h>
#include <display/core/Event.h>
#include <display/core/PluginManager.h>
#include <mutex>

namespace {
const char *TAG = "GLinkPlugin";

// Single plugin instance (Controller registers one). The event handler records into doseSave, loop() consumes it; both run
// on the loop task, so no lock is needed here.
glinkc::DoseSave doseSave;
Controller *busyController = nullptr;

// Same predicate as a brew or grind running or the system not being ready: a connect blocks the loop task for seconds.
bool gaggimateBusy() { return busyController == nullptr || busyController->isActive() || busyController->getSystemState() != SYSTEM_READY; }

// Event handler: only records (spec 3.5). Everything else is decided by applyPendingDose().
void onGrindFinished(const Event &event) {
    const float grams = event.getFloat("value");
    if (!std::isfinite(grams) || grams < 0.0f || grams > 1000.0f) {
        return;
    }
    const int32_t tenths = static_cast<int32_t>(lroundf(grams * 10.0f));
    if (tenths < glinkc::DoseSaveLimits::DOSE_MIN_TENTHS || tenths > glinkc::DoseSaveLimits::DOSE_MAX_TENTHS) {
        ESP_LOGI(TAG, "Finished grind of %d.%d g is outside 5.0 to 40.0 g, not used as the dose", static_cast<int>(tenths / 10),
                 static_cast<int>(tenths % 10));
    }
    doseSave.on_finished(tenths, millis());
}
} // namespace

void GLinkPlugin::setup(Controller *c, PluginManager *pluginManager) {
    if (c == nullptr || pluginManager == nullptr) {
        ESP_LOGE(TAG, "Invalid controller or manager passed to setup");
        return;
    }
    controller = c;
    busyController = c;
    GLinkGrinder::setBusyProbe(&gaggimateBusy);
    GLinkGrinder::registerDriver();
    pluginManager->on("grinder:grind:finished", [](Event const &event) { onGrindFinished(event); });
}

void GLinkPlugin::loop() {
    if (controller == nullptr) {
        return;
    }
    applyPendingDose(millis()); // every pass: a pending dose expires even while the machine is busy
}

// Loop task, once per pass. The only place that writes the selected profile's dose; the event handler only records into
// doseSave. Like Smart Dose the write is memory-only: it is lost on a profile reselect or reboot.
void GLinkPlugin::applyPendingDose(uint32_t now) {
    if (!doseSave.has_pending()) {
        return; // cheap path: no lock
    }
    int32_t doseTenths = 0;
    bool taken = false;
    {
        std::lock_guard<std::recursive_mutex> lock(controller->getProcessLock());
        Profile &profile = controller->getProfileManager()->getSelectedProfile();
        const bool canApply = controller->getMode() == MODE_BREW && !controller->isActive() && !controller->isUpdating() &&
                              !controller->isAutotuning() && !profile.utility;
        taken = doseSave.take(canApply, now, &doseTenths);
        if (taken) {
            const float dose = static_cast<float>(doseTenths) / 10.0f;
            if (fabsf(profile.dose - dose) >= 0.001f) { // compare-first: an equal dose is not written again
                profile.dose = dose;
            }
        }
    }
    if (taken) {
        // Integers only: float printf is stack-hungry and the loop task has little headroom. Tenths are 50..400 here.
        ESP_LOGI(TAG, "Dose set to %d.%d g from the finished grind", static_cast<int>(doseTenths / 10),
                 static_cast<int>(doseTenths % 10));
    } else if (!doseSave.has_pending()) {
        ESP_LOGI(TAG, "A finished grind's dose expired before the machine was free");
    }
}

#endif // GAGGIMATE_SIM

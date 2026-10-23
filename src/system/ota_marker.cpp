#include "ota_marker.h"
#include <Arduino.h>
#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "grinder";
constexpr const char* kKeyBuild = "new_build_nr";
constexpr const char* kKeyVersion = "new_fw_ver";

// Only existing keys: removing a missing key makes the Preferences library log an error
void remove_keys(Preferences& prefs) {
    if (prefs.isKey(kKeyBuild)) prefs.remove(kKeyBuild);
    if (prefs.isKey(kKeyVersion)) prefs.remove(kKeyVersion);
}
}  // namespace

bool ota_marker_store(int32_t build, const char* version) {
    const bool has_version = version && version[0] != '\0';
    if (build < 0 && !has_version) return false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    // Stale keys of an earlier attempt must not survive when this one carries less information
    remove_keys(prefs);
    bool ok = true;
    if (build >= 0) {
        char text[12];
        snprintf(text, sizeof(text), "%ld", static_cast<long>(build));
        ok = prefs.putString(kKeyBuild, text) == strlen(text);
    }
    if (has_version) ok = (prefs.putString(kKeyVersion, version) == strlen(version)) && ok;
    prefs.end();
    return ok;
}

void ota_marker_clear() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    remove_keys(prefs);
    prefs.end();
}

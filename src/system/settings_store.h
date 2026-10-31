#pragma once
#include <stdint.h>

// Key/value store behind UiSettings (no Arduino/ESP dependencies, so the settings facade is host-testable).
// The firmware implementation is backed by Arduino Preferences; the get/put type decides the NVS entry type,
// so a key must always be read and written with the same type. put_* return false when nothing was stored.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    virtual bool get_bool(const char* ns, const char* key, bool default_value) = 0;
    virtual bool put_bool(const char* ns, const char* key, bool value) = 0;

    virtual int32_t get_int(const char* ns, const char* key, int32_t default_value) = 0;
    virtual bool put_int(const char* ns, const char* key, int32_t value) = 0;

    virtual float get_float(const char* ns, const char* key, float default_value) = 0;
    virtual bool put_float(const char* ns, const char* key, float value) = 0;
};

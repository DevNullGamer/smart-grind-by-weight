#pragma once
#include <stddef.h>
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

    // Raw bytes (GrindSizeStore, chg020). Not pure: a store without blob support (the typed-only test fakes) behaves as
    // "nothing stored, nothing can be stored".
    // get_blob returns the number of bytes copied into buf, or 0 when the entry is missing or does not fit in cap
    // (a stored entry longer than cap is never truncated). put_blob stores the bytes as one unit: on failure the
    // previous entry is left as it was. A zero-length put erases the entry. erase_blob removes a key of any type and
    // returns true when the entry is gone afterwards (also when it never existed).
    virtual size_t get_blob(const char* ns, const char* key, uint8_t* buf, size_t cap) {
        (void)ns; (void)key; (void)buf; (void)cap;
        return 0;
    }
    virtual bool put_blob(const char* ns, const char* key, const uint8_t* data, size_t len) {
        (void)ns; (void)key; (void)data; (void)len;
        return false;
    }
    virtual bool erase_blob(const char* ns, const char* key) {
        (void)ns; (void)key;
        return false;
    }
};

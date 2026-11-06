// Grind size of a profile: validity, text, stored form, compare-first store with a fake store (chg020 step 4);
// READY tab layout helpers and the history profile clamp
#include <unity.h>
#include <math.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "controllers/grind_size.h"
#include "logging/grind_history.h"
#include "system/ui_settings.h"
#include "ui/ready_tabs.h"

namespace {

// In-memory store with blob support that counts every call that would touch flash
class FakeBlobStore : public SettingsStore {
public:
    std::map<std::string, std::vector<uint8_t>> blobs;
    int puts = 0;
    int erases = 0;
    int gets = 0;
    bool accept_puts = true;

    static std::string id(const char* ns, const char* key) { return std::string(ns) + "/" + key; }

    bool get_bool(const char*, const char*, bool d) override { return d; }
    bool put_bool(const char*, const char*, bool) override { return false; }
    int32_t get_int(const char*, const char*, int32_t d) override { return d; }
    bool put_int(const char*, const char*, int32_t) override { return false; }
    float get_float(const char*, const char*, float d) override { return d; }
    bool put_float(const char*, const char*, float) override { return false; }

    size_t get_blob(const char* ns, const char* key, uint8_t* buf, size_t cap) override {
        gets++;
        auto it = blobs.find(id(ns, key));
        if (it == blobs.end() || it->second.size() > cap) return 0;
        memcpy(buf, it->second.data(), it->second.size());
        return it->second.size();
    }
    bool put_blob(const char* ns, const char* key, const uint8_t* data, size_t len) override {
        puts++;
        if (!accept_puts) return false;
        blobs[id(ns, key)] = std::vector<uint8_t>(data, data + len);
        return true;
    }
    bool erase_blob(const char* ns, const char* key) override {
        if (blobs.erase(id(ns, key)) > 0) erases++;
        return true;
    }
};

GrindSize size_of(int32_t tenths, const char* unit = "") {
    GrindSize s;
    s.present = true;
    s.tenths = tenths;
    strncpy(s.unit, unit, GRIND_UNIT_BUF - 1);
    return s;
}

std::string key_of(int slot) { return FakeBlobStore::id(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindSize[slot]); }

}  // namespace

void test_grind_size_validity() {
    TEST_ASSERT_TRUE(grind_size_valid(GrindSize()));
    TEST_ASSERT_TRUE(grind_size_valid(size_of(0)));
    TEST_ASSERT_TRUE(grind_size_valid(size_of(GRIND_SIZE_MAX_TENTHS, "um")));
    TEST_ASSERT_TRUE(grind_size_valid(size_of(145, "A-z 0.9/%_")));  // 6 characters: cut at the buffer, still valid
    TEST_ASSERT_FALSE(grind_size_valid(size_of(-1)));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(GRIND_SIZE_MAX_TENTHS + 1)));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(10, "\"")));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(10, "\\")));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(10, "\xC3\xA9")));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(10, " um")));
    TEST_ASSERT_FALSE(grind_size_valid(size_of(10, "um ")));
    GrindSize unterminated = size_of(10);
    memset(unterminated.unit, 'a', sizeof(unterminated.unit));
    TEST_ASSERT_FALSE(grind_size_valid(unterminated));
    GrindSize absent_with_value;  // not present but carrying data is not a valid state
    absent_with_value.tenths = 5;
    TEST_ASSERT_FALSE(grind_size_valid(absent_with_value));
}

void test_grind_size_equal_compares_value_and_unit() {
    TEST_ASSERT_TRUE(grind_size_equal(GrindSize(), GrindSize()));
    TEST_ASSERT_TRUE(grind_size_equal(size_of(145, "um"), size_of(145, "um")));
    TEST_ASSERT_FALSE(grind_size_equal(size_of(145, "um"), size_of(146, "um")));
    TEST_ASSERT_FALSE(grind_size_equal(size_of(145, "um"), size_of(145, "mm")));
    TEST_ASSERT_FALSE(grind_size_equal(size_of(145, "um"), size_of(145)));
    TEST_ASSERT_FALSE(grind_size_equal(size_of(0), GrindSize()));
}

void test_grind_size_text() {
    char text[GRIND_SIZE_TEXT_BUF];
    TEST_ASSERT_EQUAL_UINT(0, grind_size_format(text, sizeof(text), GrindSize()));
    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL_UINT(14, grind_size_format(text, sizeof(text), size_of(145, "um")));
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5 um", text);
    grind_size_format(text, sizeof(text), size_of(0));
    TEST_ASSERT_EQUAL_STRING("Grind: 0.0", text);
    grind_size_format(text, sizeof(text), size_of(7));
    TEST_ASSERT_EQUAL_STRING("Grind: 0.7", text);
    grind_size_format(text, sizeof(text), size_of(9999, "123456"));  // worst case
    TEST_ASSERT_EQUAL_STRING("Grind: 999.9 123456", text);
    TEST_ASSERT_EQUAL_UINT(19, strlen(text));
    TEST_ASSERT_TRUE(strlen(text) < GRIND_SIZE_TEXT_BUF);
    // an invalid size gives no text rather than markup from a corrupt source
    TEST_ASSERT_EQUAL_UINT(0, grind_size_format(text, sizeof(text), size_of(10, "\"")));
    TEST_ASSERT_EQUAL_STRING("", text);
    // a short buffer is cut and terminated
    char tiny[6];
    grind_size_format(tiny, sizeof(tiny), size_of(145, "um"));
    TEST_ASSERT_EQUAL_STRING("Grind", tiny);
    TEST_ASSERT_EQUAL_UINT(0, grind_size_format(nullptr, 0, size_of(1)));
}

void test_grind_size_blob_roundtrip() {
    const GrindSize cases[] = {size_of(0), size_of(1), size_of(145, "um"), size_of(9999, "123456"), size_of(1000, "%")};
    for (const GrindSize& original : cases) {
        uint8_t blob[GRIND_SIZE_BLOB_BYTES];
        TEST_ASSERT_EQUAL_UINT(GRIND_SIZE_BLOB_BYTES, grind_size_encode(original, blob));
        GrindSize back;
        TEST_ASSERT_TRUE(grind_size_decode(blob, sizeof(blob), &back));
        TEST_ASSERT_TRUE(grind_size_equal(original, back));
    }
}

void test_grind_size_decode_rejects_corrupt_blobs() {
    uint8_t good[GRIND_SIZE_BLOB_BYTES];
    grind_size_encode(size_of(145, "um"), good);
    GrindSize out = size_of(1);

    // wrong lengths
    TEST_ASSERT_FALSE(grind_size_decode(good, 0, &out));
    TEST_ASSERT_FALSE(out.present);
    TEST_ASSERT_FALSE(grind_size_decode(good, GRIND_SIZE_BLOB_BYTES - 1, &out));
    uint8_t longer[GRIND_SIZE_BLOB_BYTES + 1];
    memcpy(longer, good, sizeof(good));
    longer[GRIND_SIZE_BLOB_BYTES] = 0;
    TEST_ASSERT_FALSE(grind_size_decode(longer, sizeof(longer), &out));
    TEST_ASSERT_FALSE(grind_size_decode(nullptr, GRIND_SIZE_BLOB_BYTES, &out));

    // non-finite and out-of-range floats
    const float bad_values[] = {NAN, INFINITY, -INFINITY, -0.1f, 1000.0f, 5000.0f, -1.0f};
    for (float v : bad_values) {
        uint8_t blob[GRIND_SIZE_BLOB_BYTES];
        memcpy(blob, good, sizeof(blob));
        memcpy(blob, &v, sizeof(float));
        out = size_of(1);
        TEST_ASSERT_FALSE(grind_size_decode(blob, sizeof(blob), &out));
        TEST_ASSERT_FALSE(out.present);
    }

    // unit: character outside the charset, a stray byte past the 6 characters, a leading space
    uint8_t blob[GRIND_SIZE_BLOB_BYTES];
    memcpy(blob, good, sizeof(blob));
    blob[4] = '"';
    TEST_ASSERT_FALSE(grind_size_decode(blob, sizeof(blob), &out));
    memcpy(blob, good, sizeof(blob));
    blob[4 + 7] = 'x';
    TEST_ASSERT_FALSE(grind_size_decode(blob, sizeof(blob), &out));
    memcpy(blob, good, sizeof(blob));
    blob[4] = ' ';
    TEST_ASSERT_FALSE(grind_size_decode(blob, sizeof(blob), &out));
    memcpy(blob, good, sizeof(blob));
    memset(blob + 4, 'a', 8);  // no terminator
    TEST_ASSERT_FALSE(grind_size_decode(blob, sizeof(blob), &out));
}

void test_store_load_empty_and_bad_slots() {
    FakeBlobStore fake;
    GrindSizeStore store;
    store.attach(&fake);
    store.load();
    for (int slot = 0; slot < GRIND_SIZE_SLOT_COUNT; slot++) TEST_ASSERT_FALSE(store.get(slot).present);
    TEST_ASSERT_FALSE(store.get(-1).present);
    TEST_ASSERT_FALSE(store.get(GRIND_SIZE_SLOT_COUNT).present);
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(-1, size_of(10)));
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(GRIND_SIZE_SLOT_COUNT, size_of(10)));
    TEST_ASSERT_FALSE(store.clear(GRIND_SIZE_SLOT_COUNT));
    TEST_ASSERT_EQUAL_INT(0, fake.puts);
}

void test_store_without_a_store_never_succeeds() {
    GrindSizeStore store;
    store.load();
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(0, size_of(10)));
    TEST_ASSERT_FALSE(store.clear(0));
}

void test_store_is_compare_first() {
    FakeBlobStore fake;
    GrindSizeStore store;
    store.attach(&fake);
    store.load();

    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(3, size_of(145, "um")));
    TEST_ASSERT_EQUAL_INT(1, fake.puts);
    TEST_ASSERT_EQUAL_INT(12, (int)fake.blobs[key_of(3)].size());

    // same value, same unit: nothing is written
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::UNCHANGED, store.store(3, size_of(145, "um")));
    TEST_ASSERT_EQUAL_INT(1, fake.puts);

    // different value
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(3, size_of(146, "um")));
    TEST_ASSERT_EQUAL_INT(2, fake.puts);

    // same value, different unit
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(3, size_of(146, "mm")));
    TEST_ASSERT_EQUAL_INT(3, fake.puts);

    // same value, unit dropped
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(3, size_of(146)));
    TEST_ASSERT_EQUAL_INT(4, fake.puts);
    TEST_ASSERT_EQUAL_STRING("", store.get(3).unit);

    // slots are independent
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(0, size_of(146)));
    TEST_ASSERT_EQUAL_INT(5, fake.puts);
    TEST_ASSERT_EQUAL_INT(2, (int)fake.blobs.size());
}

void test_store_keeps_ram_on_a_failed_write() {
    FakeBlobStore fake;
    GrindSizeStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(1, size_of(100, "um")));

    fake.accept_puts = false;
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(1, size_of(200, "um")));
    TEST_ASSERT_EQUAL_INT(100, store.get(1).tenths);  // RAM still matches flash
    // and the retry is not mistaken for "already stored"
    fake.accept_puts = true;
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(1, size_of(200, "um")));
    TEST_ASSERT_EQUAL_INT(200, store.get(1).tenths);
}

void test_store_refuses_invalid_sizes_without_writing() {
    FakeBlobStore fake;
    GrindSizeStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(0, size_of(GRIND_SIZE_MAX_TENTHS + 1)));
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(0, size_of(-5)));
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::FAILED, store.store(0, size_of(10, "\"")));
    TEST_ASSERT_EQUAL_INT(0, fake.puts);
    TEST_ASSERT_FALSE(store.get(0).present);
}

void test_store_clears_with_an_absent_size() {
    FakeBlobStore fake;
    GrindSizeStore store;
    store.attach(&fake);
    store.load();

    // nothing stored: clearing writes and erases nothing
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::UNCHANGED, store.store(2, GrindSize()));
    TEST_ASSERT_EQUAL_INT(0, fake.erases);

    store.store(2, size_of(50, "um"));
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(2, GrindSize()));
    TEST_ASSERT_EQUAL_INT(1, fake.erases);
    TEST_ASSERT_FALSE(store.get(2).present);
    TEST_ASSERT_EQUAL_INT(0, (int)fake.blobs.size());

    store.store(2, size_of(50, "um"));
    TEST_ASSERT_TRUE(store.clear(2));
    TEST_ASSERT_EQUAL_INT(2, fake.erases);
    TEST_ASSERT_TRUE(store.clear(2));  // already gone: still true, nothing erased
    TEST_ASSERT_EQUAL_INT(2, fake.erases);
}

void test_store_load_restores_and_drops_corrupt_slots() {
    FakeBlobStore fake;
    {
        GrindSizeStore store;
        store.attach(&fake);
        store.load();
        store.store(0, size_of(120, "um"));
        store.store(3, size_of(999, "123456"));
    }
    fake.blobs[key_of(1)] = std::vector<uint8_t>(5, 0xFF);   // wrong length
    uint8_t nan_blob[GRIND_SIZE_BLOB_BYTES] = {};
    const float nan = NAN;
    memcpy(nan_blob, &nan, sizeof(float));
    fake.blobs[key_of(2)] = std::vector<uint8_t>(nan_blob, nan_blob + sizeof(nan_blob));

    GrindSizeStore reloaded;
    reloaded.attach(&fake);
    reloaded.load();
    TEST_ASSERT_TRUE(reloaded.get(0).present);
    TEST_ASSERT_EQUAL_INT(120, reloaded.get(0).tenths);
    TEST_ASSERT_EQUAL_STRING("um", reloaded.get(0).unit);
    TEST_ASSERT_FALSE(reloaded.get(1).present);
    TEST_ASSERT_FALSE(reloaded.get(2).present);
    TEST_ASSERT_TRUE(reloaded.get(3).present);
    TEST_ASSERT_EQUAL_STRING("123456", reloaded.get(3).unit);

    // a stored entry longer than the blob is not truncated into a valid one
    fake.blobs[key_of(0)] = std::vector<uint8_t>(GRIND_SIZE_BLOB_BYTES + 4, 0);
    reloaded.load();
    TEST_ASSERT_FALSE(reloaded.get(0).present);
}

void test_store_overwrites_a_corrupt_entry_instead_of_trusting_it() {
    FakeBlobStore fake;
    fake.blobs[key_of(1)] = std::vector<uint8_t>(3, 7);
    GrindSizeStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_FALSE(store.get(1).present);
    TEST_ASSERT_EQUAL(GrindSizeStoreResult::STORED, store.store(1, size_of(10)));
    TEST_ASSERT_EQUAL_INT(12, (int)fake.blobs[key_of(1)].size());
}

void test_nvs_key_names_fit_the_limit() {
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(strlen(ui_settings_keys::kGrindSize[i]) <= 15);
        TEST_ASSERT_TRUE(strlen(ui_settings_keys::kProfileWeight[i]) <= 15);
        TEST_ASSERT_TRUE(strlen(ui_settings_keys::kProfileTime[i]) <= 15);
    }
    TEST_ASSERT_EQUAL_STRING("weight3", ui_settings_keys::kProfileWeight[USER_DOSE_PROFILE_INDEX]);
    TEST_ASSERT_EQUAL_STRING("weight0", ui_settings_keys::kProfileWeight[0]);  // shipped keys never change
    TEST_ASSERT_EQUAL_STRING("time2", ui_settings_keys::kProfileTime[2]);
    TEST_ASSERT_EQUAL_STRING("profile", ui_settings_keys::kProfileCurrent);
}

void test_ready_tab_layout() {
    static_assert(READY_MENU_TAB_INDEX == USER_PROFILE_COUNT, "menu tab follows the profile count");
    static_assert(READY_MENU_TAB_INDEX_NO_DOSE == USER_BASE_PROFILE_COUNT, "without the PROFILE page MENU follows Custom");
    TEST_ASSERT_EQUAL_INT(4, ready_menu_tab_index(true));
    TEST_ASSERT_EQUAL_INT(3, ready_menu_tab_index(false));
}

void test_ready_tab_remap() {
    // PROFILE page appears: MENU moves from 3 to 4, profile pages stay
    TEST_ASSERT_EQUAL_INT(4, ready_remap_tab(3, false, true, 1));
    TEST_ASSERT_EQUAL_INT(0, ready_remap_tab(0, false, true, 1));
    TEST_ASSERT_EQUAL_INT(2, ready_remap_tab(2, false, true, 1));
    // PROFILE page disappears: MENU moves from 4 to 3; the removed page falls back to the selected profile
    TEST_ASSERT_EQUAL_INT(3, ready_remap_tab(4, true, false, 1));
    TEST_ASSERT_EQUAL_INT(1, ready_remap_tab(3, true, false, 1));
    TEST_ASSERT_EQUAL_INT(1, ready_remap_tab(1, true, false, 2));
    // a fallback outside the remaining pages becomes the first page
    TEST_ASSERT_EQUAL_INT(0, ready_remap_tab(3, true, false, 3));
    TEST_ASSERT_EQUAL_INT(0, ready_remap_tab(-1, false, true, -1));
}

void test_history_profile_is_clamped_to_custom() {
    TEST_ASSERT_EQUAL_UINT8(0, grind_history_profile_of(0));
    TEST_ASSERT_EQUAL_UINT8(1, grind_history_profile_of(1));
    TEST_ASSERT_EQUAL_UINT8(2, grind_history_profile_of(2));
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_PROFILE_MAX, grind_history_profile_of(3));  // Profile Dose
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_PROFILE_MAX, grind_history_profile_of(255));

    GrindHistoryEntry entry;
    entry.profile = grind_history_profile_of(USER_DOSE_PROFILE_INDEX);
    TEST_ASSERT_TRUE(grind_history_entry_valid(entry));
    entry.profile = USER_DOSE_PROFILE_INDEX;  // unclamped it would be refused
    TEST_ASSERT_FALSE(grind_history_entry_valid(entry));
}

void run_grind_size_tests() {
    RUN_TEST(test_grind_size_validity);
    RUN_TEST(test_grind_size_equal_compares_value_and_unit);
    RUN_TEST(test_grind_size_text);
    RUN_TEST(test_grind_size_blob_roundtrip);
    RUN_TEST(test_grind_size_decode_rejects_corrupt_blobs);
    RUN_TEST(test_store_load_empty_and_bad_slots);
    RUN_TEST(test_store_without_a_store_never_succeeds);
    RUN_TEST(test_store_is_compare_first);
    RUN_TEST(test_store_keeps_ram_on_a_failed_write);
    RUN_TEST(test_store_refuses_invalid_sizes_without_writing);
    RUN_TEST(test_store_clears_with_an_absent_size);
    RUN_TEST(test_store_load_restores_and_drops_corrupt_slots);
    RUN_TEST(test_store_overwrites_a_corrupt_entry_instead_of_trusting_it);
    RUN_TEST(test_nvs_key_names_fit_the_limit);
    RUN_TEST(test_ready_tab_layout);
    RUN_TEST(test_ready_tab_remap);
    RUN_TEST(test_history_profile_is_clamped_to_custom);
}

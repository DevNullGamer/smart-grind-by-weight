// Grind rpm of a profile (chg026 step 3): validity, compare-first int store with a fake store, grind info text
#include <unity.h>
#include <string.h>
#include <map>
#include <string>
#include "controllers/grind_rpm.h"
#include "controllers/grind_size.h"
#include "system/ui_settings.h"

namespace {

// In-memory int store that counts every call that would touch flash
class FakeIntStore : public SettingsStore {
public:
    std::map<std::string, int32_t> ints;
    int puts = 0;
    int erases = 0;
    bool accept_puts = true;

    static std::string id(const char* ns, const char* key) { return std::string(ns) + "/" + key; }

    bool get_bool(const char*, const char*, bool d) override { return d; }
    bool put_bool(const char*, const char*, bool) override { return false; }
    int32_t get_int(const char* ns, const char* key, int32_t d) override {
        auto it = ints.find(id(ns, key));
        return it == ints.end() ? d : it->second;
    }
    bool put_int(const char* ns, const char* key, int32_t v) override {
        puts++;
        if (!accept_puts) return false;
        ints[id(ns, key)] = v;
        return true;
    }
    float get_float(const char*, const char*, float d) override { return d; }
    bool put_float(const char*, const char*, float) override { return false; }
    size_t get_blob(const char*, const char*, uint8_t*, size_t) override { return 0; }
    bool put_blob(const char*, const char*, const uint8_t*, size_t) override { return false; }
    bool erase_blob(const char* ns, const char* key) override {
        if (ints.erase(id(ns, key)) > 0) erases++;
        return true;
    }
};

GrindRpm rpm_of(int32_t v) {
    GrindRpm r;
    r.present = true;
    r.value = v;
    return r;
}

GrindSize size_of(int32_t tenths, const char* unit = "") {
    GrindSize s;
    s.present = true;
    s.tenths = tenths;
    strncpy(s.unit, unit, GRIND_UNIT_BUF - 1);
    return s;
}

std::string key_of(int slot) { return FakeIntStore::id(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindRpm[slot]); }

}  // namespace

void test_rpm_validity_and_equality() {
    TEST_ASSERT_TRUE(grind_rpm_valid(GrindRpm()));
    TEST_ASSERT_TRUE(grind_rpm_valid(rpm_of(0)));
    TEST_ASSERT_TRUE(grind_rpm_valid(rpm_of(600)));
    TEST_ASSERT_TRUE(grind_rpm_valid(rpm_of(GRIND_RPM_MAX)));
    TEST_ASSERT_FALSE(grind_rpm_valid(rpm_of(-1)));
    TEST_ASSERT_FALSE(grind_rpm_valid(rpm_of(GRIND_RPM_MAX + 1)));
    GrindRpm absent_with_value;
    absent_with_value.value = 5;
    TEST_ASSERT_FALSE(grind_rpm_valid(absent_with_value));

    TEST_ASSERT_TRUE(grind_rpm_equal(GrindRpm(), GrindRpm()));
    TEST_ASSERT_TRUE(grind_rpm_equal(rpm_of(600), rpm_of(600)));
    TEST_ASSERT_FALSE(grind_rpm_equal(rpm_of(600), rpm_of(601)));
    TEST_ASSERT_FALSE(grind_rpm_equal(rpm_of(0), GrindRpm()));  // rpm 0 is not "none"
    TEST_ASSERT_TRUE(grind_rpm_equal(GrindRpm(), absent_with_value));
}

void test_rpm_store_accepts_boundaries() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    const int32_t values[] = {0, 600, GRIND_RPM_MAX};
    for (int32_t v : values) {
        TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(1, rpm_of(v)));
        TEST_ASSERT_TRUE(store.get(1).present);
        TEST_ASSERT_EQUAL_INT32(v, store.get(1).value);
        TEST_ASSERT_EQUAL_INT32(v, fake.ints[key_of(1)]);
    }
}

void test_rpm_store_rejects_invalid_and_keeps_ram() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(2, rpm_of(600)));
    const int puts = fake.puts;
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(2, rpm_of(-1)));
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(2, rpm_of(GRIND_RPM_MAX + 1)));
    GrindRpm bad;
    bad.value = 7;  // absent but carrying data
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(2, bad));
    TEST_ASSERT_EQUAL_INT(puts, fake.puts);
    TEST_ASSERT_EQUAL_INT(0, fake.erases);
    TEST_ASSERT_TRUE(store.get(2).present);
    TEST_ASSERT_EQUAL_INT32(600, store.get(2).value);
    TEST_ASSERT_EQUAL_INT32(600, fake.ints[key_of(2)]);
}

void test_rpm_store_is_compare_first() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(0, rpm_of(600)));
    TEST_ASSERT_EQUAL_INT(1, fake.puts);
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::UNCHANGED, store.store(0, rpm_of(600)));
    TEST_ASSERT_EQUAL_INT(1, fake.puts);
    TEST_ASSERT_EQUAL_INT(0, fake.erases);
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(0, rpm_of(601)));
    TEST_ASSERT_EQUAL_INT(2, fake.puts);
    // storing "none" on an empty slot touches nothing
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::UNCHANGED, store.store(3, GrindRpm()));
    TEST_ASSERT_EQUAL_INT(2, fake.puts);
    TEST_ASSERT_EQUAL_INT(0, fake.erases);
    // slots are independent
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(3, rpm_of(601)));
    TEST_ASSERT_EQUAL_INT(2, (int)fake.ints.size());
}

void test_rpm_clear_erases_once() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    store.store(1, rpm_of(0));
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(1, GrindRpm()));  // absent clears
    TEST_ASSERT_EQUAL_INT(1, fake.erases);
    TEST_ASSERT_FALSE(store.get(1).present);
    TEST_ASSERT_EQUAL_INT(0, (int)fake.ints.size());
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::UNCHANGED, store.store(1, GrindRpm()));
    TEST_ASSERT_EQUAL_INT(1, fake.erases);

    store.store(1, rpm_of(300));
    TEST_ASSERT_TRUE(store.clear(1));
    TEST_ASSERT_EQUAL_INT(2, fake.erases);
    TEST_ASSERT_FALSE(store.get(1).present);
    TEST_ASSERT_TRUE(store.clear(1));  // already gone
    TEST_ASSERT_EQUAL_INT(2, fake.erases);
}

void test_rpm_load_restores_and_drops_garbage() {
    FakeIntStore fake;
    fake.ints[key_of(0)] = 600;
    fake.ints[key_of(1)] = -1;
    fake.ints[key_of(2)] = GRIND_RPM_MAX + 1;
    fake.ints[key_of(3)] = 0;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_TRUE(store.get(0).present);
    TEST_ASSERT_EQUAL_INT32(600, store.get(0).value);
    TEST_ASSERT_FALSE(store.get(1).present);
    TEST_ASSERT_FALSE(store.get(2).present);
    TEST_ASSERT_TRUE(store.get(3).present);  // 0 is a real value
    TEST_ASSERT_EQUAL_INT32(0, store.get(3).value);
    fake.ints[key_of(3)] = INT32_MIN;
    fake.ints[key_of(0)] = INT32_MAX;
    store.load();
    TEST_ASSERT_FALSE(store.get(0).present);
    TEST_ASSERT_FALSE(store.get(3).present);
    // garbage is overwritten, not trusted
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(0, rpm_of(500)));
    TEST_ASSERT_EQUAL_INT32(500, fake.ints[key_of(0)]);
}

void test_rpm_bad_slot_and_no_store() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    TEST_ASSERT_FALSE(store.get(-1).present);
    TEST_ASSERT_FALSE(store.get(GRIND_RPM_SLOT_COUNT).present);
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(-1, rpm_of(1)));
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(GRIND_RPM_SLOT_COUNT, rpm_of(1)));
    TEST_ASSERT_FALSE(store.clear(-1));
    TEST_ASSERT_FALSE(store.clear(GRIND_RPM_SLOT_COUNT));
    TEST_ASSERT_EQUAL_INT(0, fake.puts);

    GrindRpmStore detached;
    detached.load();
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, detached.store(0, rpm_of(10)));
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, detached.store(0, GrindRpm()));
    TEST_ASSERT_FALSE(detached.clear(0));
    TEST_ASSERT_FALSE(detached.get(0).present);
}

void test_rpm_failed_put_keeps_ram() {
    FakeIntStore fake;
    GrindRpmStore store;
    store.attach(&fake);
    store.load();
    store.store(1, rpm_of(100));
    fake.accept_puts = false;
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(1, rpm_of(200)));
    TEST_ASSERT_EQUAL_INT32(100, store.get(1).value);
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::FAILED, store.store(2, rpm_of(200)));
    TEST_ASSERT_FALSE(store.get(2).present);
    fake.accept_puts = true;
    TEST_ASSERT_EQUAL(GrindRpmStoreResult::STORED, store.store(1, rpm_of(200)));  // retry not mistaken for stored
    TEST_ASSERT_EQUAL_INT32(200, store.get(1).value);
}

void test_rpm_key_names_fit_the_limit() {
    for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(strlen(ui_settings_keys::kGrindRpm[i]) <= 15);
    TEST_ASSERT_EQUAL_STRING("rpm0", ui_settings_keys::kGrindRpm[0]);
    TEST_ASSERT_EQUAL_STRING("rpm3", ui_settings_keys::kGrindRpm[3]);
}

void test_grind_info_format_combinations() {
    char text[GRIND_SIZE_TEXT_BUF];
    TEST_ASSERT_EQUAL_UINT(0, grind_info_format(text, sizeof(text), GrindSize(), GrindRpm()));
    TEST_ASSERT_EQUAL_STRING("", text);

    TEST_ASSERT_EQUAL_UINT(strlen("Grind: 14.5 um\n600 rpm"),
                           grind_info_format(text, sizeof(text), size_of(145, "um"), rpm_of(600)));
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5 um\n600 rpm", text);

    TEST_ASSERT_EQUAL_UINT(strlen("Grind: 14.5"), grind_info_format(text, sizeof(text), size_of(145), GrindRpm()));
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5", text);
    TEST_ASSERT_EQUAL_UINT(14, grind_info_format(text, sizeof(text), size_of(145, "um"), GrindRpm()));
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5 um", text);

    TEST_ASSERT_EQUAL_UINT(7, grind_info_format(text, sizeof(text), GrindSize(), rpm_of(600)));
    TEST_ASSERT_EQUAL_STRING("600 rpm", text);  // no "Grind:" prefix, no leading newline
    grind_info_format(text, sizeof(text), GrindSize(), rpm_of(0));
    TEST_ASSERT_EQUAL_STRING("0 rpm", text);

    grind_info_format(text, sizeof(text), size_of(145), rpm_of(600));  // unit absent
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5\n600 rpm", text);
}

void test_grind_info_format_worst_case_fits() {
    char text[GRIND_SIZE_TEXT_BUF];
    const size_t n = grind_info_format(text, sizeof(text), size_of(9999, "123456"), rpm_of(GRIND_RPM_MAX));
    TEST_ASSERT_EQUAL_STRING("Grind: 999.9 123456\n9999 rpm", text);
    TEST_ASSERT_EQUAL_UINT(strlen(text), n);
    TEST_ASSERT_TRUE(n < GRIND_SIZE_TEXT_BUF);
}

void test_grind_info_format_is_always_terminated() {
    // every capacity, with filler in the buffer: terminated within cap and the return value is the text length
    const GrindSize sizes[] = {GrindSize(), size_of(145, "um"), size_of(9999, "123456")};
    const GrindRpm rpms[] = {GrindRpm(), rpm_of(0), rpm_of(GRIND_RPM_MAX)};
    for (const GrindSize& s : sizes) {
        for (const GrindRpm& r : rpms) {
            for (size_t cap = 1; cap <= GRIND_SIZE_TEXT_BUF; cap++) {
                char buf[GRIND_SIZE_TEXT_BUF + 1];
                memset(buf, 'X', sizeof(buf));
                const size_t n = grind_info_format(buf, cap, s, r);
                TEST_ASSERT_TRUE(n < cap);
                TEST_ASSERT_EQUAL_UINT(n, strlen(buf));
                TEST_ASSERT_EQUAL_CHAR('X', buf[cap]);  // nothing written past cap
            }
        }
    }
    char one[1] = {'X'};
    TEST_ASSERT_EQUAL_UINT(0, grind_info_format(one, 1, size_of(145, "um"), rpm_of(600)));
    TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
    char guard = 'X';
    TEST_ASSERT_EQUAL_UINT(0, grind_info_format(&guard, 0, size_of(145, "um"), rpm_of(600)));
    TEST_ASSERT_EQUAL_CHAR('X', guard);  // capacity 0 writes nothing
    TEST_ASSERT_EQUAL_UINT(0, grind_info_format(nullptr, 0, size_of(1), rpm_of(1)));
    TEST_ASSERT_EQUAL_UINT(0, grind_info_format(nullptr, 10, size_of(1), rpm_of(1)));
}

void test_grind_info_format_ignores_invalid_rpm() {
    char text[GRIND_SIZE_TEXT_BUF];
    grind_info_format(text, sizeof(text), size_of(145, "um"), rpm_of(-1));
    TEST_ASSERT_EQUAL_STRING("Grind: 14.5 um", text);
    grind_info_format(text, sizeof(text), GrindSize(), rpm_of(GRIND_RPM_MAX + 1));
    TEST_ASSERT_EQUAL_STRING("", text);
}

void run_grind_rpm_tests() {
    RUN_TEST(test_rpm_validity_and_equality);
    RUN_TEST(test_rpm_store_accepts_boundaries);
    RUN_TEST(test_rpm_store_rejects_invalid_and_keeps_ram);
    RUN_TEST(test_rpm_store_is_compare_first);
    RUN_TEST(test_rpm_clear_erases_once);
    RUN_TEST(test_rpm_load_restores_and_drops_garbage);
    RUN_TEST(test_rpm_bad_slot_and_no_store);
    RUN_TEST(test_rpm_failed_put_keeps_ram);
    RUN_TEST(test_rpm_key_names_fit_the_limit);
    RUN_TEST(test_grind_info_format_combinations);
    RUN_TEST(test_grind_info_format_worst_case_fits);
    RUN_TEST(test_grind_info_format_is_always_terminated);
    RUN_TEST(test_grind_info_format_ignores_invalid_rpm);
}

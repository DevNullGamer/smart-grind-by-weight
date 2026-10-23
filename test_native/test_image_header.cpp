#include <unity.h>
#include <string.h>
#include <vector>
#include "system/image_header.h"

static const uint32_t kSlot = 3072u * 1024u;
static const uint32_t kGoodSize = 2000u * 1024u;

static void put_u32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; i++) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}

// A header shaped like a real ESP32-S3 app image (see build output: e9 06 02 4f d8 5f 37 40 ... 09 00 ... 32 54 cd ab)
static std::vector<uint8_t> good_header(size_t len = UPDATE_IMAGE_HEADER_BYTES) {
    std::vector<uint8_t> h(len < UPDATE_IMAGE_HEADER_BYTES ? UPDATE_IMAGE_HEADER_BYTES : len, 0);
    h[0] = 0xE9;
    h[1] = 6;
    put_u32(h, 4, 0x40375fd8);
    h[12] = 0x09;
    h[13] = 0x00;
    put_u32(h, 32, 0xABCD5432);
    h.resize(len);  // shorter than the header: truncate the otherwise valid image
    return h;
}

static ImageHeaderResult check(const std::vector<uint8_t>& h, uint32_t total = kGoodSize, uint32_t slot = kSlot) {
    return image_header_validate(h.data(), h.size(), total, slot);
}

static void test_image_header_good() {
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(good_header()));
}

static void test_image_header_extra_bytes_ok() {
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(good_header(512)));
}

static void test_image_header_flash_mapped_entry_ok() {
    auto h = good_header();
    put_u32(h, 4, 0x42000100);
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(h));
}

static void test_image_header_too_short() {
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_SHORT, check(good_header(39)));
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_SHORT, check(good_header(0)));
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_SHORT, image_header_validate(nullptr, 64, kGoodSize, kSlot));
}

static void test_image_header_bad_magic() {
    auto h = good_header();
    h[0] = 0xE8;
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_MAGIC, check(h));
}

static void test_image_header_segment_count_bounds() {
    auto h = good_header();
    h[1] = 0;
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_SEGMENTS, check(h));
    h[1] = 1;
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(h));
    h[1] = 16;
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(h));
    h[1] = 17;
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_SEGMENTS, check(h));
}

static void test_image_header_wrong_chip() {
    auto h = good_header();
    h[12] = 0x05;  // ESP32-C3
    TEST_ASSERT_EQUAL(ImageHeaderResult::WRONG_CHIP, check(h));
    h[12] = 0x09;
    h[13] = 0x01;  // high byte matters (little endian uint16)
    TEST_ASSERT_EQUAL(ImageHeaderResult::WRONG_CHIP, check(h));
}

static void test_image_header_bad_entry() {
    auto h = good_header();
    put_u32(h, 4, 0);
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_ENTRY, check(h));
    put_u32(h, 4, 0x4036FFFF);  // just below IRAM range
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_ENTRY, check(h));
    put_u32(h, 4, 0x40400000);  // just above IRAM range
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_ENTRY, check(h));
    put_u32(h, 4, 0xFFFFFFFF);
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_ENTRY, check(h));
}

static void test_image_header_bad_app_desc() {
    auto h = good_header();
    put_u32(h, 32, 0);
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_APP_DESC, check(h));
    h = good_header();
    h[35] ^= 0x01;
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_APP_DESC, check(h));
}

static void test_image_header_size_boundaries() {
    const auto h = good_header();
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_SMALL, check(h, 0));
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_SMALL, check(h, UPDATE_IMAGE_MIN_SIZE - 1));
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(h, UPDATE_IMAGE_MIN_SIZE));
    TEST_ASSERT_EQUAL(ImageHeaderResult::OK, check(h, kSlot));
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_LARGE, check(h, kSlot + 1));
    TEST_ASSERT_EQUAL(ImageHeaderResult::TOO_LARGE, check(h, 0xFFFFFFFFu));
}

static void test_image_header_header_errors_win_over_size() {
    auto h = good_header();
    h[0] = 0x00;
    TEST_ASSERT_EQUAL(ImageHeaderResult::BAD_MAGIC, check(h, 0));
}

static void test_image_header_result_text() {
    TEST_ASSERT_NOT_NULL(image_header_result_text(ImageHeaderResult::OK));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(image_header_result_text(ImageHeaderResult::WRONG_CHIP),
                                    image_header_result_text(ImageHeaderResult::BAD_MAGIC)));
}

void run_image_header_tests() {
    RUN_TEST(test_image_header_good);
    RUN_TEST(test_image_header_extra_bytes_ok);
    RUN_TEST(test_image_header_flash_mapped_entry_ok);
    RUN_TEST(test_image_header_too_short);
    RUN_TEST(test_image_header_bad_magic);
    RUN_TEST(test_image_header_segment_count_bounds);
    RUN_TEST(test_image_header_wrong_chip);
    RUN_TEST(test_image_header_bad_entry);
    RUN_TEST(test_image_header_bad_app_desc);
    RUN_TEST(test_image_header_size_boundaries);
    RUN_TEST(test_image_header_header_errors_win_over_size);
    RUN_TEST(test_image_header_result_text);
}

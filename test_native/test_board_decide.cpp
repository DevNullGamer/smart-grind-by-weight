#include <unity.h>
#include "hardware/board_detect.h"
#include "hardware/hx711_clock_mask.h"

namespace {

BoardEvidence evidence(int8_t g45, int16_t te, int8_t g18,
                       BoardOverride ov = BoardOverride::AUTO,
                       BoardVariant cached = BoardVariant::UNKNOWN) {
    return {ov, cached, g45, te, g18};
}

void expect(const BoardDecision& d, BoardVariant variant, BoardSource source) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(variant), static_cast<int>(d.variant));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(source), static_cast<int>(d.source));
}

}  // namespace

void test_v1_bare_board() {
    expect(decide_variant(evidence(1, -1, 0)), BoardVariant::V1, BoardSource::PROBE);
}

void test_v1_hint_not_read() {
    expect(decide_variant(evidence(1, -1, -1)), BoardVariant::V1, BoardSource::PROBE);
}

void test_v2_with_te_edges() {
    expect(decide_variant(evidence(0, 12, 1)), BoardVariant::V2, BoardSource::PROBE);
}

void test_v2_at_min_edges() {
    expect(decide_variant(evidence(0, BOARD_TE_MIN_EDGES, 1)), BoardVariant::V2, BoardSource::PROBE);
}

void test_v2_below_min_edges_is_unknown() {
    expect(decide_variant(evidence(0, BOARD_TE_MIN_EDGES - 1, 1)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_v2_no_edges_is_unknown() {
    expect(decide_variant(evidence(0, 0, 1)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_v2_probe_failed_is_unknown() {
    expect(decide_variant(evidence(0, -1, 1)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_gpio45_not_read_is_unknown() {
    expect(decide_variant(evidence(-1, -1, 0)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_v1_level_but_gpio18_high_is_conflict() {
    expect(decide_variant(evidence(1, -1, 1)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_v2_level_but_gpio18_low_is_conflict() {
    expect(decide_variant(evidence(0, 12, 0)), BoardVariant::UNKNOWN, BoardSource::NONE);
}

void test_v2_te_active_gpio18_not_read_accepted() {
    expect(decide_variant(evidence(0, 12, -1)), BoardVariant::V2, BoardSource::PROBE);
}

void test_conflict_uses_cached_board_without_probe_source() {
    expect(decide_variant(evidence(0, 0, 1, BoardOverride::AUTO, BoardVariant::V2)),
           BoardVariant::V2, BoardSource::CACHED);
    expect(decide_variant(evidence(1, -1, 1, BoardOverride::AUTO, BoardVariant::V1)),
           BoardVariant::V1, BoardSource::CACHED);
}

void test_probe_beats_cache() {
    expect(decide_variant(evidence(1, -1, 0, BoardOverride::AUTO, BoardVariant::V2)),
           BoardVariant::V1, BoardSource::PROBE);
}

void test_override_wins_over_evidence() {
    expect(decide_variant(evidence(1, -1, 0, BoardOverride::FORCE_V2)), BoardVariant::V2, BoardSource::USER);
    expect(decide_variant(evidence(0, 12, 1, BoardOverride::FORCE_V1)), BoardVariant::V1, BoardSource::USER);
    expect(decide_variant(evidence(-1, -1, -1, BoardOverride::FORCE_V1)), BoardVariant::V1, BoardSource::USER);
}

void test_needs_te_probe_only_for_auto_gpio45_low() {
    TEST_ASSERT_TRUE(board_needs_te_probe(evidence(0, -1, 1)));
    TEST_ASSERT_FALSE(board_needs_te_probe(evidence(1, -1, 0)));
    TEST_ASSERT_FALSE(board_needs_te_probe(evidence(-1, -1, 0)));
    TEST_ASSERT_FALSE(board_needs_te_probe(evidence(0, -1, 1, BoardOverride::FORCE_V2)));
}

void test_reason_is_always_set() {
    TEST_ASSERT_NOT_NULL(decide_variant(evidence(0, 0, 0)).reason);
    TEST_ASSERT_NOT_NULL(decide_variant(evidence(1, -1, 0)).reason);
}

void test_v2_te_active_beats_cached_v1() {
    expect(decide_variant(evidence(0, 12, 1, BoardOverride::AUTO, BoardVariant::V1)),
           BoardVariant::V2, BoardSource::PROBE);
}

void test_motor_disabled_without_probe_or_user() {
    const BoardVariant variants[] = {BoardVariant::UNKNOWN, BoardVariant::V1, BoardVariant::V2};
    const BoardSource sources[] = {BoardSource::NONE, BoardSource::CACHED, BoardSource::COMPILE_DEFAULT};
    for (BoardVariant v : variants) {
        for (BoardSource src : sources) {
            const BoardConfig c = board_config_for(v, src);
            TEST_ASSERT_EQUAL_INT(-1, c.motor_pin);
            TEST_ASSERT_EQUAL_INT(-1, c.motor_pin_alt);
            TEST_ASSERT_FALSE(c.motor_enabled());
        }
    }
}

void test_unknown_never_has_motor_even_with_probe_or_user() {
    TEST_ASSERT_EQUAL_INT(-1, board_config_for(BoardVariant::UNKNOWN, BoardSource::PROBE).motor_pin);
    TEST_ASSERT_EQUAL_INT(-1, board_config_for(BoardVariant::UNKNOWN, BoardSource::USER).motor_pin);
}

void test_v1_motor_pins_16_and_18_on_probe_or_user() {
    const BoardSource sources[] = {BoardSource::PROBE, BoardSource::USER};
    for (BoardSource src : sources) {
        const BoardConfig c = board_config_for(BoardVariant::V1, src);
        TEST_ASSERT_EQUAL_INT(16, c.motor_pin);
        TEST_ASSERT_EQUAL_INT(18, c.motor_pin_alt);
        TEST_ASSERT_TRUE(c.motor_enabled());
    }
}

void test_v2_motor_pin_16_only_never_18() {
    const BoardSource sources[] = {BoardSource::PROBE, BoardSource::USER};
    for (BoardSource src : sources) {
        const BoardConfig c = board_config_for(BoardVariant::V2, src);
        TEST_ASSERT_EQUAL_INT(16, c.motor_pin);
        TEST_ASSERT_EQUAL_INT(-1, c.motor_pin_alt);
    }
}

void test_alt_motor_pin_never_without_probe_or_user() {
    const BoardVariant variants[] = {BoardVariant::UNKNOWN, BoardVariant::V1, BoardVariant::V2};
    const BoardSource sources[] = {BoardSource::NONE, BoardSource::CACHED, BoardSource::COMPILE_DEFAULT};
    for (BoardVariant v : variants) {
        for (BoardSource src : sources) {
            TEST_ASSERT_EQUAL_INT(-1, board_config_for(v, src).motor_pin_alt);
        }
    }
}

void test_motor_pin_18_allowed_only_on_v1() {
    TEST_ASSERT_TRUE(board_motor_pin_allowed(BoardVariant::V1, 18));
    TEST_ASSERT_FALSE(board_motor_pin_allowed(BoardVariant::V2, 18));
    TEST_ASSERT_FALSE(board_motor_pin_allowed(BoardVariant::UNKNOWN, 18));
}

void test_motor_pin_16_allowed_on_known_boards_only() {
    TEST_ASSERT_TRUE(board_motor_pin_allowed(BoardVariant::V1, 16));
    TEST_ASSERT_TRUE(board_motor_pin_allowed(BoardVariant::V2, 16));
    TEST_ASSERT_FALSE(board_motor_pin_allowed(BoardVariant::UNKNOWN, 16));
}

void test_motor_pin_rejects_reserved_pins() {
    // HX711 (1,2,3), display (9..14, 21, CS 46), IMU INT (9, 17, 46), touch/IMU I2C (47,48),
    // strapping (0, 3, 45, 46), USB (19,20), UART (43,44), flash/PSRAM (26..37), out of range
    const int reserved[] = {0, 1, 2, 3, 9, 10, 11, 12, 13, 14, 17, 19, 20, 21,
                            26, 30, 33, 37, 43, 44, 45, 46, 47, 48, -1, 49, 255};
    for (int pin : reserved) {
        TEST_ASSERT_FALSE(board_motor_pin_allowed(BoardVariant::V1, pin));
        TEST_ASSERT_FALSE(board_motor_pin_allowed(BoardVariant::V2, pin));
    }
}

void test_board_config_pins_pass_allow_list() {
    const BoardVariant variants[] = {BoardVariant::V1, BoardVariant::V2};
    for (BoardVariant v : variants) {
        const BoardConfig c = board_config_for(v, BoardSource::PROBE);
        TEST_ASSERT_TRUE(board_motor_pin_allowed(v, c.motor_pin));
        if (c.motor_pin_alt >= 0) {
            TEST_ASSERT_TRUE(board_motor_pin_allowed(v, c.motor_pin_alt));
        }
    }
}

void test_loadcell_clock_pair_is_gpio1_and_gpio2_on_all_boards() {
    const BoardVariant variants[] = {BoardVariant::UNKNOWN, BoardVariant::V1, BoardVariant::V2};
    const BoardSource sources[] = {BoardSource::NONE, BoardSource::CACHED, BoardSource::PROBE, BoardSource::USER};
    for (BoardVariant v : variants) {
        for (BoardSource src : sources) {
            const BoardConfig c = board_config_for(v, src);
            TEST_ASSERT_NOT_EQUAL(c.loadcell_sck_pin, c.loadcell_sck_pin_alt);
            TEST_ASSERT_TRUE((c.loadcell_sck_pin == 1 && c.loadcell_sck_pin_alt == 2) ||
                             (c.loadcell_sck_pin == 2 && c.loadcell_sck_pin_alt == 1));
        }
    }
}

void test_default_board_has_no_motor_but_dual_clock() {
    const BoardConfig& c = board();
    TEST_ASSERT_FALSE(c.motor_enabled());
    TEST_ASSERT_FALSE(motor_enabled());
    TEST_ASSERT_EQUAL_INT(-1, c.motor_pin_alt);
    TEST_ASSERT_TRUE(c.loadcell_sck_pin_alt >= 0);
}

void test_board_set_enables_motor_helper() {
    const BoardConfig previous = board();
    board_set(board_config_for(BoardVariant::V2, BoardSource::USER));
    TEST_ASSERT_TRUE(motor_enabled());
    board_set(board_config_for(BoardVariant::V2, BoardSource::CACHED));
    TEST_ASSERT_FALSE(motor_enabled());
    board_set(previous);
}

void setUp() {}
void tearDown() {}

void test_hx711_mask_dual_clock_gpio1_and_gpio2() {
    TEST_ASSERT_EQUAL_HEX32((1UL << 1) | (1UL << 2), hx711_clock_mask(2, 1));
    TEST_ASSERT_EQUAL_HEX32((1UL << 1) | (1UL << 2), hx711_clock_mask(1, 2));
}

void test_hx711_mask_single_clock() {
    TEST_ASSERT_EQUAL_HEX32(1UL << 2, hx711_clock_mask(2, -1));
}

void test_hx711_mask_same_pin_twice_is_single() {
    TEST_ASSERT_EQUAL_HEX32(1UL << 2, hx711_clock_mask(2, 2));
}

void test_hx711_mask_highest_gpio_in_register() {
    TEST_ASSERT_EQUAL_HEX32(0x80000001UL, hx711_clock_mask(0, 31));
}

void test_hx711_mask_pin_outside_register_falls_back() {
    TEST_ASSERT_EQUAL_HEX32(0, hx711_clock_mask(32, -1));
    TEST_ASSERT_EQUAL_HEX32(0, hx711_clock_mask(2, 32));
    TEST_ASSERT_EQUAL_HEX32(0, hx711_clock_mask(-1, 2));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_v1_bare_board);
    RUN_TEST(test_v1_hint_not_read);
    RUN_TEST(test_v2_with_te_edges);
    RUN_TEST(test_v2_at_min_edges);
    RUN_TEST(test_v2_below_min_edges_is_unknown);
    RUN_TEST(test_v2_no_edges_is_unknown);
    RUN_TEST(test_v2_probe_failed_is_unknown);
    RUN_TEST(test_gpio45_not_read_is_unknown);
    RUN_TEST(test_v1_level_but_gpio18_high_is_conflict);
    RUN_TEST(test_v2_level_but_gpio18_low_is_conflict);
    RUN_TEST(test_v2_te_active_gpio18_not_read_accepted);
    RUN_TEST(test_conflict_uses_cached_board_without_probe_source);
    RUN_TEST(test_probe_beats_cache);
    RUN_TEST(test_override_wins_over_evidence);
    RUN_TEST(test_needs_te_probe_only_for_auto_gpio45_low);
    RUN_TEST(test_reason_is_always_set);
    RUN_TEST(test_v2_te_active_beats_cached_v1);
    RUN_TEST(test_motor_disabled_without_probe_or_user);
    RUN_TEST(test_unknown_never_has_motor_even_with_probe_or_user);
    RUN_TEST(test_v1_motor_pins_16_and_18_on_probe_or_user);
    RUN_TEST(test_v2_motor_pin_16_only_never_18);
    RUN_TEST(test_alt_motor_pin_never_without_probe_or_user);
    RUN_TEST(test_motor_pin_18_allowed_only_on_v1);
    RUN_TEST(test_motor_pin_16_allowed_on_known_boards_only);
    RUN_TEST(test_motor_pin_rejects_reserved_pins);
    RUN_TEST(test_board_config_pins_pass_allow_list);
    RUN_TEST(test_loadcell_clock_pair_is_gpio1_and_gpio2_on_all_boards);
    RUN_TEST(test_default_board_has_no_motor_but_dual_clock);
    RUN_TEST(test_board_set_enables_motor_helper);
    RUN_TEST(test_hx711_mask_dual_clock_gpio1_and_gpio2);
    RUN_TEST(test_hx711_mask_single_clock);
    RUN_TEST(test_hx711_mask_same_pin_twice_is_single);
    RUN_TEST(test_hx711_mask_highest_gpio_in_register);
    RUN_TEST(test_hx711_mask_pin_outside_register_falls_back);
    return UNITY_END();
}

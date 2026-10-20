#include <unity.h>
#include "hardware/board_detect.h"

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

void test_v1_motor_pin_18_on_probe_or_user() {
    TEST_ASSERT_EQUAL_INT(18, board_config_for(BoardVariant::V1, BoardSource::PROBE).motor_pin);
    TEST_ASSERT_EQUAL_INT(18, board_config_for(BoardVariant::V1, BoardSource::USER).motor_pin);
}

void test_v2_motor_pin_16_never_18() {
    TEST_ASSERT_EQUAL_INT(16, board_config_for(BoardVariant::V2, BoardSource::PROBE).motor_pin);
    TEST_ASSERT_EQUAL_INT(16, board_config_for(BoardVariant::V2, BoardSource::USER).motor_pin);
    TEST_ASSERT_NOT_EQUAL(18, board_config_for(BoardVariant::V2, BoardSource::PROBE).motor_pin_alt);
}

void setUp() {}
void tearDown() {}

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
    RUN_TEST(test_v1_motor_pin_18_on_probe_or_user);
    RUN_TEST(test_v2_motor_pin_16_never_18);
    return UNITY_END();
}

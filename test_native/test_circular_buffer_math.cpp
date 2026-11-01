// Characterization tests: they pin what CircularBufferMath does today, including quirks.
// Expected values are worked out from the code and the injected clock. Where a test
// documents surprising behaviour it says so, so a later change has to update it on purpose.
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include "hardware/circular_buffer_math/circular_buffer_math.h"

namespace {

uint32_t g_now_ms = 0;
uint32_t fake_clock() { return g_now_ms; }

// Samples at 10 SPS every 100 ms ending at last_ts, constant or alternating by index.
void feed_steady(CircularBufferMath& cbm, int count, uint32_t first_ts, int32_t base, int32_t alt) {
    for (int k = 0; k < count; k++) {
        int32_t value = (alt != 0 && (k % 2) == 1) ? base + alt : base;
        cbm.add_sample(value, first_ts + 100u * static_cast<uint32_t>(k));
    }
}

}  // namespace

// --- Trimmed mean (apply_outlier_rejection via get_smoothed_raw) ---

static void test_trimmed_mean_one_sample_returns_it() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(500, 1000);
    TEST_ASSERT_EQUAL_INT32(500, cbm.get_smoothed_raw(100));
}

static void test_trimmed_mean_two_samples_average_truncates_toward_zero() {
    g_now_ms = 1000;
    CircularBufferMath positive(fake_clock);
    positive.add_sample(10, 1000);
    positive.add_sample(21, 1000);
    TEST_ASSERT_EQUAL_INT32(15, positive.get_smoothed_raw(100));  // 31 / 2

    CircularBufferMath negative(fake_clock);
    negative.add_sample(-3, 1000);
    negative.add_sample(-4, 1000);
    TEST_ASSERT_EQUAL_INT32(-3, negative.get_smoothed_raw(100));  // -7 / 2 in C
}

static void test_trimmed_mean_three_samples_keeps_median() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(10, 1000);
    cbm.add_sample(30, 1000);
    cbm.add_sample(20, 1000);
    TEST_ASSERT_EQUAL_INT32(20, cbm.get_smoothed_raw(100));
}

static void test_trimmed_mean_four_samples_drops_min_and_max() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1, 1000);
    cbm.add_sample(100, 1000);
    cbm.add_sample(3, 1000);
    cbm.add_sample(2, 1000);
    TEST_ASSERT_EQUAL_INT32(2, cbm.get_smoothed_raw(100));  // sorted 1,2,3,100 -> (2+3)/2
}

static void test_trimmed_mean_many_samples_is_order_independent() {
    g_now_ms = 1000;
    CircularBufferMath shuffled(fake_clock);
    const int32_t values[] = {7, 0, 11, 3, 9, 1, 5, 10, 2, 8, 4, 6};  // 0..11 in no order
    for (int32_t v : values) shuffled.add_sample(v, 1000);
    // 300 ms window allows 13 samples; sorted 0..11, trim 0 and 11, mean of 1..10 = 55 / 10
    TEST_ASSERT_EQUAL_INT32(5, shuffled.get_smoothed_raw(300));

    CircularBufferMath outlier(fake_clock);
    outlier.add_sample(0, 1000);
    outlier.add_sample(10, 1000);
    outlier.add_sample(20, 1000);
    outlier.add_sample(30, 1000);
    outlier.add_sample(1000, 1000);
    TEST_ASSERT_EQUAL_INT32(20, outlier.get_smoothed_raw(100));  // (10+20+30) / 3
}

// --- Window boundary ---

static void test_window_start_is_inclusive_and_older_sample_excluded() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1, 899);   // window_start = 1000 - 100 = 900, so this is outside
    cbm.add_sample(2, 900);   // exactly on the boundary: inside
    cbm.add_sample(3, 1000);  // now: inside

    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100));

    int32_t delta = 0;
    uint32_t span = 0;
    int count = 0;
    TEST_ASSERT_TRUE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_INT32(1, delta);   // 3 - 2
    TEST_ASSERT_EQUAL_UINT32(100, span); // 1000 - 900
}

static void test_smoothed_falls_back_to_newest_when_window_is_empty() {
    // CHARACTERIZATION: no sample inside the window, so the smoothed read returns the newest
    // sample however old it is (here 900 ms old), not 0 and not a flag.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(77, 100);
    TEST_ASSERT_EQUAL_INT32(77, cbm.get_smoothed_raw(100));
    TEST_ASSERT_EQUAL_INT32(77, cbm.get_raw_low_latency());
    TEST_ASSERT_EQUAL_INT(0, cbm.get_sample_count_in_window(100));
}

// --- millis() wrap and timestamps ahead of now ---

static void test_age_is_wrap_safe_across_millis_rollover() {
    // Newest sample at UINT32_MAX - 9 is 160 ms old when the clock has wrapped to 150.
    g_now_ms = 150;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(100, UINT32_MAX - 9);
    uint32_t age = 0;
    TEST_ASSERT_TRUE(cbm.get_newest_sample_age_ms(&age));
    TEST_ASSERT_EQUAL_UINT32(160, age);
}

static void test_span_is_wrap_safe_but_window_membership_is_not() {
    // CHARACTERIZATION (surprising): the window start is a plain unsigned compare, so a sample
    // from before the rollover (160 ms old, outside a 100 ms window) is still counted. The span
    // helper does handle the wrap, so the delta spans 150 ms.
    g_now_ms = 150;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(100, UINT32_MAX - 9);
    cbm.add_sample(200, 140);

    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100));

    int32_t delta = 0;
    uint32_t span = 0;
    int count = 0;
    TEST_ASSERT_TRUE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_INT32(100, delta);   // 200 - 100
    TEST_ASSERT_EQUAL_UINT32(150, span);   // 140 - (UINT32_MAX - 9) across the wrap
}

static void test_window_longer_than_clock_excludes_all_samples() {
    // CHARACTERIZATION (surprising): while now < window_ms (first window after boot or after a
    // rollover) the window start wraps to a huge value and every real sample is excluded.
    g_now_ms = 50;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(9, 40);
    cbm.add_sample(11, 50);

    TEST_ASSERT_EQUAL_INT(0, cbm.get_sample_count_in_window(100));

    int32_t delta = 99;
    uint32_t span = 99;
    int count = 99;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_EQUAL_INT32(11, cbm.get_smoothed_raw(100));  // newest-sample fallback
}

static void test_timestamp_ahead_of_now_reports_zero_age_and_counts_in_window() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(42, 1005);

    uint32_t age = 99;
    TEST_ASSERT_TRUE(cbm.get_newest_sample_age_ms(&age));
    TEST_ASSERT_EQUAL_UINT32(0, age);
    TEST_ASSERT_EQUAL_INT(1, cbm.get_sample_count_in_window(100));
    TEST_ASSERT_EQUAL_INT32(42, cbm.get_instant_raw());
}

// --- Statistics ---

static void test_standard_deviation_uses_n_minus_1() {
    // Values 2,4,4,4,5,5,7,9: mean 5, sum of squared deviations 32.
    // Sample (n-1) form gives sqrt(32/7) = 2.138; population form would give 2.0.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    const int32_t values[] = {2, 4, 4, 4, 5, 5, 7, 9};
    for (int32_t v : values) cbm.add_sample(v, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, static_cast<float>(sqrt(32.0 / 7.0)),
                             cbm.get_standard_deviation_raw(300));
}

static void test_standard_deviation_of_single_sample_is_zero() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(123, 1000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_standard_deviation_raw(300));
}

static void test_flow_rate_is_positive_for_rising_and_negative_for_falling() {
    g_now_ms = 1000;

    CircularBufferMath rising(fake_clock);
    rising.add_sample(1000, 900);
    rising.add_sample(1100, 1000);
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, rising.get_raw_flow_rate(200));  // +100 raw over 100 ms

    CircularBufferMath falling(fake_clock);
    falling.add_sample(1100, 900);
    falling.add_sample(1000, 1000);
    TEST_ASSERT_EQUAL_FLOAT(-1000.0f, falling.get_raw_flow_rate(200));
}

static void test_flow_rate_with_equal_timestamps_is_zero() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1000, 1000);
    cbm.add_sample(1100, 1000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate(200));
}

static void test_flow_95th_percentile_falls_back_under_10_samples() {
    // Five samples 100 ms apart, rising 100 raw per sample. Under 10 samples the percentile
    // path is skipped and the plain flow rate over the 200 ms window is returned (1000/s).
    g_now_ms = 1500;
    CircularBufferMath cbm(fake_clock);
    for (int32_t k = 0; k < 5; k++) {
        cbm.add_sample(k * 100, static_cast<uint32_t>(1000 + 100 * k));
    }
    TEST_ASSERT_EQUAL_FLOAT(cbm.get_raw_flow_rate(200), cbm.get_raw_flow_rate_95th_percentile(200));
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, cbm.get_raw_flow_rate_95th_percentile(200));
}

static void test_flow_95th_percentile_falls_back_when_no_sub_window_is_valid() {
    // Nine samples share ts 1000 and one sits at ts 1500. The 1000 ms effective window gives
    // eight 300 ms sub-windows; none holds a non-zero time span, so no valid rate is collected
    // and the plain flow rate is used: (50 - 0) over 500 ms = 100/s.
    g_now_ms = 1500;
    CircularBufferMath cbm(fake_clock);
    for (int k = 0; k < 9; k++) cbm.add_sample(0, 1000);
    cbm.add_sample(50, 1500);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cbm.get_raw_flow_rate_95th_percentile(200));
}

// --- is_settled ---

static void test_is_settled_false_on_empty_buffer() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    TEST_ASSERT_FALSE(cbm.is_settled(300, 1000));
}

static void test_is_settled_stale_buffer_is_not_settled_until_fresh() {
    // Window 2000 ms needs 12 samples. 14 constant samples from 1000 to 2300 ms.
    // At now = 3000 the newest is 700 ms old (stale). At now = 2800 it is 500 ms old, which is
    // still fresh (stale means strictly older than 500 ms) and the window holds all 14 samples.
    g_now_ms = 3000;
    CircularBufferMath cbm(fake_clock);
    feed_steady(cbm, 14, 1000, 1000, 0);
    TEST_ASSERT_FALSE(cbm.is_settled(2000, 1000));

    g_now_ms = 2800;
    TEST_ASSERT_TRUE(cbm.is_settled(2000, 1000));
}

static void test_is_settled_sparse_window_is_not_settled() {
    // 11 fresh constant samples: one short of the 12 a 2000 ms window needs.
    g_now_ms = 2000;
    CircularBufferMath cbm(fake_clock);
    feed_steady(cbm, 11, 1000, 1000, 0);
    TEST_ASSERT_FALSE(cbm.is_settled(2000, 1000));
}

static void test_is_settled_respects_threshold_on_settled_buffer() {
    // 14 fresh samples alternating 1000 / 1100: sample std dev = sqrt(35000 / 13) = 51.9.
    g_now_ms = 2800;
    CircularBufferMath cbm(fake_clock);
    feed_steady(cbm, 14, 1000, 1000, 100);
    TEST_ASSERT_FALSE(cbm.is_settled(2000, 10));
    TEST_ASSERT_TRUE(cbm.is_settled(2000, 1000));
}

// --- get_window_delta with fewer than two samples ---

static void test_window_delta_with_no_samples_clears_outputs() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    int32_t delta = 99;
    uint32_t span = 77;
    int count = 55;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT32(0, delta);
    TEST_ASSERT_EQUAL_UINT32(0, span);
    TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_window_delta_with_one_sample_reports_zero_count() {
    // CHARACTERIZATION (surprising): one buffered sample inside the window still reports a count
    // of 0, because the samples_count < 2 early exit runs before the window is walked.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(5, 1000);
    int32_t delta = 99;
    uint32_t span = 77;
    int count = 55;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT32(0, delta);
    TEST_ASSERT_EQUAL_UINT32(0, span);
    TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_window_delta_null_delta_returns_false_and_clears_count() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(5, 1000);
    cbm.add_sample(6, 1000);
    int count = 55;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100, nullptr, nullptr, &count));
    TEST_ASSERT_EQUAL_INT(0, count);
}

// --- Ring buffer wrap past capacity (1024 samples) ---

static void test_ring_wrap_keeps_newest_1024_samples() {
    // 1029 samples written at 1000 + i ms with value i. Only i = 5..1028 survive.
    g_now_ms = 2100;
    CircularBufferMath cbm(fake_clock);
    for (int32_t i = 0; i < 1029; i++) {
        cbm.add_sample(i, static_cast<uint32_t>(1000 + i));
    }

    TEST_ASSERT_EQUAL_UINT16(1024, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT32(1028, cbm.get_instant_raw());
    TEST_ASSERT_EQUAL_UINT32(1023, cbm.get_buffer_time_span_ms());  // 2028 - 1005
    TEST_ASSERT_EQUAL_INT(1024, cbm.get_sample_count_in_window(1200));

    int32_t delta = 0;
    uint32_t span = 0;
    int count = 0;
    TEST_ASSERT_TRUE(cbm.get_window_delta(1200, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(1024, count);
    TEST_ASSERT_EQUAL_INT32(1023, delta);   // 1028 - 5
    TEST_ASSERT_EQUAL_UINT32(1023, span);
}

// --- Empty buffer and clear_all_samples ---

static void test_empty_buffer_readers_return_zero() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    TEST_ASSERT_EQUAL_UINT16(0, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_instant_raw());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_smoothed_raw(100));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_raw_low_latency());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_raw_high_latency());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_min_raw(300));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_max_raw(300));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_standard_deviation_raw(300));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate(200));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate_95th_percentile(200));
    TEST_ASSERT_EQUAL_UINT32(0, cbm.get_buffer_time_span_ms());
}

static void test_empty_buffer_reports_full_confidence_and_stable_flow() {
    // CHARACTERIZATION (surprising): with no data the confidence is 1.0 (zero deviation) and the
    // flow counts as stable (0 is within 10% of 0). is_settled() is still false.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, cbm.get_settling_confidence(300));
    TEST_ASSERT_TRUE(cbm.raw_flowrate_is_stable(100));
    TEST_ASSERT_FALSE(cbm.is_settled(300, 1000));
}

static void test_clear_all_samples_then_reads_are_empty() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(100, 990);
    cbm.add_sample(200, 995);
    cbm.add_sample(300, 1000);
    cbm.clear_all_samples();

    TEST_ASSERT_EQUAL_UINT16(0, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_instant_raw());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_smoothed_raw(300));
    TEST_ASSERT_EQUAL_INT(0, cbm.get_sample_count_in_window(300));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_max_raw(300));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_min_raw(300));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_standard_deviation_raw(300));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate(200));
    TEST_ASSERT_FALSE(cbm.is_settled(300, 1000));
    uint32_t age = 99;
    TEST_ASSERT_FALSE(cbm.get_newest_sample_age_ms(&age));
    TEST_ASSERT_EQUAL_UINT32(0, cbm.get_buffer_time_span_ms());

    // Writes after a clear start a fresh buffer: only the new sample is visible.
    cbm.add_sample(7, 1000);
    TEST_ASSERT_EQUAL_UINT16(1, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT32(7, cbm.get_instant_raw());
    TEST_ASSERT_EQUAL_INT32(7, cbm.get_max_raw(300));
    TEST_ASSERT_EQUAL_INT32(7, cbm.get_min_raw(300));
}

// --- Min / max, window boundary ---

// CHARACTERIZATION (latent, deliberately not triggered): calculate_max_samples_for_window() sizes
// the alloca at window_ms * SPS / 1000 + 10 (capped by samples_count), but get_samples_in_window()
// collects every in-window sample with no bound. E.g. 12 samples at the same timestamp with a
// 100 ms window write 12 ints into an 11-slot buffer. Not reachable at 10 SPS in firmware. The
// tests in this file stay within the cap on purpose; step 2 replaces the alloca with a 64-sample bound.

static void test_min_max_use_only_in_window_samples() {
    // Unordered window: 5, -3, 9, 0 at 1000 ms. Older 99 at 899 ms is outside a 100 ms window.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(99, 899);
    cbm.add_sample(5, 1000);
    cbm.add_sample(-3, 1000);
    cbm.add_sample(9, 1000);
    cbm.add_sample(0, 1000);
    TEST_ASSERT_EQUAL_INT32(-3, cbm.get_min_raw(100));  // negatives handled
    TEST_ASSERT_EQUAL_INT32(9, cbm.get_max_raw(100));   // 99 excluded
}

static void test_window_boundary_sample_included_and_older_excluded() {
    // window_start = 1000 - 100 = 900. -500 at 899 is one ms too old, 40 at 900 is exactly on the
    // boundary, 60 at 1000 is now. Min 40 proves both: -500 is excluded, and 40 is not dropped.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(-500, 899);
    cbm.add_sample(40, 900);
    cbm.add_sample(60, 1000);
    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100));
    TEST_ASSERT_EQUAL_INT32(40, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(60, cbm.get_max_raw(100));
}

// --- Display filter ---

static void test_display_raw_empty_init_then_small_rise_stays_in_deadband() {
    // The first call on an empty buffer initialises the filter at 0. A later 50 is inside the
    // 100 raw deadband, so the display stays at 0.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
    cbm.add_sample(50, 1000);
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
}

static void test_display_raw_rise_immediate_fall_decays_by_alpha() {
    // Rise: 0 -> 500 is taken at once. Fall: 500 -> 0 uses alpha 0.9 (SYS_DISPLAY_FILTER_ALPHA_DOWN)
    // on the current value: 0.9 * 0 + 0.1 * 500 = 50. A further 50 -> 0 is inside the deadband.
    CircularBufferMath cbm(fake_clock);
    g_now_ms = 1000;
    cbm.add_sample(0, 1000);
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());

    g_now_ms = 2000;  // window 300 ms: only the sample at 2000 is inside
    cbm.add_sample(500, 2000);
    TEST_ASSERT_EQUAL_INT32(500, cbm.get_display_raw());

    g_now_ms = 3000;
    cbm.add_sample(0, 3000);
    TEST_ASSERT_EQUAL_INT32(50, cbm.get_display_raw());

    g_now_ms = 4000;
    cbm.add_sample(0, 4000);
    TEST_ASSERT_EQUAL_INT32(50, cbm.get_display_raw());
}

// --- 95th percentile happy path ---

static void test_flow_95th_percentile_steady_ramp_equals_flow_rate() {
    // 20 samples every 100 ms, value 200 per sample (2 raw/ms = 2000 raw/s), now = 2900 ms.
    // Eight 300 ms sub-windows each hold 4 samples, every one at 2000 raw/s.
    g_now_ms = 2900;
    CircularBufferMath cbm(fake_clock);
    for (int32_t k = 0; k < 20; k++) {
        cbm.add_sample(200 * k, static_cast<uint32_t>(1000 + 100 * k));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2000.0f, cbm.get_raw_flow_rate_95th_percentile(1000));
}

static void test_flow_95th_percentile_picks_index_floor_95_percent() {
    // Same ramp, but the newest sample (2900 ms) is 1000 higher. Only the sub-window ending at
    // 2900 sees it: (4800 - 3200) over 300 ms = 5333.3/s. The other seven stay at 2000/s.
    // Eight valid rates sorted: seven 2000s then 5333.3. Index int(8 * 0.95) = 7 selects the max.
    g_now_ms = 2900;
    CircularBufferMath cbm(fake_clock);
    for (int32_t k = 0; k < 20; k++) {
        int32_t value = 200 * k + (k == 19 ? 1000 : 0);
        cbm.add_sample(value, static_cast<uint32_t>(1000 + 100 * k));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5333.333f, cbm.get_raw_flow_rate_95th_percentile(1000));
}

// --- Settling confidence and flow stability with real data ---

static void test_settling_confidence_with_real_spread() {
    // Same 14 alternating samples as the is_settled test: sample std dev = sqrt(35000 / 13) = 51.89.
    // Confidence = 1 - 51.89 / 1000 = 0.948.
    g_now_ms = 2800;
    CircularBufferMath cbm(fake_clock);
    feed_steady(cbm, 14, 1000, 1000, 100);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9481f, cbm.get_settling_confidence(2000));
}

static void test_flowrate_stable_when_recent_slope_matches() {
    // Ramp of 1 raw/ms: 1000/s over 200 ms and over the last 100 ms. Difference 0 <= 10% of 1000.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    const uint32_t ts[] = {800, 850, 900, 950, 1000};
    for (uint32_t t : ts) cbm.add_sample(static_cast<int32_t>(t), t);
    TEST_ASSERT_TRUE(cbm.raw_flowrate_is_stable(200));
}

static void test_flowrate_unstable_when_recent_slope_jumps() {
    // Over 200 ms: (300 - 0) / 200 = 1500/s. Over the last 100 ms: (300 - 100) / 100 = 2000/s.
    // The gap of 500 is more than 10% of 1500 (150), so unstable.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(0, 800);
    cbm.add_sample(50, 850);
    cbm.add_sample(100, 900);
    cbm.add_sample(150, 950);
    cbm.add_sample(300, 1000);
    TEST_ASSERT_FALSE(cbm.raw_flowrate_is_stable(200));
}

void run_circular_buffer_math_tests() {
    RUN_TEST(test_min_max_use_only_in_window_samples);
    RUN_TEST(test_window_boundary_sample_included_and_older_excluded);
    RUN_TEST(test_display_raw_empty_init_then_small_rise_stays_in_deadband);
    RUN_TEST(test_display_raw_rise_immediate_fall_decays_by_alpha);
    RUN_TEST(test_flow_95th_percentile_steady_ramp_equals_flow_rate);
    RUN_TEST(test_flow_95th_percentile_picks_index_floor_95_percent);
    RUN_TEST(test_settling_confidence_with_real_spread);
    RUN_TEST(test_flowrate_stable_when_recent_slope_matches);
    RUN_TEST(test_flowrate_unstable_when_recent_slope_jumps);
    RUN_TEST(test_trimmed_mean_one_sample_returns_it);
    RUN_TEST(test_trimmed_mean_two_samples_average_truncates_toward_zero);
    RUN_TEST(test_trimmed_mean_three_samples_keeps_median);
    RUN_TEST(test_trimmed_mean_four_samples_drops_min_and_max);
    RUN_TEST(test_trimmed_mean_many_samples_is_order_independent);
    RUN_TEST(test_window_start_is_inclusive_and_older_sample_excluded);
    RUN_TEST(test_smoothed_falls_back_to_newest_when_window_is_empty);
    RUN_TEST(test_age_is_wrap_safe_across_millis_rollover);
    RUN_TEST(test_span_is_wrap_safe_but_window_membership_is_not);
    RUN_TEST(test_window_longer_than_clock_excludes_all_samples);
    RUN_TEST(test_timestamp_ahead_of_now_reports_zero_age_and_counts_in_window);
    RUN_TEST(test_standard_deviation_uses_n_minus_1);
    RUN_TEST(test_standard_deviation_of_single_sample_is_zero);
    RUN_TEST(test_flow_rate_is_positive_for_rising_and_negative_for_falling);
    RUN_TEST(test_flow_rate_with_equal_timestamps_is_zero);
    RUN_TEST(test_flow_95th_percentile_falls_back_under_10_samples);
    RUN_TEST(test_flow_95th_percentile_falls_back_when_no_sub_window_is_valid);
    RUN_TEST(test_is_settled_false_on_empty_buffer);
    RUN_TEST(test_is_settled_stale_buffer_is_not_settled_until_fresh);
    RUN_TEST(test_is_settled_sparse_window_is_not_settled);
    RUN_TEST(test_is_settled_respects_threshold_on_settled_buffer);
    RUN_TEST(test_window_delta_with_no_samples_clears_outputs);
    RUN_TEST(test_window_delta_with_one_sample_reports_zero_count);
    RUN_TEST(test_window_delta_null_delta_returns_false_and_clears_count);
    RUN_TEST(test_ring_wrap_keeps_newest_1024_samples);
    RUN_TEST(test_empty_buffer_readers_return_zero);
    RUN_TEST(test_empty_buffer_reports_full_confidence_and_stable_flow);
    RUN_TEST(test_clear_all_samples_then_reads_are_empty);
}

// Characterization tests: they pin what CircularBufferMath does today, including quirks.
// Expected values are worked out from the code and the injected clock. Where a test
// documents surprising behaviour it says so, so a later change has to update it on purpose.
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include "config/hardware.h"
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

static void test_window_membership_is_wrap_safe_across_rollover() {
    // CHANGED in step 2 (was a characterization of the bug): membership now uses the wrap-safe age,
    // so the sample from before the rollover (160 ms old) is outside a 100 ms window but inside a
    // 200 ms window. Step 1 counted it in both.
    g_now_ms = 150;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(100, UINT32_MAX - 9);
    cbm.add_sample(200, 140);

    TEST_ASSERT_EQUAL_INT(1, cbm.get_sample_count_in_window(100));
    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(200));

    int32_t delta = 0;
    uint32_t span = 0;
    int count = 0;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100, &delta, &span, &count));  // only one sample in window
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_TRUE(cbm.get_window_delta(200, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_INT32(100, delta);   // 200 - 100
    TEST_ASSERT_EQUAL_UINT32(150, span);   // 140 - (UINT32_MAX - 9) across the wrap

    // Min/max, flow and smoothed use the same membership: the 100 ms window holds only the 200.
    TEST_ASSERT_EQUAL_INT32(200, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(200, cbm.get_smoothed_raw(100));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate(100));  // one sample: no rate
    // 200 ms window: (200 - 100) over 150 ms = 666.67/s
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 666.667f, cbm.get_raw_flow_rate(200));
}

static void test_window_longer_than_clock_now_includes_samples() {
    // CHANGED in step 2 (FIX): while now < window_ms (first minutes after boot or a rollover) step 1
    // excluded every real sample (window start wrapped to a huge value). Age-based membership
    // includes them: ages are 10 and 0, both within 100 ms.
    g_now_ms = 50;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(9, 40);
    cbm.add_sample(11, 50);

    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100));

    int32_t delta = 99;
    uint32_t span = 99;
    int count = 99;
    TEST_ASSERT_TRUE(cbm.get_window_delta(100, &delta, &span, &count));
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_INT32(2, delta);     // 11 - 9
    TEST_ASSERT_EQUAL_UINT32(10, span);    // 50 - 40
    TEST_ASSERT_EQUAL_INT32(10, cbm.get_smoothed_raw(100));  // two samples: (9 + 11) / 2
    TEST_ASSERT_EQUAL_INT32(9, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(11, cbm.get_max_raw(100));
}

static void test_clock_at_zero_includes_sample_stamped_zero() {
    g_now_ms = 0;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(5, 0);
    TEST_ASSERT_EQUAL_INT(1, cbm.get_sample_count_in_window(300));
    TEST_ASSERT_EQUAL_INT32(5, cbm.get_max_raw(300));
}

static void test_timestamp_ahead_of_now_is_in_every_window_reader() {
    // Age 0 for a timestamp ahead of now: in the window for count, min/max and the sample copy.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(10, 950);
    cbm.add_sample(30, 1005);
    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100));
    TEST_ASSERT_EQUAL_INT32(10, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(30, cbm.get_max_raw(100));
    int32_t out[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(2, cbm.get_samples_in_window(100, out, 4));
    TEST_ASSERT_EQUAL_INT32(30, out[0]);  // newest first
    TEST_ASSERT_EQUAL_INT32(10, out[1]);
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

// CHANGED in step 2: the latent alloca overrun (12 samples at one timestamp into an 11-slot buffer)
// is gone. Collection is bounded by an explicit capacity (CBM_MAX_WINDOW_SAMPLES = 64, newest
// kept); see the bounded-window tests below.

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

static void test_display_raw_empty_does_not_latch_filter_at_zero() {
    // CHANGED in step 2: step 1 initialised the filter at 0 on an empty buffer, so a first sample
    // of 50 stayed inside the 100 raw deadband and displayed 0. Now an empty read returns 0 without
    // initialising, and the first real sample initialises the filter (50).
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
    cbm.add_sample(50, 1000);
    TEST_ASSERT_EQUAL_INT32(50, cbm.get_display_raw());
    // A small move afterwards is inside the deadband and ignored: the filter is live.
    cbm.add_sample(120, 1100);
    g_now_ms = 1100;
    TEST_ASSERT_EQUAL_INT32(50, cbm.get_display_raw());  // window 300: samples 50,120 -> (50+120)/2 = 85, |85-50| < 100
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

// --- Bounded windows (step 2) ---

namespace {

// 100 samples at 200 SPS (5 ms apart), value = index, ts 1000..1495. Every one is inside a
// 500 ms window at now = 1495 (oldest age 495).
void feed_200_sps(CircularBufferMath& cbm) {
    g_now_ms = 1495;
    for (int32_t i = 0; i < 100; i++) {
        cbm.add_sample(i, static_cast<uint32_t>(1000 + 5 * i));
    }
}

const int32_t kCanary = 0x7EADBEEF;

}  // namespace

static void test_collect_never_writes_past_capacity_and_keeps_newest() {
    // The window holds 100 samples; the copy is capped at 64 (newest first) and 8 canary slots
    // after the array stay untouched. A smaller capacity behaves the same way.
    CircularBufferMath cbm(fake_clock);
    feed_200_sps(cbm);
    TEST_ASSERT_EQUAL_INT(100, cbm.get_sample_count_in_window(500));

    int32_t buf[CBM_MAX_WINDOW_SAMPLES + 8];
    for (int32_t& v : buf) v = kCanary;
    TEST_ASSERT_EQUAL_INT(CBM_MAX_WINDOW_SAMPLES, cbm.get_samples_in_window(500, buf, CBM_MAX_WINDOW_SAMPLES));
    TEST_ASSERT_EQUAL_INT32(99, buf[0]);   // newest
    TEST_ASSERT_EQUAL_INT32(36, buf[CBM_MAX_WINDOW_SAMPLES - 1]);  // 64th newest: 99 - 63
    for (int k = CBM_MAX_WINDOW_SAMPLES; k < CBM_MAX_WINDOW_SAMPLES + 8; k++) {
        TEST_ASSERT_EQUAL_INT32(kCanary, buf[k]);
    }

    for (int32_t& v : buf) v = kCanary;
    TEST_ASSERT_EQUAL_INT(10, cbm.get_samples_in_window(500, buf, 10));
    TEST_ASSERT_EQUAL_INT32(99, buf[0]);
    TEST_ASSERT_EQUAL_INT32(90, buf[9]);
    TEST_ASSERT_EQUAL_INT32(kCanary, buf[10]);

    TEST_ASSERT_EQUAL_INT(0, cbm.get_samples_in_window(500, buf, 0));  // zero capacity writes nothing
}

static void test_capped_readers_use_newest_64_but_min_max_and_flow_are_unbounded() {
    // Smoothed and std dev see only values 36..99 (newest 64).
    //  - smoothed: drop 36 and 99, mean of 37..98 = 4185 / 62 = 67 (67.5 truncated)
    //  - std dev of 64 consecutive integers: sqrt(64 * 65 / 12) = 18.619
    // Min/max walk all 100 samples in place (0 and 99), and the plain flow rate uses the newest
    // and oldest in-window samples: 99 raw over 495 ms = 200/s.
    CircularBufferMath cbm(fake_clock);
    feed_200_sps(cbm);
    TEST_ASSERT_EQUAL_INT32(67, cbm.get_smoothed_raw(500));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, static_cast<float>(sqrt(64.0 * 65.0 / 12.0)),
                             cbm.get_standard_deviation_raw(500));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_min_raw(500));
    TEST_ASSERT_EQUAL_INT32(99, cbm.get_max_raw(500));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, cbm.get_raw_flow_rate(500));
}

static void test_percentile_with_200_sps_stays_in_bounds() {
    // Effective window 1000 ms holds all 100 samples; collection stops at the newest 64 (ts 1180..1495).
    // Sub-windows 0..3 reach back far enough to hold 3+ of them and each is a 200/s ramp; sub-windows
    // 4..7 start older than the oldest kept sample and are invalid. Four valid rates, index
    // int(4 * 0.95) = 3, all 200.
    CircularBufferMath cbm(fake_clock);
    feed_200_sps(cbm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, cbm.get_raw_flow_rate_95th_percentile(500));
}

static void test_min_max_single_pass_matches_separate_calls() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(-500, 899);  // age 101: outside
    cbm.add_sample(40, 900);    // age 100: on the boundary, inside
    cbm.add_sample(-7, 950);
    cbm.add_sample(60, 1000);

    int32_t lo = 99, hi = 99;
    TEST_ASSERT_TRUE(cbm.get_min_max_raw(100, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(-7, lo);
    TEST_ASSERT_EQUAL_INT32(60, hi);
    TEST_ASSERT_EQUAL_INT32(lo, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(hi, cbm.get_max_raw(100));

    // Nothing in the window: false, outputs 0
    g_now_ms = 5000;
    lo = 99;
    hi = 99;
    TEST_ASSERT_FALSE(cbm.get_min_max_raw(100, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(0, lo);
    TEST_ASSERT_EQUAL_INT32(0, hi);
}

static void test_min_max_after_ring_wrap_and_window_longer_than_buffer() {
    // 1029 samples (value i at 1000 + i): survivors are 5..1028. A huge window is limited by the
    // buffer, not by the window (documented ~100 s limit at 10 SPS): min 5, max 1028.
    g_now_ms = 2100;
    CircularBufferMath cbm(fake_clock);
    for (int32_t i = 0; i < 1029; i++) {
        cbm.add_sample(i, static_cast<uint32_t>(1000 + i));
    }
    int32_t lo = 0, hi = 0;
    TEST_ASSERT_TRUE(cbm.get_min_max_raw(300000, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(5, lo);
    TEST_ASSERT_EQUAL_INT32(1028, hi);

    // Narrow window across the wrap point: newest 11 samples (ages 0..10) = values 1018..1028
    g_now_ms = 2028;
    TEST_ASSERT_TRUE(cbm.get_min_max_raw(10, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(1018, lo);
    TEST_ASSERT_EQUAL_INT32(1028, hi);
}

// --- Residual characterization gaps ---

static void run_irregular_ramp(CircularBufferMath& cbm, uint32_t first_ts) {
    // Oldest to newest. After the first sample the ramp is 1 raw/ms until 2800, then jumps
    // 1000 raw in the last 200 ms.
    cbm.add_sample(0, first_ts);
    for (int32_t k = 1; k <= 8; k++) {
        cbm.add_sample(100 * k, static_cast<uint32_t>(2000 + 100 * k));
    }
    cbm.add_sample(1800, 3000);
}

static void test_percentile_collection_boundary_with_irregular_ramp() {
    // now = 3000, window 1000. Newest first: 3000:1800, 2800:800, 2700:700 ... 2100:100, 2000:0.
    // Sample at 2000 has age 1000 (on the boundary): kept, 10 samples collected, percentile path.
    //  Sub-window i holds ages [100 i, 100 i + 300]. i = 0: ts 3000, 2800, 2700 -> 1100 raw / 300 ms
    //  = 3666.67/s. i = 1 starts at age 100, so it skips the sample at 3000 and holds 2800..2600
    //  -> 1000/s (this is what a missing step would break). i = 2..7 are all 1000/s.
    //  Eight rates sorted: seven 1000s then 3666.67; index int(8 * 0.95) = 7 picks 3666.67.
    g_now_ms = 3000;
    CircularBufferMath on_boundary(fake_clock);
    run_irregular_ramp(on_boundary, 2000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3666.667f, on_boundary.get_raw_flow_rate_95th_percentile(1000));

    // Same data with the oldest sample at 1999 (age 1001): only 9 collected, so the percentile
    // falls back to the plain flow rate over 1000 ms: (1800 - 100) over 900 ms = 1888.89/s.
    CircularBufferMath just_outside(fake_clock);
    run_irregular_ramp(just_outside, 1999);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1888.889f, just_outside.get_raw_flow_rate_95th_percentile(1000));
}

static void test_percentile_sub_window_needs_three_samples() {
    // now = 3000, window 1000, ten samples (ts:value, ages 1000..0): 2000:-4000 (outlier), 2100:100,
    // 2350:350, 2400:400, 2500:500, 2600:600, 2700:700, 2800:800, 2900:900, 3000:1000 (value = ts - 2000,
    // a 1000/s ramp, except the outlier). Sub-windows 0..6 each hold 3+ samples on the ramp: 1000/s.
    // Sub-window 7 (ages 700..1000) holds only 2100 and 2000: two samples, below the minimum of 3, so
    // it is dropped even though its rate would be 4100 raw / 100 ms = 41000/s. Seven valid rates, all
    // 1000, index int(7 * 0.95) = 6 -> 1000. Without the minimum the result would be 41000.
    g_now_ms = 3000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(-4000, 2000);
    cbm.add_sample(100, 2100);
    cbm.add_sample(350, 2350);
    for (int32_t v = 400; v <= 1000; v += 100) cbm.add_sample(v, static_cast<uint32_t>(2000 + v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1000.0f, cbm.get_raw_flow_rate_95th_percentile(1000));
}

static void test_flow_stability_threshold_small_nonzero_difference() {
    // now = 1000. Window 200 ms flow = (200 - 0) / 200 ms = 1000/s. Last 100 ms flow = (200 - v) / 100 ms.
    // Threshold is 10% of 1000 = 100. v = 91 -> 1090/s, difference 90: stable.
    // v = 89 -> 1110/s, difference 110: unstable.
    g_now_ms = 1000;
    CircularBufferMath near_stable(fake_clock);
    near_stable.add_sample(0, 800);
    near_stable.add_sample(91, 900);
    near_stable.add_sample(200, 1000);
    TEST_ASSERT_TRUE(near_stable.raw_flowrate_is_stable(200));

    CircularBufferMath near_unstable(fake_clock);
    near_unstable.add_sample(0, 800);
    near_unstable.add_sample(89, 900);
    near_unstable.add_sample(200, 1000);
    TEST_ASSERT_FALSE(near_unstable.raw_flowrate_is_stable(200));
}

static void test_is_settled_threshold_between_std_dev_neighbours() {
    // Alternating 1000 / 1100, 14 samples: sample std dev = sqrt(35000 / 13) = 51.89.
    // settle_check is std_dev <= threshold: 51 fails, 52 passes.
    g_now_ms = 2800;
    CircularBufferMath cbm(fake_clock);
    feed_steady(cbm, 14, 1000, 1000, 100);
    TEST_ASSERT_FALSE(cbm.is_settled(2000, 51));
    TEST_ASSERT_TRUE(cbm.is_settled(2000, 52));
}

// --- Step 3: leftovers from the step 2 audit ---

static void test_min_max_all_negative_values() {
    // The first in-window sample seeds both min and max; a "start from 0" bug would report max 0.
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(-50, 900);
    cbm.add_sample(-10, 950);
    cbm.add_sample(-300, 1000);
    int32_t lo = 99, hi = 99;
    TEST_ASSERT_TRUE(cbm.get_min_max_raw(100, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(-300, lo);
    TEST_ASSERT_EQUAL_INT32(-10, hi);
    TEST_ASSERT_EQUAL_INT32(-300, cbm.get_min_raw(100));
    TEST_ASSERT_EQUAL_INT32(-10, cbm.get_max_raw(100));
}

static void test_percentile_pins_sub_window_step_spacing() {
    // Values k^2 at ts 1000 + 100 k (k = 0..30), now = 4000, window 3000 (31 samples, under the cap).
    // Sub-windows: 1 + (3000 - 300) / 100 = 28, sub-window i spans ages [100 i, 100 i + 300] = 4
    // samples k = 27-i .. 30-i, rate (k_new^2 - k_old^2) / 300 ms. Rates rise as i falls, so the
    // sorted array ends with i = 0 and index int(28 * 0.95) = 26 is i = 1:
    // (29^2 - 26^2) = 165 raw / 300 ms = 550/s. A different STEP changes the sub-window count and
    // therefore both the index and the picked sub-window.
    g_now_ms = 4000;
    CircularBufferMath cbm(fake_clock);
    for (int32_t k = 0; k <= 30; k++) {
        cbm.add_sample(k * k, static_cast<uint32_t>(1000 + 100 * k));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 550.0f, cbm.get_raw_flow_rate_95th_percentile(3000));
}

static void test_percentile_pins_the_64_sample_cap() {
    // 100 samples at 200 SPS (5 ms), value 400 k - 2 k^2 (rate falls with age), ts 1000 + 5 k,
    // now = 1495, window 1000 (all 100 in the window). Collection keeps the newest 64: ages 0..315.
    // Valid sub-windows are i = 0..3 (i = 3 holds ages 300..315, k = 39..36); i >= 4 start beyond
    // the kept samples. Rates: i0 24800, i1 34000, i2 42000, i3 (750 raw / 15 ms) 50000/s;
    // index int(4 * 0.95) = 3 -> 50000. Without the cap sub-window 4 (k = 19..0, 6878 raw / 95 ms
    // = 72400/s) would be valid and win.
    g_now_ms = 1495;
    CircularBufferMath cbm(fake_clock);
    for (int32_t k = 0; k < 100; k++) {
        cbm.add_sample(400 * k - 2 * k * k, static_cast<uint32_t>(1000 + 5 * k));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 50000.0f, cbm.get_raw_flow_rate_95th_percentile(1000));
}

// --- Step 3: locking and the writer-owned display filter ---
// These run against the no-op lock shim (host builds are single-threaded), so they pin the
// single-threaded behaviour of the locked paths only. They cannot prove thread safety; that needs
// the firmware's portMUX spinlock and a hardware run.

static void test_display_value_changes_only_through_add_sample() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1000, 1000);
    TEST_ASSERT_EQUAL_INT32(1000, cbm.get_display_raw());

    // Time passes and every reader runs; with no new sample the display value must not move,
    // even though the sample is long outside the 300 ms window.
    g_now_ms = 5000;
    cbm.get_smoothed_raw(300);
    cbm.get_raw_low_latency();
    cbm.get_raw_high_latency();
    cbm.get_standard_deviation_raw(300);
    cbm.get_raw_flow_rate(200);
    TEST_ASSERT_EQUAL_INT32(1000, cbm.get_display_raw());

    // A new sample advances the filter once: window at ts 1100 holds 1000 and 3000 -> 2000, a rise.
    g_now_ms = 1100;
    cbm.add_sample(3000, 1100);
    TEST_ASSERT_EQUAL_INT32(2000, cbm.get_display_raw());
}

static void test_get_display_raw_is_const_and_idempotent() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(5000, 1000);
    g_now_ms = 2000;
    cbm.add_sample(0, 2000);  // fall: 0.9 * 0 + 0.1 * 5000 = 500 (alpha is applied once, here)

    const CircularBufferMath& view = cbm;  // compiles only while get_display_raw() is const
    int32_t first = view.get_display_raw();
    int32_t second = view.get_display_raw();
    TEST_ASSERT_EQUAL_INT32(500, first);
    TEST_ASSERT_EQUAL_INT32(first, second);  // a read does not decay the filter further
    TEST_ASSERT_EQUAL_INT32(500, view.get_display_raw());
}

static void test_reset_display_filter_reseeds_on_next_sample() {
    g_now_ms = 1000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1000, 1000);
    TEST_ASSERT_EQUAL_INT32(1000, cbm.get_display_raw());

    cbm.reset_display_filter();
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());  // nothing until the next sample
    TEST_ASSERT_EQUAL_UINT16(1, cbm.get_sample_count());  // samples are untouched

    // -500 is a fall from the reset value 0: a filter that stayed initialised would decay it to
    // 0.9 * -500 + 0.1 * 0 = -450 (and from the old 1000 to a different value). Only a re-seed from
    // the first new sample shows exactly -500.
    g_now_ms = 3000;
    cbm.add_sample(-500, 3000);
    TEST_ASSERT_EQUAL_INT32(-500, cbm.get_display_raw());
}

// Fresh buffer seeded with 1000 at ts 1000; one more sample far later (window holds only it, so the
// smoothed value is exactly `value`); returns the display value.
static int32_t display_after_second_sample(int32_t value) {
    g_now_ms = 5000;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1000, 1000);
    cbm.add_sample(value, 5000);
    return cbm.get_display_raw();
}

static void test_display_window_edge_is_300ms_inclusive_at_sample_timestamp() {
    // Seed 1000 @1000, 2000 @1200 (window {1000, 2000} -> 1500, a rise). Third sample 5000:
    //  - at 1300 the sample at 1000 has age exactly 300: inside; window {1000, 2000, 5000},
    //    trimmed mean keeps the middle one = 2000, a rise from 1500 -> 2000
    //  - at 1301 it has age 301: outside; window {2000, 5000} -> 3500
    g_now_ms = 0;  // far from the sample timestamps: the clock must not matter
    CircularBufferMath inside(fake_clock);
    inside.add_sample(1000, 1000);
    inside.add_sample(2000, 1200);
    TEST_ASSERT_EQUAL_INT32(1500, inside.get_display_raw());
    inside.add_sample(5000, 1300);
    TEST_ASSERT_EQUAL_INT32(2000, inside.get_display_raw());

    CircularBufferMath outside(fake_clock);
    outside.add_sample(1000, 1000);
    outside.add_sample(2000, 1200);
    outside.add_sample(5000, 1301);
    TEST_ASSERT_EQUAL_INT32(3500, outside.get_display_raw());
}

static void test_display_deadband_boundaries_99_and_100() {
    TEST_ASSERT_EQUAL_INT32(1000, display_after_second_sample(1000));  // equal: unchanged
    TEST_ASSERT_EQUAL_INT32(1000, display_after_second_sample(1099));  // rise by 99: inside deadband
    TEST_ASSERT_EQUAL_INT32(1100, display_after_second_sample(1100));  // rise by 100: taken at once
    TEST_ASSERT_EQUAL_INT32(1000, display_after_second_sample(901));   // fall by 99: inside deadband
    // Fall by 100 is filtered: 0.9 * 900 + 0.1 * 1000 = 910 (float rounding may truncate to 909)
    TEST_ASSERT_INT32_WITHIN(1, 910, display_after_second_sample(900));
}

static void test_display_filter_anchors_window_at_new_sample_timestamp() {
    // The clock (999999) is nowhere near the samples. Anchored at the sample timestamp (1100, only ~100 ms
    // after the first sample) the window holds 1000 and 3000 -> 2000. Anchored at the clock the window would be empty and the result would
    // not be 2000.
    g_now_ms = 999999;
    CircularBufferMath cbm(fake_clock);
    cbm.add_sample(1000, 1000);
    cbm.add_sample(3000, 1100);
    TEST_ASSERT_EQUAL_INT32(2000, cbm.get_display_raw());
}

static void test_clear_all_samples_is_logically_empty_after_many_wraps() {
    // Fill the ring twice over with non-zero data, clear (O(1): entries are not zeroed), and check
    // that no reader sees a stale entry.
    g_now_ms = 5000;
    CircularBufferMath cbm(fake_clock);
    for (int32_t i = 0; i < 2100; i++) {
        cbm.add_sample(1000 + i, static_cast<uint32_t>(3000 + i));
    }
    TEST_ASSERT_EQUAL_UINT16(1024, cbm.get_sample_count());
    cbm.clear_all_samples();

    TEST_ASSERT_EQUAL_UINT16(0, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_instant_raw());
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_smoothed_raw(300));
    TEST_ASSERT_EQUAL_INT32(0, cbm.get_display_raw());
    TEST_ASSERT_EQUAL_INT(0, cbm.get_sample_count_in_window(100000));
    int32_t lo = 99, hi = 99;
    TEST_ASSERT_FALSE(cbm.get_min_max_raw(100000, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(0, lo);
    TEST_ASSERT_EQUAL_INT32(0, hi);
    int32_t out[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(0, cbm.get_samples_in_window(100000, out, 4));
    int32_t delta = 99;
    TEST_ASSERT_FALSE(cbm.get_window_delta(100000, &delta));
    TEST_ASSERT_EQUAL_UINT32(0, cbm.get_buffer_time_span_ms());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cbm.get_raw_flow_rate_95th_percentile(1000));

    // New writes start at slot 0 again and only they are visible.
    g_now_ms = 5100;
    cbm.add_sample(-7, 5000);
    cbm.add_sample(-9, 5100);
    TEST_ASSERT_EQUAL_UINT16(2, cbm.get_sample_count());
    TEST_ASSERT_EQUAL_INT(2, cbm.get_sample_count_in_window(100000));
    TEST_ASSERT_TRUE(cbm.get_min_max_raw(100000, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(-9, lo);
    TEST_ASSERT_EQUAL_INT32(-7, hi);
    TEST_ASSERT_EQUAL_UINT32(100, cbm.get_buffer_time_span_ms());
    // Seeded by the first new sample (-7); the second (window mean -8) is inside the deadband.
    TEST_ASSERT_EQUAL_INT32(-7, cbm.get_display_raw());
}

void run_circular_buffer_math_tests() {
    RUN_TEST(test_min_max_all_negative_values);
    RUN_TEST(test_percentile_pins_sub_window_step_spacing);
    RUN_TEST(test_percentile_pins_the_64_sample_cap);
    RUN_TEST(test_display_window_edge_is_300ms_inclusive_at_sample_timestamp);
    RUN_TEST(test_display_deadband_boundaries_99_and_100);
    RUN_TEST(test_display_filter_anchors_window_at_new_sample_timestamp);
    RUN_TEST(test_display_value_changes_only_through_add_sample);
    RUN_TEST(test_get_display_raw_is_const_and_idempotent);
    RUN_TEST(test_reset_display_filter_reseeds_on_next_sample);
    RUN_TEST(test_clear_all_samples_is_logically_empty_after_many_wraps);
    RUN_TEST(test_collect_never_writes_past_capacity_and_keeps_newest);
    RUN_TEST(test_capped_readers_use_newest_64_but_min_max_and_flow_are_unbounded);
    RUN_TEST(test_percentile_with_200_sps_stays_in_bounds);
    RUN_TEST(test_min_max_single_pass_matches_separate_calls);
    RUN_TEST(test_min_max_after_ring_wrap_and_window_longer_than_buffer);
    RUN_TEST(test_percentile_collection_boundary_with_irregular_ramp);
    RUN_TEST(test_percentile_sub_window_needs_three_samples);
    RUN_TEST(test_flow_stability_threshold_small_nonzero_difference);
    RUN_TEST(test_is_settled_threshold_between_std_dev_neighbours);
    RUN_TEST(test_clock_at_zero_includes_sample_stamped_zero);
    RUN_TEST(test_timestamp_ahead_of_now_is_in_every_window_reader);
    RUN_TEST(test_min_max_use_only_in_window_samples);
    RUN_TEST(test_window_boundary_sample_included_and_older_excluded);
    RUN_TEST(test_display_raw_empty_does_not_latch_filter_at_zero);
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
    RUN_TEST(test_window_membership_is_wrap_safe_across_rollover);
    RUN_TEST(test_window_longer_than_clock_now_includes_samples);
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

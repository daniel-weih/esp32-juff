#include <assert.h>
#include <limits.h>
#include <string.h>

#include "battery_estimate.h"

static void sample(battery_estimate_t *state, int millivolts, bool charging,
                   uint32_t timestamp)
{
    battery_estimate_update(state, true, millivolts, charging, timestamp);
    // A plausible reading always leaves a number, including charging and load changes.
    assert(state->valid && state->percent >= 0 && state->percent <= 100);
    assert(state->millivolts == millivolts);
}

static void curve(void)
{
    assert(battery_percent_from_voltage(INT_MIN) == 0);
    assert(battery_percent_from_voltage(INT_MAX) == 100);
    int previous = -1;
    for (int mv = 0; mv <= 6000; ++mv) {
        const int percent = battery_percent_from_voltage(mv);
        assert(percent >= previous && percent >= 0 && percent <= 100);
        previous = percent;
    }
    assert(battery_percent_from_voltage(3300) == 0);
    assert(battery_percent_from_voltage(3500) == 5);
    assert(battery_percent_from_voltage(3700) == 20);
    assert(battery_percent_from_voltage(3790) == 38);
    assert(battery_percent_from_voltage(3855) == 51);
    assert(battery_percent_from_voltage(3930) == 66);
    assert(battery_percent_from_voltage(4199) == 99);
    assert(battery_percent_from_voltage(4200) == 100);
}

static void continuous_numbers(void)
{
    for (unsigned charging = 0; charging < 2; ++charging) {
        // First valid reading is useful immediately, without a settling placeholder.
        battery_estimate_t state = {0};
        sample(&state, 3930, charging, 0);
        assert(state.percent == 66);
        uint32_t now = 2000;
        for (int mv = BATTERY_MIN_PLAUSIBLE_MV;
             mv <= BATTERY_MAX_PLAUSIBLE_MV; mv += 7, now += 2000) {
            sample(&state, mv, charging, now);
        }
    }
    // The display retains one-percent resolution instead of rounding to five-percent bands.
    const int voltages[] = {3340, 3790, 3855, 3930, 4199};
    const int percentages[] = {1, 38, 51, 66, 99};
    for (unsigned i = 0; i < sizeof(voltages) / sizeof(voltages[0]); ++i) {
        battery_estimate_t state = {0};
        sample(&state, voltages[i], false, 0);
        assert(state.percent == percentages[i]);
    }
}

static void invalid(void)
{
    battery_estimate_t state = {0};
    const int invalid_mv[] = {INT_MIN, -1, 0, BATTERY_MIN_PLAUSIBLE_MV - 1,
                             BATTERY_MAX_PLAUSIBLE_MV + 1, INT_MAX};
    for (unsigned i = 0; i < sizeof(invalid_mv) / sizeof(invalid_mv[0]); ++i) {
        const uint32_t start = i * 10000U;
        state = (battery_estimate_t){0};
        sample(&state, 4200, false, start);
        assert(state.percent == 100);
        battery_estimate_update(&state, true, invalid_mv[i], false, start + 2000);
        assert(!state.valid && state.percent == -1 && state.millivolts == 0);
        sample(&state, 3790, false, start + 4000);
        assert(state.percent == 38); // Invalid input cannot leave a full-battery history.
    }
    for (unsigned charging = 0; charging < 2; ++charging) {
        // Even a plausible numeric value is unusable if ADC/calibration failed.
        battery_estimate_update(&state, false, 4200, charging, 100000);
        assert(!state.valid && state.percent == -1 && state.millivolts == 0);
        sample(&state, 3855, charging, 102000);
        assert(state.percent == 51);
    }
    state = (battery_estimate_t){0};
    sample(&state, BATTERY_MIN_PLAUSIBLE_MV, false, 0);
    assert(state.percent == 0);
    battery_estimate_update(&state, false, 0, false, 2000);
    sample(&state, BATTERY_MAX_PLAUSIBLE_MV, false, 4000);
    assert(state.percent == 100);
}

static void charging_transition(void)
{
    battery_estimate_t state = {0};
    sample(&state, 3930, true, 0);
    assert(state.percent == 66);
    // GPIO edge observation must not erase a number before another ADC reading exists.
    battery_estimate_set_charging(&state, false, 150);
    assert(state.valid && state.percent == 66 && state.millivolts == 3930);
    for (uint32_t now = 160; now < 2000; now += 10) {
        battery_estimate_set_charging(&state, false, now);
        assert(state.percent == 66);
    }
    sample(&state, 3790, false, 2000);
    assert(state.percent == 38); // First fresh off-charge reading bypasses charged history.
    battery_estimate_set_charging(&state, true, 2150);
    assert(state.valid && state.percent == 38);
    sample(&state, 3930, true, 4000);
    assert(state.percent == 66);

    // The update API itself must also observe edges when no helper call preceded it.
    sample(&state, 3790, false, 6000);
    assert(state.percent == 38);
    sample(&state, 3930, true, 8000);
    assert(state.percent == 66);
}

static void isolated_pulses(void)
{
    const int pulses[] = {3300, 4300};
    for (unsigned i = 0; i < sizeof(pulses) / sizeof(pulses[0]); ++i) {
        battery_estimate_t state = {0};
        sample(&state, 3850, false, 0);
        sample(&state, pulses[i], false, 2000);
        assert(state.percent == 50); // One low/high pulse must not masquerade as lost/gained charge.
        sample(&state, 3850, false, 4000);
        assert(state.percent == 50);
        sample(&state, 3850, false, 6000);
        assert(state.percent == 50);
    }
}

static void sustained_changes(void)
{
    battery_estimate_t state = {0};
    sample(&state, 3930, false, 0);
    sample(&state, 3790, false, 2000);
    assert(state.percent == 66);
    sample(&state, 3790, false, 4000);
    assert(state.percent == 38); // No multi-second EMA tail after the second new reading.
    sample(&state, 3930, false, 6000);
    assert(state.percent == 38);
    sample(&state, 3930, false, 8000);
    assert(state.percent == 66); // Voltage recovery is permitted during battery operation.

    state = (battery_estimate_t){0};
    sample(&state, 3930, true, 0);
    sample(&state, 3790, true, 2000);
    sample(&state, 3790, true, 4000);
    assert(state.percent == 38); // Charging does not force a fabricated monotonic increase.
}

static void gaps_and_duplicates(void)
{
    battery_estimate_t state = {0};
    sample(&state, 3930, false, 0);
    sample(&state, 3790, false, 2000);
    assert(state.percent == 66);
    const unsigned position = state.next;
    for (unsigned i = 0; i < 100; ++i) {
        sample(&state, 3790, false, 2000);
        assert(state.percent == 66 && state.next == position);
    }
    sample(&state, 3790, false, 4000);
    assert(state.percent == 38);

    // The first measurement after a long acquisition gap discards old history.
    sample(&state, 3930, false, 12000);
    assert(state.percent == 66);
    sample(&state, 3790, false, 20000);
    assert(state.percent == 38);

    state = (battery_estimate_t){0};
    sample(&state, 3930, false, 0);
    sample(&state, 3790, false, BATTERY_MAX_SAMPLE_GAP_MS);
    assert(state.percent == 66); // Exactly the allowed gap does not reset the filter.
    sample(&state, 3790, false, BATTERY_MAX_SAMPLE_GAP_MS + 2000);
    assert(state.percent == 38);
}

static void clock_wrap(void)
{
    const uint32_t start = UINT32_MAX - 2500U;
    battery_estimate_t state = {0};
    sample(&state, 3930, false, start);
    sample(&state, 3790, false, start + 2000U);
    assert(state.percent == 66);
    sample(&state, 3790, false, start + 4000U);
    assert(state.percent == 38);
    sample(&state, 3930, false, start + 12000U);
    assert(state.percent == 66); // Gap detection remains correct across wrap too.
}

static void endpoint_recovery(void)
{
    battery_estimate_t state = {0};
    sample(&state, 3300, false, 0);
    assert(state.percent == 0);
    sample(&state, 3340, false, 2000);
    sample(&state, 3340, false, 4000);
    assert(state.percent == 1); // A previous 0% must not latch the minimum band.
    sample(&state, 3300, false, 6000);
    sample(&state, 3300, false, 8000);
    assert(state.percent == 0);

    state = (battery_estimate_t){0};
    sample(&state, 4200, false, 0);
    assert(state.percent == 100);
    sample(&state, 4190, false, 2000);
    sample(&state, 4190, false, 4000);
    assert(state.percent == 99);
    sample(&state, 4200, false, 6000);
    sample(&state, 4200, false, 8000);
    assert(state.percent == 100);
}

static void charge(void)
{
    battery_charge_filter_t filter = {0};
    assert(!battery_charge_update(&filter, true, 10000));
    assert(!battery_charge_update(&filter, true, 10149));
    assert(battery_charge_update(&filter, true, 10150));
    assert(battery_charge_update(&filter, false, 20000));
    assert(battery_charge_update(&filter, false, 20149));
    assert(battery_charge_update(&filter, true, 20149)); // Bounce cancels the pending edge.
    assert(battery_charge_update(&filter, false, 20200));
    assert(battery_charge_update(&filter, false, 20349));
    assert(!battery_charge_update(&filter, false, 20350));

    filter = (battery_charge_filter_t){0};
    assert(!battery_charge_update(&filter, false, 0));
    for (uint32_t now = 10; now <= 500; now += 10) {
        assert(!battery_charge_update(&filter, (now / 10) % 2 != 0, now));
    }
    assert(!battery_charge_update(&filter, true, 510));
    assert(!battery_charge_update(&filter, true, 659));
    assert(battery_charge_update(&filter, true, 660));

    filter = (battery_charge_filter_t){0};
    const uint32_t start = UINT32_MAX - 75U;
    assert(!battery_charge_update(&filter, true, start));
    assert(!battery_charge_update(&filter, true, start + 149U));
    assert(battery_charge_update(&filter, true, start + 150U));
    // Charging indication is independent of a 100% estimate or an ADC failure.
    battery_estimate_t state = {0};
    sample(&state, 4200, true, 0);
    assert(state.percent == 100 && battery_charge_update(&filter, true, start + 160U));
    battery_estimate_update(&state, false, 0, true, 2000);
    assert(!state.valid && battery_charge_update(&filter, true, start + 170U));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "curve") == 0) curve();
    else if (strcmp(argv[1], "continuous-numbers") == 0) continuous_numbers();
    else if (strcmp(argv[1], "invalid") == 0) invalid();
    else if (strcmp(argv[1], "charging-transition") == 0) charging_transition();
    else if (strcmp(argv[1], "isolated-pulses") == 0) isolated_pulses();
    else if (strcmp(argv[1], "sustained-changes") == 0) sustained_changes();
    else if (strcmp(argv[1], "gaps-duplicates") == 0) gaps_and_duplicates();
    else if (strcmp(argv[1], "clock-wrap") == 0) clock_wrap();
    else if (strcmp(argv[1], "endpoint-recovery") == 0) endpoint_recovery();
    else if (strcmp(argv[1], "charge") == 0) charge();
    else assert(!"Unknown test scenario");
    return 0;
}

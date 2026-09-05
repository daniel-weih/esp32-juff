#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATTERY_MIN_PLAUSIBLE_MV 2600
#define BATTERY_MAX_PLAUSIBLE_MV 4350
#define BATTERY_CHARGE_DEBOUNCE_MS 150U
#define BATTERY_MAX_SAMPLE_GAP_MS 6000U
#define BATTERY_WINDOW_SAMPLES 3U

typedef struct {
    bool initialized;
    bool charging;
    bool valid;
    bool have_sample;
    int percent;
    int millivolts;
    unsigned count;
    unsigned next;
    uint32_t last_sample_ms;
    int samples[BATTERY_WINDOW_SAMPLES];
} battery_estimate_t;

typedef struct {
    bool initialized;
    bool raw_charging;
    bool stable_charging;
    uint32_t changed_at_ms;
} battery_charge_filter_t;

// Approximate single-cell 3.7 V lithium voltage curve, not a fuel gauge or a
// protection threshold. Cell chemistry, load, temperature and charging current
// affect the estimate; charge completion cannot be inferred from this curve.
static inline int battery_percent_from_voltage(int millivolts)
{
    static const struct { int mv; int percent; } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 10}, {3700, 20}, {3750, 30},
        {3800, 40}, {3850, 50}, {3900, 60}, {4000, 80},
        {4100, 90}, {4200, 100},
    };
    if (millivolts <= curve[0].mv) {
        return 0;
    }
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (millivolts < curve[i].mv) {
            const int range = curve[i].mv - curve[i - 1].mv;
            const int rise = curve[i].percent - curve[i - 1].percent;
            return curve[i - 1].percent +
                   ((millivolts - curve[i - 1].mv) * rise) / range;
        }
    }
    return 100;
}

// Discard samples from a different power state without hiding the last number.
// The next real ADC sample immediately replaces that estimate (normally <= 2 s).
static inline void battery_estimate_restart(battery_estimate_t *estimate)
{
    estimate->count = 0;
    estimate->next = 0;
    estimate->have_sample = false;
}

// STAT high means "not charging", not necessarily "USB unplugged" or "full".
static inline void battery_estimate_set_charging(battery_estimate_t *estimate,
                                                bool charging, uint32_t now_ms)
{
    (void)now_ms;
    if (!estimate->initialized || estimate->charging != charging) {
        estimate->initialized = true;
        estimate->charging = charging;
        battery_estimate_restart(estimate);
    }
}

// Every valid measurement produces a numeric estimate, including while charging.
// A three-sample median rejects one brief noise/load spike, but follows a sustained
// change after two samples. It is noise rejection, NOT a cell-capacity calibration.
// Charging still raises terminal voltage: do not invent an unmeasured correction,
// force monotonic percentages, or carry charging history into battery operation.
static inline void battery_estimate_update(battery_estimate_t *estimate,
                                           bool measurement_valid,
                                           int measured_mv,
                                           bool charging,
                                           uint32_t now_ms)
{
    battery_estimate_set_charging(estimate, charging, now_ms);
    if (!measurement_valid || measured_mv < BATTERY_MIN_PLAUSIBLE_MV ||
        measured_mv > BATTERY_MAX_PLAUSIBLE_MV) {
        battery_estimate_restart(estimate);
        estimate->valid = false;
        estimate->percent = -1;
        estimate->millivolts = 0;
        return;
    }
    if (!estimate->valid || (estimate->have_sample &&
        now_ms - estimate->last_sample_ms > BATTERY_MAX_SAMPLE_GAP_MS)) {
        battery_estimate_restart(estimate);
    }
    if (estimate->have_sample && now_ms == estimate->last_sample_ms) return;
    estimate->valid = true;
    estimate->millivolts = measured_mv;
    estimate->have_sample = true;
    estimate->last_sample_ms = now_ms;
    if (estimate->count == 0) {
        // Seed with the first actual sample; no startup wait or placeholder value.
        for (unsigned i = 0; i < BATTERY_WINDOW_SAMPLES; ++i) {
            estimate->samples[i] = measured_mv;
        }
        estimate->count = BATTERY_WINDOW_SAMPLES;
    }
    estimate->samples[estimate->next] = measured_mv;
    estimate->next = (estimate->next + 1) % BATTERY_WINDOW_SAMPLES;
    int a = estimate->samples[0], b = estimate->samples[1], c = estimate->samples[2];
    if (a > b) { const int temp = a; a = b; b = temp; }
    if (b > c) { const int temp = b; b = c; c = temp; }
    if (a > b) b = a;
    estimate->percent = battery_percent_from_voltage(b);
}

// The board's charger STAT is active low. Debounce both edges independently of
// ADC availability and percentage, including while the estimate is 100%.
static inline bool battery_charge_update(battery_charge_filter_t *filter,
                                         bool charging, uint32_t now_ms)
{
    if (!filter->initialized) {
        filter->initialized = true;
        filter->raw_charging = charging;
        filter->changed_at_ms = now_ms;
    } else if (filter->raw_charging != charging) {
        filter->raw_charging = charging;
        filter->changed_at_ms = now_ms;
    }
    if (now_ms - filter->changed_at_ms >= BATTERY_CHARGE_DEBOUNCE_MS) {
        filter->stable_charging = charging;
    }
    return filter->stable_charging;
}

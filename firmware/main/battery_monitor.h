#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool supported;
    // A calibrated, plausible voltage is available. This does not detect a
    // connected battery: USB alone can also leave VBAT at a plausible voltage.
    bool valid;
    bool charging;
    int percent;      // Voltage-derived estimate, 0–100; -1 only if unavailable.
    int millivolts;   // Latest calibrated battery-terminal voltage; 0 if invalid.
} battery_status_t;

// Call once during app startup. Creates a low-priority sampling task.
// Errors are nonfatal; the display can still show an unavailable reading.
esp_err_t battery_monitor_init(void);
// Permanently stop this boot's sampling/publication. Never waits for ADC I/O;
// the worker exits after its current read and releases its ADC resources.
void battery_monitor_stop(void);
// Returns a synchronized copy, safe from the display or other task.
battery_status_t battery_monitor_get(void);

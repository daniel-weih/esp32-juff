#pragma once

#include <stdbool.h>
#include "lvgl.h"

// A 48x22 status icon; update only from the LVGL task. Valid estimates always
// show a number, including zero, charging, and estimator transitions. Charging
// adds a left-side bolt and green fill; unavailable readings show "--".
lv_obj_t *battery_indicator_create(lv_obj_t *parent);
void battery_indicator_set(lv_obj_t *object, bool valid, int percent, bool charging);

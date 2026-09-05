#pragma once

#include "esp_err.h"

// Called first at boot, before any peripheral initialization.
void power_manager_hold(void);
// Called once audio and display have initialized. No-op on boards without PWR.
esp_err_t power_manager_start(void);

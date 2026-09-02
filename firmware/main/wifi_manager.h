#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
bool wifi_manager_has_credentials(void);
bool wifi_manager_wait_connected(unsigned timeout_ms);
bool wifi_manager_is_connected(void);

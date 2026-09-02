#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t board_display_init(void);
bool board_display_is_ready(void);
void board_display_start_audio_test(void);
void board_display_cycle_brightness(void);
void board_display_set_voice_state(const char *state);
void board_display_set_notice(const char *title,
                              const char *detail,
                              uint32_t duration_ms);

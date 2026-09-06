#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t wake_word_init(void);
// Capture task only. Uses the microphone slot, never the playback reference.
// Returns true when the entire read belongs to standby and must be discarded.
bool wake_word_process(const int16_t *pcm, size_t frames, unsigned channels,
                       bool playing, bool diagnostic);
// Capture task only; activity detection never participates in interruption.
void wake_word_observe_activity(const uint8_t *pcm, size_t bytes);
void wake_word_reset(void);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef void (*audio_pcm_callback_t)(const uint8_t *data, size_t size, void *context);
typedef void (*audio_playback_callback_t)(const char *event_type,
                                          const char *response_id,
                                          void *context);
typedef bool (*audio_barge_in_callback_t)(void *context);

esp_err_t audio_io_init(audio_pcm_callback_t pcm_callback,
                        audio_playback_callback_t playback_callback,
                        audio_barge_in_callback_t barge_in_callback,
                        audio_barge_in_callback_t barge_in_allowed_callback,
                        void *context);
esp_err_t audio_io_run_self_test(void);
esp_err_t audio_io_begin_response(const char *response_id, uint32_t sample_rate);
esp_err_t audio_io_push_pcm(const uint8_t *data, size_t size);
esp_err_t audio_io_end_response(const char *response_id);
void audio_io_clear(const char *reason);
void audio_io_set_capture_enabled(bool enabled);
bool audio_io_is_playing(void);
bool audio_io_is_available(void);
bool audio_io_supports_voice_barge_in(void);
i2c_master_bus_handle_t audio_io_i2c_bus(void);
esp_err_t audio_io_set_expander_pin(unsigned pin, bool high);

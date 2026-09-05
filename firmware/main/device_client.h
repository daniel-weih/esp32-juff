#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t device_client_start(void);
esp_err_t device_client_save_config(const char *bridge_uri,
                                    const char *device_token);
bool device_client_is_connected(void);
bool device_client_is_voice_active(void);
// Whether the active session currently accepts microphone PCM.
bool device_client_is_ready(void);
void device_client_send_pcm(const uint8_t *data, size_t size);
void device_client_send_interrupt(void);
bool device_client_try_voice_interrupt(void);
bool device_client_allows_voice_interrupt(void);
void device_client_send_playback_event(const char *event_type,
                                       const char *response_id);

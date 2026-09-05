#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_manager_start(void);
esp_err_t ble_manager_start_pairing(void);
void ble_manager_cancel_pairing(void);
bool ble_manager_is_connected(void);
bool ble_manager_is_advertising(void);
bool ble_manager_is_pairing(void);
uint32_t ble_manager_pairing_seconds_remaining(void);
bool ble_manager_audio_is_connected(void);
// Session ownership remains active while half-duplex playback pauses the mic.
bool ble_manager_is_voice_active(void);
// Whether the active session currently accepts microphone PCM.
bool ble_manager_is_voice_ready(void);
const char *ble_manager_device_name(void);
void ble_manager_send_pcm(const uint8_t *data, size_t size);
void ble_manager_send_interrupt(void);
// Automatic speech interruption must respect an explicit host input suspension.
bool ble_manager_try_voice_interrupt(void);
bool ble_manager_allows_voice_interrupt(void);
void ble_manager_send_playback_event(const char *event_type,
                                     const char *response_id);

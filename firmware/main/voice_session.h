#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct cJSON cJSON;

#define VOICE_SESSION_IDLE_MS 180000ULL

typedef enum { VOICE_MODE_CONTINUOUS, VOICE_MODE_WAKE } voice_mode_t;
typedef struct {
    voice_mode_t mode;
    bool active;
    bool ready;
    bool supported;
    bool connected;
    bool acknowledged;
    uint32_t generation;
    uint64_t last_activity_ms;
} voice_session_status_t;

esp_err_t voice_session_init(void);
esp_err_t voice_session_start(void);
void voice_session_set_supported(bool supported);
voice_session_status_t voice_session_get(void);
esp_err_t voice_session_set_mode(voice_mode_t mode);
bool voice_session_waiting(void);
bool voice_session_can_stream(void);
void voice_session_wake(void);
void voice_session_activity(void);
void voice_session_ack(uint32_t generation);
// Returns false for acknowledgements and stale/standby conversation events.
bool voice_session_handle_control(const cJSON *message);
const char *voice_session_mode_name(voice_mode_t mode);

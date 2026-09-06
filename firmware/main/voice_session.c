#include "voice_session.h"
#include <math.h>
#include <string.h>
#include "cJSON.h"

#include "audio_io.h"
#include "ble_manager.h"
#include "board_display.h"
#include "device_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mode_mutex;
static voice_session_status_t s_state = { .active = true, .generation = 1 };
static unsigned s_previous_transport;
static uint64_t s_last_sent;
static uint32_t s_last_generation;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

const char *voice_session_mode_name(voice_mode_t mode)
{
    return mode == VOICE_MODE_WAKE ? "wake" : "continuous";
}

voice_session_status_t voice_session_get(void)
{
    portENTER_CRITICAL(&s_lock);
    const voice_session_status_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

// Caller holds s_lock. Every boundary invalidates host events and queued PCM.
static void transition(bool active, uint64_t now)
{
    s_state.active = active;
    s_state.ready = false;
    s_state.acknowledged = false;
    if (++s_state.generation == 0) ++s_state.generation;
    s_state.last_activity_ms = now;
}

static void clear_turn(void)
{
    ble_manager_reset_conversation();
    device_client_reset_conversation();
    audio_io_clear("conversation boundary");
    board_display_set_voice_state("idle");
    board_display_set_notice(NULL, NULL, 0);
}

esp_err_t voice_session_init(void)
{
    s_mode_mutex = xSemaphoreCreateMutex();
    if (s_mode_mutex == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    uint8_t mode = VOICE_MODE_CONTINUOUS;
    esp_err_t error = nvs_open("juff", NVS_READONLY, &handle);
    if (error == ESP_OK) {
        error = nvs_get_u8(handle, "voice_mode", &mode);
        nvs_close(handle);
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return error;
    portENTER_CRITICAL(&s_lock);
    s_state.mode = mode == VOICE_MODE_WAKE ? VOICE_MODE_WAKE : VOICE_MODE_CONTINUOUS;
    transition(s_state.mode == VOICE_MODE_CONTINUOUS, now_ms());
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void voice_session_set_supported(bool supported)
{
    portENTER_CRITICAL(&s_lock);
    s_state.supported = supported;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t voice_session_set_mode(voice_mode_t mode)
{
    if (mode != VOICE_MODE_CONTINUOUS && mode != VOICE_MODE_WAKE) return ESP_ERR_INVALID_ARG;
    if (s_mode_mutex == NULL || xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    const voice_session_status_t before = voice_session_get();
    esp_err_t error = ESP_OK;
    if (mode == VOICE_MODE_WAKE && !before.supported) error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK && before.mode != mode) {
        nvs_handle_t handle;
        error = nvs_open("juff", NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_u8(handle, "voice_mode", mode);
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
        if (error == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_state.mode = mode;
            transition(mode == VOICE_MODE_CONTINUOUS, now_ms());
            portEXIT_CRITICAL(&s_lock);
            clear_turn();
            ESP_LOGI("juff_voice", "Conversation mode: %s", voice_session_mode_name(mode));
        }
    }
    xSemaphoreGive(s_mode_mutex);
    return error;
}

bool voice_session_waiting(void)
{
    const voice_session_status_t state = voice_session_get();
    return state.mode == VOICE_MODE_WAKE && !state.active;
}

bool voice_session_can_stream(void)
{
    const voice_session_status_t state = voice_session_get();
    return state.active && state.ready;
}

void voice_session_wake(void)
{
    bool woke = false;
    portENTER_CRITICAL(&s_lock);
    if (s_state.mode == VOICE_MODE_WAKE && !s_state.active && s_state.supported
        && s_state.connected && s_state.acknowledged) {
        transition(true, now_ms());
        woke = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (woke) ESP_LOGI("juff_voice", "Wake word detected; opening conversation");
}

void voice_session_activity(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state.active) s_state.last_activity_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);
}

void voice_session_ack(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state.generation == generation) s_state.acknowledged = true;
    portEXIT_CRITICAL(&s_lock);
}

bool voice_session_handle_control(const cJSON *message)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
    if (!cJSON_IsString(type)) return false;
    const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(message, "conversationGeneration");
    const bool tagged = cJSON_IsNumber(epoch) && epoch->valuedouble >= 1
        && epoch->valuedouble <= UINT32_MAX && floor(epoch->valuedouble) == epoch->valuedouble;
    const uint32_t generation = tagged ? (uint32_t)epoch->valuedouble : 0;
    if (epoch != NULL && !tagged) return false;
    if (strcmp(type->valuestring, "conversation.ack") == 0) {
        if (tagged) voice_session_ack(generation);
        return false;
    }
    const char *name = type->valuestring;
    const bool scoped = strncmp(name, "voice.", 6) == 0 || strncmp(name, "audio.", 6) == 0
        || strncmp(name, "input.", 6) == 0 || strcmp(name, "playback.clear") == 0;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(message, "state");
    const bool activity = strcmp(name, "voice.state") == 0 && cJSON_IsString(state)
        && (strcmp(state->valuestring, "listening") == 0
            || strcmp(state->valuestring, "processing") == 0
            || strcmp(state->valuestring, "speaking") == 0);
    portENTER_CRITICAL(&s_lock);
    const bool accepted = !(scoped || epoch != NULL) || (s_state.active
        && (tagged ? generation == s_state.generation
            : s_state.mode == VOICE_MODE_CONTINUOUS && !s_state.acknowledged));
    if (accepted) {
        if (strcmp(name, "voice.ready") == 0) {
            s_state.ready = true;
        } else if (strcmp(name, "voice.deactivated") == 0) s_state.ready = false;
        else if (activity) s_state.last_activity_ms = now_ms();
    }
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}

static void session_tick(void)
{
    if (xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const uint64_t now = now_ms();
    const unsigned transport = ble_manager_audio_is_connected() ? 1
        : device_client_is_connected() ? 2 : 0;
    const bool playing = audio_io_is_playing();
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (transport != s_previous_transport) {
        s_state.connected = transport != 0;
        transition(s_state.mode == VOICE_MODE_CONTINUOUS, now);
        changed = true;
    }
    if (s_state.mode == VOICE_MODE_WAKE && s_state.active) {
        if (playing) s_state.last_activity_ms = now;
        else if (now - s_state.last_activity_ms >= VOICE_SESSION_IDLE_MS) {
            transition(false, now);
            changed = true;
        }
    }
    const voice_session_status_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    s_previous_transport = transport;
    if (changed) clear_turn();
    if (transport != 0 && (!state.acknowledged || state.generation != s_last_generation)
        && (state.generation != s_last_generation || now - s_last_sent >= 1000)) {
        if (transport == 1) ble_manager_send_conversation_state();
        else device_client_send_conversation_state();
        s_last_sent = now;
        s_last_generation = state.generation;
    }
    xSemaphoreGive(s_mode_mutex);
}

static void session_task(void *argument)
{
    (void)argument;
    while (true) {
        session_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t voice_session_start(void)
{
    return xTaskCreate(session_task, "juff_session", 4096, NULL, 3, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

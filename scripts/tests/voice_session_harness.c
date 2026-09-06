#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "voice_session.h"
// Parsed JSON boundary; field validation is the production controller's job.
struct cJSON {
    const char *valuestring;
    double valuedouble;
    bool number;
    const cJSON *type, *epoch, *state;
};
static bool cJSON_IsString(const cJSON *value) { return value && value->valuestring; }
static bool cJSON_IsNumber(const cJSON *value) { return value && value->number; }
static const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *value, const char *key)
{ return !strcmp(key, "type") ? value->type : !strcmp(key, "state") ? value->state : value->epoch; }

#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NOT_SUPPORTED 5
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
static unsigned critical;
#define portENTER_CRITICAL(lock) ((void)(lock), ++critical)
#define portEXIT_CRITICAL(lock) ((void)(lock), --critical)
typedef void *SemaphoreHandle_t;
static bool locked;
static void *xSemaphoreCreateMutex(void) { return (void *)1; }
static int xSemaphoreTake(void *mutex, unsigned ticks) { assert(!locked); locked = true; return pdTRUE; }
static void xSemaphoreGive(void *mutex) { assert(locked); locked = false; }
static int xTaskCreate(void (*task)(void *), const char *name, int size, void *arg, int priority, void *handle) { return pdPASS; }
static void vTaskDelay(unsigned ticks) {}
static uint64_t clock_ms = 1000;
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static bool ble, wifi, playing;
static unsigned clears, sends;
static bool ble_manager_audio_is_connected(void) { return ble; }
static bool device_client_is_connected(void) { return wifi; }
static bool audio_io_is_playing(void) { return playing; }
static void ble_manager_reset_conversation(void) { assert(!critical); }
static void device_client_reset_conversation(void) { assert(!critical); }
static void audio_io_clear(const char *reason) { assert(!critical); ++clears; playing = false; }
static void board_display_set_voice_state(const char *state) {}
static void board_display_set_notice(const char *title, const char *detail, unsigned ms) {}
static void ble_manager_send_conversation_state(void) { assert(!critical); ++sends; }
static void device_client_send_conversation_state(void) { assert(!critical); ++sends; }
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef unsigned nvs_handle_t;
static uint8_t stored, pending;
static bool saved, fail_save;
static int nvs_open(const char *name, int flags, unsigned *handle) { *handle = 1; return ESP_OK; }
static void nvs_close(unsigned handle) {}
static int nvs_get_u8(unsigned handle, const char *key, uint8_t *value)
{ *value = stored; return saved ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
static int nvs_set_u8(unsigned handle, const char *key, uint8_t value)
{ pending = value; return fail_save ? ESP_ERR_INVALID_STATE : ESP_OK; }
static int nvs_commit(unsigned handle) { stored = pending; saved = true; return ESP_OK; }

#include "session.inc"

static bool control(const char *type, uint32_t generation, const char *state)
{
    const cJSON name = { .valuestring = type }, epoch = { .valuedouble = generation, .number = true },
        value = { .valuestring = state }, message = { .type = &name, .epoch = &epoch, .state = &value };
    return voice_session_handle_control(&message);
}

static void acknowledge(void) { voice_session_ack(voice_session_get().generation); }
static void wake(void)
{
    acknowledge();
    voice_session_wake();
    assert(voice_session_get().active && !voice_session_can_stream());
    acknowledge();
    assert(control("voice.ready", voice_session_get().generation, NULL));
    assert(voice_session_can_stream());
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    saved = true;
    stored = strcmp(argv[1], "continuous") == 0 ? VOICE_MODE_CONTINUOUS : VOICE_MODE_WAKE;
    assert(voice_session_init() == ESP_OK);
    voice_session_set_supported(true);
    assert(!voice_session_can_stream());
    if (!strcmp(argv[1], "reconnect")) {
        voice_session_wake();
        assert(voice_session_waiting());
    }
    ble = true;
    session_tick();
    if (!strcmp(argv[1], "continuous")) {
        assert(!voice_session_waiting());
        acknowledge();
        assert(control("voice.ready", voice_session_get().generation, NULL));
        clock_ms += 24ULL * 60 * 60 * 1000;
        session_tick();
        assert(voice_session_can_stream());
    } else {
        assert(voice_session_waiting());
        wake();
        const uint32_t epoch = voice_session_get().generation;
        if (!strcmp(argv[1], "timeout")) {
            clock_ms += VOICE_SESSION_IDLE_MS - 1;
            session_tick();
            assert(voice_session_can_stream());
            assert(control("voice.ready", epoch, NULL)); // Connection readiness is not a dialogue.
            ++clock_ms;
            session_tick();
            assert(voice_session_waiting() && !voice_session_can_stream());
            assert(!control("voice.ready", epoch, NULL));
            wake();
            assert(voice_session_get().generation != epoch);
        } else if (!strcmp(argv[1], "activity")) {
            clock_ms += 179000;
            voice_session_activity();
            clock_ms += 179999;
            session_tick();
            assert(voice_session_can_stream());
            playing = true;
            clock_ms += 180000;
            session_tick();
            assert(voice_session_can_stream());
            playing = false;
            voice_session_activity();
            clock_ms += 179999;
            session_tick();
            assert(voice_session_can_stream());
            assert(control("voice.state", epoch, "idle"));
            ++clock_ms;
            session_tick();
            assert(voice_session_waiting());
        } else if (!strcmp(argv[1], "reconnect")) {
            ble = false;
            session_tick();
            assert(voice_session_waiting());
            wifi = true;
            session_tick();
            assert(voice_session_waiting());
            assert(!control("voice.ready", epoch, NULL));
            voice_session_wake(); // No handshake yet.
            assert(voice_session_waiting());
            wake();
        } else if (!strcmp(argv[1], "stale")) {
            assert(voice_session_set_mode(VOICE_MODE_CONTINUOUS) == ESP_OK);
            assert(!control("voice.ready", epoch, NULL));
            const cJSON type = { .valuestring = "voice.ready" }, fraction = { .valuedouble = 1.5, .number = true },
                invalid = { .type = &type, .epoch = &fraction };
            assert(!voice_session_handle_control(&invalid));
            voice_session_ack(epoch);
            assert(!voice_session_get().acknowledged);
            acknowledge();
            const cJSON legacy = { .type = &type };
            assert(!voice_session_handle_control(&legacy));
            assert(control("voice.ready", voice_session_get().generation, NULL));
        } else if (!strcmp(argv[1], "persistence")) {
            fail_save = true;
            assert(voice_session_set_mode(VOICE_MODE_CONTINUOUS) != ESP_OK);
            assert(voice_session_get().mode == VOICE_MODE_WAKE && voice_session_can_stream());
            fail_save = false;
            assert(voice_session_set_mode(VOICE_MODE_CONTINUOUS) == ESP_OK);
            assert(saved && stored == VOICE_MODE_CONTINUOUS);
            voice_session_set_supported(false);
            assert(voice_session_set_mode(VOICE_MODE_WAKE) == ESP_ERR_NOT_SUPPORTED);
            assert(voice_session_get().mode == VOICE_MODE_CONTINUOUS);
        } else assert(false);
    }
    assert(!critical && !locked && clears && sends);
    puts("Conversation policy passed");
    return 0;
}

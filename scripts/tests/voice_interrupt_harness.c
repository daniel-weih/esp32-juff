#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_manager.h"
#include "battery_monitor.h"
#include "device_client.h"

#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_ARG 3
#define JUFF_BOARD_ID "waveshare-lcd-1.54"
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

typedef enum {
    VOICE_VISUAL_IDLE, VOICE_VISUAL_LISTENING,
    VOICE_VISUAL_PROCESSING, VOICE_VISUAL_SPEAKING
} voice_visual_state_t;
static voice_visual_state_t s_voice_visual_state;
static bool playing, capture_enabled;
static unsigned ble_interrupts, wifi_interrupts, wifi_pcm_attempts;
static unsigned ble_pcm_attempts;
static const char *notice_title;

static bool s_notify_enabled, s_advertising, s_microphone_notify_enabled;
static bool s_voice_ready, s_input_suspended, s_playback_suspended;
static uint32_t s_microphone_generation;
static void *s_speaker_mutex = (void *)1;
static char s_flow_response[64];
static uint8_t s_phy_tx = 2, s_phy_rx = 2;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static const char *s_device_name = "JUFF-test";

typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = { .version = "0.6.0" };
    return &description;
}
battery_status_t battery_monitor_get(void)
{
    return (battery_status_t){ .supported = true, .valid = true,
                               .percent = 40, .millivolts = 3790 };
}
static bool audio_io_is_playing(void) { return playing; }
static bool audio_io_is_available(void) { return true; }
static bool audio_io_supports_voice_barge_in(void) { return true; }
static void audio_io_set_capture_enabled(bool value) { capture_enabled = value; }
static void audio_io_clear(const char *reason) { playing = false; }
static void reset_speaker_stream(void) {}
static void mark_speaker_terminal(const char *id, const char *state) {}
static void notify_speaker_credit(void) {}
static int xSemaphoreTake(void *mutex, unsigned timeout) { return pdTRUE; }
static void xSemaphoreGive(void *mutex) { (void)mutex; }
static void board_display_set_voice_state(const char *state)
{
    s_voice_visual_state = strcmp(state, "speaking") == 0
        ? VOICE_VISUAL_SPEAKING : VOICE_VISUAL_IDLE;
}
static void board_display_set_notice(const char *title, const char *detail, uint32_t ms)
{ notice_title = title; }
static void board_display_cycle_brightness(void) {}
static void board_display_start_audio_test(void) {}
static int begin_ble_speaker(const char *id, uint32_t rate, uint32_t version, uint32_t stream)
{
    snprintf(s_flow_response, sizeof(s_flow_response), "%s", id);
    playing = true;
    s_playback_suspended = true;
    board_display_set_voice_state("speaking");
    capture_enabled = false;
    return ESP_OK;
}
static int end_ble_speaker(const char *id, uint32_t stream) { return ESP_OK; }
static bool wifi_manager_has_credentials(void) { return false; }
static bool wifi_manager_is_connected(void) { return device_client_is_connected(); }
static int wifi_manager_save_credentials(const char *ssid, const char *password)
{ return ESP_ERR_INVALID_ARG; }
int device_client_save_config(const char *uri, const char *token)
{ return ESP_ERR_INVALID_ARG; }
bool ble_manager_is_pairing(void) { return false; }
static void vTaskDelay(unsigned ticks) { (void)ticks; }
static void esp_restart(void) {}

// The JSON parser boundary returns these already-decoded command fields.
typedef struct cJSON { const char *type; const char *name; } cJSON;
#include "voice_session_stubs.h"
static cJSON *cJSON_Parse(const char *input) { return (cJSON *)input; }
static void cJSON_Delete(cJSON *value) { (void)value; }
static const char *json_string(const cJSON *message, const char *key)
{
    if (strcmp(key, "type") == 0) return message->type;
    if (strcmp(key, "name") == 0) return message->name;
    return NULL;
}
static uint32_t json_number(const cJSON *message, const char *key, uint32_t fallback)
{ return fallback; }
static void notify_ack(const char *request, bool ok, const char *error) {}
static void notify_current_status(void) {}
void ble_manager_send_conversation_state(void) {}
static bool notify_text(const char *text)
{
    if (strcmp(text, "{\"type\":\"interrupt\"}") == 0) ++ble_interrupts;
    return true;
}

#include "ble_manager.inc"

static bool wifi_connected, wifi_voice_active, wifi_input_suspended;
static void send_simple_event(const char *type)
{
    if (strcmp(type, "interrupt") == 0) ++wifi_interrupts;
}
#define s_transport_connected wifi_connected
#define s_voice_ready wifi_voice_active
#define s_input_suspended wifi_input_suspended
#include "device_client.inc"
#undef s_transport_connected
#undef s_voice_ready
#undef s_input_suspended

typedef int lv_event_t;
#define LV_EVENT_CLICKED 1
static int lv_event_get_code(const lv_event_t *event) { return *event; }
static void *s_state_mutex = (void *)1;
#include "board_display.inc"

void ble_manager_send_pcm(const uint8_t *data, size_t size) { ++ble_pcm_attempts; }
void device_client_send_pcm(const uint8_t *data, size_t size) { ++wifi_pcm_attempts; }
#include "app_main.inc"

static void command(const char *type, const char *name)
{
    const cJSON message = { .type = type, .name = name };
    handle_command((const char *)&message);
}

static void reset(void)
{
    set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
    wifi_connected = wifi_voice_active = wifi_input_suspended = false;
    playing = capture_enabled = false;
    ble_interrupts = wifi_interrupts = wifi_pcm_attempts = ble_pcm_attempts = 0;
    s_voice_visual_state = VOICE_VISUAL_IDLE;
    s_shutdown_state = DISPLAY_RUNNING;
    notice_title = "";
}

static void start_ble_playback(void)
{
    set_link_state(1, true, false);
    s_microphone_notify_enabled = true;
    command("voice.ready", NULL);
    assert(ble_manager_is_voice_ready());
    command("audio.begin", NULL);
    assert(playing && !capture_enabled);
    assert(ble_manager_is_voice_active());
    assert(!ble_manager_is_voice_ready());
}

int main(void)
{
    lv_event_t clicked = LV_EVENT_CLICKED;
    char status[512];

    // PWR shutdown owns the screen: queued touch events cannot trigger a
    // voice command, and a repeated prepare cannot undo the powered-off latch.
    reset();
    start_ble_playback();
    const char *pre_shutdown_notice = notice_title;
    board_display_prepare_shutdown();
    primary_clicked(&clicked);
    assert(ble_interrupts == 0 && wifi_interrupts == 0 && playing);
    assert(notice_title == pre_shutdown_notice);
    s_shutdown_state = DISPLAY_POWERED_OFF;
    board_display_prepare_shutdown();
    assert(display_shutdown_state() == DISPLAY_POWERED_OFF);
    primary_clicked(&clicked);
    assert(ble_interrupts == 0 && wifi_interrupts == 0 && playing);

    // Touch, BOOT and the BLE control command must stay on BLE during playback.
    for (unsigned entry = 0; entry < 3; ++entry) {
        reset();
        start_ble_playback();
        wifi_connected = wifi_voice_active = true;
        if (entry == 0) primary_clicked(&clicked);
        if (entry == 1) send_interrupt();
        if (entry == 2) command("command", "interrupt");
        assert(ble_interrupts == 1 && wifi_interrupts == 0);
        assert(!playing && ble_manager_is_voice_ready());
        assert(capture_enabled);
    }

    // Input pause affects microphone flow, not connectivity or transport ownership.
    reset();
    start_ble_playback();
    assert(build_status(status, sizeof(status)) > 0);
    assert(strstr(status, "\"voice\":true") != NULL);
    assert(strstr(status, "\"bridge\":true") != NULL);
    assert(strstr(status, "\"firmware\":\"0.6.0\"") != NULL);
    assert(strstr(status, "\"board\":\"waveshare-lcd-1.54\"") != NULL);
    assert(strstr(status, "\"voiceInterrupt\":true") != NULL);
    assert(strstr(status, "\"battery\":{\"valid\":true,\"charging\":false,"
                          "\"percent\":40,\"millivolts\":3790}") != NULL);
    wifi_connected = wifi_voice_active = true;
    const uint8_t pcm[] = { 0, 0 };
    microphone_pcm(pcm, sizeof(pcm), NULL);
    assert(ble_pcm_attempts == 1 && wifi_pcm_attempts == 0);

    // Resume cannot enable the microphone until speaker playback has stopped.
    reset();
    start_ble_playback();
    uint32_t microphone_generation = s_microphone_generation;
    command("input.suspend", NULL);
    assert(s_microphone_generation != microphone_generation);
    microphone_generation = s_microphone_generation;
    command("input.resume", NULL);
    assert(s_microphone_generation == microphone_generation);
    assert(!ble_manager_is_voice_ready() && !capture_enabled);
    ble_manager_send_playback_event("playback.ended", "old-response");
    assert(!ble_manager_is_voice_ready() && !capture_enabled);
    ble_manager_send_playback_event("playback.ended", "ble-response");
    assert(!ble_manager_is_voice_ready() && !capture_enabled);
    playing = false;
    ble_manager_send_playback_event("playback.ended", "ble-response");
    assert(ble_manager_is_voice_ready() && capture_enabled);
    // Restoring capture after playback must retain the generation used by the
    // frontend's immediately queued speech history.
    assert(s_microphone_generation == microphone_generation);
    command("voice.deactivated", NULL);
    assert(s_microphone_generation != microphone_generation);
    microphone_generation = s_microphone_generation;
    command("voice.ready", NULL);
    assert(s_microphone_generation != microphone_generation);

    // Manual stop works during an external suspension, but cannot revoke it.
    for (unsigned finish = 0; finish < 3; ++finish) {
        reset();
        start_ble_playback();
        command("input.suspend", NULL);
        assert(!ble_manager_try_voice_interrupt());
        assert(ble_interrupts == 0 && playing);
        if (finish == 0) primary_clicked(&clicked);
        if (finish == 1) command("playback.clear", NULL);
        if (finish == 2) {
            playing = false;
            ble_manager_send_playback_event("playback.cancelled", "ble-response");
        }
        assert(ble_manager_is_voice_active());
        assert(!ble_manager_is_voice_ready() && !capture_enabled);
        command("input.resume", NULL);
        assert(ble_manager_is_voice_ready() && capture_enabled);
    }

    reset();
    start_ble_playback();
    assert(ble_manager_try_voice_interrupt());
    assert(ble_interrupts == 1 && !playing && capture_enabled);
    command("voice.deactivated", NULL);
    assert(!ble_manager_try_voice_interrupt());
    set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
    assert(!ble_manager_is_voice_active());

    // A BLE control link without voice ownership must still allow WiFi fallback.
    for (unsigned entry = 0; entry < 3; ++entry) {
        reset();
        set_link_state(1, true, false);
        wifi_connected = wifi_voice_active = wifi_input_suspended = true;
        playing = true;
        assert(device_client_is_voice_active() && !device_client_is_ready());
        assert(!device_client_try_voice_interrupt());
        if (entry == 0) primary_clicked(&clicked);
        if (entry == 1) send_interrupt();
        if (entry == 2) command("command", "interrupt");
        assert(wifi_interrupts == 1 && ble_interrupts == 0 && !playing);
        assert(!device_client_is_ready());
        wifi_input_suspended = false;
        assert(device_client_try_voice_interrupt());
        assert(wifi_interrupts == 2);
        wifi_connected = false;
        assert(!device_client_try_voice_interrupt());
    }

    reset();
    primary_clicked(&clicked);
    assert(strcmp(notice_title, "Let's connect") == 0);
    assert(ble_interrupts == 0 && wifi_interrupts == 0);
    puts("Voice interruption routing passed");
    return 0;
}

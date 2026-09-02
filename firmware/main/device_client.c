#include "device_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_io.h"
#include "board_display.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TEXT_MESSAGE_SIZE 4096
#define SEND_TIMEOUT_MS 1000
#define DEVICE_NVS_NAMESPACE "juff"
#define DEVICE_NVS_BRIDGE_URI_KEY "bridge_uri"
#define DEVICE_NVS_TOKEN_KEY "device_token"
#define BRIDGE_URI_SIZE 160
#define DEVICE_TOKEN_SIZE 96

static const char *TAG = "juff_bridge";
static esp_websocket_client_handle_t s_client;
static SemaphoreHandle_t s_send_mutex;
static volatile bool s_transport_connected;
static volatile bool s_voice_ready;
static volatile bool s_input_suspended;
static bool s_binary_message;
static char s_text_message[TEXT_MESSAGE_SIZE];
static char s_bridge_uri[BRIDGE_URI_SIZE];
static char s_device_token[DEVICE_TOKEN_SIZE];

static esp_err_t load_device_config(void)
{
    strlcpy(s_bridge_uri, CONFIG_JUFF_BRIDGE_URI, sizeof(s_bridge_uri));
    strlcpy(s_device_token, CONFIG_JUFF_DEVICE_TOKEN, sizeof(s_device_token));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(DEVICE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }

    size_t bridge_uri_size = sizeof(s_bridge_uri);
    error = nvs_get_str(handle,
                        DEVICE_NVS_BRIDGE_URI_KEY,
                        s_bridge_uri,
                        &bridge_uri_size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(s_bridge_uri, CONFIG_JUFF_BRIDGE_URI, sizeof(s_bridge_uri));
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        size_t token_size = sizeof(s_device_token);
        error = nvs_get_str(handle,
                            DEVICE_NVS_TOKEN_KEY,
                            s_device_token,
                            &token_size);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(s_device_token, CONFIG_JUFF_DEVICE_TOKEN, sizeof(s_device_token));
            error = ESP_OK;
        }
    }
    nvs_close(handle);
    return error;
}

static bool valid_bridge_uri(const char *bridge_uri)
{
    return bridge_uri != NULL
        && (strncmp(bridge_uri, "ws://", 5) == 0
            || strncmp(bridge_uri, "wss://", 6) == 0)
        && strlen(bridge_uri) < BRIDGE_URI_SIZE;
}

static bool valid_device_token(const char *device_token)
{
    return device_token != NULL
        && strlen(device_token) >= 16
        && strlen(device_token) < DEVICE_TOKEN_SIZE
        && strncmp(device_token, "change-", 7) != 0
        && strncmp(device_token, "change_", 7) != 0
        && strncmp(device_token, "replace-", 8) != 0
        && strncmp(device_token, "replace_", 8) != 0;
}

static bool lock_sender(void)
{
    return s_send_mutex != NULL
        && xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE;
}

static void unlock_sender(void)
{
    xSemaphoreGive(s_send_mutex);
}

static void send_json(cJSON *message)
{
    if (message == NULL || !s_transport_connected || s_client == NULL) {
        cJSON_Delete(message);
        return;
    }
    char *serialized = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (serialized == NULL) {
        return;
    }
    if (lock_sender()) {
        const int result = esp_websocket_client_send_text(s_client,
                                                           serialized,
                                                           strlen(serialized),
                                                           pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        unlock_sender();
        if (result < 0) {
            ESP_LOGW(TAG, "Unable to send JSON event");
        }
    }
    free(serialized);
}

static void send_simple_event(const char *type)
{
    cJSON *message = cJSON_CreateObject();
    if (message != NULL) {
        cJSON_AddStringToObject(message, "type", type);
    }
    send_json(message);
}

static void send_hello(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[40];
    snprintf(device_id,
             sizeof(device_id),
             "esp32s3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *message = cJSON_CreateObject();
    if (message != NULL) {
        cJSON_AddStringToObject(message, "type", "hello");
        cJSON_AddStringToObject(message, "token", s_device_token);
        cJSON_AddStringToObject(message, "deviceId", device_id);
        cJSON_AddStringToObject(message, "firmware", "juff-voice-terminal/0.2.0");
        cJSON_AddBoolToObject(message, "audioInputEnabled", audio_io_is_available());
        cJSON_AddBoolToObject(message, "audioOutputEnabled", audio_io_is_available());
        cJSON_AddNumberToObject(message, "inputSampleRate", 16000);
        cJSON_AddNumberToObject(message, "outputSampleRate", 24000);
    }
    send_json(message);
}

static const char *json_string(cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static uint32_t json_number(cJSON *object, const char *name, uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) && item->valuedouble > 0
        ? (uint32_t)item->valuedouble
        : fallback;
}

static void handle_json_message(const char *data, size_t size)
{
    cJSON *message = cJSON_ParseWithLength(data, size);
    if (message == NULL) {
        ESP_LOGW(TAG, "Ignoring invalid JSON from host bridge");
        return;
    }
    const char *type = json_string(message, "type");

    if (strcmp(type, "device.ready") == 0) {
        ESP_LOGI(TAG,
                 "Host bridge ready (model=%s, input=%" PRIu32 " Hz)",
                 json_string(message, "realtimeModel"),
                 json_number(message, "inputSampleRate", 16000));
    } else if (strcmp(type, "voice.ready") == 0) {
        s_voice_ready = true;
        board_display_set_voice_state("idle");
        if (!s_input_suspended) {
            audio_io_set_capture_enabled(true);
        }
        ESP_LOGI(TAG, "Qwen realtime voice is ready");
    } else if (strcmp(type, "voice.deactivated") == 0) {
        s_voice_ready = false;
        board_display_set_voice_state("idle");
        audio_io_set_capture_enabled(false);
        ESP_LOGW(TAG, "Voice ownership moved to another client");
    } else if (strcmp(type, "audio.begin") == 0) {
        board_display_set_voice_state("speaking");
        const esp_err_t error = audio_io_begin_response(
            json_string(message, "responseId"),
            json_number(message, "sampleRate", 24000));
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Unable to begin playback: %s", esp_err_to_name(error));
        }
    } else if (strcmp(type, "audio.done") == 0) {
        (void)audio_io_end_response(json_string(message, "responseId"));
        board_display_set_voice_state("idle");
    } else if (strcmp(type, "playback.clear") == 0) {
        board_display_set_voice_state("idle");
        audio_io_clear(json_string(message, "reason"));
    } else if (strcmp(type, "input.suspend") == 0) {
        s_input_suspended = true;
        audio_io_set_capture_enabled(false);
        send_simple_event("input.suspend.ack");
    } else if (strcmp(type, "input.resume") == 0) {
        s_input_suspended = false;
        audio_io_set_capture_enabled(s_voice_ready);
    } else if (strcmp(type, "voice.state") == 0) {
        const char *state = json_string(message, "state");
        board_display_set_voice_state(state);
        ESP_LOGI(TAG, "Voice state: %s", state == NULL ? "unknown" : state);
    } else if (strcmp(type, "transcript.final") == 0) {
        ESP_LOGI(TAG,
                 "%s: %s",
                 json_string(message, "role"),
                 json_string(message, "text"));
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGE(TAG, "Gateway error: %s", json_string(message, "message"));
    }
    cJSON_Delete(message);
}

static void handle_data_event(const esp_websocket_event_data_t *event)
{
    if (event->payload_offset == 0) {
        s_binary_message = event->op_code == 0x2;
    }

    if (s_binary_message) {
        const esp_err_t error = audio_io_push_pcm((const uint8_t *)event->data_ptr,
                                                   event->data_len);
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Dropping speaker PCM: %s", esp_err_to_name(error));
        }
        return;
    }

    const size_t end = (size_t)event->payload_offset + (size_t)event->data_len;
    if (event->payload_len >= sizeof(s_text_message) || end >= sizeof(s_text_message)) {
        ESP_LOGW(TAG, "Host JSON event too large (%d bytes)", event->payload_len);
        return;
    }
    memcpy(s_text_message + event->payload_offset, event->data_ptr, event->data_len);
    if (end == (size_t)event->payload_len) {
        s_text_message[end] = '\0';
        handle_json_message(s_text_message, end);
    }
}

static void websocket_event(void *handler_args,
                            esp_event_base_t event_base,
                            int32_t event_id,
                            void *event_data)
{
    (void)handler_args;
    (void)event_base;
    const esp_websocket_event_data_t *event = event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_transport_connected = true;
        ESP_LOGI(TAG, "Connected to host bridge");
        send_hello();
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_transport_connected = false;
        s_voice_ready = false;
        audio_io_set_capture_enabled(false);
        audio_io_clear("bridge disconnected");
        ESP_LOGW(TAG, "Host bridge disconnected; reconnecting automatically");
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        handle_data_event(event);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "Host bridge WebSocket error");
    }
}

esp_err_t device_client_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }
    esp_err_t error = load_device_config();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Unable to load device bridge config: %s", esp_err_to_name(error));
        return error;
    }
    if (!valid_bridge_uri(s_bridge_uri)) {
        ESP_LOGE(TAG, "Invalid device bridge URI");
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_device_token(s_device_token)) {
        ESP_LOGE(TAG, "JUFF_DEVICE_TOKEN must be a private random value");
        return ESP_ERR_INVALID_ARG;
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (s_send_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_websocket_client_config_t websocket_config = {
        .uri = s_bridge_uri,
        .buffer_size = 4096,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .task_stack = 6144,
    };
    s_client = esp_websocket_client_init(&websocket_config);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_client,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_event,
                                                  NULL));
    ESP_LOGI(TAG, "Connecting to %s", s_bridge_uri);
    return esp_websocket_client_start(s_client);
}

esp_err_t device_client_save_config(const char *bridge_uri,
                                    const char *device_token)
{
    if (!valid_bridge_uri(bridge_uri) || !valid_device_token(device_token)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(DEVICE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, DEVICE_NVS_BRIDGE_URI_KEY, bridge_uri);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, DEVICE_NVS_TOKEN_KEY, device_token);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

bool device_client_is_ready(void)
{
    return s_transport_connected && s_voice_ready && !s_input_suspended;
}

bool device_client_is_connected(void)
{
    return s_transport_connected;
}

void device_client_send_pcm(const uint8_t *data, size_t size)
{
    if (!device_client_is_ready() || data == NULL || size == 0 || !lock_sender()) {
        return;
    }
    const int result = esp_websocket_client_send_bin(s_client,
                                                      (const char *)data,
                                                      size,
                                                      pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    unlock_sender();
    if (result < 0) {
        ESP_LOGW(TAG, "Microphone PCM send failed");
    }
}

void device_client_send_interrupt(void)
{
    audio_io_clear("local interrupt");
    send_simple_event("interrupt");
}

void device_client_send_playback_event(const char *event_type,
                                       const char *response_id)
{
    cJSON *message = cJSON_CreateObject();
    if (message != NULL) {
        cJSON_AddStringToObject(message, "type", event_type);
        cJSON_AddStringToObject(message,
                               "responseId",
                               response_id == NULL ? "" : response_id);
    }
    send_json(message);
}

#include "ble_manager.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_io.h"
#include "board_config.h"
#include "board_display.h"
#include "cJSON.h"
#include "device_client.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi_manager.h"

#define BLE_MESSAGE_MAX_BYTES 512
#define BLE_COMMAND_QUEUE_DEPTH 4
#define BLE_MIC_PCM_FRAME_MAX_BYTES 3200
#define BLE_MIC_ENCODED_FRAME_MAX_BYTES (BLE_MIC_PCM_FRAME_MAX_BYTES / 2)
#define BLE_MIC_QUEUE_DEPTH 5
#define BLE_SPEAKER_CHUNK_BYTES 4800
#define BLE_SPEAKER_BUFFER_BYTES (BLE_SPEAKER_CHUNK_BYTES * 4)
#define BLE_NOTIFY_RETRY_COUNT 8
#define BLE_PAIRING_WINDOW_MS 120000U
#define BLE_PAIRING_BLOCKED_PEERS_MAX 4

typedef struct {
    char text[BLE_MESSAGE_MAX_BYTES + 1];
} ble_command_t;

typedef struct {
    uint16_t size;
    uint8_t data[BLE_MIC_ENCODED_FRAME_MAX_BYTES];
} ble_audio_frame_t;

static const char *TAG = "juff_ble";

/* Canonical UUIDs used by the macOS companion:
 * B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001 service
 * B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A002 control (Mac -> ESP32)
 * B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A003 status  (ESP32 -> Mac)
 * B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A004 microphone PCM (notify)
 * B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A005 speaker PCM (write without response)
 */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x01, 0xa0, 0x49, 0x6c, 0x5b, 0x7b, 0x6c, 0x9c,
    0xb1, 0x4d, 0x0f, 0x8a, 0xa1, 0xfc, 0xa5, 0xb8);
static const ble_uuid128_t s_control_uuid = BLE_UUID128_INIT(
    0x02, 0xa0, 0x49, 0x6c, 0x5b, 0x7b, 0x6c, 0x9c,
    0xb1, 0x4d, 0x0f, 0x8a, 0xa1, 0xfc, 0xa5, 0xb8);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x03, 0xa0, 0x49, 0x6c, 0x5b, 0x7b, 0x6c, 0x9c,
    0xb1, 0x4d, 0x0f, 0x8a, 0xa1, 0xfc, 0xa5, 0xb8);
static const ble_uuid128_t s_microphone_uuid = BLE_UUID128_INIT(
    0x04, 0xa0, 0x49, 0x6c, 0x5b, 0x7b, 0x6c, 0x9c,
    0xb1, 0x4d, 0x0f, 0x8a, 0xa1, 0xfc, 0xa5, 0xb8);
static const ble_uuid128_t s_speaker_uuid = BLE_UUID128_INIT(
    0x05, 0xa0, 0x49, 0x6c, 0x5b, 0x7b, 0x6c, 0x9c,
    0xb1, 0x4d, 0x0f, 0x8a, 0xa1, 0xfc, 0xa5, 0xb8);

static QueueHandle_t s_command_queue;
static QueueHandle_t s_microphone_queue;
static SemaphoreHandle_t s_speaker_mutex;
static uint8_t s_own_addr_type;
static uint16_t s_control_handle;
static uint16_t s_status_handle;
static uint16_t s_microphone_handle;
static uint16_t s_speaker_handle;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_notify_enabled;
static bool s_microphone_notify_enabled;
static bool s_voice_ready;
// Host input suspension and half-duplex playback have independent lifetimes.
static bool s_input_suspended;
static bool s_playback_suspended;
static bool s_speaker_response_active;
static bool s_advertising;
static bool s_started;
static char s_device_name[16] = "JUFF";
static char s_incoming[BLE_MESSAGE_MAX_BYTES + 1];
static size_t s_incoming_length;
static uint8_t s_speaker_buffer[BLE_SPEAKER_BUFFER_BYTES];
static size_t s_speaker_length;
static uint32_t s_dropped_microphone_frames;
static uint32_t s_dropped_speaker_bytes;
static uint64_t s_pairing_until_ms;
static ble_addr_t s_pairing_blocked_peers[BLE_PAIRING_BLOCKED_PEERS_MAX];
static int s_pairing_blocked_peer_count;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

void ble_store_config_init(void);

static int gap_event(struct ble_gap_event *event, void *argument);

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static bool addresses_equal(const ble_addr_t *first, const ble_addr_t *second)
{
    return first != NULL && second != NULL && first->type == second->type
        && memcmp(first->val, second->val, sizeof(first->val)) == 0;
}

static bool pairing_active_locked(uint64_t now_ms)
{
    if (s_pairing_until_ms == 0 || now_ms >= s_pairing_until_ms) {
        s_pairing_until_ms = 0;
        s_pairing_blocked_peer_count = 0;
        return false;
    }
    return true;
}

static bool pairing_blocks_peer(const struct ble_gap_conn_desc *description)
{
    bool blocked = false;
    const uint64_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_state_lock);
    if (pairing_active_locked(now_ms)) {
        for (int index = 0; index < s_pairing_blocked_peer_count; ++index) {
            if (addresses_equal(&description->peer_id_addr,
                                &s_pairing_blocked_peers[index])
                || addresses_equal(&description->peer_ota_addr,
                                   &s_pairing_blocked_peers[index])) {
                blocked = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return blocked;
}

static void finish_pairing_window(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_pairing_until_ms = 0;
    s_pairing_blocked_peer_count = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static uint8_t linear_pcm_to_mulaw(int16_t sample)
{
    const int bias = 0x84;
    const int clip = 32635;
    int value = sample;
    const int sign = value < 0 ? 0x80 : 0;
    if (value < 0) {
        value = -value;
    }
    if (value > clip) {
        value = clip;
    }
    value += bias;

    int exponent = 7;
    int mask = 0x4000;
    while (exponent > 0 && (value & mask) == 0) {
        --exponent;
        mask >>= 1;
    }
    const int mantissa = (value >> (exponent + 3)) & 0x0f;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

static int16_t mulaw_to_linear_pcm(uint8_t encoded)
{
    const int value = (~encoded) & 0xff;
    const int sign = value & 0x80;
    const int exponent = (value >> 4) & 0x07;
    const int mantissa = value & 0x0f;
    int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    if (sign != 0) {
        sample = -sample;
    }
    return (int16_t)sample;
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL
        ? item->valuestring
        : NULL;
}

static uint32_t json_number(const cJSON *object,
                            const char *name,
                            uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) && item->valuedouble > 0
        ? (uint32_t)item->valuedouble
        : fallback;
}

static void snapshot_link_state(uint16_t *connection_handle,
                                bool *notify_enabled,
                                bool *advertising)
{
    portENTER_CRITICAL(&s_state_lock);
    if (connection_handle != NULL) {
        *connection_handle = s_connection_handle;
    }
    if (notify_enabled != NULL) {
        *notify_enabled = s_notify_enabled;
    }
    if (advertising != NULL) {
        *advertising = s_advertising;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void snapshot_audio_state(bool *microphone_notify_enabled,
                                 bool *voice_ready,
                                 bool *input_suspended)
{
    portENTER_CRITICAL(&s_state_lock);
    if (microphone_notify_enabled != NULL) {
        *microphone_notify_enabled = s_microphone_notify_enabled;
    }
    if (voice_ready != NULL) {
        *voice_ready = s_voice_ready;
    }
    if (input_suspended != NULL) {
        *input_suspended = s_input_suspended || s_playback_suspended;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_link_state(uint16_t connection_handle,
                           bool notify_enabled,
                           bool advertising)
{
    portENTER_CRITICAL(&s_state_lock);
    s_connection_handle = connection_handle;
    s_notify_enabled = notify_enabled;
    s_advertising = advertising;
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_microphone_notify_enabled = false;
        s_voice_ready = false;
        s_input_suspended = false;
        s_playback_suspended = false;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void mark_connected(uint16_t connection_handle)
{
    portENTER_CRITICAL(&s_state_lock);
    s_connection_handle = connection_handle;
    /*
     * A bonded macOS central can restore encryption and the CCCD almost
     * immediately. Preserve a subscribe event that races the tail of the
     * connect callback; disconnect already clears this flag for a new link.
     */
    s_advertising = false;
    portEXIT_CRITICAL(&s_state_lock);
}

static size_t build_status(char *buffer, size_t buffer_size)
{
    const bool ble_voice = ble_manager_is_voice_active();
    const bool bridge_connected = ble_manager_audio_is_connected()
        || device_client_is_connected();
    const bool voice_ready = ble_voice || device_client_is_voice_active();
    const int length = snprintf(
        buffer,
        buffer_size,
        "{\"type\":\"status\",\"name\":\"%s\",\"firmware\":\"%s\",\"board\":\"%s\","
        "\"wifiConfigured\":%s,\"wifi\":%s,\"ble\":%s,\"pairing\":%s,\"bridge\":%s,"
        "\"voice\":%s,\"bleAudio\":%s,\"audio\":%s,\"playing\":%s,\"voiceInterrupt\":%s}",
        s_device_name,
        esp_app_get_description()->version,
        JUFF_BOARD_ID,
        wifi_manager_has_credentials() ? "true" : "false",
        wifi_manager_is_connected() ? "true" : "false",
        ble_manager_is_connected() ? "true" : "false",
        ble_manager_is_pairing() ? "true" : "false",
        bridge_connected ? "true" : "false",
        voice_ready ? "true" : "false",
        ble_manager_audio_is_connected() ? "true" : "false",
        audio_io_is_available() ? "true" : "false",
        audio_io_is_playing() ? "true" : "false",
        audio_io_supports_voice_barge_in() ? "true" : "false");
    if (length < 0 || (size_t)length >= buffer_size) {
        return 0;
    }
    return (size_t)length;
}

static bool notify_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    uint16_t connection_handle;
    bool notify_enabled;
    snapshot_link_state(&connection_handle, &notify_enabled, NULL);
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE || !notify_enabled) {
        return false;
    }

    const size_t text_length = strlen(text);
    if (text_length > BLE_MESSAGE_MAX_BYTES) {
        return false;
    }
    char framed[BLE_MESSAGE_MAX_BYTES + 2];
    memcpy(framed, text, text_length);
    framed[text_length] = '\n';
    const size_t framed_length = text_length + 1;

    const uint16_t mtu = ble_att_mtu(connection_handle);
    const size_t chunk_size = mtu > 3 ? mtu - 3 : 20;
    size_t offset = 0;
    while (offset < framed_length) {
        const size_t remaining = framed_length - offset;
        const size_t length = remaining < chunk_size ? remaining : chunk_size;
        struct os_mbuf *packet = ble_hs_mbuf_from_flat(framed + offset,
                                                       (uint16_t)length);
        if (packet == NULL) {
            return false;
        }
        const int result = ble_gatts_notify_custom(connection_handle,
                                                    s_status_handle,
                                                    packet);
        if (result != 0) {
            ESP_LOGW(TAG, "Status notification failed: %d", result);
            return false;
        }
        offset += length;
        if (offset < framed_length) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return true;
}

static void notify_current_status(void)
{
    char status[BLE_MESSAGE_MAX_BYTES];
    if (build_status(status, sizeof(status)) > 0) {
        (void)notify_text(status);
    }
}

static void notify_ack(const char *request, bool ok, const char *error)
{
    char response[192];
    if (ok) {
        snprintf(response,
                 sizeof(response),
                 "{\"type\":\"ack\",\"request\":\"%s\",\"ok\":true}",
                 request);
    } else {
        snprintf(response,
                 sizeof(response),
                 "{\"type\":\"ack\",\"request\":\"%s\",\"ok\":false,"
                 "\"error\":\"%s\"}",
                 request,
                 error == NULL ? "failed" : error);
    }
    (void)notify_text(response);
}

static bool queue_received_byte(uint8_t value)
{
    if (value == '\r') {
        return true;
    }
    if (value == '\n') {
        if (s_incoming_length == 0) {
            return true;
        }
        ble_command_t command = { 0 };
        memcpy(command.text, s_incoming, s_incoming_length);
        command.text[s_incoming_length] = '\0';
        s_incoming_length = 0;
        return xQueueSend(s_command_queue, &command, 0) == pdTRUE;
    }
    if (s_incoming_length >= BLE_MESSAGE_MAX_BYTES) {
        s_incoming_length = 0;
        return false;
    }
    s_incoming[s_incoming_length++] = (char)value;
    return true;
}

static void flush_speaker_locked(bool include_partial)
{
    while (s_speaker_response_active
           && (s_speaker_length >= BLE_SPEAKER_CHUNK_BYTES
               || (include_partial && s_speaker_length >= sizeof(int16_t)))) {
        size_t size = s_speaker_length >= BLE_SPEAKER_CHUNK_BYTES
            ? BLE_SPEAKER_CHUNK_BYTES
            : s_speaker_length;
        size &= ~(sizeof(int16_t) - 1U);
        if (size == 0) {
            break;
        }
        const esp_err_t error = audio_io_push_pcm(s_speaker_buffer, size);
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Dropping BLE speaker PCM: %s", esp_err_to_name(error));
        }
        const size_t remaining = s_speaker_length - size;
        if (remaining > 0) {
            memmove(s_speaker_buffer, s_speaker_buffer + size, remaining);
        }
        s_speaker_length = remaining;
    }
}

static void append_speaker_data(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0 || s_speaker_mutex == NULL
        || xSemaphoreTake(s_speaker_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_dropped_speaker_bytes += size;
        return;
    }

    size_t offset = 0;
    while (offset < size) {
        if (s_speaker_response_active) {
            flush_speaker_locked(false);
        }
        const size_t free_bytes = sizeof(s_speaker_buffer) - s_speaker_length;
        if (free_bytes == 0) {
            s_dropped_speaker_bytes += size - offset;
            break;
        }
        const size_t copy_size = size - offset < free_bytes
            ? size - offset
            : free_bytes;
        memcpy(s_speaker_buffer + s_speaker_length, data + offset, copy_size);
        s_speaker_length += copy_size;
        offset += copy_size;
    }
    if (s_speaker_response_active) {
        flush_speaker_locked(false);
    }
    xSemaphoreGive(s_speaker_mutex);
}

static void append_speaker_mulaw(const uint8_t *data, size_t size)
{
    uint8_t pcm[256];
    while (data != NULL && size > 0) {
        const size_t sample_count = size < (sizeof(pcm) / sizeof(int16_t))
            ? size
            : (sizeof(pcm) / sizeof(int16_t));
        for (size_t index = 0; index < sample_count; ++index) {
            const uint16_t sample = (uint16_t)mulaw_to_linear_pcm(data[index]);
            pcm[index * 2] = (uint8_t)(sample & 0xff);
            pcm[index * 2 + 1] = (uint8_t)(sample >> 8);
        }
        append_speaker_data(pcm, sample_count * sizeof(int16_t));
        data += sample_count;
        size -= sample_count;
    }
}

static void reset_speaker_stream(void)
{
    if (s_speaker_mutex == NULL
        || xSemaphoreTake(s_speaker_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_speaker_response_active = false;
    s_speaker_length = 0;
    xSemaphoreGive(s_speaker_mutex);
}

static int gatt_access(uint16_t connection_handle,
                       uint16_t attribute_handle,
                       struct ble_gatt_access_ctxt *context,
                       void *argument)
{
    (void)connection_handle;
    (void)argument;

    if (attribute_handle == s_control_handle
        && context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        for (struct os_mbuf *packet = context->om;
             packet != NULL;
             packet = SLIST_NEXT(packet, om_next)) {
            for (uint16_t index = 0; index < packet->om_len; ++index) {
                if (!queue_received_byte(packet->om_data[index])) {
                    ESP_LOGW(TAG, "Dropped oversized BLE command or full queue");
                    return BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            }
        }
        return 0;
    }

    if (attribute_handle == s_status_handle
        && context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char status[BLE_MESSAGE_MAX_BYTES];
        const size_t length = build_status(status, sizeof(status));
        if (length == 0 || os_mbuf_append(context->om, status, length) != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    if (attribute_handle == s_speaker_handle
        && context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        for (struct os_mbuf *packet = context->om;
             packet != NULL;
             packet = SLIST_NEXT(packet, om_next)) {
            append_speaker_mulaw(packet->om_data, packet->om_len);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_control_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE
                       | BLE_GATT_CHR_F_WRITE_ENC
                       | BLE_GATT_CHR_F_WRITE_AUTHEN,
                .val_handle = &s_control_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {
                .uuid = &s_microphone_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_microphone_handle,
            },
            {
                .uuid = &s_speaker_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP
                       | BLE_GATT_CHR_F_WRITE_ENC
                       | BLE_GATT_CHR_F_WRITE_AUTHEN,
                .val_handle = &s_speaker_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to set advertising fields: %d", result);
        return;
    }

    struct ble_hs_adv_fields response = { 0 };
    response.name = (uint8_t *)s_device_name;
    response.name_len = strlen(s_device_name);
    response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to set BLE scan response: %d", result);
        return;
    }

    struct ble_gap_adv_params parameters = { 0 };
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(s_own_addr_type,
                               NULL,
                               BLE_HS_FOREVER,
                               &parameters,
                               gap_event,
                               NULL);
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Unable to start BLE advertising: %d", result);
        return;
    }
    set_link_state(BLE_HS_CONN_HANDLE_NONE, false, true);
    ESP_LOGI(TAG, "Advertising encrypted setup service as %s", s_device_name);
}

static void display_passkey(uint16_t connection_handle)
{
    struct ble_sm_io response = { 0 };
    response.action = BLE_SM_IOACT_DISP;
    response.passkey = esp_random() % 1000000U;
    const int result = ble_sm_inject_io(connection_handle, &response);
    if (result != 0) {
        ESP_LOGW(TAG, "Unable to provide BLE passkey: %d", result);
        return;
    }
    char detail[48];
    snprintf(detail,
             sizeof(detail),
             "Enter %06" PRIu32 " on your Mac",
             response.passkey);
    board_display_set_notice("Pair with JUFF", detail, 120000);
    ESP_LOGI(TAG, "BLE pairing code is shown on the device display");
}

static void configure_audio_link(uint16_t connection_handle)
{
    const struct ble_gap_upd_params parameters = {
        .itvl_min = 6,
        .itvl_max = 12,
        .latency = 0,
        .supervision_timeout = 600,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int result = ble_gap_update_params(connection_handle, &parameters);
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "BLE connection parameter update failed: %d", result);
    }
    result = ble_gap_set_data_len(connection_handle, 251, 2120);
    if (result != 0) {
        ESP_LOGW(TAG, "BLE data length update failed: %d", result);
    }
    result = ble_gap_set_prefered_le_phy(connection_handle,
                                         BLE_GAP_LE_PHY_2M_MASK,
                                         BLE_GAP_LE_PHY_2M_MASK,
                                         0);
    if (result != 0) {
        ESP_LOGW(TAG, "BLE 2M PHY request failed: %d; continuing on 1M", result);
    }
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connection failed: %d", event->connect.status);
            start_advertising();
            return 0;
        }
        {
            struct ble_gap_conn_desc description;
            if (ble_gap_conn_find(event->connect.conn_handle, &description) == 0
                && pairing_blocks_peer(&description)) {
                ESP_LOGI(TAG,
                         "Pair-new-Mac window rejected a previously bonded central");
                set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
                const int result = ble_gap_terminate(event->connect.conn_handle,
                                                     BLE_ERR_REM_USER_CONN_TERM);
                if (result != 0) {
                    ESP_LOGW(TAG, "Unable to reject bonded central: %d", result);
                }
                return 0;
            }
        }
        s_incoming_length = 0;
        mark_connected(event->connect.conn_handle);
        if (ble_manager_is_pairing()) {
            board_display_set_notice("New Mac found",
                                     "Securing your new private audio link",
                                     2500);
        } else {
            board_display_set_notice("Mac connected",
                                     "Securing your private audio link",
                                     2500);
        }
        ESP_LOGI(TAG, "Mac connected over BLE; starting secure pairing");
        {
            const int result = ble_gap_security_initiate(event->connect.conn_handle);
            if (result != 0 && result != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Unable to initiate BLE pairing: %d", result);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE central disconnected: %d", event->disconnect.reason);
        s_incoming_length = 0;
        set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
        board_display_set_voice_state("idle");
        reset_speaker_stream();
        if (!device_client_is_ready()) {
            audio_io_set_capture_enabled(false);
        }
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &description) == 0
            && description.sec_state.encrypted) {
            const bool completed_new_pairing = ble_manager_is_pairing();
            if (completed_new_pairing) {
                finish_pairing_window();
                board_display_set_notice("New Mac is ready",
                                         "Paired securely and remembered",
                                         3500);
            } else {
                board_display_set_notice("Secure connection",
                                         description.sec_state.bonded
                                             ? "Your Mac is paired and remembered"
                                             : "Your Mac connection is encrypted",
                                         2500);
            }
            ESP_LOGI(TAG,
                     "BLE encrypted (authenticated=%d bonded=%d)",
                     description.sec_state.authenticated,
                     description.sec_state.bonded);
            configure_audio_link(event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE encryption failed: %d", event->enc_change.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            display_passkey(event->passkey.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            portENTER_CRITICAL(&s_state_lock);
            s_notify_enabled = event->subscribe.cur_notify != 0;
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGI(TAG,
                     "BLE status notifications %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == s_microphone_handle) {
            portENTER_CRITICAL(&s_state_lock);
            s_microphone_notify_enabled = event->subscribe.cur_notify != 0;
            if (!s_microphone_notify_enabled) {
                s_voice_ready = false;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (!event->subscribe.cur_notify && !device_client_is_ready()) {
                audio_io_set_capture_enabled(false);
            }
            ESP_LOGI(TAG,
                     "BLE microphone notifications %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU negotiated: %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                              &description) == 0) {
            ble_store_util_delete_peer(&description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void on_stack_reset(int reason)
{
    set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
}

static void on_stack_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result != 0) {
        ESP_LOGE(TAG, "No usable BLE identity address: %d", result);
        return;
    }
    result = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to determine BLE address type: %d", result);
        return;
    }
    start_advertising();
}

static void nimble_host_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void handle_command(const char *text)
{
    cJSON *message = cJSON_Parse(text);
    const char *type = message == NULL ? NULL : json_string(message, "type");
    if (type == NULL) {
        notify_ack("unknown", false, "invalid-json");
        cJSON_Delete(message);
        return;
    }

    if (strcmp(type, "status") == 0) {
        notify_current_status();
    } else if (strcmp(type, "device.ready") == 0) {
        ESP_LOGI(TAG,
                 "Mac BLE audio bridge ready (model=%s)",
                 json_string(message, "realtimeModel") == NULL
                     ? "Qwen Realtime"
                     : json_string(message, "realtimeModel"));
        notify_current_status();
    } else if (strcmp(type, "voice.ready") == 0) {
        bool microphone_notify_enabled;
        snapshot_audio_state(&microphone_notify_enabled, NULL, NULL);
        portENTER_CRITICAL(&s_state_lock);
        s_voice_ready = microphone_notify_enabled;
        s_input_suspended = false;
        s_playback_suspended = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (microphone_notify_enabled) {
            audio_io_set_capture_enabled(true);
            board_display_set_voice_state("idle");
            board_display_set_notice("Ready when you are",
                                     "Qwen is listening over Bluetooth",
                                     2500);
            ESP_LOGI(TAG, "Qwen realtime voice is ready over BLE");
        }
        notify_current_status();
    } else if (strcmp(type, "voice.deactivated") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_voice_ready = false;
        s_input_suspended = false;
        s_playback_suspended = false;
        portEXIT_CRITICAL(&s_state_lock);
        reset_speaker_stream();
        board_display_set_voice_state("idle");
        if (!device_client_is_ready()) {
            audio_io_set_capture_enabled(false);
        }
        audio_io_clear("BLE voice deactivated");
        notify_current_status();
    } else if (strcmp(type, "audio.begin") == 0) {
        const char *response_id = json_string(message, "responseId");
        const uint32_t sample_rate = json_number(message, "sampleRate", 24000);
        if (response_id == NULL) {
            response_id = "ble-response";
        }
        portENTER_CRITICAL(&s_state_lock);
        s_playback_suspended = true;
        portEXIT_CRITICAL(&s_state_lock);
        board_display_set_voice_state("speaking");
        audio_io_set_capture_enabled(false);
        esp_err_t error = ESP_ERR_TIMEOUT;
        if (s_speaker_mutex != NULL
            && xSemaphoreTake(s_speaker_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_speaker_response_active) {
                s_speaker_length = 0;
            }
            error = audio_io_begin_response(response_id, sample_rate);
            if (error == ESP_OK) {
                s_speaker_response_active = true;
                flush_speaker_locked(false);
            }
            xSemaphoreGive(s_speaker_mutex);
        }
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Unable to begin BLE playback: %s", esp_err_to_name(error));
        }
    } else if (strcmp(type, "audio.done") == 0) {
        const char *response_id = json_string(message, "responseId");
        if (response_id == NULL) {
            response_id = "ble-response";
        }
        if (s_speaker_mutex != NULL
            && xSemaphoreTake(s_speaker_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            flush_speaker_locked(true);
            s_speaker_response_active = false;
            s_speaker_length = 0;
            xSemaphoreGive(s_speaker_mutex);
        }
        (void)audio_io_end_response(response_id);
        board_display_set_voice_state("idle");
    } else if (strcmp(type, "playback.clear") == 0) {
        reset_speaker_stream();
        board_display_set_voice_state("idle");
        const char *reason = json_string(message, "reason");
        audio_io_clear(reason == NULL ? "Mac requested clear" : reason);
        portENTER_CRITICAL(&s_state_lock);
        s_playback_suspended = false;
        portEXIT_CRITICAL(&s_state_lock);
        audio_io_set_capture_enabled(ble_manager_is_voice_ready()
                                     || device_client_is_ready());
    } else if (strcmp(type, "input.suspend") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_input_suspended = true;
        portEXIT_CRITICAL(&s_state_lock);
        audio_io_set_capture_enabled(false);
        (void)notify_text("{\"type\":\"input.suspend.ack\"}");
    } else if (strcmp(type, "input.resume") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_input_suspended = false;
        portEXIT_CRITICAL(&s_state_lock);
        audio_io_set_capture_enabled(ble_manager_is_voice_ready()
                                     || device_client_is_ready());
    } else if (strcmp(type, "voice.state") == 0) {
        const char *state = json_string(message, "state");
        board_display_set_voice_state(state);
        ESP_LOGI(TAG, "BLE Qwen voice state: %s", state == NULL ? "unknown" : state);
    } else if (strcmp(type, "error") == 0) {
        const char *error = json_string(message, "message");
        ESP_LOGE(TAG, "Mac BLE bridge error: %s", error == NULL ? "unknown" : error);
        board_display_set_notice("Qwen needs attention",
                                 error == NULL ? "Check the Mac service" : error,
                                 5000);
    } else if (strcmp(type, "command") == 0) {
        const char *name = json_string(message, "name");
        if (name != NULL && strcmp(name, "interrupt") == 0) {
            if (ble_manager_is_voice_active()) {
                ble_manager_send_interrupt();
            } else {
                device_client_send_interrupt();
            }
            board_display_set_notice("Stopping this turn",
                                     "The Mac received your request",
                                     1800);
            notify_ack("interrupt", true, NULL);
        } else if (name != NULL && strcmp(name, "brightness") == 0) {
            board_display_cycle_brightness();
            notify_ack("brightness", true, NULL);
        } else if (name != NULL && strcmp(name, "audio_test") == 0) {
            board_display_start_audio_test();
            notify_ack("audio_test", true, NULL);
        } else {
            notify_ack("command", false, "unknown-command");
        }
    } else if (strcmp(type, "provision") == 0
               || strcmp(type, "juff.provision.v1") == 0) {
        const char *ssid = json_string(message, "ssid");
        const char *password = json_string(message, "password");
        const char *bridge_uri = json_string(message, "bridgeUri");
        const char *device_token = json_string(message, "deviceToken");
        esp_err_t error = ESP_ERR_INVALID_ARG;
        if (ssid != NULL && password != NULL
            && bridge_uri != NULL && device_token != NULL) {
            error = device_client_save_config(bridge_uri, device_token);
            if (error == ESP_OK) {
                error = wifi_manager_save_credentials(ssid, password);
            }
        }
        if (error == ESP_OK) {
            board_display_set_notice("Setup saved",
                                     "Restarting with the new connection",
                                     0);
            notify_ack("provision", true, NULL);
            cJSON_Delete(message);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            return;
        }
        ESP_LOGW(TAG, "BLE provisioning rejected: %s", esp_err_to_name(error));
        notify_ack("provision", false, "invalid-settings");
    } else {
        notify_ack(type, false, "unsupported-request");
    }
    cJSON_Delete(message);
}

static void command_task(void *argument)
{
    (void)argument;
    ble_command_t command;
    while (true) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            handle_command(command.text);
        }
    }
}

static void status_task(void *argument)
{
    (void)argument;
    while (true) {
        notify_current_status();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void microphone_task(void *argument)
{
    (void)argument;
    while (true) {
        ble_audio_frame_t *frame = NULL;
        if (xQueueReceive(s_microphone_queue, &frame, portMAX_DELAY) != pdTRUE
            || frame == NULL) {
            continue;
        }

        uint16_t connection_handle;
        bool microphone_notify_enabled;
        bool voice_ready;
        bool input_suspended;
        snapshot_link_state(&connection_handle, NULL, NULL);
        snapshot_audio_state(&microphone_notify_enabled,
                             &voice_ready,
                             &input_suspended);
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE
            || !microphone_notify_enabled
            || !voice_ready
            || input_suspended) {
            free(frame);
            continue;
        }

        const uint16_t mtu = ble_att_mtu(connection_handle);
        size_t chunk_size = mtu > 3 ? mtu - 3 : 20;
        chunk_size &= ~(sizeof(int16_t) - 1U);
        if (chunk_size < sizeof(int16_t)) {
            chunk_size = 20;
        }

        bool dropped = false;
        size_t offset = 0;
        while (offset < frame->size) {
            const size_t remaining = frame->size - offset;
            const size_t length = remaining < chunk_size ? remaining : chunk_size;
            bool sent = false;
            for (unsigned retry = 0; retry < BLE_NOTIFY_RETRY_COUNT; ++retry) {
                struct os_mbuf *packet = ble_hs_mbuf_from_flat(frame->data + offset,
                                                               (uint16_t)length);
                if (packet != NULL) {
                    const int result = ble_gatts_notify_custom(connection_handle,
                                                               s_microphone_handle,
                                                               packet);
                    if (result == 0) {
                        sent = true;
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (!sent) {
                dropped = true;
                break;
            }
            offset += length;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        free(frame);
        if (dropped) {
            ++s_dropped_microphone_frames;
            if ((s_dropped_microphone_frames % 20U) == 1U) {
                ESP_LOGW(TAG,
                         "BLE microphone congestion; dropped frames=%" PRIu32,
                         s_dropped_microphone_frames);
            }
        }
    }
}

esp_err_t ble_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    uint8_t mac[6] = { 0 };
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read device MAC");
    snprintf(s_device_name,
             sizeof(s_device_name),
             "JUFF-%02X%02X",
             mac[4],
             mac[5]);

    s_command_queue = xQueueCreate(BLE_COMMAND_QUEUE_DEPTH, sizeof(ble_command_t));
    s_microphone_queue = xQueueCreate(BLE_MIC_QUEUE_DEPTH,
                                      sizeof(ble_audio_frame_t *));
    s_speaker_mutex = xSemaphoreCreateMutex();
    if (s_command_queue == NULL || s_microphone_queue == NULL
        || s_speaker_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(error));
        return error;
    }

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC
                               | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC
                                 | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_gatts_count_cfg(s_services);
    if (result == 0) {
        result = ble_gatts_add_svcs(s_services);
    }
    if (result == 0) {
        result = ble_svc_gap_device_name_set(s_device_name);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to initialize BLE GATT service: %d", result);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_store_config_init();
    if (xTaskCreate(command_task,
                    "juff_ble_cmd",
                    5120,
                    NULL,
                    4,
                    NULL) != pdPASS
        || xTaskCreate(status_task,
                       "juff_ble_status",
                       3072,
                       NULL,
                       3,
                       NULL) != pdPASS) {
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(microphone_task,
                    "juff_ble_mic",
                    4096,
                    NULL,
                    5,
                    NULL) != pdPASS) {
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(nimble_host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE setup/control/audio transport initialized as %s", s_device_name);
    ESP_LOGI(TAG, "Internal heap after BLE startup: free=%u, largest=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

esp_err_t ble_manager_start_pairing(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_addr_t blocked_peers[BLE_PAIRING_BLOCKED_PEERS_MAX] = { 0 };
    int blocked_peer_count = 0;
    const int store_result = ble_store_util_bonded_peers(
        blocked_peers,
        &blocked_peer_count,
        BLE_PAIRING_BLOCKED_PEERS_MAX);
    if (store_result != 0) {
        blocked_peer_count = 0;
        ESP_LOGW(TAG, "Unable to enumerate bonded Macs: %d", store_result);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_pairing_until_ms = monotonic_ms() + BLE_PAIRING_WINDOW_MS;
    s_pairing_blocked_peer_count = blocked_peer_count;
    if (blocked_peer_count > 0) {
        memcpy(s_pairing_blocked_peers,
               blocked_peers,
               (size_t)blocked_peer_count * sizeof(blocked_peers[0]));
    }
    s_voice_ready = false;
    s_input_suspended = false;
    s_playback_suspended = false;
    portEXIT_CRITICAL(&s_state_lock);

    reset_speaker_stream();
    audio_io_clear("pairing a new Mac");
    audio_io_set_capture_enabled(device_client_is_ready());

    uint16_t connection_handle;
    bool advertising;
    snapshot_link_state(&connection_handle, NULL, &advertising);
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        const int result = ble_gap_terminate(connection_handle,
                                             BLE_ERR_REM_USER_CONN_TERM);
        if (result != 0 && result != BLE_HS_ENOTCONN) {
            finish_pairing_window();
            ESP_LOGW(TAG, "Unable to release the current Mac: %d", result);
            return ESP_FAIL;
        }
    } else if (!advertising) {
        start_advertising();
    }

    board_display_set_notice("Pair a new Mac",
                             "Discoverable for 2 minutes",
                             BLE_PAIRING_WINDOW_MS);
    ESP_LOGI(TAG,
             "Pair-new-Mac window opened for 120 seconds; protected bonds=%d",
             blocked_peer_count);
    return ESP_OK;
}

void ble_manager_cancel_pairing(void)
{
    const bool was_pairing = ble_manager_is_pairing();
    finish_pairing_window();
    if (!was_pairing) {
        return;
    }

    uint16_t connection_handle;
    snapshot_link_state(&connection_handle, NULL, NULL);
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        const int result = ble_gap_terminate(connection_handle,
                                             BLE_ERR_REM_USER_CONN_TERM);
        if (result != 0 && result != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "Unable to cancel pending pairing link: %d", result);
        }
    }
}

bool ble_manager_is_connected(void)
{
    uint16_t connection_handle;
    snapshot_link_state(&connection_handle, NULL, NULL);
    return connection_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_manager_is_advertising(void)
{
    bool advertising;
    snapshot_link_state(NULL, NULL, &advertising);
    return advertising;
}

bool ble_manager_is_pairing(void)
{
    bool active;
    const uint64_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_state_lock);
    active = pairing_active_locked(now_ms);
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

uint32_t ble_manager_pairing_seconds_remaining(void)
{
    uint32_t seconds = 0;
    const uint64_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_state_lock);
    if (pairing_active_locked(now_ms)) {
        seconds = (uint32_t)((s_pairing_until_ms - now_ms + 999U) / 1000U);
    }
    portEXIT_CRITICAL(&s_state_lock);
    return seconds;
}

bool ble_manager_audio_is_connected(void)
{
    uint16_t connection_handle;
    bool microphone_notify_enabled;
    snapshot_link_state(&connection_handle, NULL, NULL);
    snapshot_audio_state(&microphone_notify_enabled, NULL, NULL);
    return connection_handle != BLE_HS_CONN_HANDLE_NONE
        && microphone_notify_enabled;
}

bool ble_manager_is_voice_active(void)
{
    bool microphone_notify_enabled;
    bool voice_ready;
    snapshot_audio_state(&microphone_notify_enabled, &voice_ready, NULL);
    return ble_manager_is_connected() && microphone_notify_enabled && voice_ready;
}

bool ble_manager_is_voice_ready(void)
{
    bool microphone_notify_enabled;
    bool voice_ready;
    bool input_suspended;
    snapshot_audio_state(&microphone_notify_enabled,
                         &voice_ready,
                         &input_suspended);
    return ble_manager_is_connected()
        && microphone_notify_enabled
        && voice_ready
        && !input_suspended;
}

void ble_manager_send_pcm(const uint8_t *data, size_t size)
{
    if (!ble_manager_is_voice_ready() || data == NULL || size == 0
        || (size % sizeof(int16_t)) != 0 || s_microphone_queue == NULL) {
        return;
    }

    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        size_t frame_size = remaining < BLE_MIC_PCM_FRAME_MAX_BYTES
            ? remaining
            : BLE_MIC_PCM_FRAME_MAX_BYTES;
        frame_size &= ~(sizeof(int16_t) - 1U);
        if (frame_size == 0) {
            return;
        }
        ble_audio_frame_t *frame = malloc(sizeof(*frame));
        if (frame == NULL) {
            ++s_dropped_microphone_frames;
            return;
        }
        frame->size = (uint16_t)(frame_size / sizeof(int16_t));
        for (size_t index = 0; index < frame->size; ++index) {
            int16_t sample;
            memcpy(&sample,
                   data + offset + index * sizeof(sample),
                   sizeof(sample));
            frame->data[index] = linear_pcm_to_mulaw(sample);
        }
        if (xQueueSend(s_microphone_queue, &frame, 0) != pdTRUE) {
            free(frame);
            ++s_dropped_microphone_frames;
            return;
        }
        offset += frame_size;
    }
}

void ble_manager_send_interrupt(void)
{
    reset_speaker_stream();
    audio_io_clear("local BLE interrupt");
    portENTER_CRITICAL(&s_state_lock);
    s_playback_suspended = false;
    portEXIT_CRITICAL(&s_state_lock);
    audio_io_set_capture_enabled(ble_manager_is_voice_ready()
                                 || device_client_is_ready());
    (void)notify_text("{\"type\":\"interrupt\"}");
}

bool ble_manager_try_voice_interrupt(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool input_suspended = s_input_suspended;
    portEXIT_CRITICAL(&s_state_lock);
    if (!ble_manager_is_voice_active() || input_suspended) {
        return false;
    }
    ble_manager_send_interrupt();
    return true;
}

bool ble_manager_allows_voice_interrupt(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool input_suspended = s_input_suspended;
    portEXIT_CRITICAL(&s_state_lock);
    return ble_manager_is_voice_active() && !input_suspended;
}

void ble_manager_send_playback_event(const char *event_type,
                                     const char *response_id)
{
    if (event_type == NULL || response_id == NULL) {
        return;
    }
    if (strcmp(event_type, "playback.ended") == 0
        || strcmp(event_type, "playback.cancelled") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_playback_suspended = false;
        portEXIT_CRITICAL(&s_state_lock);
        audio_io_set_capture_enabled(ble_manager_is_voice_ready()
                                     || device_client_is_ready());
    }
    char message[192];
    const int length = snprintf(message,
                                sizeof(message),
                                "{\"type\":\"%s\",\"responseId\":\"%s\"}",
                                event_type,
                                response_id);
    if (length > 0 && (size_t)length < sizeof(message)) {
        (void)notify_text(message);
    }
}

const char *ble_manager_device_name(void)
{
    return s_device_name;
}

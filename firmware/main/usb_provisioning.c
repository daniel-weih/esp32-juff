#include "usb_provisioning.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "device_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#define PROVISIONING_LINE_SIZE 384

static const char *TAG = "juff_usb_setup";
static bool s_started;

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL
        ? item->valuestring
        : NULL;
}

static void send_marker(const char *marker)
{
    printf("%s\n", marker);
    fflush(stdout);
}

static void handle_provisioning_line(const char *line)
{
    cJSON *message = cJSON_Parse(line);
    const char *type = message == NULL ? NULL : json_string(message, "type");
    const char *ssid = message == NULL ? NULL : json_string(message, "ssid");
    const char *password = message == NULL ? NULL : json_string(message, "password");
    const char *bridge_uri = message == NULL
        ? NULL
        : json_string(message, "bridgeUri");
    const char *device_token = message == NULL
        ? NULL
        : json_string(message, "deviceToken");
    if (type == NULL || strcmp(type, "juff.provision.v1") != 0
        || ssid == NULL || password == NULL
        || bridge_uri == NULL || device_token == NULL) {
        cJSON_Delete(message);
        send_marker("JUFF_PROVISION_ERROR invalid-request");
        return;
    }

    esp_err_t error = device_client_save_config(bridge_uri, device_token);
    if (error == ESP_OK) {
        error = wifi_manager_save_credentials(ssid, password);
    }
    cJSON_Delete(message);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Unable to save provisioned settings: %s", esp_err_to_name(error));
        send_marker("JUFF_PROVISION_ERROR save-failed");
        return;
    }

    ESP_LOGI(TAG, "Provisioned settings saved to NVS; restarting");
    send_marker("JUFF_PROVISION_OK");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void provisioning_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG,
             "USB provisioning ready; run .venv/bin/python scripts/provision_wifi.py");
    send_marker("JUFF_PROVISION_READY");

    uint8_t incoming[64];
    char line[PROVISIONING_LINE_SIZE];
    size_t line_length = 0;
    bool overflow = false;
    while (true) {
        const ssize_t received = read(STDIN_FILENO, incoming, sizeof(incoming));
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (ssize_t index = 0; index < received; ++index) {
            const uint8_t byte = incoming[index];
            if (byte == '\r') {
                continue;
            }
            if (byte == '\n') {
                if (overflow) {
                    send_marker("JUFF_PROVISION_ERROR request-too-large");
                } else if (line_length > 0) {
                    line[line_length] = '\0';
                    handle_provisioning_line(line);
                }
                line_length = 0;
                overflow = false;
                continue;
            }
            if (!overflow && line_length < sizeof(line) - 1) {
                line[line_length++] = (char)byte;
            } else {
                overflow = true;
            }
        }
    }
}

esp_err_t usb_provisioning_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (xTaskCreate(provisioning_task,
                    "juff_usb_setup",
                    4096,
                    NULL,
                    4,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

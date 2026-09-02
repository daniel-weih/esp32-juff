#include "wifi_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_NVS_NAMESPACE "juff"
#define WIFI_NVS_SSID_KEY "wifi_ssid"
#define WIFI_NVS_PASSWORD_KEY "wifi_pass"
#define WIFI_SSID_BUFFER_SIZE 32
#define WIFI_PASSWORD_BUFFER_SIZE 64

static const char *TAG = "juff_wifi";
static EventGroupHandle_t s_events;
static bool s_started;

static esp_err_t load_credentials(char ssid[WIFI_SSID_BUFFER_SIZE],
                                  char password[WIFI_PASSWORD_BUFFER_SIZE])
{
    strlcpy(ssid, CONFIG_JUFF_WIFI_SSID, WIFI_SSID_BUFFER_SIZE);
    strlcpy(password, CONFIG_JUFF_WIFI_PASSWORD, WIFI_PASSWORD_BUFFER_SIZE);

    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }

    char stored_ssid[WIFI_SSID_BUFFER_SIZE] = { 0 };
    size_t ssid_size = sizeof(stored_ssid);
    error = nvs_get_str(handle, WIFI_NVS_SSID_KEY, stored_ssid, &ssid_size);
    if (error == ESP_OK && stored_ssid[0] != '\0') {
        char stored_password[WIFI_PASSWORD_BUFFER_SIZE] = { 0 };
        size_t password_size = sizeof(stored_password);
        const esp_err_t password_error = nvs_get_str(handle,
                                                     WIFI_NVS_PASSWORD_KEY,
                                                     stored_password,
                                                     &password_size);
        if (password_error != ESP_OK && password_error != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return password_error;
        }
        strlcpy(ssid, stored_ssid, WIFI_SSID_BUFFER_SIZE);
        strlcpy(password, stored_password, WIFI_PASSWORD_BUFFER_SIZE);
        ESP_LOGI(TAG, "Using Wi-Fi credentials provisioned over USB");
        error = ESP_OK;
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    nvs_close(handle);
    return error;
}

static void handle_wifi_event(void *argument,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    (void)argument;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u), reconnecting", event->reason);
        esp_wifi_connect();
    }
}

static void handle_ip_event(void *argument,
                            esp_event_base_t event_base,
                            int32_t event_id,
                            void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event = event_data;
    ESP_LOGI(TAG, "Wi-Fi ready, address=" IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
}

esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    char ssid[WIFI_SSID_BUFFER_SIZE] = { 0 };
    char password[WIFI_PASSWORD_BUFFER_SIZE] = { 0 };
    esp_err_t error = load_credentials(ssid, password);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Unable to load Wi-Fi credentials: %s", esp_err_to_name(error));
        return error;
    }
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "JUFF_WIFI_SSID is empty; staying in USB diagnostic mode");
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init_config);
    if (error != ESP_OK) {
        return error;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               handle_wifi_event,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               handle_ip_event,
                                               NULL));

    wifi_config_t station_config = { 0 };
    strlcpy((char *)station_config.sta.ssid,
            ssid,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password,
            password,
            sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    error = esp_wifi_start();
    if (error == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "Connecting to Wi-Fi SSID %s", ssid);
    }
    return error;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length >= WIFI_SSID_BUFFER_SIZE
        || password_length >= WIFI_PASSWORD_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, WIFI_NVS_SSID_KEY, ssid);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, WIFI_NVS_PASSWORD_KEY, password);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

bool wifi_manager_has_credentials(void)
{
    char ssid[WIFI_SSID_BUFFER_SIZE] = { 0 };
    char password[WIFI_PASSWORD_BUFFER_SIZE] = { 0 };
    return load_credentials(ssid, password) == ESP_OK && ssid[0] != '\0';
}

bool wifi_manager_wait_connected(unsigned timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(s_events,
                                                  WIFI_CONNECTED_BIT,
                                                  pdFALSE,
                                                  pdFALSE,
                                                  pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_events != NULL
        && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

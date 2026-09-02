#include <inttypes.h>

#include "audio_io.h"
#include "ble_manager.h"
#include "board_display.h"
#include "device_client.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "usb_provisioning.h"
#include "wifi_manager.h"

static const char *TAG = "juff";

static void microphone_pcm(const uint8_t *data, size_t size, void *context)
{
    (void)context;
    if (ble_manager_is_voice_ready()) {
        ble_manager_send_pcm(data, size);
    } else {
        device_client_send_pcm(data, size);
    }
}

static void playback_event(const char *event_type,
                           const char *response_id,
                           void *context)
{
    (void)context;
    if (ble_manager_audio_is_connected()) {
        ble_manager_send_playback_event(event_type, response_id);
    } else {
        device_client_send_playback_event(event_type, response_id);
    }
}

static void send_interrupt(void)
{
    if (ble_manager_is_voice_ready()) {
        ble_manager_send_interrupt();
    } else {
        device_client_send_interrupt();
    }
}

static void interrupt_button_task(void *argument)
{
    (void)argument;
    bool was_released = true;
    while (true) {
        const bool pressed = gpio_get_level(CONFIG_JUFF_BUTTON_GPIO) == 0;
        if (pressed && was_released) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(CONFIG_JUFF_BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Interrupt button pressed");
                send_interrupt();
                was_released = false;
            }
        } else if (!pressed) {
            was_released = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void initialize_button(void)
{
    if (CONFIG_JUFF_BUTTON_GPIO < 0) {
        return;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_JUFF_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    xTaskCreate(interrupt_button_task,
                "juff_button",
                2048,
                NULL,
                4,
                NULL);
}

static void log_hardware(void)
{
    esp_chip_info_t chip = { 0 };
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_flash_get_size(NULL, &flash_size));
    ESP_LOGI(TAG,
             "ESP32-S3 revision %u.%u, %u core(s), flash=%" PRIu32 " MB, PSRAM=%u KB",
             chip.revision / 100,
             chip.revision % 100,
             chip.cores,
             flash_size / (1024 * 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    ESP_LOGI(TAG, "JUFF voice companion 0.5.0 starting");
    log_hardware();

    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    error = audio_io_init(microphone_pcm, playback_event, NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG,
                 "Audio hardware unavailable: %s; continuing with networking diagnostics",
                 esp_err_to_name(error));
    }

    const esp_err_t display_error = board_display_init();
    if (display_error != ESP_OK) {
        ESP_LOGE(TAG,
                 "LCD/touch initialization failed: %s",
                 esp_err_to_name(display_error));
    }

    if (error == ESP_OK && !wifi_manager_has_credentials()) {
        board_display_set_notice("A quick sound check",
                                 "You'll hear two soft tones",
                                 0);
        error = audio_io_run_self_test();
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "USB audio self-test failed: %s", esp_err_to_name(error));
            board_display_set_notice("Sound needs attention",
                                     "Open the Mac dashboard for details",
                                     4500);
        } else {
            board_display_set_notice("JUFF is ready",
                                     "Your speaker and microphone are working",
                                     2200);
        }
    }
    initialize_button();

    error = usb_provisioning_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to start USB provisioning: %s", esp_err_to_name(error));
    }

    error = ble_manager_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to start Bluetooth audio transport: %s", esp_err_to_name(error));
        board_display_set_notice("BLUETOOTH ERROR",
                                 esp_err_to_name(error),
                                 5000);
    }

    error = wifi_manager_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG,
                 "Wi-Fi not started. Configure Juff voice terminal > Wi-Fi SSID, then rebuild.");
        return;
    }
    while (!wifi_manager_wait_connected(10000)) {
        ESP_LOGW(TAG, "Still waiting for Wi-Fi");
    }

    error = device_client_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Unable to start device bridge: %s", esp_err_to_name(error));
    }
}

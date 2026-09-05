#include "power_manager.h"

#include "board_config.h"

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
#include "audio_io.h"
#include "battery_monitor.h"
#include "board_display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "power_button.h"

static const char *TAG = "power_manager";

static void stop_radios(void *argument)
{
    TaskHandle_t caller = argument;
    // Wi-Fi may never have been configured. Stopping it is harmless then.
    const esp_err_t error = esp_wifi_stop();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "Wi-Fi stop: %s", esp_err_to_name(error));
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        const int result = nimble_port_stop();
        if (result != 0) {
            ESP_LOGW(TAG, "Bluetooth host stop: %d", result);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_controller_disable());
    }
    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

static void power_off(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_display_power_off());

    // A broken radio link must not indefinitely prevent the power key working.
    if (xTaskCreate(stop_radios, "juff_radio_off", 4096,
                    xTaskGetCurrentTaskHandle(), 4, NULL) == pdPASS) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000)) == 0) {
            ESP_LOGW(TAG, "Radio shutdown timed out; continuing to sleep");
        }
    }

    // No timer or touch wakeup: a new PWR press wakes a USB-powered board.
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        1ULL << BOARD_POWER_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOARD_POWER_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOARD_POWER_BUTTON_GPIO));

    // Keep amplifier and backlight low even while USB continues supplying VSYS.
    // Detach PWM after the display has permanently blocked brightness changes.
    ESP_ERROR_CHECK(gpio_reset_pin(LCD_PIN_BACKLIGHT));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 0));
    ESP_ERROR_CHECK(gpio_set_direction(LCD_PIN_BACKLIGHT, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_en(LCD_PIN_BACKLIGHT));
#if CONFIG_JUFF_CODEC_PA_GPIO >= 0
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_JUFF_CODEC_PA_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_direction(CONFIG_JUFF_CODEC_PA_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_en(CONFIG_JUFF_CODEC_PA_GPIO));
#endif

    // Radio teardown can take time. Require a fresh stable release immediately
    // before sleep too, otherwise a second press could wake us straight back up.
    unsigned released_ms = 0;
    while (released_ms < POWER_BUTTON_DEBOUNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        released_ms = gpio_get_level(BOARD_POWER_BUTTON_GPIO) ? released_ms + 10 : 0;
    }
    ESP_LOGI(TAG, "Power off: battery hold low; USB fallback is deep sleep, PWR wakes");
    // The physical PWR switch also enables the battery rail, hence release first.
    ESP_ERROR_CHECK(gpio_set_level(BOARD_POWER_HOLD_GPIO, 0));
    ESP_ERROR_CHECK(gpio_hold_en(BOARD_POWER_HOLD_GPIO));
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}

static void power_button_task(void *argument)
{
    (void)argument;
    power_button_t button = { 0 };
    while (true) {
        const power_button_event_t event = power_button_update(
            &button, gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 0,
            (uint32_t)(esp_timer_get_time() / 1000));
        if (event == POWER_BUTTON_PREPARE) {
            ESP_LOGI(TAG, "PWR held for 3 seconds; release to power off");
            battery_monitor_stop();
            board_display_prepare_shutdown();
            ESP_ERROR_CHECK_WITHOUT_ABORT(audio_io_shutdown());
        } else if (event == POWER_BUTTON_OFF) {
            power_off();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

void power_manager_hold(void)
{
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    // Configure the desired high level before releasing sleep's output hold.
    ESP_ERROR_CHECK(gpio_set_level(BOARD_POWER_HOLD_GPIO, 1));
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_deep_sleep_hold_dis();
    ESP_ERROR_CHECK(gpio_hold_dis(BOARD_POWER_HOLD_GPIO));
    ESP_ERROR_CHECK(gpio_hold_dis(LCD_PIN_BACKLIGHT));
#if CONFIG_JUFF_CODEC_PA_GPIO >= 0
    ESP_ERROR_CHECK(gpio_hold_dis(CONFIG_JUFF_CODEC_PA_GPIO));
#endif
    ESP_ERROR_CHECK(rtc_gpio_deinit(BOARD_POWER_BUTTON_GPIO));
#endif
}

esp_err_t power_manager_start(void)
{
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure PWR key");
    if (xTaskCreate(power_button_task, "juff_power", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PWR GPIO%d: hold 3 seconds, then release to power off",
             BOARD_POWER_BUTTON_GPIO);
#endif
    return ESP_OK;
}

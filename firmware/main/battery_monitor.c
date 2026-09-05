#include "battery_monitor.h"

#include "board_config.h"

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
#include <limits.h>

#include "battery_estimate.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BATTERY_SAMPLE_INTERVAL_US INT64_C(2000000)
#define BATTERY_SAMPLE_COUNT 8

static const char *TAG = "battery_monitor";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_status_t s_status = { .supported = true, .percent = -1 };
static battery_estimate_t s_estimate;
static battery_charge_filter_t s_charge_filter;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_calibration;
static bool s_init_called;
static bool s_charge_ready;
static bool s_reported;
static bool s_stop_requested;
static esp_err_t s_init_result;
static int64_t s_next_sample_us;

static void battery_worker(void *argument);

static void release_adc(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s_calibration != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_calibration);
        s_calibration = NULL;
    }
#endif
    if (s_adc != NULL) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}

static bool stop_requested(void)
{
    portENTER_CRITICAL(&s_status_lock);
    const bool stopped = s_stop_requested;
    portEXIT_CRITICAL(&s_status_lock);
    return stopped;
}

static esp_err_t sample_voltage(int *battery_mv)
{
    int total = 0;
    int minimum = INT_MAX;
    int maximum = 0;
    // Fixed count, no retries or delay. IDF returns lock-contention/conversion
    // errors without aborting the app. Its HAL conversion wait has no software
    // deadline, so this is not a hard wall-clock timeout against hardware faults.
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; ++i) {
        if (stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        int raw = 0;
        const esp_err_t error = adc_oneshot_read(s_adc, BOARD_BATTERY_ADC_CHANNEL, &raw);
        if (error != ESP_OK) {
            return error;
        }
        if (stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        total += raw;
        if (raw < minimum) minimum = raw;
        if (raw > maximum) maximum = raw;
    }
    const int average = (total - minimum - maximum) / (BATTERY_SAMPLE_COUNT - 2);
    int pin_mv = 0;
    const esp_err_t error = adc_cali_raw_to_voltage(s_calibration, average, &pin_mv);
    if (error != ESP_OK) {
        return error;
    }
    // Reject impossible calibration output before multiplication, then check
    // the narrower plausible cell range in the shared estimator.
    if (pin_mv < 0 || pin_mv > 2000) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *battery_mv = pin_mv * BOARD_BATTERY_DIVIDER_RATIO;
    return ESP_OK;
}

esp_err_t battery_monitor_init(void)
{
    if (s_init_called) {
        return s_init_result;
    }
    s_init_called = true;
    if (stop_requested()) {
        s_init_result = ESP_ERR_INVALID_STATE;
        return s_init_result;
    }
    const gpio_config_t charge_config = {
        .pin_bit_mask = 1ULL << BOARD_BATTERY_CHARGE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    s_init_result = gpio_config(&charge_config);
    if (s_init_result != ESP_OK) {
        ESP_LOGW(TAG, "Charging input unavailable: %s", esp_err_to_name(s_init_result));
        return s_init_result;
    }
    s_charge_ready = true;
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    s_init_result = adc_oneshot_new_unit(&unit_config, &s_adc);
    if (s_init_result != ESP_OK) {
        goto unavailable;
    }
    // VBAT / 3 is 0.87–1.45 V over the accepted cell range. ESP32-S3's
    // calibrated 6 dB range reaches 1.6 V and has lower specified error than
    // 12 dB (datasheet Table 5-6). Use the SAME attenuation for calibration.
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_init_result = adc_oneshot_config_channel(s_adc, BOARD_BATTERY_ADC_CHANNEL,
                                              &channel_config);
    if (s_init_result != ESP_OK) {
        goto unavailable;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BOARD_BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_init_result = adc_cali_create_scheme_curve_fitting(&calibration_config, &s_calibration);
#else
    s_init_result = ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_init_result != ESP_OK) {
        goto unavailable;
    }
    ESP_LOGI(TAG, "%s battery: GPIO%d, divider x%d; charging GPIO%d active low",
             JUFF_BOARD_ID, BOARD_BATTERY_ADC_GPIO, BOARD_BATTERY_DIVIDER_RATIO,
             BOARD_BATTERY_CHARGE_GPIO);
    goto start_worker;

unavailable:
    release_adc();
    ESP_LOGW(TAG, "Calibrated battery voltage unavailable: %s", esp_err_to_name(s_init_result));

start_worker:
    // Charging status remains useful even when calibrated voltage is unavailable.
    if (xTaskCreate(battery_worker, "juff_battery", 3072, NULL, 2, NULL) != pdPASS) {
        release_adc();
        s_init_result = ESP_ERR_NO_MEM;
    }
    return s_init_result;
}

static void battery_poll(void)
{
    if (!s_init_called || !s_charge_ready) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const bool charging = battery_charge_update(
        &s_charge_filter, gpio_get_level(BOARD_BATTERY_CHARGE_GPIO) == 0,
        (uint32_t)(now_us / 1000));
    battery_estimate_set_charging(&s_estimate, charging, (uint32_t)(now_us / 1000));
    if (s_adc != NULL && s_calibration != NULL && now_us >= s_next_sample_us) {
        s_next_sample_us = now_us + BATTERY_SAMPLE_INTERVAL_US;
        int battery_mv = 0;
        const esp_err_t error = sample_voltage(&battery_mv);
        battery_estimate_update(&s_estimate, error == ESP_OK, battery_mv,
                                charging, (uint32_t)(now_us / 1000));
    }
    const battery_status_t updated = {
        .supported = true,
        .valid = s_estimate.valid,
        .charging = charging,
        .percent = s_estimate.valid ? s_estimate.percent : -1,
        .millivolts = s_estimate.valid ? s_estimate.millivolts : 0,
    };
    portENTER_CRITICAL(&s_status_lock);
    bool report = false;
    if (!s_stop_requested) {
        report = !s_reported || s_status.valid != updated.valid ||
                 s_status.charging != updated.charging ||
                 (updated.valid && s_status.percent != updated.percent);
        s_status = updated;
        s_reported = true;
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (report) {
        if (updated.valid && updated.percent >= 0) {
            ESP_LOGI(TAG, "Battery: %dmV, estimated %d%%, charging=%s",
                     updated.millivolts, updated.percent,
                     updated.charging ? "yes" : "no");
        } else {
            ESP_LOGW(TAG, "Battery: voltage unavailable, charging=%s",
                     updated.charging ? "yes" : "no");
        }
    }
}

static void battery_worker(void *argument)
{
    (void)argument;
    while (!stop_requested()) {
        battery_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    release_adc();
    vTaskDelete(NULL);
}

void battery_monitor_stop(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_stop_requested = true;
    portEXIT_CRITICAL(&s_status_lock);
}

battery_status_t battery_monitor_get(void)
{
    portENTER_CRITICAL(&s_status_lock);
    const battery_status_t status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return status;
}
#else
esp_err_t battery_monitor_init(void)
{
    return ESP_OK;
}

void battery_monitor_stop(void)
{
}

battery_status_t battery_monitor_get(void)
{
    return (battery_status_t){ .percent = -1 };
}
#endif

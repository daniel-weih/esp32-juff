#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { int unit_id, ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten, bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { int unit_id, chan, atten, bitwidth; } adc_cali_curve_fitting_config_t;

#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_RESPONSE 5
#define ESP_ERR_INVALID_STATE 6
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#define ADC_UNIT_1 1
#define ADC_CHANNEL_0 0
#define ADC_ULP_MODE_DISABLE 0
#define ADC_ATTEN_DB_6 6
#define ADC_BITWIDTH_DEFAULT 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) (++critical_depth)
#define portEXIT_CRITICAL(lock) (--critical_depth)
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

#include "battery_monitor.h"
static const char *scenario;
static unsigned critical_depth, gpio_configs, adc_creates, adc_deletes, cal_creates, cal_deletes;
static unsigned task_creates, reads, calibrations, stop_at_read, fail_at_read;
static bool charging_pin_low, conversion_failure, worker_deleted;
static int64_t now_us;
static int nominal_raw = 1250, forced_mv = -1, last_calibrated_raw;
static void (*worker)(void *);

static int gpio_config(const gpio_config_t *config)
{
    ++gpio_configs;
    assert(config->pin_bit_mask == (1ULL << 3) && config->mode == GPIO_MODE_INPUT);
    assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    return strcmp(scenario, "gpio-failure") == 0 ? ESP_FAIL : ESP_OK;
}
static int gpio_get_level(int pin) { assert(pin == 3); return !charging_pin_low; }
static int64_t esp_timer_get_time(void) { return now_us; }
static int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *config, void **handle)
{
    ++adc_creates;
    assert(critical_depth == 0 && config->unit_id == ADC_UNIT_1);
    assert(config->ulp_mode == ADC_ULP_MODE_DISABLE);
    if (strcmp(scenario, "adc-failure") == 0) return ESP_FAIL;
    *handle = (void *)1;
    return ESP_OK;
}
static int adc_oneshot_config_channel(void *handle, int channel, const adc_oneshot_chan_cfg_t *config)
{
    assert(handle == (void *)1 && channel == ADC_CHANNEL_0);
    assert(config->atten == ADC_ATTEN_DB_6 && config->bitwidth == ADC_BITWIDTH_DEFAULT);
    return strcmp(scenario, "channel-failure") == 0 ? ESP_FAIL : ESP_OK;
}
static int adc_oneshot_del_unit(void *handle)
{ assert(critical_depth == 0 && handle == (void *)1); ++adc_deletes; return ESP_OK; }
static int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config, void **handle)
{
    ++cal_creates;
    assert(config->unit_id == ADC_UNIT_1 && config->chan == ADC_CHANNEL_0);
    assert(config->atten == ADC_ATTEN_DB_6 && config->bitwidth == ADC_BITWIDTH_DEFAULT);
    if (strcmp(scenario, "calibration-failure") == 0) return ESP_ERR_NOT_SUPPORTED;
    *handle = (void *)2;
    return ESP_OK;
}
static int adc_cali_delete_scheme_curve_fitting(void *handle)
{ assert(critical_depth == 0 && handle == (void *)2); ++cal_deletes; return ESP_OK; }
static int adc_oneshot_read(void *handle, int channel, int *raw)
{
    assert(critical_depth == 0 && handle == (void *)1 && channel == ADC_CHANNEL_0);
    ++reads;
    if (reads == fail_at_read) return ESP_ERR_TIMEOUT;
    // Two outliers bracket six identical values in each complete sample batch.
    const unsigned index = (reads - 1) % 8;
    *raw = index == 0 ? 0 : index == 7 ? 4095 : nominal_raw;
    if (reads == stop_at_read) battery_monitor_stop();
    return ESP_OK;
}
static int adc_cali_raw_to_voltage(void *handle, int raw, int *millivolts)
{
    assert(critical_depth == 0 && handle == (void *)2);
    ++calibrations;
    last_calibrated_raw = raw;
    if (conversion_failure) return ESP_FAIL;
    *millivolts = forced_mv >= 0 ? forced_mv : raw;
    return ESP_OK;
}
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, void *handle)
{
    ++task_creates;
    assert(critical_depth == 0 && priority < 3 && stack <= 4096);
    if (strcmp(scenario, "task-failure") == 0) return 0;
    worker = task;
    return pdPASS;
}
static void vTaskDelay(unsigned milliseconds) { assert(milliseconds == 10); now_us += 10000; }
static void vTaskDelete(void *task) { worker_deleted = true; }

#include "battery_monitor.c"

static void assert_invalid(void)
{
    const battery_status_t status = battery_monitor_get();
    assert(!status.valid && status.percent == -1 && status.millivolts == 0);
}

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
static void poll_at(int64_t timestamp)
{
    now_us = timestamp;
    battery_poll();
    assert(critical_depth == 0);
}

static void sampling(void)
{
    assert(battery_monitor_init() == ESP_OK);
    assert(task_creates == 1 && worker != NULL && reads == 0);
    assert_invalid();
    charging_pin_low = true;
    poll_at(0);
    battery_status_t status = battery_monitor_get();
    assert(reads == 8 && calibrations == 1 && last_calibrated_raw == nominal_raw);
    assert(status.supported && status.valid && status.millivolts == 3750
           && status.percent == 30);
    assert(!status.charging);
    poll_at(149000);
    assert(!battery_monitor_get().charging && reads == 8);
    poll_at(150000);
    assert(battery_monitor_get().charging && reads == 8);
    poll_at(1999999);
    assert(reads == 8);
    poll_at(2000000);
    assert(reads == 16 && calibrations == 2);
}

static void measurement_failures(void)
{
    assert(battery_monitor_init() == ESP_OK);
    poll_at(0);
    assert(battery_monitor_get().valid);
    fail_at_read = reads + 3;
    poll_at(2000000);
    assert(reads == fail_at_read && calibrations == 1);
    assert_invalid();
    fail_at_read = 0;
    conversion_failure = true;
    poll_at(4000000);
    assert_invalid();
    conversion_failure = false;
    forced_mv = 0; // A missing/implausible voltage is unknown, not fake 0% or 100%.
    poll_at(6000000);
    assert_invalid();
    forced_mv = 2001;
    poll_at(8000000);
    assert_invalid();
    forced_mv = 1200;
    poll_at(10000000);
    const battery_status_t recovered = battery_monitor_get();
    assert(recovered.valid && recovered.millivolts == 3600 && recovered.percent == 10);
    poll_at(12000000);
    assert(battery_monitor_get().percent == 10);

}

static void charge_transition(void)
{
    assert(battery_monitor_init() == ESP_OK);
    forced_mv = 1310; // 3.930 V: display the voltage estimate, including charging.
    charging_pin_low = true;
    poll_at(0);
    poll_at(150000);
    assert(battery_monitor_get().charging && battery_monitor_get().percent == 66);
    for (int64_t time = 2000000; time <= 60000000; time += 2000000) {
        poll_at(time);
        const battery_status_t status = battery_monitor_get();
        assert(status.charging && status.valid && status.millivolts == 3930
               && status.percent == 66);
    }
    // The debounced edge preserves the last number until a real fresh sample.
    charging_pin_low = false;
    forced_mv = 1264; // 3.792 V; a synthetic transition, not measured cell data.
    poll_at(61000000);
    poll_at(61149000);
    assert(battery_monitor_get().charging);
    poll_at(61150000);
    assert(!battery_monitor_get().charging && battery_monitor_get().percent == 66);
    poll_at(62000000);
    assert(battery_monitor_get().valid && battery_monitor_get().percent == 38);
    // No charge-era filter tail, no wait, and no forced 5% display bucket.
    poll_at(64000000);
    assert(battery_monitor_get().percent == 38);
    charging_pin_low = true;
    forced_mv = 1310;
    poll_at(65000000);
    poll_at(65150000);
    assert(battery_monitor_get().charging && battery_monitor_get().percent == 38);
    poll_at(66000000);
    assert(battery_monitor_get().percent == 66);
}

static void stop_in_read(void)
{
    assert(battery_monitor_init() == ESP_OK);
    poll_at(0);
    const battery_status_t previous = battery_monitor_get();
    assert(previous.valid);
    now_us = 2000000;
    stop_at_read = reads + 4;
    forced_mv = 1400;
    worker(NULL);
    assert(worker_deleted && reads == stop_at_read && calibrations == 1);
    assert(adc_deletes == 1 && cal_deletes == 1);
    const battery_status_t stopped = battery_monitor_get();
    assert(stopped.valid == previous.valid && stopped.percent == previous.percent
           && stopped.millivolts == previous.millivolts && stopped.charging == previous.charging);
    battery_monitor_stop();
    (void)battery_monitor_init();
    assert(task_creates == 1 && reads == stop_at_read);
}

static void init_failure(void)
{
    assert(battery_monitor_init() != ESP_OK);
    assert_invalid();
    assert(reads == 0);
    if (strcmp(scenario, "gpio-failure") == 0) {
        assert(task_creates == 0 && adc_creates == 0);
    } else if (strcmp(scenario, "task-failure") == 0) {
        assert(worker == NULL && adc_deletes == 1 && cal_deletes == 1);
    } else {
        assert(worker != NULL && task_creates == 1);
        assert(adc_deletes == (strcmp(scenario, "adc-failure") == 0 ? 0U : 1U));
        assert(cal_deletes == 0);
        charging_pin_low = true;
        poll_at(0);
        poll_at(150000);
        assert_invalid();
        assert(battery_monitor_get().charging && reads == 0);
    }
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    if (strcmp(scenario, "sampling") == 0) sampling();
    else if (strcmp(scenario, "measurement-failures") == 0) measurement_failures();
    else if (strcmp(scenario, "charge-transition") == 0) charge_transition();
    else if (strcmp(scenario, "stop-in-read") == 0) stop_in_read();
    else if (strcmp(scenario, "stop-before-init") == 0) {
        battery_monitor_stop();
        assert(battery_monitor_init() == ESP_ERR_INVALID_STATE);
        assert_invalid();
        assert(task_creates == 0 && reads == 0 && adc_creates == 0 && gpio_configs == 0);
    } else init_failure();
#else
    assert(battery_monitor_init() == ESP_OK);
    battery_monitor_stop();
    assert_invalid();
    assert(!battery_monitor_get().supported);
    assert(task_creates == 0 && reads == 0 && adc_creates == 0 && gpio_configs == 0);
#endif
    assert(critical_depth == 0);
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef int esp_err_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) ((void)pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) ((void)pthread_mutex_unlock(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_FAIL 4
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(result) ((void)(result))
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0
#define LEDC_TIMER_1 1
#define LEDC_TIMER_10_BIT 10
#define LEDC_AUTO_CLK 0
#define LEDC_INTR_DISABLE 0
#define LCD_PIN_BACKLIGHT 42

typedef struct {
    int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg;
} ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, intr_type, timer_sel, duty, hpoint;
} ledc_channel_config_t;

#include "display_state.inc"

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t driver_condition = PTHREAD_COND_INITIALIZER;
static bool fail_lock, fail_update, stall_brightness, driver_entered, release_driver;
static unsigned driver_calls, notice_calls;
static atomic_uint requested_lock_ms;
static uint32_t pending_duty, visible_duty;

static void pause_ms(void)
{
    const struct timespec pause = { .tv_nsec = 1000000 };
    nanosleep(&pause, NULL);
}

static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait_ms)
{
    requested_lock_ms = wait_ms;
    if (fail_lock) return 0;
    // The concurrency case controls exactly when the driver is released; use
    // a real blocking mutex so host scheduler load cannot create a false timeout.
    if (stall_brightness) return pthread_mutex_lock(mutex) == 0 ? pdTRUE : 0;
    for (unsigned elapsed = 0; elapsed < wait_ms; ++elapsed) {
        if (pthread_mutex_trylock(mutex) == 0) return pdTRUE;
        pause_ms();
    }
    return 0;
}

static void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
}

static esp_err_t ledc_timer_config(const ledc_timer_config_t *config)
{
    (void)config;
    ++driver_calls;
    return ESP_OK;
}

static esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    ++driver_calls;
    assert(config->duty == 0);
    pending_duty = visible_duty = 0;
    return ESP_OK;
}

static esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty)
{
    (void)mode;
    (void)channel;
    ++driver_calls;
    pthread_mutex_lock(&driver_mutex);
    if (stall_brightness && duty != 0) {
        driver_entered = true;
        pthread_cond_broadcast(&driver_condition);
        while (!release_driver) pthread_cond_wait(&driver_condition, &driver_mutex);
    }
    pending_duty = duty;
    pthread_mutex_unlock(&driver_mutex);
    return ESP_OK;
}

static esp_err_t ledc_update_duty(int mode, int channel)
{
    (void)mode;
    (void)channel;
    ++driver_calls;
    if (fail_update) return ESP_FAIL;
    visible_duty = pending_duty;
    return ESP_OK;
}

static void board_display_set_notice(const char *title, const char *detail, uint32_t ms)
{
    (void)title;
    (void)detail;
    (void)ms;
    ++notice_calls;
}

#include "display_shutdown.inc"

static void start_display(void)
{
    s_state_mutex = &state_mutex;
    assert(initialize_backlight() == ESP_OK);
    assert(set_brightness(100) == ESP_OK);
    assert(visible_duty == 1023);
}

static void *brighten_thread(void *context)
{
    (void)context;
    assert(set_brightness(65) == ESP_OK);
    return NULL;
}

static void *power_off_thread(void *context)
{
    (void)context;
    assert(board_display_power_off() == ESP_OK);
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "uninitialized") == 0) {
        board_display_prepare_shutdown();
        assert(board_display_power_off() == ESP_OK);
        board_display_cycle_brightness();
        assert(driver_calls == 0 && notice_calls == 0);
        s_state_mutex = &state_mutex;
        assert(board_display_power_off() == ESP_OK);
        assert(initialize_backlight() == ESP_ERR_INVALID_STATE);
        assert(driver_calls == 0);
    } else if (strcmp(argv[1], "prepare") == 0) {
        start_display();
        board_display_prepare_shutdown();
        const unsigned before = driver_calls;
        board_display_cycle_brightness();
        assert(set_brightness(100) == ESP_ERR_INVALID_STATE);
        assert(driver_calls == before && notice_calls == 0 && visible_duty == 1023);
        assert(board_display_power_off() == ESP_OK);
        assert(visible_duty == 0);
        board_display_prepare_shutdown();
        assert(display_shutdown_state() == DISPLAY_POWERED_OFF);
        const unsigned off_calls = driver_calls;
        assert(board_display_power_off() == ESP_OK);
        board_display_cycle_brightness();
        assert(driver_calls == off_calls && visible_duty == 0);
    } else if (strcmp(argv[1], "retry") == 0) {
        start_display();
        fail_lock = true;
        assert(board_display_power_off() == ESP_ERR_TIMEOUT);
        assert(requested_lock_ms == 100);
        assert(display_shutdown_state() == DISPLAY_POWERED_OFF);
        fail_lock = false;
        assert(set_brightness(65) == ESP_ERR_INVALID_STATE);
        fail_update = true;
        assert(board_display_power_off() == ESP_FAIL);
        fail_update = false;
        assert(board_display_power_off() == ESP_OK);
        assert(visible_duty == 0);
    } else if (strcmp(argv[1], "inflight") == 0) {
        start_display();
        stall_brightness = true;
        pthread_t brightness, shutdown;
        assert(pthread_create(&brightness, NULL, brighten_thread, NULL) == 0);
        pthread_mutex_lock(&driver_mutex);
        while (!driver_entered) pthread_cond_wait(&driver_condition, &driver_mutex);
        pthread_mutex_unlock(&driver_mutex);
        assert(pthread_create(&shutdown, NULL, power_off_thread, NULL) == 0);
        while (display_shutdown_state() != DISPLAY_POWERED_OFF) pause_ms();
        pthread_mutex_lock(&driver_mutex);
        release_driver = true;
        pthread_cond_broadcast(&driver_condition);
        pthread_mutex_unlock(&driver_mutex);
        assert(pthread_join(brightness, NULL) == 0);
        assert(pthread_join(shutdown, NULL) == 0);
        assert(visible_duty == 0);
        assert(set_brightness(100) == ESP_ERR_INVALID_STATE);
        assert(visible_duty == 0);
    } else {
        assert(!"Unknown test case");
    }
    puts("Display shutdown passed");
    return 0;
}

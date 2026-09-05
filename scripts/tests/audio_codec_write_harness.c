#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_pcm_format.h"
#include "codec_write_config.inc"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -10
#define ESP_ERR_INVALID_ARG 101
#define ESP_ERR_INVALID_STATE 102
#define ESP_ERR_TIMEOUT 103
#define CODEC_SAMPLE_RATE 24000
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(call) assert((call) == ESP_OK)

enum { INPUT_FRAMES = 1001 };
static uint8_t observed[INPUT_FRAMES * 4];
static size_t observed_size, write_calls, maximum_progress;
static bool locked, mutex_available = true, amplifier = true;
static unsigned locks, unlocks, disables, enables;
static uint32_t s_codec_write_generation;
static int64_t time_us;
static void *s_tx_channel = &observed, *s_codec_write_mutex = &locked;
static const char *scenario;

static uint32_t codec_write_generation(void);
static void cancel_codec_writes(void);
static int xSemaphoreTake(void *mutex, unsigned wait)
{
    assert(mutex == s_codec_write_mutex && !locked);
    if (!mutex_available) return 0;
    locked = true;
    ++locks;
    return pdTRUE;
}
static void xSemaphoreGive(void *mutex)
{
    assert(locked);
    locked = false;
    ++unlocks;
}
static int64_t esp_timer_get_time(void) { return time_us; }
static esp_err_t i2s_channel_disable(void *channel)
{
    assert(locked && !amplifier);
    ++disables;
    return ESP_OK;
}
static esp_err_t i2s_channel_enable(void *channel)
{
    assert(locked && disables == enables + 1);
    ++enables;
    return ESP_OK;
}
static esp_err_t set_amplifier_enabled(bool enabled)
{
    assert(locked);
    amplifier = enabled;
    return ESP_OK;
}
static esp_err_t i2s_channel_write(void *channel, const void *data, size_t size,
                                  size_t *written, unsigned timeout_ms)
{
    assert(locked && channel == s_tx_channel);
    assert(++write_calls < 10000);
    assert(timeout_ms > 0 && timeout_ms <= 20);
    assert(size <= CODEC_WRITE_CHUNK_FRAMES * CODEC_TX_CHANNELS * sizeof(int16_t));
    if (strcmp(scenario, "timeout-empty") == 0) {
        *written = 0;
        return ESP_ERR_TIMEOUT;
    }
    *written = maximum_progress != 0 && size > maximum_progress ? maximum_progress : size;
    assert(observed_size + *written <= sizeof(observed));
    memcpy(observed + observed_size, data, *written);
    observed_size += *written;
    if (strcmp(scenario, "cancel") == 0) cancel_codec_writes();
    if (strcmp(scenario, "deadline") == 0) time_us += 20000;
    return strcmp(scenario, "partial-timeout") == 0 ? ESP_ERR_TIMEOUT : ESP_OK;
}

#include "codec_write.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    const bool stereo = CONFIG_JUFF_BOARD_WAVESHARE_LCD_35 && CONFIG_JUFF_VOICE_BARGE_IN;
    assert(CODEC_TX_CHANNELS == (stereo ? 2 : 1));
    int16_t input[INPUT_FRAMES];
    for (size_t i = 0; i < INPUT_FRAMES; ++i) input[i] = (int16_t)(i * 319 - 32768);
    if (strcmp(scenario, "format") != 0) maximum_progress = 3;
    uint32_t generation = codec_write_generation();
    if (strcmp(scenario, "stale") == 0) cancel_codec_writes();
    if (strcmp(scenario, "mutex") == 0) mutex_available = false;
    const esp_err_t result = write_codec_pcm_mono(input, sizeof(input), generation);
    assert(!locked && locks == unlocks);
    if (strcmp(scenario, "format") == 0 || strcmp(scenario, "partial-timeout") == 0) {
        assert(result == ESP_OK && locks == 1);
        assert(observed_size == INPUT_FRAMES * (stereo ? 4U : 2U));
        assert(amplifier && disables == 0 && enables == 0);
        for (size_t i = 0; i < INPUT_FRAMES; ++i) {
            assert(memcmp(observed + i * CODEC_TX_CHANNELS * 2, &input[i], 2) == 0);
            if (stereo) assert(memcmp(observed + i * 4 + 2, &input[i], 2) == 0);
        }
    } else if (strcmp(scenario, "stale") == 0 || strcmp(scenario, "mutex") == 0) {
        assert(result == (mutex_available ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT));
        assert(write_calls == 0 && locks == 0 && disables == 0);
    } else {
        assert(result == (strcmp(scenario, "cancel") == 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT));
        assert(!amplifier && disables == 1 && enables == 1);
        assert(locks == 1 && observed_size < sizeof(input));
        if (strcmp(scenario, "cancel") == 0) assert(write_calls == 1 && observed_size == 3);
        if (strcmp(scenario, "timeout-empty") == 0) assert(write_calls == 1 && observed_size == 0);
        if (strcmp(scenario, "deadline") == 0) assert(write_calls < 20 && time_us < 400000);
    }
    puts("Codec write boundary passed");
    return 0;
}

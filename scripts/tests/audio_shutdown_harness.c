#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_ARG 5
#define CONFIG_JUFF_CODEC_PA_GPIO 7
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_SPIRAM 2
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(lock) (++critical_depth)
#define portEXIT_CRITICAL(lock) (--critical_depth)

#include "audio_item.inc"
#include "audio_buffer_globals.inc"

static bool s_shutdown_requested, s_capture_enabled, s_playing, s_available;
static bool s_self_test_running, s_amplifier_enabled, s_voice_frontend_ready;
static void *s_codec = (void *)1, *s_tx_channel = (void *)2;
static void *s_codec_write_mutex = (void *)3, *s_playback_queue = (void *)4;
static void (*s_pcm_callback)(const uint8_t *, size_t, void *);
static bool (*s_barge_in_callback)(void *);
static const char *TAG = "shutdown-test";
static audio_item_t queued[4];
static unsigned queued_count, uploads, interrupts, critical_depth;
static bool writer_busy, fail_pa, fail_mute, pa_enabled, tx_enabled = true;
static unsigned mutex_depth;

static void copy_response_id(char *destination, const char *source)
{ snprintf(destination, RESPONSE_ID_SIZE, "%s", source); }
static void *heap_caps_malloc(size_t size, unsigned caps) { return malloc(size); }
static int xQueueSend(void *queue, const audio_item_t *item, unsigned timeout)
{
    assert(!s_shutdown_requested && queued_count < 4);
    queued[queued_count++] = *item;
    return pdTRUE;
}
static int xQueueReceive(void *queue, audio_item_t *item, unsigned timeout)
{
    assert(timeout == 0);
    if (!queued_count) return 0;
    *item = queued[--queued_count];
    return pdTRUE;
}
static int xSemaphoreTake(void *mutex, unsigned timeout)
{
    assert(s_shutdown_requested && timeout == 100 && mutex_depth == 0);
    if (writer_busy) return 0;
    ++mutex_depth;
    return pdTRUE;
}
static void xSemaphoreGive(void *mutex) { assert(mutex_depth == 1); --mutex_depth; }
static int gpio_set_level(int pin, bool high)
{
    assert(pin == CONFIG_JUFF_CODEC_PA_GPIO && critical_depth == 1);
    assert(!high || !s_shutdown_requested);
    if (!high && fail_pa) return ESP_FAIL;
    pa_enabled = high;
    return ESP_OK;
}
static int esp_codec_dev_set_out_mute(void *codec, bool mute)
{
    assert(s_shutdown_requested && mute);
    return fail_mute ? ESP_FAIL : ESP_OK;
}
static int i2s_channel_disable(void *channel)
{
    assert(s_shutdown_requested && mutex_depth == 1);
    if (!tx_enabled) return ESP_ERR_INVALID_STATE;
    tx_enabled = false;
    return ESP_OK;
}
static void uploaded(const uint8_t *data, size_t size, void *context) { ++uploads; }
static bool interrupted(void *context) { ++interrupts; return true; }

#include "shutdown.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    writer_busy = strcmp(argv[1], "writer-busy") == 0;
    fail_pa = strcmp(argv[1], "pa-failure") == 0;
    fail_mute = strcmp(argv[1], "mute-failure") == 0;
    s_available = s_voice_frontend_ready = true;
    s_pcm_callback = uploaded;
    s_barge_in_callback = interrupted;
    // AFE owns the ordinary upload gate, including accepted pending history.
    // This wrapper adds only permanent shutdown, not another capture gate.
    const uint8_t pcm[] = { 1, 2, 3, 4 };
    deliver_captured_pcm(pcm, sizeof(pcm), NULL);
    assert(uploads == 1 && !s_capture_enabled);
    uploads = 0;
    assert(set_amplifier_enabled(true) == ESP_OK && pa_enabled);
    audio_io_set_capture_enabled(true);
    assert(audio_io_begin_response("active", 24000) == ESP_OK);
    assert(audio_io_push_pcm(pcm, sizeof(pcm)) == ESP_OK);
    audio_item_t held;
    assert(xQueueReceive(s_playback_queue, &held, 0) == pdTRUE);
    assert(audio_io_push_pcm(pcm, sizeof(pcm)) == ESP_OK);
    const uint32_t old_generation = s_codec_write_generation;
    const esp_err_t expected = fail_pa || fail_mute ? ESP_FAIL
        : writer_busy ? ESP_ERR_TIMEOUT : ESP_OK;
    assert(audio_io_shutdown() == expected);
    assert(s_shutdown_requested && s_codec_write_generation != old_generation);
    assert(!s_capture_enabled && !s_available && !s_playing && queued_count == 0);
    assert(s_buffered_bytes == sizeof(pcm) && s_response_bytes == 0 && !s_buffering);
    // The current playback block retains its reservation until that task exits
    // its cancellable wait/write and returns ownership; shutdown frees the queue.
    free_item(&held);
    assert(s_buffered_bytes == 0);
    assert(pa_enabled == fail_pa && mutex_depth == 0 && critical_depth == 0);
    audio_io_set_capture_enabled(true);
    assert(!s_capture_enabled);
    assert(audio_io_begin_response("late", 24000) == ESP_ERR_INVALID_STATE);
    assert(audio_io_push_pcm(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE);
    assert(audio_io_end_response("late") == ESP_ERR_INVALID_STATE);
    assert(set_amplifier_enabled(true) == ESP_ERR_INVALID_STATE);
    deliver_captured_pcm(pcm, sizeof(pcm), NULL);
    assert(!request_voice_interrupt(NULL) && uploads == 0 && interrupts == 0);
    assert(!audio_io_is_available() && !audio_io_is_playing() && !audio_io_supports_voice_barge_in());
    writer_busy = fail_pa = fail_mute = false;
    assert(audio_io_shutdown() == ESP_OK && !pa_enabled && !tx_enabled);
    assert(!s_capture_enabled && queued_count == 0 && critical_depth == 0);
    puts("Audio shutdown passed");
    return 0;
}

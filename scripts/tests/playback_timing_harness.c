#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "voice_frontend.h"

#define CONFIG_JUFF_VOICE_BARGE_IN 1
#define RESPONSE_ID_SIZE 64
#define CODEC_SAMPLE_RATE 24000
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGW(...) playback_test_log(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(call) assert((call) == 0)
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define portENTER_CRITICAL_ISR(lock) ((void)0)
#define portEXIT_CRITICAL_ISR(lock) ((void)0)
#define IRAM_ATTR
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMAX_DELAY 0

#include "audio_item.inc"

static bool s_playing, s_amplifier_enabled, s_frontend_pcm_started, s_self_test_running;
static uint32_t s_frontend_playback_generation;
static void *s_playback_queue, *s_codec, *s_codec_write_mutex;
static const char *TAG = "playback-test";
static bool s_voice_frontend_ready = true;
static uint32_t s_rx_overflow_count;
static unsigned frontend_resets, warnings, tx_resets;
typedef void *i2s_chan_handle_t;
typedef struct { size_t size; } i2s_event_data_t;
void voice_frontend_reset_stream(void) { ++frontend_resets; }
static void playback_test_log(const char *tag, const char *format, ...) { ++warnings; }
#include "playback_state.inc"

static jmp_buf finished;
static audio_item_t queue[8];
static unsigned queue_count, queue_position, codec_writes, start_notifications;
static uint32_t write_generations[8];
static voice_frontend_playback_t before_first_write, previous_write;
static bool assert_active_before_final_end;

static int xQueueReceive(void *handle, audio_item_t *item, unsigned timeout)
{
    if (queue_position == queue_count) longjmp(finished, 1);
    *item = queue[queue_position++];
    if (assert_active_before_final_end && item->type == AUDIO_ITEM_END
        && strcmp(item->response_id, "active") == 0) {
        assert(s_playing && s_amplifier_enabled && codec_writes == 1);
        assert_active_before_final_end = false;
    }
    return pdTRUE;
}
static void copy_response_id(char *destination, const char *source)
{ snprintf(destination, RESPONSE_ID_SIZE, "%s", source); }
static void free_item(audio_item_t *item) { assert(item->data == NULL); }
static int set_amplifier_enabled(bool enabled)
{
    s_amplifier_enabled = enabled;
    if (enabled) {
        const voice_frontend_playback_t state = snapshot_frontend_playback();
        assert(state.playing && !state.pcm_started);
    }
    return 0;
}
static void vTaskDelay(unsigned ms)
{
    if (ms == 5) {
        const voice_frontend_playback_t state = snapshot_frontend_playback();
        assert(state.playing && !state.pcm_started);
    }
}
static int esp_codec_dev_set_out_mute(void *codec, bool muted) { return 0; }
static uint32_t codec_write_generation(void) { return 17; }
static int xSemaphoreTake(void *mutex, unsigned timeout) { return pdTRUE; }
static void xSemaphoreGive(void *mutex) {}
static int reset_codec_tx_locked(void) { ++tx_resets; return 0; }
static const char *esp_err_to_name(int error) { return "fixture error"; }
static void audio_io_clear(const char *reason) { assert(!"Unexpected playback error"); }
static void notify_playback(const char *type, const char *response)
{
    if (strcmp(type, "playback.started") == 0) {
        ++start_notifications;
        before_first_write = snapshot_frontend_playback();
        // Even a slow playback.started notification cannot advance warmup.
        assert(before_first_write.playing && !before_first_write.pcm_started);
    }
}
static esp_err_t write_codec_pcm_mono(const void *data, size_t size, uint32_t generation)
{
    assert(generation == codec_write_generation());
    const voice_frontend_playback_t state = snapshot_frontend_playback();
    assert(state.playing && state.pcm_started);
    write_generations[codec_writes] = state.generation;
    const voice_frontend_playback_t mixed_read =
        snapshot_frontend_playback_after_read(before_first_write);
    assert(mixed_read.playing && !mixed_read.pcm_started);
    assert(snapshot_frontend_playback_after_read(state).pcm_started);
    if (codec_writes != 0) {
        assert(previous_write.playing && state.playing);
        assert(previous_write.generation != state.generation);
        // The preceding ADC read can belong to a prior response even if its
        // playing and pcm_started booleans both remained true throughout.
        assert(!snapshot_frontend_playback_after_read(previous_write).pcm_started);
    }
    previous_write = state;
    ++codec_writes;
    return ESP_CODEC_DEV_OK;
}

#include "playback_task.inc"

static void reset(void)
{
    s_playing = s_amplifier_enabled = s_frontend_pcm_started = false;
    s_frontend_playback_generation = 0;
    queue_count = queue_position = codec_writes = start_notifications = 0;
    memset(queue, 0, sizeof(queue));
}
static void append(audio_item_type_t type, const char *response)
{
    audio_item_t *item = &queue[queue_count++];
    item->type = type;
    item->sample_rate = CODEC_SAMPLE_RATE;
    copy_response_id(item->response_id, response);
}
static void run_task(void)
{
    if (setjmp(finished) == 0) playback_task(NULL);
}

int main(void)
{
    reset();
    append(AUDIO_ITEM_BEGIN, "first");
    append(AUDIO_ITEM_PCM, "first");
    append(AUDIO_ITEM_BEGIN, "second");
    append(AUDIO_ITEM_PCM, "second");
    append(AUDIO_ITEM_END, "second");
    run_task();
    assert(codec_writes == 2 && start_notifications == 2);
    assert(write_generations[0] == 1 && write_generations[1] == 2);
    assert(!snapshot_frontend_playback().playing);

    reset();
    append(AUDIO_ITEM_BEGIN, "cleared");
    append(AUDIO_ITEM_PCM, "cleared");
    append(AUDIO_ITEM_CLEAR, "cleared");
    run_task();
    assert(tx_resets == 1 && !snapshot_frontend_playback().playing);

    reset();
    s_self_test_running = true;
    s_playing = s_amplifier_enabled = true;
    append(AUDIO_ITEM_BEGIN, "during-diagnostic");
    append(AUDIO_ITEM_PCM, "during-diagnostic");
    append(AUDIO_ITEM_END, "during-diagnostic");
    run_task();
    assert(codec_writes == 0 && start_notifications == 0);
    assert(s_playing && s_amplifier_enabled);
    s_self_test_running = false;

    reset();
    append(AUDIO_ITEM_BEGIN, "active");
    append(AUDIO_ITEM_PCM, "active");
    append(AUDIO_ITEM_END, "obsolete");
    append(AUDIO_ITEM_END, "active");
    assert_active_before_final_end = true;
    run_task();
    assert(!assert_active_before_final_end && !s_playing && !s_amplifier_enabled);

    reset();
    append(AUDIO_ITEM_BEGIN, "no-pcm");
    append(AUDIO_ITEM_END, "no-pcm");
    run_task();
    assert(codec_writes == 0 && start_notifications == 0);
    assert(s_frontend_playback_generation == 1 && !s_frontend_pcm_started);
    assert(!snapshot_frontend_playback().playing);

    // An immediate amplifier mute after PCM releases upload while a blocking
    // codec write is still completing and s_playing has not yet been cleared.
    s_playing = s_amplifier_enabled = true;
    begin_frontend_playback();
    mark_frontend_pcm_started();
    assert(snapshot_frontend_playback().playing);
    set_amplifier_enabled(false);
    assert(s_playing && !snapshot_frontend_playback().playing);

    // The ISR only counts. Capture sees each batch once and resets before it
    // can feed a block spanning a dropped DMA buffer into the frontend.
    uint32_t seen_overflows = 0;
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(s_rx_overflow_count == 2 && frontend_resets == 0 && warnings == 0);
    assert(recover_frontend_rx_overflow(&seen_overflows));
    assert(seen_overflows == 2 && frontend_resets == 1 && warnings == 1);
    assert(!recover_frontend_rx_overflow(&seen_overflows));
    assert(frontend_resets == 1 && warnings == 1);
    s_rx_overflow_count = seen_overflows = UINT32_MAX;
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(recover_frontend_rx_overflow(&seen_overflows));
    assert(seen_overflows == 0 && frontend_resets == 2 && warnings == 2);
    puts("Playback timing passed");
    return 0;
}

#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice_frontend.h"

#define CONFIG_JUFF_VOICE_BARGE_IN 1
#define CONFIG_JUFF_BOARD_WAVESHARE_LCD_35 0
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_ARG 5
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGW(...) playback_test_log(__VA_ARGS__)
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(call) assert((call) == 0)
#define portENTER_CRITICAL(lock) (++critical_depth)
#define portEXIT_CRITICAL(lock) (--critical_depth)
#define portENTER_CRITICAL_ISR(lock) (++critical_depth)
#define portEXIT_CRITICAL_ISR(lock) (--critical_depth)
#define IRAM_ATTR
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_SPIRAM 2

typedef unsigned TickType_t;
typedef void *i2s_chan_handle_t;
typedef struct { size_t size; } i2s_event_data_t;
#include "audio_item.inc"
#include "audio_buffer_globals.inc"
static bool s_playing, s_amplifier_enabled, s_frontend_pcm_started, s_self_test_running;
static bool s_shutdown_requested, s_available = true;
static uint32_t s_frontend_playback_generation, s_rx_overflow_count;
static void *s_playback_queue = (void *)1, *s_codec = (void *)2, *s_codec_write_mutex = (void *)3;
static const char *TAG = "playback-test";
static bool s_voice_frontend_ready = true;
static unsigned frontend_resets, warnings, tx_resets, critical_depth;
static audio_item_t queue[PLAYBACK_QUEUE_DEPTH];
static unsigned queue_count;
static bool fail_allocation, fail_send, clear_during_allocation, end_during_allocation;
static int64_t time_us, dma_tail_us, stop_at_us = 2500000;
static int64_t first_write_us = -1, last_write_us, terminal_us;
static size_t submitted_bytes, played_bytes;
static unsigned writes, starts, ends, cancels;
static jmp_buf finished;
static const char *scenario;
static voice_frontend_playback_t before_first_write;
static bool wifi_consumer;
static int64_t consume_at_us;
static size_t consumed_bytes;

static uint32_t codec_write_generation(void);
static voice_frontend_playback_t snapshot_frontend_playback(void);
static voice_frontend_playback_t snapshot_frontend_playback_after_read(voice_frontend_playback_t before);
static void audio_io_clear(const char *reason);
static esp_err_t audio_io_end_response(const char *response_id);
static void advance_to(int64_t target);
static void consume_wifi_block(void);
static audio_playback_buffer_status_t audio_io_playback_buffer_status(void);
static void playback_test_log(const char *tag, const char *format, ...) { ++warnings; }
void voice_frontend_reset_stream(void) { ++frontend_resets; }
static int64_t esp_timer_get_time(void) { return time_us; }
static void vTaskDelay(unsigned ms) { assert(!critical_depth); advance_to(time_us + ms * 1000LL); }
static void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(audio_io_playback_buffer_status().queued_bytes >= bytes);
    if (clear_during_allocation) { clear_during_allocation = false; audio_io_clear("allocation race"); }
    if (end_during_allocation) { end_during_allocation = false; assert(audio_io_end_response("active") == ESP_OK); }
    return fail_allocation ? NULL : malloc(bytes);
}
static int xQueueSend(void *handle, const audio_item_t *item, unsigned timeout)
{
    assert(!critical_depth);
    if (fail_send || queue_count == PLAYBACK_QUEUE_DEPTH) return 0;
    queue[queue_count++] = *item;
    return pdTRUE;
}
static int xQueueReceive(void *handle, audio_item_t *item, unsigned timeout)
{
    assert(!critical_depth && timeout == 0);
    if (!queue_count) return 0;
    *item = queue[0];
    memmove(queue, queue + 1, --queue_count * sizeof(*queue));
    return pdTRUE;
}
static int set_amplifier_enabled(bool enabled)
{
    s_amplifier_enabled = enabled;
    if (enabled) {
        const voice_frontend_playback_t state = snapshot_frontend_playback();
        assert(state.playing && !state.pcm_started);
    }
    return ESP_OK;
}
static int esp_codec_dev_set_out_mute(void *codec, bool muted) { return ESP_OK; }
static int xSemaphoreTake(void *mutex, unsigned timeout) { assert(!critical_depth); return pdTRUE; }
static void xSemaphoreGive(void *mutex) {}
static int reset_codec_tx_locked(void) { ++tx_resets; dma_tail_us = time_us; return ESP_OK; }
static const char *esp_err_to_name(int error) { return "fixture error"; }
static void notify_playback(const char *type, const char *response)
{
    assert(!critical_depth && response[0]);
    if (strcmp(type, "playback.started") == 0) {
        ++starts;
        before_first_write = snapshot_frontend_playback();
        assert(before_first_write.playing && !before_first_write.pcm_started);
        assert(!s_amplifier_enabled);
    } else {
        assert(!s_amplifier_enabled);
        // A concurrently committed new BEGIN retains its own playing state.
        assert(s_playing == s_response_open);
        terminal_us = time_us;
        if (strcmp(type, "playback.ended") == 0) {
            ++ends;
            assert(time_us >= dma_tail_us); // Last submitted sample was not muted early.
        } else { assert(strcmp(type, "playback.cancelled") == 0); ++cancels; }
    }
}
static esp_err_t write_codec_pcm_mono(const void *data, size_t size, uint32_t generation)
{
    assert(!critical_depth && data && size && s_amplifier_enabled);
    assert(audio_io_playback_buffer_status().queued_bytes >= size);
    const voice_frontend_playback_t state = snapshot_frontend_playback();
    assert(state.playing && state.pcm_started);
    assert(!snapshot_frontend_playback_after_read(before_first_write).pcm_started);
    assert(snapshot_frontend_playback_after_read(state).pcm_started);
    if (first_write_us < 0) first_write_us = time_us;
    last_write_us = time_us;
    ++writes;
    // Model a bounded 60 ms DMA ring. The actual writer's partial-byte and
    // cancellation behavior is separately executed by audio_codec_write tests.
    for (size_t offset = 0; offset < size; offset += 480) {
        if (generation != codec_write_generation() || s_shutdown_requested) return ESP_ERR_INVALID_STATE;
        size_t bytes = size - offset < 480 ? size - offset : 480;
        if (dma_tail_us < time_us) dma_tail_us = time_us;
        dma_tail_us += (int64_t)bytes * 1000000 / 48000;
        int64_t wait_until = dma_tail_us - 60000;
        if (wait_until > time_us) advance_to(wait_until);
        submitted_bytes += bytes;
    }
    played_bytes += size;
    return ESP_OK;
}

#include "playback.inc"

typedef enum { BEGIN, PCM, END, CLEAR, NEW_BEGIN } action_t;
typedef struct { int64_t at_us; action_t action; size_t bytes; } event_t;
static event_t events[32];
static unsigned event_count, next_event;
static uint8_t pcm[PLAYBACK_BUFFER_CAPACITY_BYTES];
static void add(unsigned ms, action_t action, size_t bytes)
{ events[event_count++] = (event_t) { .at_us = ms * 1000LL, .action = action, .bytes = bytes }; }
static void advance_to(int64_t target)
{
    while (next_event < event_count && events[next_event].at_us <= target) {
        const event_t event = events[next_event++];
        time_us = event.at_us;
        switch (event.action) {
        case BEGIN: assert(audio_io_begin_response("active", 24000) == ESP_OK); break;
        case NEW_BEGIN:
            assert(audio_io_begin_response("new", 24000) == ESP_OK);
            break;
        case PCM: assert(audio_io_push_pcm(pcm, event.bytes) == ESP_OK); break;
        case END: assert(audio_io_end_response(NULL) == ESP_OK); break;
        case CLEAR: audio_io_clear("test interrupt"); assert(!s_amplifier_enabled && !s_playing); break;
        }
    }
    if (wifi_consumer && consume_at_us <= target) consume_wifi_block();
    time_us = target;
    if (time_us >= stop_at_us) longjmp(finished, 1);
}
static void run_task(void)
{
    advance_to(0);
    if (setjmp(finished) == 0) playback_task(NULL);
    assert(!critical_depth && !queue_count && s_buffered_bytes == 0);
    assert(!s_response_bytes && !s_buffering && !s_playing);
}
static void discard_all(void)
{
    audio_item_t item;
    while (xQueueReceive(s_playback_queue, &item, 0)) free_item(&item);
}
static uint8_t wifi_sample(size_t offset)
{
    // A long, non-repeating block sequence detects omissions and reordering.
    return (uint8_t)((offset * 17) ^ (offset >> 8) ^ (offset >> 16));
}
static void consume_wifi_block(void)
{
    audio_item_t item;
    assert(xQueueReceive(s_playback_queue, &item, 0));
    assert(item.type == AUDIO_ITEM_PCM && item.generation == codec_write_generation());
    for (size_t i = 0; i < item.size; ++i)
        assert(item.data[i] == wifi_sample(consumed_bytes + i));
    consumed_bytes += item.size;
    consume_at_us += (int64_t)item.size * 1000000 / 48000;
    free_item(&item);
}
static void test_wifi(const char *name)
{
    assert(audio_io_begin_response("active", 24000) == ESP_OK);
    s_begin_pending = false; // Playback task has consumed BEGIN.
    if (!strcmp(name, "wifi-burst")) {
        wifi_consumer = true;
        consume_at_us = 4096LL * 1000000 / 48000;
        const size_t total = 5 * 48000;
        for (size_t offset = 0; offset < total; offset += 4096) {
            const size_t length = total - offset < 4096 ? total - offset : 4096;
            for (size_t i = 0; i < length; ++i) pcm[i] = wifi_sample(offset + i);
            assert(audio_io_push_pcm_wait(pcm, length, 200) == ESP_OK);
            assert(s_buffered_bytes <= PLAYBACK_BUFFER_CAPACITY_BYTES);
        }
        assert(time_us > 0 && s_playing && s_response_open);
        while (queue_count) consume_wifi_block();
        assert(consumed_bytes == total && !s_buffered_bytes && !s_response_bytes);
        assert(!warnings && !tx_resets);
        return;
    }
    assert(audio_io_push_pcm(pcm, sizeof(pcm)) == ESP_OK);
    const uint32_t generation = codec_write_generation();
    if (!strcmp(name, "wifi-clear")) add(30, CLEAR, 0);
    if (!strcmp(name, "wifi-replace")) add(30, NEW_BEGIN, 0);
    const int result = audio_io_push_pcm_wait(pcm, 4096, 200);
    if (!strcmp(name, "wifi-timeout")) {
        assert(result == ESP_ERR_TIMEOUT && time_us == 200000);
        assert(!s_response_open && !s_playing && !s_response_bytes);
        assert(codec_write_generation() == generation + 1);
        assert(audio_io_push_pcm(pcm, 4096) == ESP_ERR_INVALID_STATE);
    } else {
        assert(result == ESP_ERR_INVALID_STATE && time_us == 30000);
        assert(codec_write_generation() == generation + 1);
        if (!strcmp(name, "wifi-replace")) {
            assert(s_playing && s_response_open && !strcmp(s_response_id, "new"));
            assert(!s_response_bytes); // No old PCM was retargeted into new BEGIN.
        } else assert(!s_playing && !s_response_open);
    }
    discard_all();
    assert(!s_buffered_bytes);
}
static void test_accounting(void)
{
    assert(audio_io_begin_response("active", 24000) == ESP_OK);
    uint32_t generation = codec_write_generation();
    assert(audio_io_begin_response("rejected", 24000) == ESP_ERR_TIMEOUT);
    assert(generation == codec_write_generation() && s_playing);
    assert(audio_io_push_pcm(pcm, sizeof(pcm)) == ESP_OK);
    assert(audio_io_playback_buffer_status().queued_bytes == 192000);
    assert(audio_io_push_pcm(pcm, 2) == ESP_ERR_NO_MEM);
    assert(s_buffered_bytes == 192000 && queue_count == 1);
    audio_item_t held;
    assert(xQueueReceive(s_playback_queue, &held, 0));
    assert(s_buffered_bytes == 192000); // Taking a block does not grant credit.
    free_item(&held);
    free_item(&held); // Cleanup is idempotent.
    assert(!s_buffered_bytes && !s_response_bytes);
    fail_allocation = true;
    assert(audio_io_push_pcm(pcm, 4800) == ESP_ERR_NO_MEM);
    fail_allocation = false;
    fail_send = true;
    assert(audio_io_push_pcm(pcm, 4800) == ESP_ERR_NO_MEM);
    fail_send = false;
    assert(!s_buffered_bytes && !s_response_bytes);
    assert(audio_io_end_response("obsolete") == ESP_ERR_INVALID_STATE && !s_response_ended);
    end_during_allocation = true;
    assert(audio_io_push_pcm(pcm, 4800) == ESP_OK);
    assert(s_response_ended && s_response_bytes == 4800);
    assert(audio_io_push_pcm(pcm, 2) == ESP_ERR_INVALID_STATE);
    discard_all();
    s_begin_pending = false; // Simulate consumption of the control mailbox.
    assert(audio_io_begin_response("active", 24000) == ESP_OK);
    clear_during_allocation = true;
    assert(audio_io_push_pcm(pcm, 4800) == ESP_ERR_INVALID_STATE);
    assert(!s_buffered_bytes && !s_response_bytes && !s_playing);
    s_begin_pending = false;
    assert(audio_io_begin_response("active", 24000) == ESP_OK);
    assert(audio_io_push_pcm(pcm, 4800) == ESP_OK);
    s_begin_pending = false;
    assert(audio_io_begin_response("new", 24000) == ESP_OK);
    assert(audio_io_push_pcm(pcm, 2400) == ESP_OK);
    assert(s_buffered_bytes == 7200 && s_response_bytes == 2400);
    assert(xQueueReceive(s_playback_queue, &held, 0));
    free_item(&held);
    assert(s_buffered_bytes == 2400 && s_response_bytes == 2400);
    clear_failed_playback(generation); // Old writer failure cannot cancel the new response.
    assert(s_response_open && s_playing && s_response_bytes == 2400);
    clear_failed_playback(codec_write_generation());
    assert(!s_response_open && !s_playing && !s_response_bytes);
    discard_all();
    assert(!s_buffered_bytes && !s_response_bytes);
    assert(audio_io_playback_buffer_status().capacity_bytes == 192000);
}
static void test_frontend(void)
{
    s_playing = s_amplifier_enabled = true;
    begin_frontend_playback();
    mark_frontend_pcm_started();
    voice_frontend_playback_t previous = snapshot_frontend_playback();
    begin_frontend_playback();
    mark_frontend_pcm_started();
    assert(!snapshot_frontend_playback_after_read(previous).pcm_started);
    assert(snapshot_frontend_playback().playing);
    set_amplifier_enabled(false);
    assert(s_playing && !snapshot_frontend_playback().playing);
    uint32_t seen = 0;
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(recover_frontend_rx_overflow(&seen) && seen == 2 && frontend_resets == 1);
    assert(!recover_frontend_rx_overflow(&seen));
    s_rx_overflow_count = seen = UINT32_MAX;
    assert(!frontend_rx_overflow(NULL, NULL, NULL));
    assert(recover_frontend_rx_overflow(&seen) && seen == 0 && frontend_resets == 2);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    if (!strcmp(scenario, "accounting")) test_accounting();
    else if (!strcmp(scenario, "frontend")) test_frontend();
    else if (!strncmp(scenario, "wifi-", 5)) test_wifi(scenario);
    else {
        add(0, BEGIN, 0);
        if (!strcmp(scenario, "burst")) {
            add(20, PCM, 96000); add(30, END, 0);
            run_task();
            assert(first_write_us <= 40000 && played_bytes == 96000 && ends == 1);
            assert(terminal_us >= 2030000 && s_playback_underruns == 0);
            assert(tx_resets == 0);
        } else if (!strcmp(scenario, "short")) {
            add(20, PCM, 2400); add(30, END, 0); run_task();
            assert(first_write_us <= 50000 && played_bytes == 2400 && ends == 1);
            assert(tx_resets == 0);
        } else if (!strcmp(scenario, "empty")) {
            add(20, END, 0); run_task();
            assert(!writes && !starts && ends == 1 && terminal_us <= 30000);
            assert(tx_resets == 0);
        } else if (!strcmp(scenario, "consecutive")) {
            add(20, PCM, 2400); add(30, END, 0);
            add(200, BEGIN, 0); add(220, PCM, 2400); add(230, END, 0);
            run_task();
            assert(played_bytes == 4800 && starts == 2 && ends == 2 && !cancels);
            assert(tx_resets == 0 && s_frontend_playback_generation == 2);
        } else if (!strcmp(scenario, "deadline")) {
            add(20, PCM, 2400); add(800, END, 0); run_task();
            assert(first_write_us >= 620000 && first_write_us <= 640000);
            assert(ends == 1 && s_playback_underruns == 1);
        } else if (!strcmp(scenario, "refill")) {
            add(20, PCM, 14400); add(500, PCM, 4800); add(550, PCM, 4800);
            add(600, PCM, 2400); add(610, END, 0); run_task();
            assert(played_bytes == 26400 && ends == 1 && s_playback_underruns == 1);
            assert(last_write_us >= 600000 && last_write_us < 850000);
        } else if (!strcmp(scenario, "clear-buffer")) {
            add(20, PCM, 2400); add(70, CLEAR, 0); run_task();
            assert(!writes && !starts && cancels == 1 && terminal_us <= 80000);
            assert(tx_resets == 1);
        } else if (!strcmp(scenario, "clear-write")) {
            add(20, PCM, 96000); add(200, CLEAR, 0); run_task();
            assert(submitted_bytes < 20000 && cancels == 1 && terminal_us <= 220000);
            assert(tx_resets >= 1);
        } else if (!strcmp(scenario, "replace")) {
            add(20, PCM, 2400); add(70, NEW_BEGIN, 0);
            add(80, PCM, 2400); add(90, END, 0); run_task();
            assert(played_bytes == 2400 && cancels == 1 && ends == 1 && starts == 1);
            assert(s_frontend_playback_generation == 2);
            assert(tx_resets == 1);
        } else if (!strcmp(scenario, "clear-pending")) {
            add(0, CLEAR, 0); run_task();
            assert(!writes && !starts && cancels == 1 && terminal_us == 0);
            assert(tx_resets == 1);
        } else assert(!"unknown scenario");
    }
    puts("Playback timing passed");
    return 0;
}

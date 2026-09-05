#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice_frontend.h"

#define CODEC_FRAME_SAMPLES 2400
#define GATEWAY_FRAME_SAMPLES 1600
#define ESP_CODEC_DEV_OK 0
#define MALLOC_CAP_8BIT 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define BOARD_HAS_ES7210 CONFIG_JUFF_BOARD_WAVESHARE_LCD_154

#if CONFIG_JUFF_VOICE_BARGE_IN
enum { TEST_FRAMES = 480, TEST_CHANNELS = 2 };
#else
enum { TEST_FRAMES = CODEC_FRAME_SAMPLES, TEST_CHANNELS = 1 };
#endif

static bool s_voice_frontend_ready, s_capture_enabled, s_playing, s_diagnostic_collecting;
static voice_frontend_pcm_callback_t s_pcm_callback;
static voice_frontend_interrupt_callback_t s_barge_in_allowed_callback;
static void *s_callback_context, *s_input_codec;
static const char *TAG = "capture-test";
static void *allocations[2];
static unsigned allocations_count, reads, uploads, frontend_calls, diagnostic_calls;
static bool expected_upload, expected_detection;
static jmp_buf finished;

static int16_t mic_sample(size_t frame) { return (int16_t)(frame * 6 - 1200); }
static int16_t reference_sample(size_t frame) { return frame & 1 ? -27000 : 27000; }
static void test_log(const char *tag, const char *format, ...) {}
static void vTaskDelete(void *task) { assert(!"Unexpected capture task deletion"); }
static void vTaskDelay(unsigned ms) {}
static void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(allocations_count < 2);
    void *allocation = malloc(size);
    assert(allocation != NULL);
    allocations[allocations_count++] = allocation;
    return allocation;
}

static int esp_codec_dev_read(void *codec, void *data, int bytes)
{
    if (reads++) longjmp(finished, 1);
    assert(bytes == TEST_FRAMES * TEST_CHANNELS * (int)sizeof(int16_t));
    int16_t *samples = data;
    for (size_t i = 0; i < TEST_FRAMES; ++i) {
        samples[i * TEST_CHANNELS] = mic_sample(i);
#if CONFIG_JUFF_VOICE_BARGE_IN
        samples[i * TEST_CHANNELS + 1] = reference_sample(i);
#endif
    }
    return ESP_CODEC_DEV_OK;
}

static void collect_diagnostic_stats(const int16_t *samples, size_t count)
{
    ++diagnostic_calls;
    assert(count == TEST_FRAMES);
    for (size_t i = 0; i < count; ++i) assert(samples[i] == mic_sample(i));
}

static void microphone_callback(const uint8_t *data, size_t bytes, void *context)
{
    ++uploads;
    assert(context == s_callback_context);
    assert(bytes == TEST_FRAMES * 2 / 3 * sizeof(int16_t));
    for (size_t i = 0; i < bytes / sizeof(int16_t); ++i) {
        int16_t sample;
        memcpy(&sample, data + i * sizeof(sample), sizeof(sample));
        // Input is a ramp: 24k→16k linear interpolation has an exact analytic
        // result. A reference-slot leak is visibly far outside this ramp.
        assert(sample == (int16_t)(i * 9 - 1200));
    }
}

static bool barge_allowed(void *context) { return true; }
static bool recover_frontend_rx_overflow(uint32_t *seen) { return false; }
static voice_frontend_playback_t snapshot_frontend_playback(void)
{
    return (voice_frontend_playback_t){ .generation = 9, .playing = s_playing,
                                       .pcm_started = s_playing };
}
static voice_frontend_playback_t snapshot_frontend_playback_after_read(voice_frontend_playback_t before)
{ return before; }
void voice_frontend_reset_stream(void) { assert(!"Unexpected frontend reset"); }
void voice_frontend_process(const int16_t *stereo, size_t frames,
                            voice_frontend_playback_t playback,
                            bool upload_enabled, bool detect_speech)
{
    ++frontend_calls;
    assert(frames == TEST_FRAMES);
    assert(upload_enabled == expected_upload && detect_speech == expected_detection);
    assert(playback.generation == 9 && playback.playing == s_playing);
    assert(playback.pcm_started == s_playing);
    for (size_t i = 0; i < frames; ++i) {
        assert(stereo[i * 2] == mic_sample(i));
        assert(stereo[i * 2 + 1] == reference_sample(i));
    }
}

#if !CONFIG_JUFF_VOICE_BARGE_IN
#include "resample.inc"
#endif
#include "capture_task.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_capture_enabled = true;
    s_pcm_callback = microphone_callback;
    s_barge_in_allowed_callback = barge_allowed;
    s_callback_context = &uploads;
    s_voice_frontend_ready = strncmp(argv[1], "frontend", 8) == 0;
    s_playing = s_voice_frontend_ready;
    expected_detection = true;
    if (strstr(argv[1], "muted")) s_capture_enabled = false;
    if (strstr(argv[1], "playing")) s_playing = true;
    if (strstr(argv[1], "no-callback")) s_pcm_callback = NULL;
    if (strstr(argv[1], "diagnostic")) {
        s_diagnostic_collecting = true;
        expected_detection = false;
    }
    if (strstr(argv[1], "no-allowed")) {
        s_barge_in_allowed_callback = NULL;
        expected_detection = false;
    }
    expected_upload = s_capture_enabled;
    if (setjmp(finished) == 0) capture_task(NULL);
    assert(diagnostic_calls == 1);
    assert(frontend_calls == (s_voice_frontend_ready ? 1U : 0U));
    assert(uploads == (!s_voice_frontend_ready && s_capture_enabled
                       && !s_playing && s_pcm_callback != NULL ? 1U : 0U));
    for (unsigned i = 0; i < allocations_count; ++i) free(allocations[i]);
    puts("Capture routing passed");
    return 0;
}

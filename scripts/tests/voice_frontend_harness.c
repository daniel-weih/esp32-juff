#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice_frontend.h"
#include "esp_aec.h"
#include "esp_vad.h"
#include "esp_heap_caps.h"
#undef free

enum { LIMIT = 200000, FRAME = 160, INPUT_FRAME = 240, REFERENCE_BASE = 1000 };
enum { HISTORY_FRAMES = CONFIG_JUFF_BOARD_WAVESHARE_LCD_35 ? 30 : 80 };
static int chunk_size;
static int16_t observed_mic[LIMIT], observed_reference[LIMIT];
static size_t observed_count;
static int16_t uploaded[LIMIT];
static size_t uploaded_count;
static unsigned upload_calls, interrupt_calls, reset_calls, vad_calls;
static unsigned aec_destroy_calls, vad_destroy_calls;
static unsigned allocation_attempts, fail_allocation, live_allocations;
static struct {
    void *pointer;
    size_t size;
    unsigned caps;
} allocations[16];
static bool accept_interrupt = true;
static uint32_t playback_generation;
static bool playback_pcm_started = true;
static vad_state_t vad_result = VAD_SILENCE;
static int context;
struct mock_aec { int unused; };
struct mock_vad { int unused; };
static struct mock_aec aec;
static struct mock_vad vad;
static char last_aec_log[512];
static unsigned aec_log_count;
static char last_interrupt_log[512];
static unsigned interrupt_log_count;
static bool mutate_aec_inputs;
static bool override_aec_clean;
static int16_t forced_aec_clean;
size_t frontend_test_guard_samples(void);
bool frontend_test_guard_allows(void);

aec_handle_t *aec_create_from_config(aec_config_t *config)
{
    assert(config->mic_num == 1 && config->ref_num == 1 && config->out_num == 1);
    assert(config->sample_rate == 16000);
    assert(config->mode == AEC_MODE_FD_HIGH_PERF);
    assert(config->nlp_level == (CONFIG_JUFF_BOARD_WAVESHARE_LCD_35
        ? AEC_NLP_LEVEL_VERYAGGR : AEC_NLP_LEVEL_NORMAL));
    return &aec;
}
void aec_destroy(aec_handle_t *handle)
{ assert(handle == &aec); ++aec_destroy_calls; }
int aec_get_chunksize(const aec_handle_t *handle) { return chunk_size; }
void aec_process(const aec_handle_t *handle, int16_t *mic,
                 int16_t *reference, int16_t *clean)
{
    assert(observed_count + (size_t)chunk_size <= LIMIT);
    for (int index = 0; index < chunk_size; ++index) {
        observed_mic[observed_count] = mic[index];
        observed_reference[observed_count++] = reference[index];
        // An ideal subtractor gives known stream values; it is not an AEC model.
        clean[index] = override_aec_clean ? forced_aec_clean
            : (int16_t)(mic[index] - reference[index]);
        // The real AEC accepts writable input. Diagnostics must retain the
        // physical ADC level even if library preprocessing changes buffers.
        if (mutate_aec_inputs) mic[index] = reference[index] = 0;
    }
}
vad_handle_t vad_create_with_param(int mode, int rate, int frame_ms,
                                   int speech_ms, int silence_ms)
{
    assert(mode == VAD_MODE_3 && rate == 16000 && frame_ms == 10);
    assert(speech_ms == 120 && silence_ms == 100);
    return &vad;
}
void vad_destroy(vad_handle_t handle)
{ assert(handle == &vad); ++vad_destroy_calls; }
void vad_reset_trigger(vad_handle_t handle) { ++reset_calls; }
vad_state_t vad_process_with_trigger(vad_handle_t handle, int16_t *samples)
{ ++vad_calls; return vad_result; }
static void *allocate(size_t size, unsigned caps)
{
    ++allocation_attempts;
    if (allocation_attempts == fail_allocation) return NULL;
    assert(allocation_attempts <= sizeof(allocations) / sizeof(allocations[0]));
    void *pointer = malloc(size);
    assert(pointer != NULL);
    allocations[allocation_attempts - 1].pointer = pointer;
    allocations[allocation_attempts - 1].size = size;
    allocations[allocation_attempts - 1].caps = caps;
    ++live_allocations;
    return pointer;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    assert(alignment == 16 && size == (size_t)chunk_size * sizeof(int16_t));
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return allocate(size, caps);
}
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(size == HISTORY_FRAMES * FRAME * sizeof(int16_t)
        || size == (HISTORY_FRAMES + 20) * FRAME * sizeof(int16_t));
    return allocate(size, caps);
}
void frontend_test_free(void *pointer)
{
    if (pointer == NULL) return;
    for (size_t index = 0; index < sizeof(allocations) / sizeof(allocations[0]); ++index) {
        if (allocations[index].pointer == pointer) {
            allocations[index].pointer = NULL;
            assert(live_allocations > 0);
            --live_allocations;
            free(pointer);
            return;
        }
    }
    assert(false && "frontend freed an unowned or already released allocation");
}
int64_t esp_timer_get_time(void) { static int64_t tick; return ++tick; }
void frontend_test_log(const char *tag, const char *format, ...)
{
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(message));
    if (strncmp(message, "Playback AEC:", 13) == 0) {
        memcpy(last_aec_log, message, (size_t)length + 1);
        ++aec_log_count;
    } else if (strncmp(message, "Voice interruption detected:", 28) == 0) {
        memcpy(last_interrupt_log, message, (size_t)length + 1);
        ++interrupt_log_count;
    }
}

static void pcm(const uint8_t *bytes, size_t count, void *arg)
{
    assert(arg == &context && count == 1600 * sizeof(int16_t));
    assert(uploaded_count + count / sizeof(int16_t) <= LIMIT);
    memcpy(uploaded + uploaded_count, bytes, count);
    uploaded_count += count / sizeof(int16_t);
    ++upload_calls;
}
static bool interrupt(void *arg)
{
    assert(arg == &context);
    ++interrupt_calls;
    return accept_interrupt;
}
static void process_input(const int16_t *input, size_t frames,
                           bool playing, bool upload, bool detect)
{
    const voice_frontend_playback_t playback = {
        .generation = playback_generation,
        .playing = playing,
        .pcm_started = playback_pcm_started,
    };
    voice_frontend_process(input, frames, playback, upload, detect);
}
static void feed(size_t input_frames, int16_t mic, int16_t reference,
                  bool playing, bool upload, bool detect)
{
    int16_t *input = malloc(input_frames * 2 * sizeof(*input));
    assert(input != NULL);
    for (size_t frame = 0; frame < input_frames; ++frame) {
        input[frame * 2] = mic;
        input[frame * 2 + 1] = reference;
    }
    process_input(input, input_frames, playing, upload, detect);
    free(input);
}
static void feed_clean(size_t input_frames, int16_t clean,
                       bool playing, bool upload, bool detect)
{
    feed(input_frames, clean + REFERENCE_BASE, REFERENCE_BASE, playing, upload, detect);
}
static void feed_frames(unsigned count, int16_t value, bool playing,
                         bool upload, bool detect)
{
    for (unsigned frame = 0; frame < count; ++frame)
        feed_clean(INPUT_FRAME, value, playing, upload, detect);
}
static void expect_value(size_t start, size_t count, int16_t value)
{
    assert(start + count <= uploaded_count);
    for (size_t sample = start; sample < start + count; ++sample) {
        if (uploaded[sample] != value) {
            fprintf(stderr, "sample %zu: expected %d, got %d\n",
                    sample, value, uploaded[sample]);
            abort();
        }
    }
}
static void prepare_history(void)
{
    // Let reset recovery elapse before the separate playback warmup.
    feed_frames(50, 0, false, false, true);
    for (unsigned frame = 0; frame < HISTORY_FRAMES + 35; ++frame) {
        // Fill and wrap the complete board-specific history before triggering.
        vad_result = frame < HISTORY_FRAMES + 18 ? VAD_SILENCE : VAD_SPEECH;
        feed_clean(INPUT_FRAME, 1000 + (int16_t)frame, true, false, true);
    }
    assert(interrupt_calls == 1 && uploaded_count == 0);
}

static void test_phase(void)
{
    const size_t frames = (size_t)chunk_size * 3;
    int16_t input[frames * 2];
    for (size_t index = 0; index < frames; ++index) {
        input[index * 2] = (int16_t)(index * 3 + 1);
        input[index * 2 + 1] = (int16_t)(3 - (int)index * 5);
    }
    // An incomplete call must not advance either channel's phase.
    process_input(input, 1, false, false, false);
    process_input(NULL, frames, false, false, false);
    assert(observed_count == 0);
    process_input(input, frames / 2, false, false, false);
    process_input(input + frames, frames / 2, false, false, false);
    assert(observed_count == frames * 2 / 3);
    size_t output = 0;
    for (size_t source = 0; source < frames; source += 3) {
        assert(observed_mic[output] == input[source * 2]);
        assert(observed_reference[output++] == input[source * 2 + 1]);
        assert(observed_mic[output]
               == (input[(source + 1) * 2] + input[(source + 2) * 2]) / 2);
        assert(observed_reference[output++]
               == (input[(source + 1) * 2 + 1] + input[(source + 2) * 2 + 1]) / 2);
    }
}

static void test_echo(void)
{
    vad_result = VAD_SPEECH;
    // Even a forced VAD false positive cannot pass the clean-energy gate here.
    for (unsigned frame = 0; frame < 150; ++frame)
        feed(INPUT_FRAME, 4000, 4000, true, true, true);
    assert(vad_calls > 100 && uploaded_count == 0 && interrupt_calls == 0);
}

static unsigned telemetry_value(const char *key)
{
    const char *value = strstr(last_aec_log, key);
    assert(value != NULL);
    char *end = NULL;
    const unsigned long parsed = strtoul(value + strlen(key), &end, 10);
    assert(end != value + strlen(key) && (*end == ' ' || *end == '\0'));
    return (unsigned)parsed;
}

static unsigned interrupt_telemetry_value(const char *key)
{
    const char *value = strstr(last_interrupt_log, key);
    assert(value != NULL);
    char *end = NULL;
    const unsigned long parsed = strtoul(value + strlen(key), &end, 10);
    assert(end != value + strlen(key) && (*end == ' ' || *end == '\0'));
    return (unsigned)parsed;
}

static void test_interrupt_telemetry(bool varied, bool zero_mic)
{
    mutate_aec_inputs = true;
    const int16_t mic = zero_mic ? 0 : 1000;
    const int16_t reference = zero_mic ? -800 : 200;
    // Warm the actual frontend while the fake VAD reports silence. The fake
    // AEC erases both writable inputs, after computing their known difference.
    for (unsigned frame = 0; frame < 60; ++frame)
        feed(INPUT_FRAME, mic, reference, true, false, true);
    assert(interrupt_calls == 0 && interrupt_log_count == 0);
    vad_result = VAD_SPEECH;
    if (varied) {
        assert(chunk_size == FRAME);
        // This interrupted candidate must not contaminate the later 120 ms.
        for (unsigned frame = 0; frame < 3; ++frame)
            feed(INPUT_FRAME, 1000, 0, true, false, true);
        vad_result = VAD_SILENCE;
        feed(INPUT_FRAME, 1000, 0, true, false, true);
        vad_result = VAD_SPEECH;
        for (unsigned frame = 0; frame < 6; ++frame)
            feed(INPUT_FRAME, 800, 400, true, false, true);
        for (unsigned frame = 0; frame < 5; ++frame)
            feed(INPUT_FRAME, mic, reference, true, false, true);
        assert(interrupt_calls == 0 && interrupt_log_count == 0);
        feed(INPUT_FRAME, mic, reference, true, false, true);
    } else {
        // Also cover an actual 512-sample AEC block spanning 10 ms VAD frames.
        for (unsigned frame = 0; frame < 20; ++frame)
            feed(INPUT_FRAME, mic, reference, true, false, true);
    }
    assert(interrupt_calls == 1 && interrupt_log_count == 1);
    assert(interrupt_telemetry_value("block_mic=") == (unsigned)mic);
    assert(interrupt_telemetry_value("block_ref=") == (unsigned)(reference < 0 ? -reference : reference));
    assert(interrupt_telemetry_value("block_clean=") == 800);
    assert(interrupt_telemetry_value("candidate_clean_mic_power_min=") == (zero_mic ? 0U : varied ? 250U : 640U));
    assert(interrupt_telemetry_value("max=") == (zero_mic ? 0U : 640U));
    assert(interrupt_telemetry_value("scale=") == 1000);
    assert(interrupt_telemetry_value("frames=") == 12);
    assert(interrupt_telemetry_value("zero_mic=") == (zero_mic ? 12U : 0U));
    for (unsigned frame = 0; frame < 20; ++frame)
        feed(INPUT_FRAME, mic, reference, true, false, true);
    assert(interrupt_calls == 1 && interrupt_log_count == 1);
}

static void test_mic_telemetry(void)
{
    mutate_aec_inputs = true;
    // Use both signed extrema. Keeping reference zero means the fake AEC also
    // avoids overflow; detection is disabled during this telemetry check.
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, frame & 1 ? INT16_MIN : INT16_MAX, 0, true, false, false);
    assert(aec_log_count == 1);
    assert(telemetry_value("mic_peak=") == 32768);
    assert(telemetry_value("mic_fullscale=") == 16000);
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 1234, 0, true, false, false);
    assert(aec_log_count == 2);
    assert(telemetry_value("mic_peak=") == 1234);
    assert(telemetry_value("mic_fullscale=") == 0);
    assert(interrupt_calls == 0 && uploaded_count == 0);
}

static void test_gates(void)
{
    vad_result = VAD_SPEECH;
    feed_frames(49, 800, true, true, true);
    assert(interrupt_calls == 0 && uploaded_count == 0);
    vad_result = VAD_SILENCE;
    feed_frames(20, 800, true, true, true);
    assert(interrupt_calls == 0);
    vad_result = VAD_SPEECH;
    feed_frames(20, 800, true, true, false);
    assert(interrupt_calls == 0);
    feed_frames(49, 100, true, true, true);
    assert(interrupt_calls == 0);
    feed_frames(11, 800, true, true, true);
    assert(interrupt_calls == 0);
    feed_frames(1, 800, true, true, true);
    // Replace enough of the small-board echo window for strong evidence;
    // preceding rejected/weak frames cannot pre-fill fast confirmation.
    if (CONFIG_JUFF_BOARD_WAVESHARE_LCD_154)
        feed_frames(8, 800, true, true, true);
    assert(interrupt_calls == 1);
    feed_frames(50, 800, true, true, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
    assert(reset_calls > 0);
    feed_frames(1, 0, false, false, true);
    // The next playback has its own warmup and independent 120 ms candidate.
    feed_frames(50, 800, true, false, true);
    assert(interrupt_calls == 1);
    feed_frames(1, 800, true, false, true);
    assert(interrupt_calls == 2);
}

static void test_spikes(void)
{
    feed_frames(50, 0, false, false, true);
    feed_frames(40, 0, true, false, true);
    // Model a stale/hanging VAD result while residual echo creates brief peaks.
    vad_result = VAD_SPEECH;
    for (unsigned spike = 0; spike < 5; ++spike) {
        feed_frames(1, 800, true, false, true);
        feed_frames(1, 100, true, false, true);
    }
    assert(interrupt_calls == 0);
    feed_frames(11, 800, true, false, true);
    assert(interrupt_calls == 0);
    vad_result = VAD_SILENCE;
    feed_frames(1, 800, true, false, true);
    vad_result = VAD_SPEECH;
    feed_frames(11, 800, true, false, true);
    assert(interrupt_calls == 0);
    feed_frames(1, 100, true, false, true);
    feed_frames(11, 800, true, false, true);
    assert(interrupt_calls == 0);
    feed_frames(1, 800, true, false, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
}

static void test_candidate_mute(void)
{
    feed_frames(50, 0, false, false, true);
    feed_frames(40, 0, true, false, true);
    vad_result = VAD_SPEECH;
    feed_frames(11, 800, true, false, true);
    assert(interrupt_calls == 0);
    // A mute shorter than a VAD frame must still invalidate the candidate.
    feed_clean(3, 800, true, false, false);
    feed_frames(49, 800, true, false, true);
    assert(interrupt_calls == 0);
    feed_frames(11, 800, true, false, true);
    assert(interrupt_calls == 0);
    feed_frames(1, 800, true, false, true);
    assert(interrupt_calls == 1);
}

static void test_delayed_pcm(void)
{
    feed_frames(50, 0, false, false, true);
    ++playback_generation;
    playback_pcm_started = false;
    vad_result = VAD_SPEECH;
    // BEGIN may be followed by a long network or scheduling delay. It must
    // keep the microphone uplink paused without consuming speaker warmup.
    feed_frames(200, 800, true, true, true);
    assert(interrupt_calls == 0 && uploaded_count == 0);
    playback_pcm_started = true;
    feed_frames(50, 800, true, true, true);
    assert(interrupt_calls == 0 && uploaded_count == 0);
    feed_frames(1, 800, true, true, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
}

static void test_generation(void)
{
    feed_frames(50, 0, false, false, true);
    vad_result = VAD_SPEECH;
    ++playback_generation;
    feed_frames(51, 800, true, false, true);
    assert(interrupt_calls == 1);
    // The capture task sees playing=true in both responses and entirely misses
    // the second BEGIN. A changed generation must reset interrupted/warmup.
    ++playback_generation;
    feed_frames(50, 800, true, false, true);
    assert(interrupt_calls == 1);
    feed_frames(1, 800, true, false, true);
    assert(interrupt_calls == 2 && uploaded_count == 0);
}

static void test_reference_warmup(void)
{
    ++playback_generation;
    vad_result = VAD_SPEECH;
    // A stream can submit PCM long before its first audible speaker sample.
    for (unsigned frame = 0; frame < 200; ++frame)
        feed(INPUT_FRAME, 0, 0, true, true, true);
    assert(interrupt_calls == 0 && uploaded_count == 0);
    // Neither microphone speech alone nor reference RMS below 80 arms AEC.
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 800, 0, true, true, true);
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 879, 79, true, true, true);
    assert(interrupt_calls == 0 && uploaded_count == 0);
    for (unsigned frame = 0; frame < 39; ++frame)
        feed(INPUT_FRAME, 880, 80, true, true, true);
    assert(interrupt_calls == 0);
    feed(INPUT_FRAME, 880, 80, true, true, true); // 400 ms, first candidate frame.
    assert(interrupt_calls == 0);
    for (unsigned frame = 0; frame < 10; ++frame)
        feed(INPUT_FRAME, 880, 80, true, true, true);
    assert(interrupt_calls == 0); // Only 110 ms of qualified candidates.
    feed(INPUT_FRAME, 880, 80, true, true, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);

    // New responses must rediscover reference even if playing never went false.
    ++playback_generation;
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 800, 0, true, false, true);
    assert(interrupt_calls == 1);
    feed_frames(50, 800, true, false, true);
    assert(interrupt_calls == 1);
    feed_frames(1, 800, true, false, true);
    assert(interrupt_calls == 2);
}

static void test_reference_pause(void)
{
    ++playback_generation;
    feed_frames(60, 0, true, false, true);
    // Once reference has warmed AEC, a pause between spoken sentences should
    // still let fresh nearby speech interrupt, without another reference onset.
    for (unsigned frame = 0; frame < 20; ++frame)
        feed(INPUT_FRAME, 0, 0, true, false, true);
    vad_result = VAD_SPEECH;
    for (unsigned frame = 0; frame < 11; ++frame)
        feed(INPUT_FRAME, 800, 0, true, false, true);
    assert(interrupt_calls == 0);
    feed(INPUT_FRAME, 800, 0, true, false, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
}

static void test_generation_history(void)
{
    prepare_history();
    ++playback_generation;
    playback_pcm_started = false;
    vad_result = VAD_SILENCE;
    feed_frames(1, 8000, true, false, true);
    playback_pcm_started = true;
    feed_frames(1, 8000, true, false, true);
    feed_frames(10, 3000, false, true, true);
    assert(interrupt_calls == 1 && uploaded_count == 1600);
    expect_value(0, 1600, 3000);
}

static void test_pcm_boundary(void)
{
    playback_pcm_started = false;
    ++playback_generation;
    feed_clean(3, 1111, true, false, true);
    assert(observed_count == 0);
    playback_pcm_started = true;
    feed_clean((size_t)chunk_size * 3 / 2, 2222, true, false, true);
    assert(observed_count == (size_t)chunk_size);
    for (size_t index = 0; index < observed_count; ++index)
        assert(observed_mic[index] == 2222 + REFERENCE_BASE);

    // Neither pending AEC input nor the 96/32-sample VAD remainder may cross
    // into a new generation when capture never saw playing=false.
    feed_clean(3, 3333, true, false, true);
    ++playback_generation;
    const unsigned vad_before = vad_calls;
    feed_clean((size_t)chunk_size * 3 / 2, 4444, true, false, true);
    assert(observed_count == (size_t)chunk_size * 2);
    assert(vad_calls - vad_before == (unsigned)chunk_size / FRAME);
    for (size_t index = chunk_size; index < observed_count; ++index)
        assert(observed_mic[index] == 4444 + REFERENCE_BASE);
    assert(uploaded_count == 0 && interrupt_calls == 0);
}

static void test_rejected(void)
{
    accept_interrupt = false;
    vad_result = VAD_SPEECH;
    feed_frames(100, 800, true, false, true);
    assert(interrupt_calls > 0 && uploaded_count == 0);
    feed_frames(10, 2000, false, true, true);
    assert(uploaded_count == 1600 && upload_calls == 1);
    expect_value(0, uploaded_count, 2000);
}

static void test_history(void)
{
    prepare_history();
    feed_frames(1, 2000, false, true, true);
    assert(uploaded_count == HISTORY_FRAMES * FRAME && upload_calls == HISTORY_FRAMES / 10);
    for (unsigned frame = 0; frame < HISTORY_FRAMES; ++frame)
        expect_value(frame * FRAME, FRAME, 1030 + (int16_t)frame);
    for (unsigned frame = 1; frame <= 4; ++frame)
        feed_frames(1, 2000 + (int16_t)frame, false, true, true);
    assert(uploaded_count == (HISTORY_FRAMES + 10) * FRAME && upload_calls == HISTORY_FRAMES / 10 + 1);
    for (unsigned frame = 0; frame < 5; ++frame)
        expect_value((HISTORY_FRAMES + frame) * FRAME, FRAME, 1030 + HISTORY_FRAMES + (int16_t)frame);
    for (unsigned frame = 0; frame <= 4; ++frame)
        expect_value((HISTORY_FRAMES + 5 + frame) * FRAME, FRAME, 2000 + (int16_t)frame);
    assert(interrupt_calls == 1);
}

static void test_muted(bool short_mute)
{
    prepare_history();
    feed_clean(short_mute ? 3 : INPUT_FRAME, 8000, false, false, false);
    feed_frames(10, 3000, false, true, true);
    assert(uploaded_count == 1600 && upload_calls == 1);
    expect_value(0, uploaded_count, 3000);
    // Discard a partially filled ordinary uplink frame as well.
    feed_frames(4, 4000, false, true, true);
    feed_clean(INPUT_FRAME, 8000, false, false, false);
    feed_frames(10, 5000, false, true, true);
    assert(uploaded_count == 3200 && upload_calls == 2);
    expect_value(1600, 1600, 5000);
}

static void test_full_history(void)
{
    prepare_history();
    // Waiting longer than the bounded 200 ms pending tail must not consume
    // space indefinitely or drop the first frame once live upload resumes.
    for (unsigned frame = HISTORY_FRAMES + 35; frame < HISTORY_FRAMES + 70; ++frame)
        feed_frames(1, 1000 + (int16_t)frame, true, false, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
    feed_frames(10, 2000, false, true, true);
    assert(uploaded_count == (HISTORY_FRAMES + 30) * FRAME && upload_calls == HISTORY_FRAMES / 10 + 3);
    for (unsigned frame = 0; frame < HISTORY_FRAMES + 20; ++frame)
        expect_value(frame * FRAME, FRAME, 1030 + (int16_t)frame);
    expect_value((HISTORY_FRAMES + 20) * FRAME, 1600, 2000);
}

static void test_playing_mute(void)
{
    prepare_history();
    feed_clean(3, 9000, true, false, false);
    feed_frames(10, 10000, true, false, true);
    assert(interrupt_calls == 1 && uploaded_count == 0);
    feed_frames(10, 2000, false, true, true);
    assert(uploaded_count == 1600);
    expect_value(0, 1600, 2000);
}

static void test_reset(void)
{
    // For 256-sample AEC chunks this leaves 160 buffered input samples; with
    // 160-sample chunks it instead leaves a partially filled upload/history.
    feed_clean(INPUT_FRAME, 1111, false, true, true);
    voice_frontend_reset_stream();
    const size_t observed_before = observed_count;
    feed_frames(24, 2222, false, true, true);
    assert(uploaded_count == 3200);
    expect_value(0, uploaded_count, 2222);
    for (size_t sample = observed_before; sample < observed_count; ++sample)
        assert(observed_mic[sample] == 2222 + REFERENCE_BASE);

    // 256/512-sample AEC outputs leave 96/32 samples in the VAD frame.
    // Reset must clear that remainder too, independently of the AEC input.
    voice_frontend_reset_stream();
    feed_clean((size_t)chunk_size * 3 / 2, 1111, false, true, true);
    voice_frontend_reset_stream();
    feed_frames(24, 4444, false, true, true);
    assert(uploaded_count == 6400);
    expect_value(3200, 3200, 4444);
    if (chunk_size == 160) {
        uploaded_count = upload_calls = 0;
        prepare_history();
        voice_frontend_reset_stream();
        feed_frames(10, 3333, false, true, true);
        assert(uploaded_count == 1600);
        expect_value(0, uploaded_count, 3333);
    }
}

static void test_allocation_caps(void)
{
    assert(allocation_attempts == 5 && live_allocations == 5);
    size_t internal_bytes = 0, external_bytes = 0;
    for (size_t index = 0; index < allocation_attempts; ++index) {
        if (allocations[index].caps & MALLOC_CAP_SPIRAM)
            external_bytes += allocations[index].size;
        else
            internal_bytes += allocations[index].size;
    }
    assert(external_bytes == (HISTORY_FRAMES * 2 + 20) * FRAME * sizeof(int16_t));
    assert(internal_bytes == 3 * (size_t)chunk_size * sizeof(int16_t));
    // Exercise the full board-specific pending buffer and fixed upload size.
    test_full_history();
}

static void test_allocation_failure(unsigned failed_attempt)
{
    fail_allocation = failed_attempt;
    assert(voice_frontend_init(pcm, interrupt, &context) == ESP_ERR_NO_MEM);
    assert(live_allocations == 0);
    assert(aec_destroy_calls == 1 && vad_destroy_calls == 1);
    feed_frames(10, 3000, true, true, true);
    assert(observed_count == 0 && uploaded_count == 0 && interrupt_calls == 0);

    // Retry after an allocation failure must start with fresh, usable buffers.
    fail_allocation = 0;
    assert(voice_frontend_init(pcm, interrupt, &context) == ESP_OK);
    assert(live_allocations == 5);
    feed_frames(10, 2000, false, true, true);
    assert(uploaded_count == 1600);
    expect_value(0, uploaded_count, 2000);
}

static void test_echo_guard_low(void)
{
    vad_result = VAD_SPEECH;
    mutate_aec_inputs = true;
    // The clean RMS still exceeds the unchanged 180 floor for well over
    // 120 ms, but it is only 0.09% of the raw microphone's power.
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 10000, 9700, true, false, true);
    assert(interrupt_calls == (CONFIG_JUFF_BOARD_WAVESHARE_LCD_154 ? 0U : 1U));
    assert(frontend_test_guard_allows() == !CONFIG_JUFF_BOARD_WAVESHARE_LCD_154);
}

static void test_echo_guard_speech(bool soft, bool quiet)
{
    mutate_aec_inputs = true;
    if (quiet) {
        // Reference has warmed the existing detector before a pause. A low
        // clean/raw ratio must not disable nearby speech during that pause.
        for (unsigned frame = 0; frame < 60; ++frame)
            feed(INPUT_FRAME, 1000, 1000, true, false, true);
        override_aec_clean = true;
        forced_aec_clean = 300;
    }
    vad_result = VAD_SPEECH;
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, quiet ? 10000 : soft ? 1800 : 1400,
             quiet ? 0 : soft ? 1600 : 1000, true, false, true);
    assert(interrupt_calls == 1 && interrupt_log_count == 1);
}

static unsigned fill_echo_guard(int16_t mic, int16_t reference)
{
    // Feed exactly one entire AEC block at a time, including grouped buckets
    // for native fixtures whose block size differs from the device's 512.
    const unsigned blocks_per_bucket = (512U + chunk_size - 1U) / chunk_size;
    for (unsigned block = 0; block < 4 * blocks_per_bucket; ++block)
        feed((size_t)chunk_size * 3 / 2, mic, reference, true, false, true);
    const unsigned samples = 4 * blocks_per_bucket * chunk_size;
    assert(frontend_test_guard_samples() == (CONFIG_JUFF_BOARD_WAVESHARE_LCD_154 ? samples : 0U));
    return samples;
}

static void test_echo_guard_window(void)
{
    const int16_t empty[6] = { 0 };
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    // A partially filled window never extends the existing warmup or inserts
    // another time delay. Real detection already has its separate 400 ms gate.
    feed((size_t)chunk_size * 3 / 2, 10000, 9700, true, false, true);
    assert(frontend_test_guard_allows());
    const unsigned samples = fill_echo_guard(10000, 9700);
    assert(samples >= 2048);
    assert(frontend_test_guard_allows() == !CONFIG_JUFF_BOARD_WAVESHARE_LCD_154);

    ++playback_generation;
    process_input(empty, 0, true, false, true);
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    fill_echo_guard(10000, 9700);
    process_input(empty, 0, true, false, false); // Explicit detection mute.
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    process_input(empty, 0, true, false, true);
    assert(frontend_test_guard_samples() == 0);
    fill_echo_guard(10000, 9700);
    playback_pcm_started = false;
    process_input(empty, 0, true, false, true);
    playback_pcm_started = true;
    process_input(empty, 0, true, false, true);
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    fill_echo_guard(10000, 9700);
    process_input(empty, 0, false, false, true);
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    fill_echo_guard(10000, 9700);
    voice_frontend_reset_stream();
    assert(frontend_test_guard_samples() == 0 && frontend_test_guard_allows());
    assert(interrupt_calls == 0);
}

static void test_echo_guard_boundary(void)
{
    override_aec_clean = true;
    forced_aec_clean = 100;
    fill_echo_guard(1000, 80); // Exactly 1% power, reference exactly active.
    assert(frontend_test_guard_allows());
    forced_aec_clean = 99;
    fill_echo_guard(1000, 80);
    assert(frontend_test_guard_allows() == !CONFIG_JUFF_BOARD_WAVESHARE_LCD_154);
    fill_echo_guard(1000, 79); // Below the existing reference RMS threshold.
    assert(frontend_test_guard_allows());
    forced_aec_clean = INT16_MIN;
    fill_echo_guard(INT16_MIN, 80); // Worst signed magnitude in both energies.
    assert(frontend_test_guard_allows());
    assert(interrupt_calls == 0);
}

static void test_echo_guard_candidate(void)
{
    override_aec_clean = true;
    forced_aec_clean = 300;
    vad_result = VAD_SPEECH;
    // A sustained residual can satisfy VAD and the absolute energy floor,
    // while the complete AEC window correctly identifies it as echo.
    for (unsigned frame = 0; frame < 100; ++frame)
        feed(INPUT_FRAME, 10000, 1000, true, false, true);
    if (!CONFIG_JUFF_BOARD_WAVESHARE_LCD_154) {
        assert(interrupt_calls == 1); // Large-board policy is independent.
        return;
    }
    assert(interrupt_calls == 0 && !frontend_test_guard_allows());
    forced_aec_clean = 500;
    // Replace the echo window with near-end speech. The final bucket is the
    // first qualifying evidence: rejected echo must not pre-fill its 120 ms.
    fill_echo_guard(1000, 1000);
    assert(frontend_test_guard_allows() && interrupt_calls == 0);
    // Include one bucket for a partial input block to leave the energy window,
    // then a complete strong-speech candidate.
    for (unsigned block = 0; block < (1920U + 640U + chunk_size - 1U) / chunk_size; ++block)
        feed((size_t)chunk_size * 3 / 2, 1000, 1000, true, false, true);
    assert(interrupt_calls == 1);
}

static void test_weak_speech(bool strengthens)
{
    override_aec_clean = true;
    forced_aec_clean = 300;
    for (unsigned frame = 0; frame < 60; ++frame)
        feed(INPUT_FRAME, 2500, 1000, true, false, true);
    vad_result = VAD_SPEECH;
    for (unsigned frame = 0; frame < 20; ++frame)
        feed(INPUT_FRAME, 2500, 1000, true, false, true);
    if (!CONFIG_JUFF_BOARD_WAVESHARE_LCD_154) {
        assert(interrupt_calls == 1);
        return;
    }
    assert(interrupt_calls == 0 && "A brief 1.44% residual must not interrupt");
    if (strengthens) {
        forced_aec_clean = 500;
        fill_echo_guard(1000, 1000);
        assert(interrupt_calls == 0 && "Weak evidence must not pre-fill the strong path");
    }
    for (unsigned frame = 0; frame < 36; ++frame)
        feed(INPUT_FRAME, strengthens ? 1000 : 2500, 1000, true, false, true);
    assert(interrupt_calls == 1);
    // Strong input may complete the independent 360 ms path before it has
    // provided 120 ms above the stronger threshold.
    assert(interrupt_telemetry_value("qualified_ms=") == 360U);
}

static void test_varying_weak_speech(void)
{
    override_aec_clean = true;
    forced_aec_clean = 300;
    for (unsigned frame = 0; frame < 60; ++frame)
        feed(INPUT_FRAME, 2500, 1000, true, false, true);
    vad_result = VAD_SPEECH;
    for (unsigned block = 0; block < 30; ++block) {
        // Both levels are valid near-end candidates. Crossing the 5% boundary
        // may reset the fast path, but must not erase sustained weak evidence.
        forced_aec_clean = block % 8 < 4 ? 300 : 750;
        feed((size_t)chunk_size * 3 / 2, 2500, 1000, true, false, true);
    }
    assert(interrupt_calls == 1);
}

static void test_weak_reference_residual(void)
{
    // A quiet MIC makes clean/MIC power look strong even though the residual
    // is small beside the active speaker reference (e.g. at a response tail).
    override_aec_clean = true;
    forced_aec_clean = 300;
    for (unsigned frame = 0; frame < 60; ++frame)
        feed(INPUT_FRAME, 400, 1200, true, false, true);
    vad_result = VAD_SPEECH;
    for (unsigned frame = 0; frame < 20; ++frame)
        feed(INPUT_FRAME, 400, 1200, true, false, true);
    if (!CONFIG_JUFF_BOARD_WAVESHARE_LCD_154) {
        assert(interrupt_calls == 1);
        return;
    }
    assert(interrupt_calls == 0 && "Small residual must not become strong because MIC is quiet");
    for (unsigned frame = 0; frame < 36; ++frame)
        feed(INPUT_FRAME, 400, 1200, true, false, true);
    assert(interrupt_calls == 1 && "Sustained quiet speech must remain eligible");
    assert(interrupt_telemetry_value("qualified_ms=") == 360U);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    chunk_size = atoi(argv[2]);
    if (strcmp(argv[1], "history-allocation-failure") == 0) {
        test_allocation_failure(4);
        puts("Frontend stream/control passed");
        return 0;
    }
    if (strcmp(argv[1], "pending-allocation-failure") == 0) {
        test_allocation_failure(5);
        puts("Frontend stream/control passed");
        return 0;
    }
    assert(voice_frontend_init(pcm, interrupt, &context) == ESP_OK);
    if (strcmp(argv[1], "phase") == 0) test_phase();
    else if (strcmp(argv[1], "allocation-caps") == 0) test_allocation_caps();
    else if (strcmp(argv[1], "echo") == 0) test_echo();
    else if (strcmp(argv[1], "mic-telemetry") == 0) test_mic_telemetry();
    else if (strcmp(argv[1], "interrupt-telemetry") == 0) test_interrupt_telemetry(true, false);
    else if (strcmp(argv[1], "interrupt-telemetry-block") == 0) test_interrupt_telemetry(false, false);
    else if (strcmp(argv[1], "interrupt-telemetry-zero") == 0) test_interrupt_telemetry(false, true);
    else if (strcmp(argv[1], "echo-guard-low") == 0) test_echo_guard_low();
    else if (strcmp(argv[1], "echo-guard-normal") == 0) test_echo_guard_speech(false, false);
    else if (strcmp(argv[1], "echo-guard-soft") == 0) test_echo_guard_speech(true, false);
    else if (strcmp(argv[1], "echo-guard-quiet") == 0) test_echo_guard_speech(false, true);
    else if (strcmp(argv[1], "echo-guard-window") == 0) test_echo_guard_window();
    else if (strcmp(argv[1], "echo-guard-boundary") == 0) test_echo_guard_boundary();
    else if (strcmp(argv[1], "echo-guard-candidate") == 0) test_echo_guard_candidate();
    else if (strcmp(argv[1], "weak-speech") == 0) test_weak_speech(false);
    else if (strcmp(argv[1], "weak-to-strong") == 0) test_weak_speech(true);
    else if (strcmp(argv[1], "varying-weak-speech") == 0) test_varying_weak_speech();
    else if (strcmp(argv[1], "weak-reference-residual") == 0) test_weak_reference_residual();
    else if (strcmp(argv[1], "gates") == 0) test_gates();
    else if (strcmp(argv[1], "spikes") == 0) test_spikes();
    else if (strcmp(argv[1], "candidate-mute") == 0) test_candidate_mute();
    else if (strcmp(argv[1], "delayed-pcm") == 0) test_delayed_pcm();
    else if (strcmp(argv[1], "reference-warmup") == 0) test_reference_warmup();
    else if (strcmp(argv[1], "reference-pause") == 0) test_reference_pause();
    else if (strcmp(argv[1], "generation") == 0) test_generation();
    else if (strcmp(argv[1], "generation-history") == 0) test_generation_history();
    else if (strcmp(argv[1], "pcm-boundary") == 0) test_pcm_boundary();
    else if (strcmp(argv[1], "rejected") == 0) test_rejected();
    else if (strcmp(argv[1], "history") == 0) test_history();
    else if (strcmp(argv[1], "full-history") == 0) test_full_history();
    else if (strcmp(argv[1], "muted") == 0) test_muted(false);
    else if (strcmp(argv[1], "short-mute") == 0) test_muted(true);
    else if (strcmp(argv[1], "playing-mute") == 0) test_playing_mute();
    else if (strcmp(argv[1], "reset") == 0) test_reset();
    else abort();
    puts("Frontend stream/control passed");
    return 0;
}

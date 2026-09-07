#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wake_word.h"
#include "voice_session.h"
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define DET_MODE_90 0
#define VAD_MODE_3 3
#define VAD_SPEECH 1
typedef void *vad_handle_t;
typedef struct { unsigned index; bool live; } model_iface_data_t;
typedef struct {} srmodel_list_t;
typedef struct {
    model_iface_data_t *(*create)(const void *, int);
    int (*get_samp_rate)(model_iface_data_t *);
    int (*get_channel_num)(model_iface_data_t *);
    int (*get_samp_chunksize)(model_iface_data_t *);
    void (*clean)(model_iface_data_t *);
    void (*destroy)(model_iface_data_t *);
    int (*detect)(model_iface_data_t *, int16_t *);
} esp_wn_iface_t;
static voice_session_status_t state = { .mode = VOICE_MODE_WAKE, .generation = 1,
    .connected = true, .acknowledged = true };
static int64_t time_us;
static unsigned trigger;
static bool missing, missing_second, mismatch, reset_failure, second_reset_failure;
static unsigned detections, wakes, activity, creates, destroys, silent_checks;
static unsigned checked[2], calls[2];
static model_iface_data_t model_data[2] = {{.index = 0}, {.index = 1}};
static srmodel_list_t models;
voice_session_status_t voice_session_get(void) { return state; }
void voice_session_set_supported(bool supported) { state.supported = supported; }
void voice_session_wake(void) { ++wakes; state.active = true; ++state.generation; }
void voice_session_activity(void) { ++activity; }
static int64_t esp_timer_get_time(void) { return time_us; }
static void *heap_caps_malloc(size_t size, unsigned caps) { return malloc(size); }
static srmodel_list_t *esp_srmodel_init(const char *partition) { return missing ? NULL : &models; }
static void esp_srmodel_deinit(srmodel_list_t *list) {}
static char *esp_srmodel_filter(srmodel_list_t *list, const char *a, const char *b)
{
    assert(!strcmp(a, "wn9_"));
    if (!strcmp(b, "himiaomiao_tts")) return "wn9_himiaomiao_tts";
    assert(!strcmp(b, "miaomiaotongxue_tts"));
    return missing_second ? NULL : "wn9_miaomiaotongxue_tts";
}
static model_iface_data_t *create(const void *name, int mode)
{
    ++creates;
    const unsigned index = !strcmp(name, "wn9_himiaomiao_tts") ? 0 : 1;
    assert(!model_data[index].live);
    if (reset_failure || (second_reset_failure && index == 1)) return NULL;
    model_data[index].live = true;
    return &model_data[index];
}
static int rate(model_iface_data_t *data) { return 16000; }
static int channels(model_iface_data_t *data) { return 1; }
static int chunk(model_iface_data_t *data) { return mismatch && data->index == 1 ? 480 : 320; }
static void clean(model_iface_data_t *data) { assert(false && "WakeNet9 clean crashes on this pinned library"); }
static void destroy(model_iface_data_t *data)
{ assert(data == &model_data[data->index] && data->live); data->live = false; ++destroys; }
static int detect(model_iface_data_t *data, int16_t *pcm)
{
    assert(data->live);
    if (!checked[data->index]) {
        for (unsigned i = 0; i < 320; ++i) assert(pcm[i] == 0);
        ++silent_checks;
        ++checked[data->index];
        return 0;
    }
    ++detections;
    ++calls[data->index];
    for (unsigned i = 0; i < 320; ++i) assert(pcm[i] == (int16_t)(i * 9 - 1200));
    return (trigger & (1U << data->index)) != 0;
}
static const esp_wn_iface_t model = { create, rate, channels, chunk, clean, destroy, detect };
static const esp_wn_iface_t *esp_wn_handle_from_name(const char *name) { return &model; }
static vad_handle_t vad_create_with_param(int mode, int hz, int ms, int start, int end) { return (void *)1; }
static void vad_reset_trigger(vad_handle_t vad) {}
static void vad_destroy(vad_handle_t vad) {}
static int vad_process_with_trigger(vad_handle_t vad, int16_t *pcm) { return VAD_SPEECH; }

#include "wake.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    missing = !strcmp(argv[1], "missing");
    missing_second = !strcmp(argv[1], "missing-second");
    mismatch = !strcmp(argv[1], "mismatch");
    second_reset_failure = !strcmp(argv[1], "second-create-failure");
    int16_t mono[480], stereo[960];
    for (unsigned i = 0; i < 480; ++i) {
        mono[i] = stereo[i * 2] = (int16_t)(i * 6 - 1200);
        stereo[i * 2 + 1] = 28000;
    }
    const int result = wake_word_init();
    if (missing || missing_second || mismatch || second_reset_failure) {
        assert(result != ESP_OK && !state.supported);
        assert(wake_word_process(mono, 480, 1, false, false));
        assert(!wake_word_process(mono, 480, 1, false, true));
        assert(!state.active && !wakes && !detections);
        assert(!model_data[0].live && !model_data[1].live);
    } else {
        assert(result == ESP_OK && state.supported);
        if (!strcmp(argv[1], "reset")) {
            assert(silent_checks == 2 && creates == 4 && destroys == 2);
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 4 && destroys == 2 && detections == 2);
            for (unsigned i = 0; i < 10; ++i) wake_word_reset();
            state.acknowledged = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 4 && destroys == 2);
            state.acknowledged = true;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 6 && destroys == 4 && detections == 4);
            ++state.generation;
            reset_failure = true;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!state.supported && !wakes && creates == 7 && destroys == 6);
            assert(!model_data[0].live && !model_data[1].live);
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!wake_word_process(mono, 480, 1, false, true));
        } else if (!strcmp(argv[1], "routing")) {
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(wake_word_process(stereo, 480, 2, false, false));
            assert(detections == 4 && calls[0] == 2 && calls[1] == 2 && !wakes);
        } else if (!strcmp(argv[1], "wake") || !strcmp(argv[1], "wake-second")
                   || !strcmp(argv[1], "wake-both")) {
            trigger = !strcmp(argv[1], "wake-second") ? 2 : !strcmp(argv[1], "wake-both") ? 3 : 1;
            assert(wake_word_process(stereo, 480, 2, false, false));
            assert(wakes == 1 && state.active);
            assert(!wake_word_process(stereo, 480, 2, false, false));
            assert(detections == (trigger == 2 ? 2U : 1U));
            // A fresh conversation boundary rearms both detectors, including
            // the detector skipped after the first phrase matched.
            state.active = false;
            ++state.generation;
            trigger = trigger == 2 ? 1 : 2;
            assert(wake_word_process(stereo, 480, 2, false, false));
            assert(wakes == 2 && state.active && creates == 6);
        } else if (!strcmp(argv[1], "reset-second-failure")) {
            assert(wake_word_process(mono, 480, 1, false, false));
            ++state.generation;
            second_reset_failure = true;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!state.supported && !wakes);
            assert(!model_data[0].live && !model_data[1].live);
        } else if (!strcmp(argv[1], "gates")) {
            assert(wake_word_process(mono, 480, 1, true, false));
            time_us = 799999;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!detections);
            time_us = 800000;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(detections == 2);
            assert(!wake_word_process(mono, 480, 1, false, true));
            time_us += 800000;
            state.acknowledged = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            state.acknowledged = true;
            state.connected = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(detections == 2 && !wakes);
        } else if (!strcmp(argv[1], "activity")) {
            state.active = true;
            int16_t voice[160];
            for (unsigned i = 0; i < 160; ++i) voice[i] = 100;
            wake_word_observe_activity((uint8_t *)voice, sizeof(voice));
            assert(!activity);
            for (unsigned i = 0; i < 160; ++i) voice[i] = 500;
            wake_word_observe_activity((uint8_t *)voice, sizeof(voice));
            assert(activity == 1);
            state.mode = VOICE_MODE_CONTINUOUS;
            wake_word_observe_activity((uint8_t *)voice, sizeof(voice));
            assert(activity == 1);
        } else assert(false);
        destroy_models();
        free(s_frame);
    }
    puts("Wake routing passed");
}

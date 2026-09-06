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
typedef struct {} model_iface_data_t;
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
static bool trigger, missing, reset_failure;
static unsigned detections, wakes, activity, creates, destroys, silent_checks;
static model_iface_data_t model_data;
static srmodel_list_t models;
voice_session_status_t voice_session_get(void) { return state; }
void voice_session_set_supported(bool supported) { state.supported = supported; }
void voice_session_wake(void) { ++wakes; state.active = true; ++state.generation; }
void voice_session_activity(void) { ++activity; }
static int64_t esp_timer_get_time(void) { return time_us; }
static void *heap_caps_malloc(size_t size, unsigned caps) { return malloc(size); }
static srmodel_list_t *esp_srmodel_init(const char *partition) { return missing ? NULL : &models; }
static void esp_srmodel_deinit(srmodel_list_t *list) {}
static char *esp_srmodel_filter(srmodel_list_t *list, const char *a, const char *b) { return "wn9_nihaoxiaozhi_tts"; }
static model_iface_data_t *create(const void *name, int mode)
{ ++creates; return reset_failure ? NULL : &model_data; }
static int rate(model_iface_data_t *data) { return 16000; }
static int channels(model_iface_data_t *data) { return 1; }
static int chunk(model_iface_data_t *data) { return 320; }
static void clean(model_iface_data_t *data) { assert(false && "WakeNet9 clean crashes on this pinned library"); }
static void destroy(model_iface_data_t *data) { assert(data == &model_data); ++destroys; }
static int detect(model_iface_data_t *data, int16_t *pcm)
{
    if (!silent_checks) {
        for (unsigned i = 0; i < 320; ++i) assert(pcm[i] == 0);
        ++silent_checks;
        return 0;
    }
    ++detections;
    for (unsigned i = 0; i < 320; ++i) assert(pcm[i] == (int16_t)(i * 9 - 1200));
    return trigger;
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
    int16_t mono[480], stereo[960];
    for (unsigned i = 0; i < 480; ++i) {
        mono[i] = stereo[i * 2] = (int16_t)(i * 6 - 1200);
        stereo[i * 2 + 1] = 28000;
    }
    const int result = wake_word_init();
    if (missing) {
        assert(result != ESP_OK && !state.supported);
        assert(wake_word_process(mono, 480, 1, false, false));
        assert(!wake_word_process(mono, 480, 1, false, true));
        assert(!state.active && !wakes && !detections);
    } else {
        assert(result == ESP_OK && state.supported);
        if (!strcmp(argv[1], "reset")) {
            assert(silent_checks == 1 && creates == 2 && destroys == 1);
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 2 && destroys == 1 && detections == 1);
            for (unsigned i = 0; i < 10; ++i) wake_word_reset();
            state.acknowledged = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 2 && destroys == 1);
            state.acknowledged = true;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(creates == 3 && destroys == 2 && detections == 2);
            ++state.generation;
            reset_failure = true;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!state.supported && !wakes && creates == 4 && destroys == 3);
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!wake_word_process(mono, 480, 1, false, true));
        } else if (!strcmp(argv[1], "routing")) {
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(wake_word_process(stereo, 480, 2, false, false));
            assert(detections == 2 && !wakes);
        } else if (!strcmp(argv[1], "wake")) {
            trigger = true;
            assert(wake_word_process(stereo, 480, 2, false, false));
            assert(wakes == 1 && state.active);
            assert(!wake_word_process(stereo, 480, 2, false, false));
            assert(detections == 1);
        } else if (!strcmp(argv[1], "gates")) {
            assert(wake_word_process(mono, 480, 1, true, false));
            time_us = 799999;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(!detections);
            time_us = 800000;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(detections == 1);
            assert(!wake_word_process(mono, 480, 1, false, true));
            time_us += 800000;
            state.acknowledged = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            state.acknowledged = true;
            state.connected = false;
            assert(wake_word_process(mono, 480, 1, false, false));
            assert(detections == 1 && !wakes);
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
        free(s_frame);
    }
    puts("Wake routing passed");
}

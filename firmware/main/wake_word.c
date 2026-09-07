#include "wake_word.h"
#include "voice_session.h"
#include "sdkconfig.h"

#if CONFIG_JUFF_WAKE_WORD
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "esp_wn_models.h"
#include "model_path.h"

#define WAKE_MODEL_COUNT 2
static const char *const s_keywords[WAKE_MODEL_COUNT] = {
    "himiaomiao_tts", "miaomiaotongxue_tts"
};
static const esp_wn_iface_t *s_model[WAKE_MODEL_COUNT];
static model_iface_data_t *s_data[WAKE_MODEL_COUNT];
static srmodel_list_t *s_models;
static const char *s_name[WAKE_MODEL_COUNT];
static vad_handle_t s_vad;
static int16_t *s_frame;
static size_t s_chunk;
static size_t s_used;
static atomic_bool s_ready;
static bool s_model_dirty;
static bool s_reset_pending;
static uint32_t s_generation;
static int64_t s_quiet_after_us;
static int16_t s_vad_frame[160];
static size_t s_vad_used;

static bool recreate_model(void)
{
    for (unsigned index = 0; index < WAKE_MODEL_COUNT; ++index) {
        if (s_data[index] != NULL) s_model[index]->destroy(s_data[index]);
        s_data[index] = s_model[index]->create(s_name[index], DET_MODE_90);
        if (s_data[index] == NULL || s_model[index]->get_samp_rate(s_data[index]) != 16000
            || s_model[index]->get_channel_num(s_data[index]) != 1
            || s_model[index]->get_samp_chunksize(s_data[index]) != (int)s_chunk) return false;
    }
    return true;
}

static void destroy_models(void)
{
    for (unsigned index = 0; index < WAKE_MODEL_COUNT; ++index) {
        if (s_data[index] != NULL) s_model[index]->destroy(s_data[index]);
        s_data[index] = NULL;
    }
}

esp_err_t wake_word_init(void)
{
    s_models = esp_srmodel_init("model");
    bool models_ready = s_models != NULL;
    for (unsigned index = 0; models_ready && index < WAKE_MODEL_COUNT; ++index) {
        char *name = esp_srmodel_filter(s_models, "wn9_", s_keywords[index]);
        s_name[index] = name;
        if (name != NULL) s_model[index] = esp_wn_handle_from_name(name);
        if (s_model[index] != NULL) s_data[index] = s_model[index]->create(name, DET_MODE_90);
        models_ready = s_data[index] != NULL
            && s_model[index]->get_samp_rate(s_data[index]) == 16000
            && s_model[index]->get_channel_num(s_data[index]) == 1;
        if (!models_ready) break;
        const int chunk = s_model[index]->get_samp_chunksize(s_data[index]);
        if (index == 0) s_chunk = chunk > 0 ? (size_t)chunk : 0;
        // Both fixed WakeNet9 models consume the same microphone frame.
        models_ready = chunk > 0 && chunk <= 1600 && (size_t)chunk == s_chunk;
    }
    if (models_ready) {
        s_frame = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_vad = vad_create_with_param(VAD_MODE_3, 16000, 10, 120, 100);
    }
    if (s_frame != NULL && s_vad != NULL) {
        // Check real inference and reset before capture can use the model.
        memset(s_frame, 0, s_chunk * sizeof(*s_frame));
        for (unsigned index = 0; index < WAKE_MODEL_COUNT; ++index)
            (void)s_model[index]->detect(s_data[index], s_frame);
        if (!recreate_model()) {
            free(s_frame);
            s_frame = NULL;
        }
    }
    if (s_frame == NULL || s_vad == NULL) {
        destroy_models();
        if (s_models != NULL) esp_srmodel_deinit(s_models);
        if (s_vad != NULL) vad_destroy(s_vad);
        free(s_frame);
        s_models = NULL;
        s_frame = NULL;
        s_vad = NULL;
        voice_session_set_supported(false);
        ESP_LOGE("juff_wake", "Offline wake model unavailable; check model partition");
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Publish only after initialization; capture owns all later model calls.
    atomic_store(&s_ready, true);
    voice_session_set_supported(true);
    ESP_LOGI("juff_wake", "Offline WakeNet ready: %s + %s, %u samples, 16000 Hz",
             s_name[0], s_name[1], (unsigned)s_chunk);
    ESP_LOGI("juff_wake", "Internal heap after model startup: free=%u, largest=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void wake_word_reset(void)
{
    if (!atomic_load(&s_ready)) return;
    s_used = s_vad_used = 0;
    // ESP-SR 2.5.3's WakeNet9 clean() walks nine queues although this model
    // allocates six, and crashes on hardware. Recreate through the public API
    // before the next listen instead; repeated idle/diagnostic resets stay cheap.
    s_reset_pending = s_reset_pending || s_model_dirty;
    vad_reset_trigger(s_vad);
}

bool wake_word_process(const int16_t *pcm, size_t frames, unsigned channels,
                       bool playing, bool diagnostic)
{
    const voice_session_status_t state = voice_session_get();
    const bool waiting = state.mode == VOICE_MODE_WAKE && !state.active;
    if (!atomic_load(&s_ready)) return waiting && !diagnostic;
    if (state.generation != s_generation) {
        wake_word_reset();
        s_generation = state.generation;
    }
    const int64_t now = esp_timer_get_time();
    if (playing || diagnostic) s_quiet_after_us = now + 800000;
    if (diagnostic) { wake_word_reset(); return false; }
    if (!waiting || !state.connected || !state.acknowledged || playing || diagnostic
        || now < s_quiet_after_us || pcm == NULL || frames % 3 != 0 || channels == 0) {
        if (s_used) wake_word_reset();
        return waiting;
    }
    if (s_reset_pending) {
        if (!recreate_model()) {
            destroy_models();
            atomic_store(&s_ready, false);
            voice_session_set_supported(false);
            ESP_LOGE("juff_wake", "Unable to reset offline wake model");
            return true;
        }
        s_model_dirty = s_reset_pending = false;
    }
    for (size_t index = 0; index < frames; index += 3) {
        const int16_t samples[2] = { pcm[index * channels],
            (int16_t)(((int32_t)pcm[(index + 1) * channels] + pcm[(index + 2) * channels]) / 2) };
        for (unsigned half = 0; half < 2; ++half) {
            s_frame[s_used++] = samples[half];
            if (s_used == s_chunk) {
                s_used = 0;
                s_model_dirty = true;
                for (unsigned model = 0; model < WAKE_MODEL_COUNT; ++model) {
                    if (s_model[model]->detect(s_data[model], s_frame) > 0) {
                        ESP_LOGI("juff_wake", "Wake phrase detected: %s", s_name[model]);
                        voice_session_wake();
                        wake_word_reset();
                        return true; // Neither wake phrase is sent to the cloud.
                    }
                }
            }
        }
    }
    return waiting;
}

void wake_word_observe_activity(const uint8_t *pcm, size_t bytes)
{
    const voice_session_status_t state = voice_session_get();
    if (!atomic_load(&s_ready) || state.mode != VOICE_MODE_WAKE || !state.active) return;
    for (size_t index = 0; index + sizeof(int16_t) <= bytes; index += sizeof(int16_t)) {
        memcpy(&s_vad_frame[s_vad_used++], pcm + index, sizeof(int16_t));
        if (s_vad_used != 160) continue;
        s_vad_used = 0;
        uint64_t energy = 0;
        for (unsigned sample = 0; sample < 160; ++sample) {
            const int32_t value = s_vad_frame[sample];
            energy += (uint64_t)(value * value);
        }
        if (vad_process_with_trigger(s_vad, s_vad_frame) == VAD_SPEECH
            && energy >= 180ULL * 180 * 160) voice_session_activity();
    }
}
#else
esp_err_t wake_word_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void wake_word_reset(void) {}
bool wake_word_process(const int16_t *pcm, size_t frames, unsigned channels, bool playing, bool diagnostic)
{ (void)pcm; (void)frames; (void)channels; (void)playing; return !diagnostic && voice_session_waiting(); }
void wake_word_observe_activity(const uint8_t *pcm, size_t bytes) { (void)pcm; (void)bytes; }
#endif

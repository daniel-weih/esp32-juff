#include "sdkconfig.h"

#if CONFIG_JUFF_VOICE_BARGE_IN

#include "voice_frontend.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_aec.h"
#include "esp_vad.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define SAMPLE_RATE 16000
#define VAD_SAMPLES 160
#define UPLOAD_SAMPLES 1600
#define HISTORY_SAMPLES 4800
#define PENDING_SAMPLES (HISTORY_SAMPLES + UPLOAD_SAMPLES * 2)
#define PLAYBACK_WARMUP_SAMPLES 6400
#define REFERENCE_MIN_RMS 80
#define DETECTOR_RECOVERY_SAMPLES 8000
#define SPEECH_CANDIDATE_SAMPLES 1920

static const char *TAG = "juff_aec";
static aec_handle_t *s_aec;
static vad_handle_t s_vad;
static int s_chunk_size;
static int16_t *s_mic;
static int16_t *s_reference;
static int16_t *s_clean;
static int s_chunk_used;
static int16_t s_vad_frame[VAD_SAMPLES];
static size_t s_vad_used;
static int16_t s_upload[UPLOAD_SAMPLES];
static size_t s_upload_used;
static int16_t *s_history;
static size_t s_history_position;
static size_t s_history_used;
static bool s_was_playing;
static bool s_was_pcm_started;
static bool s_reference_seen;
static uint32_t s_playback_generation;
static bool s_interrupted;
static bool s_pending_history;
static int16_t *s_pending;
static size_t s_pending_used;
static bool s_detection_allowed;
static size_t s_playback_samples;
static size_t s_recovery_samples;
static size_t s_speech_candidate_samples;
static float s_noise_rms = 60.0f;
static voice_frontend_pcm_callback_t s_pcm_callback;
static voice_frontend_interrupt_callback_t s_interrupt_callback;
static void *s_context;
static uint64_t s_mic_energy;
static uint64_t s_reference_energy;
static uint64_t s_clean_energy;
static size_t s_stats_samples;
static unsigned s_reference_clips;
static unsigned s_mic_peak;
static unsigned s_mic_fullscale;
static int64_t s_max_process_us;
static unsigned s_clean_peak_rms;
static unsigned s_vad_speech_frames;
static size_t s_candidate_peak_samples;

static void upload(const int16_t *samples, size_t count)
{
    // The callback only queues PCM; it must not wait for network delivery.
    while (count > 0) {
        size_t take = UPLOAD_SAMPLES - s_upload_used;
        if (take > count) take = count;
        memcpy(s_upload + s_upload_used, samples, take * sizeof(*samples));
        s_upload_used += take;
        samples += take;
        count -= take;
        if (s_upload_used == UPLOAD_SAMPLES) {
            s_pcm_callback((const uint8_t *)s_upload, sizeof(s_upload), s_context);
            s_upload_used = 0;
        }
    }
}

static void remember(const int16_t *samples)
{
    for (size_t index = 0; index < VAD_SAMPLES; ++index) {
        s_history[s_history_position] = samples[index];
        s_history_position = (s_history_position + 1) % HISTORY_SAMPLES;
        if (s_history_used < HISTORY_SAMPLES) ++s_history_used;
    }
}

static void freeze_history(void)
{
    const size_t start = (s_history_position + HISTORY_SAMPLES - s_history_used)
        % HISTORY_SAMPLES;
    size_t first = HISTORY_SAMPLES - start;
    if (first > s_history_used) first = s_history_used;
    memcpy(s_pending, s_history + start, first * sizeof(int16_t));
    memcpy(s_pending + first, s_history, (s_history_used - first) * sizeof(int16_t));
    s_pending_used = s_history_used;
    s_pending_history = true;
}

static void upload_history(void)
{
    upload(s_pending, s_pending_used);
    s_pending_history = false;
    ESP_LOGI(TAG, "Sent %u ms of speech preceding the interruption",
             (unsigned)(s_pending_used * 1000 / SAMPLE_RATE));
    s_pending_used = 0;
}

static void process_vad_frame(voice_frontend_playback_t playback,
                              bool upload_enabled, bool detect_speech)
{
    const bool playing = playback.playing;
    bool pending_has_current_frame = false;
    if (detect_speech) {
        remember(s_vad_frame);
        if (s_pending_history && s_pending_used + VAD_SAMPLES <= PENDING_SAMPLES) {
            memcpy(s_pending + s_pending_used, s_vad_frame, sizeof(s_vad_frame));
            s_pending_used += VAD_SAMPLES;
            pending_has_current_frame = true;
        }
    } else {
        s_history_position = s_history_used = s_pending_used = 0;
        s_pending_history = false;
        vad_reset_trigger(s_vad);
    }
    uint64_t energy = 0;
    for (size_t index = 0; index < VAD_SAMPLES; ++index) {
        const int32_t sample = s_vad_frame[index];
        energy += (uint64_t)(sample * sample);
    }
    const float rms = sqrtf((float)energy / VAD_SAMPLES);
    const vad_state_t speech = vad_process_with_trigger(s_vad, s_vad_frame);
    if (playing) {
        if (rms > s_clean_peak_rms) s_clean_peak_rms = (unsigned)rms;
        if (speech == VAD_SPEECH) ++s_vad_speech_frames;
    }
    if (!playing && speech == VAD_SILENCE && rms < 1000.0f) {
        s_noise_rms += (rms - s_noise_rms) / 64.0f;
    }
    if (s_recovery_samples > VAD_SAMPLES) s_recovery_samples -= VAD_SAMPLES;
    else s_recovery_samples = 0;

    if (playing) {
        if (playback.pcm_started && s_reference_seen) s_playback_samples += VAD_SAMPLES;
        const bool warm = playback.pcm_started && s_reference_seen
            && s_playback_samples >= PLAYBACK_WARMUP_SAMPLES
            && s_recovery_samples == 0;
        if (!warm || !detect_speech) {
            vad_reset_trigger(s_vad);
        }
        // VAD can stay in its speech state across low-energy frames. Require
        // 120 ms of fresh, continuously qualified signal before interrupting.
        if (warm && detect_speech && speech == VAD_SPEECH
            && rms >= fmaxf(180.0f, s_noise_rms * 3.0f)) {
            if (s_speech_candidate_samples < SPEECH_CANDIDATE_SAMPLES) {
                s_speech_candidate_samples += VAD_SAMPLES;
                if (s_speech_candidate_samples > s_candidate_peak_samples)
                    s_candidate_peak_samples = s_speech_candidate_samples;
            }
        } else {
            s_speech_candidate_samples = 0;
        }
        if (!s_interrupted && s_speech_candidate_samples >= SPEECH_CANDIDATE_SAMPLES) {
            // The application rejects voice interruption when explicitly muted.
            if (s_interrupt_callback != NULL && s_interrupt_callback(s_context)) {
                s_interrupted = true;
                freeze_history();
                pending_has_current_frame = true;
                ESP_LOGI(TAG, "Voice interruption detected: clean RMS=%u, noise=%u",
                         (unsigned)rms, (unsigned)s_noise_rms);
            }
        }
    } else {
        s_speech_candidate_samples = 0;
    }

    if (!playing && upload_enabled) {
        if (s_pending_history) {
            upload_history();
            if (!pending_has_current_frame) upload(s_vad_frame, VAD_SAMPLES);
        } else {
            upload(s_vad_frame, VAD_SAMPLES);
        }
    } else {
        s_upload_used = 0;
        // An external mute after interruption must discard buffered speech.
        if (!playing) s_pending_history = false;
    }
}

static void release_frontend(void)
{
    if (s_aec != NULL) aec_destroy(s_aec);
    if (s_vad != NULL) vad_destroy(s_vad);
    free(s_mic);
    free(s_reference);
    free(s_clean);
    free(s_history);
    free(s_pending);
    s_aec = NULL;
    s_vad = NULL;
    s_mic = s_reference = s_clean = NULL;
    s_history = s_pending = NULL;
}

esp_err_t voice_frontend_init(voice_frontend_pcm_callback_t pcm_callback,
                              voice_frontend_interrupt_callback_t interrupt_callback,
                              void *context)
{
    s_pcm_callback = pcm_callback;
    s_interrupt_callback = interrupt_callback;
    s_context = context;
    aec_config_t config = {
        .mic_num = 1, .ref_num = 1, .out_num = 1,
        .filter_length = 4, .sample_rate = SAMPLE_RATE,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_FD_HIGH_PERF,
        // The large board's digital reference precedes analog playback stages.
        // Its stronger residual echo needs a separate suppression setting.
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_35
        .nlp_level = AEC_NLP_LEVEL_VERYAGGR,
#else
        .nlp_level = AEC_NLP_LEVEL_NORMAL,
#endif
    };
    s_aec = aec_create_from_config(&config);
    s_vad = vad_create_with_param(VAD_MODE_3, SAMPLE_RATE, 10, 120, 100);
    if (s_aec == NULL || s_vad == NULL || pcm_callback == NULL) {
        release_frontend();
        return ESP_ERR_NO_MEM;
    }
    s_chunk_size = aec_get_chunksize(s_aec);
    if (s_chunk_size <= 0 || s_chunk_size > SAMPLE_RATE / 10) {
        release_frontend();
        return ESP_ERR_NOT_SUPPORTED;
    }
    const size_t size = (size_t)s_chunk_size * sizeof(int16_t);
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_mic = heap_caps_aligned_alloc(16, size, caps);
    s_reference = heap_caps_aligned_alloc(16, size, caps);
    s_clean = heap_caps_aligned_alloc(16, size, caps);
    const unsigned history_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_history = heap_caps_malloc(HISTORY_SAMPLES * sizeof(*s_history), history_caps);
    s_pending = heap_caps_malloc(PENDING_SAMPLES * sizeof(*s_pending), history_caps);
    if (s_mic == NULL || s_reference == NULL || s_clean == NULL
        || s_history == NULL || s_pending == NULL) {
        release_frontend();
        return ESP_ERR_NO_MEM;
    }
    voice_frontend_reset_stream();
    ESP_LOGI(TAG, "AEC ready: 1 microphone + 1 reference, %d Hz, %d samples; VAD 120 ms; mode=%d NLP=%d",
             SAMPLE_RATE, s_chunk_size, (int)config.mode, (int)config.nlp_level);
    return ESP_OK;
}

void voice_frontend_reset_stream(void)
{
    s_chunk_used = 0;
    s_vad_used = 0;
    s_upload_used = 0;
    s_history_position = s_history_used = 0;
    s_pending_history = false;
    s_pending_used = 0;
    s_detection_allowed = false;
    s_interrupted = false;
    s_was_playing = false;
    s_was_pcm_started = false;
    s_reference_seen = false;
    s_playback_generation = 0;
    s_playback_samples = 0;
    s_recovery_samples = DETECTOR_RECOVERY_SAMPLES;
    s_speech_candidate_samples = 0;
    if (s_vad != NULL) vad_reset_trigger(s_vad);
}

void voice_frontend_process(const int16_t *stereo, size_t frames,
                             voice_frontend_playback_t playback,
                             bool upload_enabled, bool detect_speech)
{
    if (s_aec == NULL || stereo == NULL || frames % 3 != 0) return;
    const bool playing = playback.playing;
    if (detect_speech != s_detection_allowed) {
        // Neither partial DSP frames nor history may cross an explicit mute or
        // session boundary. AEC coefficients remain live; only buffers reset.
        s_chunk_used = 0;
        s_vad_used = 0;
        s_upload_used = 0;
        s_history_position = s_history_used = s_pending_used = 0;
        s_pending_history = false;
        s_reference_seen = false;
        s_playback_samples = 0;
        s_recovery_samples = DETECTOR_RECOVERY_SAMPLES;
        s_speech_candidate_samples = 0;
        vad_reset_trigger(s_vad);
        s_detection_allowed = detect_speech;
    }
    const bool new_response = playback.generation != s_playback_generation;
    const bool pcm_started = playing && playback.pcm_started;
    if (new_response || (pcm_started && !s_was_pcm_started)) {
        // A partial AEC/VAD block from before first PCM must not count as
        // speaker warmup or bring a previous response's samples into this one.
        s_chunk_used = 0;
        s_vad_used = 0;
        s_reference_seen = false;
        s_playback_samples = 0;
    }
    if (new_response || playing != s_was_playing) {
        s_upload_used = 0;
        s_speech_candidate_samples = 0;
        vad_reset_trigger(s_vad);
        if (new_response || playing) {
            s_playback_samples = 0;
            s_reference_seen = false;
            s_interrupted = false;
            s_pending_history = false;
            s_pending_used = 0;
            s_history_position = s_history_used = 0;
            s_mic_energy = s_reference_energy = s_clean_energy = 0;
            s_stats_samples = 0;
            s_reference_clips = 0;
            s_mic_peak = s_mic_fullscale = 0;
            s_max_process_us = 0;
            s_clean_peak_rms = s_vad_speech_frames = 0;
            s_candidate_peak_samples = 0;
        }
        s_was_playing = playing;
        s_playback_generation = playback.generation;
    }
    s_was_pcm_started = pcm_started;
    // Both ADC channels use the same 3:2 sample positions and interpolation.
    for (size_t position = 0; position < frames; position += 3) {
        for (int half = 0; half < 2; ++half) {
            const size_t source = (position + (size_t)half) * 2;
            s_mic[s_chunk_used] = half == 0 ? stereo[source]
                : (int16_t)(((int32_t)stereo[source] + stereo[source + 2]) / 2);
            s_reference[s_chunk_used] = half == 0 ? stereo[source + 1]
                : (int16_t)(((int32_t)stereo[source + 1] + stereo[source + 3]) / 2);
            if (++s_chunk_used != s_chunk_size) continue;
            // First PCM can contain substantial silence. AEC convergence can
            // only start once its actual speaker-reference input has energy.
            uint64_t reference_energy = 0;
            for (int index = 0; index < s_chunk_size; ++index) {
                const int32_t reference = s_reference[index];
                reference_energy += (uint64_t)(reference * reference);
                if (playing) {
                    // ESP-SR accepts writable input buffers. Measure the ADC
                    // signal before any library preprocessing can alter it.
                    const int32_t mic = s_mic[index];
                    s_mic_energy += (uint64_t)(mic * mic);
                    const unsigned magnitude = (unsigned)(mic < 0 ? -mic : mic);
                    if (magnitude > s_mic_peak) s_mic_peak = magnitude;
                    if (mic == INT16_MAX || mic == INT16_MIN) ++s_mic_fullscale;
                    s_reference_energy += (uint64_t)(reference * reference);
                    if (reference >= 32000 || reference <= -32000) ++s_reference_clips;
                }
            }
            const bool reference_active = reference_energy
                >= (uint64_t)REFERENCE_MIN_RMS * REFERENCE_MIN_RMS * s_chunk_size;
            if (playing && playback.pcm_started && detect_speech
                && reference_active && !s_reference_seen) {
                s_reference_seen = true;
                ESP_LOGI(TAG, "Speaker reference detected (RMS=%u); starting 400 ms warmup",
                         (unsigned)sqrt((double)reference_energy / s_chunk_size));
            }
            const int64_t started = esp_timer_get_time();
            aec_process(s_aec, s_mic, s_reference, s_clean);
            const int64_t elapsed = esp_timer_get_time() - started;
            if (elapsed > s_max_process_us) s_max_process_us = elapsed;
            s_chunk_used = 0;
            for (int index = 0; index < s_chunk_size; ++index) {
                if (playing) {
                    const int32_t clean = s_clean[index];
                    s_clean_energy += (uint64_t)(clean * clean);
                    ++s_stats_samples;
                }
                s_vad_frame[s_vad_used++] = s_clean[index];
                if (s_vad_used == VAD_SAMPLES) {
                    process_vad_frame(playback, upload_enabled, detect_speech);
                    s_vad_used = 0;
                }
            }
            if (playing && s_stats_samples >= SAMPLE_RATE) {
                ESP_LOGI(TAG, "Playback AEC: mic=%u ref=%u clean=%u ref_clips=%u max_us=%lld peak=%u vad_ms=%u candidate_ms=%u noise=%u mic_peak=%u mic_fullscale=%u",
                         (unsigned)sqrt((double)s_mic_energy / s_stats_samples),
                         (unsigned)sqrt((double)s_reference_energy / s_stats_samples),
                         (unsigned)sqrt((double)s_clean_energy / s_stats_samples),
                         s_reference_clips, (long long)s_max_process_us,
                         s_clean_peak_rms, s_vad_speech_frames * 10,
                         (unsigned)(s_candidate_peak_samples * 1000 / SAMPLE_RATE),
                         (unsigned)s_noise_rms, s_mic_peak, s_mic_fullscale);
                s_mic_energy = s_reference_energy = s_clean_energy = 0;
                s_stats_samples = 0;
                s_reference_clips = 0;
                s_mic_peak = s_mic_fullscale = 0;
                s_max_process_us = 0;
                s_clean_peak_rms = s_vad_speech_frames = 0;
                s_candidate_peak_samples = 0;
            }
        }
    }
}

#endif

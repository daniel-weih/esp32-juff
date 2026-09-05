#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*voice_frontend_pcm_callback_t)(const uint8_t *, size_t, void *);
typedef bool (*voice_frontend_interrupt_callback_t)(void *);

typedef struct {
    // Changes for every response, even when capture misses an idle interval.
    uint32_t generation;
    // Includes a response waiting for its first PCM; uploading stays paused.
    bool playing;
    // True only once the playback task is about to submit the first PCM.
    bool pcm_started;
} voice_frontend_playback_t;

esp_err_t voice_frontend_init(voice_frontend_pcm_callback_t pcm_callback,
                              voice_frontend_interrupt_callback_t interrupt_callback,
                              void *context);
// Interleaved microphone/reference at 24 kHz, in blocks divisible by 3 frames.
void voice_frontend_process(const int16_t *stereo, size_t frames,
                             voice_frontend_playback_t playback,
                             bool upload_enabled, bool detect_speech);
void voice_frontend_reset_stream(void);

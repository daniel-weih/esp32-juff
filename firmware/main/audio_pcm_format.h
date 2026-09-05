#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*audio_pcm_write_fn_t)(void *context, const void *data,
                                    size_t bytes, size_t *written);

enum {
    AUDIO_PCM_WRITE_INVALID = -1,
    AUDIO_PCM_WRITE_NO_PROGRESS = -2,
    AUDIO_PCM_WRITE_BAD_PROGRESS = -3,
};

/* The writer reports consumed bytes, including partial frames. Keep the same
 * scratch block alive until every byte has been written, without duplicating
 * any prefix. Platform timeout/cancellation policy belongs to the callback. */
static inline int audio_pcm_write_mono16(const void *pcm, size_t bytes,
                                          bool stereo, int16_t *scratch,
                                          size_t scratch_frames,
                                          audio_pcm_write_fn_t write_fn,
                                          void *context)
{
    if ((bytes % sizeof(int16_t)) != 0 || write_fn == NULL
        || scratch_frames == 0 || scratch_frames > SIZE_MAX / (2 * sizeof(int16_t))
        || (bytes != 0 && pcm == NULL) || (stereo && scratch == NULL)) {
        return AUDIO_PCM_WRITE_INVALID;
    }
    const uint8_t *input = pcm;
    size_t frames = bytes / sizeof(int16_t);
    while (frames != 0) {
        const size_t count = frames < scratch_frames ? frames : scratch_frames;
        const uint8_t *output = input;
        size_t remaining = count * sizeof(int16_t);
        if (stereo) {
            for (size_t index = 0; index < count; ++index) {
                int16_t sample;
                memcpy(&sample, input + index * sizeof(sample), sizeof(sample));
                scratch[index * 2] = sample;
                scratch[index * 2 + 1] = sample;
            }
            output = (const uint8_t *)scratch;
            remaining *= 2;
        }
        while (remaining != 0) {
            size_t written = 0;
            const int result = write_fn(context, output, remaining, &written);
            if (written > remaining) return AUDIO_PCM_WRITE_BAD_PROGRESS;
            if (result != 0) return result;
            if (written == 0) return AUDIO_PCM_WRITE_NO_PROGRESS;
            output += written;
            remaining -= written;
        }
        input += count * sizeof(int16_t);
        frames -= count;
    }
    return 0;
}

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_pcm_format.h"

enum { MAX_SAMPLES = 4097, MAX_SCRATCH_FRAMES = 480 };
typedef struct {
    uint8_t data[MAX_SAMPLES * 4];
    size_t size, calls, limit, failure_call;
    int failure_mode;
} transport_t;

static int transport_write(void *context, const void *data, size_t bytes,
                           size_t *written)
{
    transport_t *transport = context;
    assert(bytes > 0);
    assert(++transport->calls <= MAX_SAMPLES * 4);
    if (transport->calls == transport->failure_call) {
        if (transport->failure_mode == 1) {
            *written = 0;
            return 77;
        }
        *written = transport->failure_mode == 2 ? 0 : bytes + 1;
        return 0;
    }
    *written = transport->limit != 0 && bytes > transport->limit
        ? transport->limit : bytes;
    assert(transport->size + *written <= sizeof(transport->data));
    memcpy(transport->data + transport->size, data, *written);
    transport->size += *written;
    return 0;
}

static void make_input(uint8_t *input, size_t count)
{
    const int16_t edges[] = { INT16_MIN, INT16_MAX, -1, 0, 1, -12345, 23456 };
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = i < sizeof(edges) / sizeof(edges[0])
            ? edges[i] : (int16_t)(i * 193U);
        memcpy(input + i * sizeof(sample), &sample, sizeof(sample));
    }
}

static void check_stream(bool stereo)
{
    const size_t lengths[] = { 0, 1, 2, 3, 127, 128, 129, MAX_SAMPLES };
    const size_t capacities[] = { 1, 2, 7, MAX_SCRATCH_FRAMES };
    // Odd transport progress deliberately straddles samples and L/R slots.
    const size_t limits[] = { 0, 1, 2, 3, 7, 19, 1918 };
    uint8_t *allocation = malloc(MAX_SAMPLES * 2 + 2);
    assert(allocation != NULL);
    uint8_t *input = allocation + 1; // Public void* input need not be aligned.
    allocation[0] = 0x5a;
    allocation[MAX_SAMPLES * 2 + 1] = 0xa5;
    make_input(input, MAX_SAMPLES);
    uint8_t original[MAX_SAMPLES * 2];
    memcpy(original, input, sizeof(original));
    for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); ++c) {
            // Allocate exactly the advertised workspace: sanitizers detect an
            // off-by-one write even when it does not corrupt final output.
            const size_t scratch_frames = capacities[c];
            int16_t *scratch = malloc(scratch_frames * 2 * sizeof(*scratch));
            assert(scratch != NULL);
            for (size_t p = 0; p < sizeof(limits) / sizeof(limits[0]); ++p) {
                transport_t transport = { .limit = limits[p] };
                const int result = audio_pcm_write_mono16(
                    input, lengths[n] * 2, stereo,
                    stereo ? scratch : NULL, scratch_frames,
                    transport_write, &transport);
                assert(result == 0);
                assert(transport.size == lengths[n] * (stereo ? 4 : 2));
                assert(lengths[n] != 0 || transport.calls == 0);
                if (stereo) {
                    // Both observable slots must match the source sample,
                    // including its sign and byte order, with no padding.
                    for (size_t i = 0; i < lengths[n]; ++i) {
                        assert(memcmp(transport.data + i * 4, input + i * 2, 2) == 0);
                        assert(memcmp(transport.data + i * 4 + 2, input + i * 2, 2) == 0);
                    }
                } else {
                    assert(memcmp(transport.data, input, lengths[n] * 2) == 0);
                }
                assert(memcmp(input, original, sizeof(original)) == 0);
                assert(allocation[0] == 0x5a && allocation[MAX_SAMPLES * 2 + 1] == 0xa5);
            }
            free(scratch);
        }
    }
    free(allocation);
}

static void invalid_arguments(void)
{
    int16_t samples[] = { -32768, 0, 32767 };
    int16_t scratch[4] = { 0 };
    transport_t transport = { 0 };
    for (unsigned stereo = 0; stereo < 2; ++stereo) {
        assert(audio_pcm_write_mono16(samples, 3, stereo, scratch, 2,
                                      transport_write, &transport) != 0);
        assert(audio_pcm_write_mono16(NULL, 2, stereo, scratch, 2,
                                      transport_write, &transport) != 0);
        assert(audio_pcm_write_mono16(samples, sizeof(samples), stereo, scratch, 2,
                                      NULL, &transport) != 0);
    }
    assert(audio_pcm_write_mono16(samples, sizeof(samples), true, NULL, 2,
                                  transport_write, &transport) != 0);
    assert(audio_pcm_write_mono16(samples, sizeof(samples), true, scratch, 0,
                                  transport_write, &transport) != 0);
    assert(transport.calls == 0 && transport.size == 0);
}

static void transport_errors(void)
{
    int16_t samples[] = { -32768, 123, 32767, -20000, 456 };
    int16_t scratch[4] = { 0 };
    for (unsigned stereo = 0; stereo < 2; ++stereo) {
        for (int mode = 1; mode <= 3; ++mode) {
            transport_t transport = { .limit = 3, .failure_call = 2,
                                      .failure_mode = mode };
            const int result = audio_pcm_write_mono16(
                samples, sizeof(samples), stereo, scratch, 2,
                transport_write, &transport);
            assert(result != 0);
            if (mode == 1) assert(result == 77);
            assert(transport.calls == 2 && transport.size == 3);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "stereo") == 0) check_stream(true);
    else if (strcmp(argv[1], "mono") == 0) check_stream(false);
    else if (strcmp(argv[1], "invalid") == 0) invalid_arguments();
    else if (strcmp(argv[1], "transport-errors") == 0) transport_errors();
    else assert(!"Unknown scenario");
    puts("PCM wire format passed");
    return 0;
}

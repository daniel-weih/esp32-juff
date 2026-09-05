#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "speaker_flow.h"
#include "ble_speaker_config.h"

#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
typedef int esp_err_t;
typedef int *SemaphoreHandle_t;
static int speaker_mutex, credit_mutex;
static SemaphoreHandle_t s_speaker_mutex = &speaker_mutex;
static SemaphoreHandle_t s_credit_mutex = &credit_mutex;
static int s_flow_lock;
static int s_state_lock;
static bool mutex_held, credit_mutex_held, critical_held;
static void (*after_snapshot)(void);
static bool refuse_pcm_lock;
static speaker_flow_t s_speaker_flow;
static bool s_flow_enabled = true;
static bool s_flow_abort_requested;
static bool s_playback_suspended;
static uint32_t s_microphone_generation;
static char s_flow_response[64];
static const char *s_flow_state = "receiving";
static bool s_speaker_response_active;
static uint8_t s_speaker_buffer[BLE_SPEAKER_BUFFER_BYTES];
static size_t s_speaker_length;
static uint32_t s_dropped_speaker_bytes;

static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{
    assert(!critical_held);
    if (mutex == &credit_mutex) {
        assert(ticks == 0);
        if (credit_mutex_held) return 0;
        credit_mutex_held = true;
        return pdTRUE;
    }
    assert(mutex == &speaker_mutex && (ticks == 0 || ticks == 20 || ticks == 100));
    if (ticks == 0 && mutex_held) return 0;
    assert(!mutex_held);
    if (ticks == 20 && refuse_pcm_lock) return 0;
    mutex_held = true;
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (mutex == &credit_mutex) {
        assert(credit_mutex_held && !critical_held);
        credit_mutex_held = false;
        return;
    }
    assert(mutex == &speaker_mutex && mutex_held && !critical_held);
    mutex_held = false;
    if (after_snapshot) {
        void (*action)(void) = after_snapshot;
        after_snapshot = NULL;
        action();
    }
}
static void portENTER_CRITICAL(int *lock)
{
    assert((lock == &s_flow_lock || lock == &s_state_lock) && !critical_held);
    critical_held = true;
}
static void portEXIT_CRITICAL(int *lock)
{
    assert((lock == &s_flow_lock || lock == &s_state_lock) && critical_held);
    critical_held = false;
}

static uint8_t queued[192000];
static size_t queued_size;
static unsigned push_calls, fail_push_at, cancel_push_at, clear_calls;
static unsigned begin_calls, end_calls, credit_calls, capture_calls, ui_calls;
static bool fail_begin, fail_end, complete_inline;
static char credit[400];

typedef struct {
    uint32_t queued_bytes, capacity_bytes, underruns;
} audio_playback_buffer_status_t;

static audio_playback_buffer_status_t audio_io_playback_buffer_status(void)
{
    assert(mutex_held && !critical_held);
    return (audio_playback_buffer_status_t){
        .queued_bytes = (uint32_t)queued_size, .capacity_bytes = sizeof(queued),
    };
}

static bool notify_text(const char *text)
{
    assert(!mutex_held && !critical_held && credit_mutex_held);
    assert(strlen(text) < sizeof(credit));
    strcpy(credit, text);
    ++credit_calls;
    return true;
}

static size_t harness_strlcpy(char *target, const char *source, size_t capacity)
{
    const size_t length = strlen(source);
    if (capacity > 0) {
        const size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(target, source, copy);
        target[copy] = '\0';
    }
    return length;
}
#define strlcpy harness_strlcpy

static void mark_speaker_terminal(const char *response_id, const char *state);
static esp_err_t audio_io_begin_response(const char *response_id, uint32_t rate)
{
    assert(mutex_held && !critical_held && response_id != NULL && rate == 24000);
    ++begin_calls;
    return fail_begin ? ESP_ERR_NO_MEM : ESP_OK;
}
static esp_err_t audio_io_end_response(const char *response_id)
{
    assert(mutex_held && !critical_held && response_id != NULL);
    assert(s_speaker_length == 0);
    if (s_flow_enabled) assert(s_speaker_flow.draining);
    ++end_calls;
    if (fail_end) return ESP_ERR_NO_MEM;
    if (complete_inline) mark_speaker_terminal(response_id, "ended");
    return ESP_OK;
}
static void audio_io_set_capture_enabled(bool enabled)
{
    assert(!critical_held && !enabled);
    ++capture_calls;
}
static void board_display_set_voice_state(const char *state)
{
    assert(!critical_held && strcmp(state, "speaking") == 0);
    ++ui_calls;
}
static esp_err_t audio_io_push_pcm(const void *data, size_t size)
{
    assert(mutex_held && !critical_held);
    assert(size > 0 && size <= BLE_SPEAKER_CHUNK_BYTES && size % 2 == 0);
    if (++push_calls == fail_push_at) return -1;
    assert(queued_size + size <= sizeof(queued));
    memcpy(queued + queued_size, data, size);
    queued_size += size;
    if (push_calls == cancel_push_at) {
        // Model a playback/interrupt callback cancelling while an enqueue was
        // in progress. It can invalidate flow without taking the speaker mutex.
        portENTER_CRITICAL(&s_flow_lock);
        speaker_flow_cancel(&s_speaker_flow);
        s_flow_state = "cancelled";
        portEXIT_CRITICAL(&s_flow_lock);
        queued_size = 0;
    }
    return ESP_OK;
}
static void audio_io_clear(const char *reason)
{
    assert(reason != NULL && !critical_held);
    ++clear_calls;
    queued_size = 0;
}

#include "ble_speaker.inc"

static void start(uint32_t stream_id, uint32_t budget)
{
    assert(speaker_flow_start(&s_speaker_flow, stream_id, budget));
    s_speaker_response_active = true;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static void frame(uint32_t stream_id, uint32_t offset, size_t size, uint8_t encoded)
{
    // Realistic ATT value sizes: the largest fixture is 508 bytes including
    // the wire header, within the production flattened-mbuf buffer.
    uint8_t value[508];
    assert(size <= sizeof(value) - 8);
    put_le32(value, stream_id);
    put_le32(value + 4, offset);
    memset(value + 8, encoded, size);
    append_speaker_mulaw(value, size + 8);
    assert(!mutex_held && !critical_held);
}

static void valid_frame(void)
{
    start(0x01020304, 96000);
    frame(0x01020304, 0, 500, 0x00);
    assert(s_speaker_flow.received == 500 && s_speaker_length == 1000);
    assert(push_calls == 0 && clear_calls == 0);
    // G.711 mu-law 0x00 decodes to -32124; neither stream nor offset header is
    // interpreted as audio. A second packet appends at the payload offset.
    for (size_t i = 0; i < s_speaker_length; i += 2) {
        assert(s_speaker_buffer[i] == 0x84 && s_speaker_buffer[i + 1] == 0x82);
    }
    frame(0x01020304, 500, 500, 0xff);
    assert(s_speaker_flow.received == 1000 && s_speaker_length == 2000);
    for (size_t i = 1000; i < s_speaker_length; ++i) assert(s_speaker_buffer[i] == 0);
}

static void stale_frame(void)
{
    start(7, 96000);
    frame(7, 0, 500, 0x00);
    uint8_t staged[1000];
    memcpy(staged, s_speaker_buffer, sizeof(staged));
    frame(6, 0, 500, 0xff);
    frame(8, 0, 500, 0xff);
    assert(s_speaker_flow.active && s_speaker_flow.stream_id == 7);
    assert(s_speaker_flow.received == 500 && s_speaker_length == sizeof(staged));
    assert(memcmp(staged, s_speaker_buffer, sizeof(staged)) == 0);
    assert(push_calls == 0 && clear_calls == 0 && s_dropped_speaker_bytes == 0);
}

static void rejected_frame(const char *kind)
{
    start(7, 500);
    frame(7, 0, 64, 0x00);
    if (strcmp(kind, "bad-header") == 0) {
        uint8_t empty_payload[8] = {7};
        append_speaker_mulaw(empty_payload, sizeof(empty_payload));
    } else if (strcmp(kind, "bad-offset") == 0) frame(7, 65, 1, 0xff);
    else if (strcmp(kind, "over-credit") == 0) frame(7, 64, 437, 0xff);
    else assert(false);
    assert(!s_speaker_flow.active && s_speaker_flow.received == 64);
    assert(!s_speaker_response_active && s_speaker_length == 0 && queued_size == 0);
    assert(strcmp(s_flow_state, "cancelled") == 0 && clear_calls == 1);
    assert(!mutex_held && !critical_held);
}

static void failure_after_earlier_queue(bool asynchronous_cancel)
{
    start(9, 96000);
    for (unsigned i = 0; i < 9; ++i) frame(9, i * 500U, 500, 0x00);
    assert(s_speaker_flow.received == 4500 && push_calls == 1);
    assert(queued_size == 4800 && s_speaker_length == 4200);
    if (asynchronous_cancel) cancel_push_at = 2;
    else fail_push_at = 2;
    frame(9, 4500, 500, 0xff);
    assert(!s_speaker_flow.active && s_speaker_flow.received == 4500);
    assert(push_calls == 2 && clear_calls == 1);
    assert(s_speaker_length == 0 && queued_size == 0 && !s_speaker_response_active);
    assert(strcmp(s_flow_state, "cancelled") == 0);
    // A subsequent stale packet cannot resurrect the remaining split data.
    frame(9, 4500, 500, 0xff);
    assert(push_calls == 2 && clear_calls == 1 && s_speaker_length == 0);
}

static void legacy_payload(void)
{
    s_flow_enabled = false;
    s_speaker_response_active = true;
    const uint8_t value[16] = {0};
    append_speaker_mulaw(value, sizeof(value));
    assert(s_speaker_length == 32 && s_speaker_flow.received == 0);
    assert(xSemaphoreTake(s_speaker_mutex, 20));
    assert(flush_speaker_locked(true));
    xSemaphoreGive(s_speaker_mutex);
    assert(push_calls == 1 && queued_size == 32 && s_speaker_length == 0);
    assert(queued[0] == 0x84 && queued[1] == 0x82 && clear_calls == 0);
}

static void initial_budget(void)
{
    queued_size = 96000; // The prior response still occupies half the PCM budget.
    const uint32_t microphone_generation = s_microphone_generation;
    assert(begin_ble_speaker("response.17", 24000, 1, 17) == ESP_OK);
    assert(s_microphone_generation != microphone_generation);
    assert(begin_calls == 1 && capture_calls == 1 && ui_calls == 1 && s_playback_suspended);
    assert(s_speaker_flow.active && s_speaker_flow.received == 0 && s_speaker_flow.limit == 48000);
    assert(credit_calls == 1 && strstr(credit, "\"responseId\":\"response.17\"") != NULL);
    assert(strstr(credit, "\"streamId\":17") != NULL && strstr(credit, "\"state\":\"open\"") != NULL);
    assert(!s_flow_abort_requested);
}

static void begin_rejection(void)
{
    assert(begin_ble_speaker("response", 24000, 1, 0) == ESP_ERR_INVALID_ARG);
    assert(begin_ble_speaker("response", 24000, 2, 1) == ESP_ERR_INVALID_ARG);
    assert(begin_ble_speaker("response", 16000, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(begin_ble_speaker("bad\"response", 24000, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(begin_calls == 0 && credit_calls == 0 && !s_speaker_flow.active);
    fail_begin = true;
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_ERR_NO_MEM);
    assert(!s_speaker_flow.active && s_speaker_flow.stream_id == 0);
    assert(capture_calls == 0 && ui_calls == 0 && !s_playback_suspended);
    fail_begin = false;
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_OK);
    const unsigned before = begin_calls;
    assert(begin_ble_speaker("replay", 24000, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(begin_calls == before && s_speaker_flow.stream_id == 1);
    assert(strcmp(s_flow_response, "response") == 0 && s_speaker_flow.active);
}

static void done_sequence(bool inline_completion)
{
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_OK);
    frame(1, 0, 500, 0x00);
    assert(end_ble_speaker("old-response", 1) == ESP_ERR_INVALID_ARG);
    assert(end_ble_speaker("response", 2) == ESP_ERR_INVALID_ARG);
    assert(s_speaker_flow.active && !s_speaker_flow.draining && s_speaker_length == 1000);
    assert(end_calls == 0 && clear_calls == 0);
    complete_inline = inline_completion;
    assert(end_ble_speaker("response", 1) == ESP_OK);
    assert(push_calls == 1 && queued_size == 1000 && end_calls == 1);
    assert(s_speaker_length == 0 && !s_speaker_response_active);
    assert(s_speaker_flow.received == 500 && s_speaker_flow.limit == 96000);
    if (inline_completion) {
        assert(!s_speaker_flow.active && strcmp(s_flow_state, "ended") == 0);
        assert(strstr(credit, "\"state\":\"ended\"") != NULL);
    } else {
        assert(s_speaker_flow.draining && strcmp(s_flow_state, "draining") == 0);
        assert(strstr(credit, "\"state\":\"draining\"") != NULL);
        assert(end_ble_speaker("response", 1) == ESP_OK && end_calls == 1);
    }
    assert(clear_calls == 0 && s_playback_suspended);
}

static void done_queue_failure(bool tail_failure)
{
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_OK);
    frame(1, 0, 500, 0x00);
    if (tail_failure) fail_push_at = 1;
    else fail_end = true;
    assert(end_ble_speaker("response", 1) == ESP_ERR_NO_MEM);
    assert(!s_speaker_flow.active && s_speaker_flow.received == 500);
    assert(s_speaker_length == 0 && queued_size == 0 && clear_calls == 1);
    assert(end_calls == (tail_failure ? 0U : 1U));
    assert(strcmp(s_flow_state, "cancelled") == 0);
    assert(strstr(credit, "\"state\":\"cancelled\"") != NULL);
}

static void credit_accounting(void)
{
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_OK);
    for (unsigned i = 0; i < 9; ++i) frame(1, i * 500U, 500, 0xff);
    assert(queued_size == 4800 && s_speaker_length == 4200);
    for (unsigned i = 0; i < 20; ++i) notify_speaker_credit();
    assert(s_speaker_flow.received == 4500 && s_speaker_flow.limit == 96000);
    assert(strstr(credit, "\"queuedMs\":187") != NULL);
    queued_size -= 2400; // Playback frees 50 ms of PCM, or 1200 mu-law bytes.
    notify_speaker_credit();
    assert(s_speaker_flow.limit == 97200 && s_speaker_flow.received == 4500);
    mark_speaker_terminal("response", "ended");
    for (unsigned i = 0; i < 3; ++i) notify_speaker_credit();
    assert(strcmp(s_flow_state, "ended") == 0 && s_speaker_flow.limit == 97200);
    assert(strstr(credit, "\"received\":4500") != NULL);
}

static void receive_contention(void)
{
    assert(begin_ble_speaker("response", 24000, 1, 9) == ESP_OK);
    for (unsigned i = 0; i < 9; ++i) frame(9, i * 500U, 500, 0xff);
    refuse_pcm_lock = true;
    frame(8, 4500, 500, 0xff); // A delayed old packet cannot abort the current stream.
    assert(s_speaker_flow.active && !s_flow_abort_requested && clear_calls == 0);
    frame(9, 4500, 500, 0xff);
    assert(!s_speaker_flow.active && s_flow_abort_requested);
    assert(s_speaker_flow.received == 4500 && queued_size == 4800);
    refuse_pcm_lock = false;
    notify_speaker_credit();
    assert(!s_flow_abort_requested && !s_speaker_response_active);
    assert(s_speaker_length == 0 && queued_size == 0 && clear_calls == 1);
    assert(strstr(credit, "\"state\":\"cancelled\"") != NULL);
    assert(s_speaker_flow.received == 4500);
}

static void competing_credit_publisher(void)
{
    // A playback block drains while the first publisher is between snapshot
    // and notify_text. The second producer must not overtake the first.
    queued_size -= 4800;
    const unsigned before = credit_calls;
    notify_speaker_credit();
    assert(credit_calls == before);
}

static void credit_publication_race(void)
{
    assert(begin_ble_speaker("response", 24000, 1, 1) == ESP_OK);
    for (unsigned i = 0; i < 96; ++i) frame(1, i * 500U, 500, 0xff);
    assert(s_speaker_flow.received == 48000 && queued_size == 96000);
    const unsigned before = credit_calls;
    after_snapshot = competing_credit_publisher;
    notify_speaker_credit();
    assert(credit_calls == before + 1);
    assert(strstr(credit, "\"received\":48000,\"limit\":96000") != NULL);
    assert(!credit_mutex_held);
    notify_speaker_credit(); // Next periodic tick publishes the skipped update.
    assert(credit_calls == before + 2);
    assert(strstr(credit, "\"received\":48000,\"limit\":98400") != NULL);
    // Contention while sampling must also release the publication mutex.
    mutex_held = true;
    notify_speaker_credit();
    mutex_held = false;
    assert(!credit_mutex_held && credit_calls == before + 2);
    notify_speaker_credit();
    assert(credit_calls == before + 3 && !credit_mutex_held);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "valid-frame") == 0) valid_frame();
    else if (strcmp(argv[1], "stale-frame") == 0) stale_frame();
    else if (strcmp(argv[1], "bad-header") == 0 || strcmp(argv[1], "bad-offset") == 0 ||
             strcmp(argv[1], "over-credit") == 0) rejected_frame(argv[1]);
    else if (strcmp(argv[1], "queue-failure") == 0) failure_after_earlier_queue(false);
    else if (strcmp(argv[1], "cancel-in-enqueue") == 0) failure_after_earlier_queue(true);
    else if (strcmp(argv[1], "legacy") == 0) legacy_payload();
    else if (strcmp(argv[1], "initial-budget") == 0) initial_budget();
    else if (strcmp(argv[1], "begin-rejection") == 0) begin_rejection();
    else if (strcmp(argv[1], "done-sequence") == 0) done_sequence(false);
    else if (strcmp(argv[1], "done-inline") == 0) done_sequence(true);
    else if (strcmp(argv[1], "done-tail-failure") == 0) done_queue_failure(true);
    else if (strcmp(argv[1], "done-marker-failure") == 0) done_queue_failure(false);
    else if (strcmp(argv[1], "credit-accounting") == 0) credit_accounting();
    else if (strcmp(argv[1], "receive-contention") == 0) receive_contention();
    else if (strcmp(argv[1], "credit-race") == 0) credit_publication_race();
    else assert(false);
    puts("BLE speaker production path passed");
    return 0;
}

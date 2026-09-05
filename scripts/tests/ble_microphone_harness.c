#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>

#include "ble_microphone_config.h"

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10U)
#define pdTRUE 1
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define BLE_HS_EAGAIN 1
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EINVAL 3
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EBUSY 15
#define BLE_HS_ENOMEM_EVT 20
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

static int s_state_lock;
static uint16_t s_connection_handle = 7, s_microphone_handle = 42;
static uint32_t s_microphone_generation = 9, s_dropped_microphone_frames;
static uint32_t s_microphone_sent_generation;
static bool s_microphone_needs_reset = true;
static bool s_microphone_notify_enabled = true, s_voice_ready = true;
static bool s_input_suspended, s_playback_suspended, s_notify_enabled, s_advertising;
static const char *TAG = "test";
static void *s_microphone_queue = (void *)1;
static ble_audio_frame_t *queued[BLE_MIC_QUEUE_DEPTH];
static size_t queue_count;
static TickType_t now;
static unsigned waits, yields, attempts, allocations, released;
static unsigned allocation_failures, failures;
static bool slow_progress;
static TickType_t last_progress;
static int failure_code = BLE_HS_ENOMEM;
static size_t fail_after;
static uint16_t mtu = 256;
static uint8_t received[BLE_MIC_QUEUE_DEPTH * BLE_MIC_ENCODED_FRAME_MAX_BYTES];
static size_t received_size;
static char last_log[256];
static unsigned reset_failures, reset_attempts, resets;
static bool trace_notifications;
static void (*wait_action)(void);
static void (*yield_action)(void);

static void log_warning(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
}
#define ESP_LOGW(...) log_warning(__VA_ARGS__)

static bool notify_text(const char *text)
{
    assert(strstr(text, "\"type\":\"microphone.reset\"") != NULL);
    ++reset_attempts;
    if (reset_failures) { --reset_failures; return false; }
    ++resets;
    if (trace_notifications) puts(text);
    return true;
}

static TickType_t xTaskGetTickCount(void) { return now; }
static void vTaskDelay(TickType_t ticks)
{
    assert(ticks >= 1);
    ++waits;
    now += ticks;
    if (wait_action) { void (*action)(void) = wait_action; wait_action = NULL; action(); }
}
static void test_yield(void)
{
    ++yields;
    if (yield_action) { void (*action)(void) = yield_action; yield_action = NULL; action(); }
}
#define taskYIELD() test_yield()
static uint16_t ble_att_mtu(uint16_t connection) { assert(connection == 7); return mtu; }

struct os_mbuf { uint16_t size; uint8_t data[BLE_MIC_ENCODED_FRAME_MAX_BYTES]; };
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t size)
{
    if (allocation_failures) { --allocation_failures; return NULL; }
    assert(size <= sizeof(((struct os_mbuf *)0)->data));
    struct os_mbuf *packet = malloc(sizeof(*packet));
    assert(packet);
    packet->size = size;
    memcpy(packet->data, data, size);
    ++allocations;
    return packet;
}
static int ble_gatts_notify_custom(uint16_t connection, uint16_t handle, struct os_mbuf *packet)
{
    assert(connection == 7 && handle == 42);
    ++attempts;
    int result = 0;
    if (slow_progress && (TickType_t)(now - last_progress) < 19) {
        result = BLE_HS_ENOMEM;
    } else if (received_size >= fail_after && failures) {
        --failures;
        result = failure_code;
    } else {
        assert(resets > 0); // Every PCM generation starts after a queued boundary.
        assert(received_size + packet->size <= sizeof(received));
        memcpy(received + received_size, packet->data, packet->size);
        received_size += packet->size;
        last_progress = now;
        if (trace_notifications) {
            printf("{\"type\":\"microphone.data\",\"hex\":\"");
            for (size_t i = 0; i < packet->size; ++i) printf("%02x", packet->data[i]);
            puts("\"}");
        }
    }
    free(packet); // NimBLE owns mbuf even on errors.
    ++released;
    return result;
}
static int xQueueSend(void *queue, ble_audio_frame_t **frame, TickType_t ticks)
{
    assert(queue == s_microphone_queue && ticks == 0);
    if (queue_count == BLE_MIC_QUEUE_DEPTH) return 0;
    queued[queue_count++] = *frame;
    return pdTRUE;
}

#include "ble_microphone.inc"

static ble_audio_frame_t frame(void)
{
    ble_audio_frame_t result = {
        .size = BLE_MIC_ENCODED_FRAME_MAX_BYTES,
        .connection_handle = s_connection_handle,
        .generation = s_microphone_generation,
    };
    for (size_t i = 0; i < result.size; ++i) result.data[i] = (uint8_t)(i * 17 + 3);
    return result;
}
static void mute_resume(void)
{
    ++s_microphone_generation;
    s_input_suspended = true;
    s_input_suspended = false;
}
static void reconnect(void)
{
    set_link_state(BLE_HS_CONN_HANDLE_NONE, false, false);
    mark_connected(7); // The controller is allowed to reuse the same handle.
    s_microphone_notify_enabled = s_voice_ready = true;
}
static void not_ready(void) { s_voice_ready = false; }
static void unsubscribed(void) { s_microphone_notify_enabled = false; }
static void playing(void) { s_playback_suspended = true; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(pdMS_TO_TICKS(2) == 0); // Reproduce the target's previous zero-delay bug.
    const char *test = argv[1];
    ble_audio_frame_t input = frame();
    if (strcmp(test, "reset-recover") == 0) {
        reset_failures = 12;
        send_microphone_frame(&input);
        assert(now == 12 && waits == 12 && resets == 1 && reset_attempts == 13);
        assert(received_size == input.size && !s_microphone_needs_reset);
    } else if (strcmp(test, "reset-timeout") == 0) {
        reset_failures = 1000;
        send_microphone_frame(&input);
        assert(now == 20 && received_size == 0 && attempts == 0);
        assert(s_dropped_microphone_frames == 1 && s_microphone_needs_reset);
        reset_failures = 0;
        send_microphone_frame(&input);
        assert(resets == 1 && received_size == input.size && !s_microphone_needs_reset);
    } else if (strcmp(test, "reset-mute") == 0) {
        reset_failures = 1; wait_action = mute_resume;
        send_microphone_frame(&input);
        assert(now == 1 && received_size == 0 && attempts == 0 && resets == 0);
        ble_audio_frame_t fresh = frame();
        send_microphone_frame(&fresh);
        assert(resets == 1 && received_size == fresh.size);
    } else if (strncmp(test, "trace-", 6) == 0) {
        trace_notifications = true;
        memset(input.data, 0x11, input.size);
        if (strcmp(test, "trace-mute") == 0) yield_action = mute_resume;
        else if (strcmp(test, "trace-failure") == 0) {
            fail_after = mtu - 3; failures = 1; failure_code = BLE_HS_EINVAL;
        } else assert(!"unknown trace");
        send_microphone_frame(&input);
        assert(received_size == mtu - 3 && s_microphone_needs_reset);
        ble_audio_frame_t fresh = frame();
        memset(fresh.data, 0x22, fresh.size);
        send_microphone_frame(&fresh);
        assert(received_size == mtu - 3 + fresh.size && resets == 2);
        assert(!s_microphone_needs_reset);
    } else if (strcmp(test, "recover") == 0) {
        fail_after = mtu - 3; failures = 12;
        send_microphone_frame(&input);
        assert(waits == 12 && now == 12 && s_dropped_microphone_frames == 0);
        assert(received_size == input.size && memcmp(received, input.data, input.size) == 0);
        assert(yields == 7);
    } else if (strcmp(test, "allocation") == 0) {
        allocation_failures = 12;
        send_microphone_frame(&input);
        assert(waits == 12 && received_size == input.size);
        assert(memcmp(received, input.data, input.size) == 0);
    } else if (strcmp(test, "timeout") == 0 || strcmp(test, "wrap") == 0) {
        if (strcmp(test, "wrap") == 0) now = UINT32_MAX - 5;
        const TickType_t started = now;
        failures = 1000;
        send_microphone_frame(&input);
        assert((TickType_t)(now - started) == pdMS_TO_TICKS(BLE_MIC_NOTIFY_TIMEOUT_MS));
        assert(waits == 20 && received_size == 0 && s_dropped_microphone_frames == 1);
        assert(strstr(last_log, "rc=6") && strstr(last_log, "accepted=0/1600"));
    } else if (strcmp(test, "slow-progress") == 0 || strcmp(test, "slow-progress-mtu23") == 0) {
        if (strcmp(test, "slow-progress-mtu23") == 0) mtu = 23;
        slow_progress = true;
        send_microphone_frame(&input);
        assert(now == pdMS_TO_TICKS(BLE_MIC_FRAME_TIMEOUT_MS));
        assert(received_size == (size_t)(mtu - 3) * 2 && s_dropped_microphone_frames == 1);
        assert(memcmp(received, input.data, received_size) == 0);
    } else if (strcmp(test, "fatal") == 0) {
        fail_after = mtu - 3; failures = 1; failure_code = BLE_HS_EINVAL;
        send_microphone_frame(&input);
        assert(waits == 0 && received_size == mtu - 3 && s_dropped_microphone_frames == 1);
        assert(memcmp(received, input.data, received_size) == 0);
        assert(strstr(last_log, "rc=3") && strstr(last_log, "accepted=253/1600"));
    } else if (strcmp(test, "mute") == 0 || strcmp(test, "reconnect") == 0) {
        fail_after = mtu - 3; failures = 1;
        wait_action = strcmp(test, "mute") == 0 ? mute_resume : reconnect;
        send_microphone_frame(&input);
        assert(waits == 1 && received_size == mtu - 3 && s_dropped_microphone_frames == 0);
        send_microphone_frame(&input); // It cannot reappear once the link is ready again.
        assert(received_size == mtu - 3);
        ble_audio_frame_t fresh = frame();
        send_microphone_frame(&fresh);
        assert(received_size == mtu - 3 + fresh.size);
        assert(memcmp(received + mtu - 3, fresh.data, fresh.size) == 0);
    } else if (strcmp(test, "not-ready") == 0 || strcmp(test, "unsubscribed") == 0
               || strcmp(test, "playing") == 0) {
        yield_action = strcmp(test, "not-ready") == 0 ? not_ready
            : strcmp(test, "unsubscribed") == 0 ? unsubscribed : playing;
        send_microphone_frame(&input);
        assert(received_size == mtu - 3 && attempts == 1 && waits == 0);
    } else if (strcmp(test, "queue") == 0) {
        assert(BLE_MIC_QUEUE_DEPTH >= 12);
        int16_t pcm[BLE_MIC_PCM_FRAME_MAX_BYTES / 2];
        for (size_t block = 0; block < BLE_MIC_QUEUE_DEPTH; ++block) {
            for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i) pcm[i] = (int16_t)(block * 100 + i);
            ble_manager_send_pcm((const uint8_t *)pcm, sizeof(pcm));
        }
        assert(queue_count == BLE_MIC_QUEUE_DEPTH && s_dropped_microphone_frames == 0);
        ble_manager_send_pcm((const uint8_t *)pcm, sizeof(pcm));
        assert(queue_count == BLE_MIC_QUEUE_DEPTH && s_dropped_microphone_frames == 1);
        for (size_t block = 0; block < queue_count; ++block) {
            assert(queued[block]->generation == s_microphone_generation);
            send_microphone_frame(queued[block]);
            free(queued[block]);
        }
        assert(received_size == BLE_MIC_QUEUE_DEPTH * BLE_MIC_ENCODED_FRAME_MAX_BYTES);
        for (size_t block = 0; block < queue_count; ++block)
            for (size_t i = 0; i < BLE_MIC_ENCODED_FRAME_MAX_BYTES; ++i)
                assert(received[block * BLE_MIC_ENCODED_FRAME_MAX_BYTES + i]
                       == linear_pcm_to_mulaw((int16_t)(block * 100 + i)));
        assert(waits == 0);
    } else if (strcmp(test, "mtu") == 0) {
        const uint16_t sizes[] = {23, 185, 256};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
            mtu = sizes[i]; received_size = 0;
            send_microphone_frame(&input);
            assert(received_size == input.size && memcmp(received, input.data, input.size) == 0);
        }
        assert(waits == 0);
    } else {
        assert(!"unknown test");
    }
    assert(allocations == released);
    if (!trace_notifications) puts("BLE microphone test passed");
    return 0;
}

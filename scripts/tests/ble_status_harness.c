#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice_session_stubs.h"

#include "battery_monitor.h"
#include "board_config.h"
#include "ble_status_config.h"

#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1

// A pthread-backed fake enforces the FreeRTOS mutex's ownership contract. The
// concurrency case waits until a competing caller attempts to enter while the
// first message is between fragments, rather than depending on scheduler luck.
static pthread_mutex_t notification_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t schedule_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_changed = PTHREAD_COND_INITIALIZER;
typedef pthread_mutex_t *SemaphoreHandle_t;
static SemaphoreHandle_t s_notify_mutex = &notification_mutex;
static _Thread_local bool notification_locked;
static unsigned take_attempts, lock_count, unlock_count;
static bool refuse_lock, pause_first_delay, first_paused;
static bool s_notify_needs_separator;
static uint8_t s_phy_tx = 2, s_phy_rx = 1;

static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{
    assert(mutex == &notification_mutex && ticks == 20);
    assert(!notification_locked);
    assert(pthread_mutex_lock(&schedule_mutex) == 0);
    ++take_attempts;
    assert(pthread_cond_broadcast(&schedule_changed) == 0);
    assert(pthread_mutex_unlock(&schedule_mutex) == 0);
    if (refuse_lock) return 0;
    assert(pthread_mutex_lock(mutex) == 0);
    notification_locked = true;
    ++lock_count;
    return pdTRUE;
}

static void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex == &notification_mutex && notification_locked);
    ++unlock_count;
    notification_locked = false;
    assert(pthread_mutex_unlock(mutex) == 0);
}

static char s_device_name[16] = "JUFF-test";
static uint16_t s_status_handle = 17;
static battery_status_t battery;
static unsigned snapshot_reads, packet_count, delays, notify_attempts, allocation_attempts;
static unsigned fail_notify_at, fail_allocation_at;
static bool connected = true;
static uint16_t mtu = 23;
static size_t received_size;
static char received[4 * (BLE_MESSAGE_MAX_BYTES + 2)];
typedef struct { char version[32]; } esp_app_desc_t;
static esp_app_desc_t application = { .version = "test-version" };
static const esp_app_desc_t *esp_app_get_description(void) { return &application; }
battery_status_t battery_monitor_get(void) { ++snapshot_reads; return battery; }
static bool ble_manager_is_voice_active(void) { return false; }
static bool ble_manager_audio_is_connected(void) { return false; }
static bool device_client_is_connected(void) { return false; }
static bool device_client_is_voice_active(void) { return false; }
static bool wifi_manager_has_credentials(void) { return false; }
static bool wifi_manager_is_connected(void) { return false; }
static bool ble_manager_is_connected(void) { return false; }
static bool ble_manager_is_pairing(void) { return false; }
static bool audio_io_is_available(void) { return false; }
static bool audio_io_is_playing(void) { return false; }
static bool audio_io_supports_voice_barge_in(void) { return false; }
static void snapshot_link_state(uint16_t *connection, bool *notify, bool *advertising)
{
    assert(notification_locked);
    *connection = connected ? 1 : BLE_HS_CONN_HANDLE_NONE;
    *notify = true;
    (void)advertising;
}
static uint16_t ble_att_mtu(uint16_t connection) { assert(connection == 1); return mtu; }
struct os_mbuf { uint16_t length; char data[BLE_MESSAGE_MAX_BYTES + 2]; };
static struct os_mbuf packet;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t size)
{
    assert(notification_locked);
    if (++allocation_attempts == fail_allocation_at) return NULL;
    assert(size <= (mtu > 3 ? mtu - 3 : 20));
    assert(size <= sizeof(packet.data));
    memcpy(packet.data, data, size);
    packet.length = size;
    return &packet;
}
static int ble_gatts_notify_custom(uint16_t connection, uint16_t attribute,
                                    struct os_mbuf *message)
{
    assert(notification_locked);
    assert(connection == 1 && attribute == s_status_handle && message == &packet);
    if (++notify_attempts == fail_notify_at) return 12;
    assert(received_size + message->length < sizeof(received));
    memcpy(received + received_size, message->data, message->length);
    received_size += message->length;
    ++packet_count;
    return 0;
}
static void vTaskDelay(unsigned ticks)
{
    assert(notification_locked && ticks == 5);
    ++delays;
    if (!pause_first_delay) return;
    pause_first_delay = false;
    assert(pthread_mutex_lock(&schedule_mutex) == 0);
    first_paused = true;
    assert(pthread_cond_broadcast(&schedule_changed) == 0);
    while (take_attempts < 2) {
        assert(pthread_cond_wait(&schedule_changed, &schedule_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&schedule_mutex) == 0);
    // The other thread tried to enter while this fragmented message still
    // holds the mutex. It cannot write until the complete newline is sent.
    assert(lock_count == 1 && unlock_count == 0);
}

#include "ble_status.inc"

static void *send_message(void *text)
{
    assert(notify_text(text));
    return NULL;
}

static void concurrent_notifications(void)
{
    char first[] = "{\"type\":\"status\",\"payload\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    char second[] = "{\"type\":\"speaker.credit\",\"payload\":\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"}";
    pause_first_delay = true;
    pthread_t a, b;
    assert(pthread_create(&a, NULL, send_message, first) == 0);
    assert(pthread_mutex_lock(&schedule_mutex) == 0);
    while (!first_paused) {
        assert(pthread_cond_wait(&schedule_changed, &schedule_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&schedule_mutex) == 0);
    assert(pthread_create(&b, NULL, send_message, second) == 0);
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    char expected[sizeof(first) + sizeof(second) + 2];
    snprintf(expected, sizeof(expected), "%s\n%s\n", first, second);
    assert(received_size == strlen(expected) && strcmp(received, expected) == 0);
    assert(take_attempts == 2 && lock_count == 2 && unlock_count == 2);
    assert(!s_notify_needs_separator && !notification_locked);
    fputs(received, stdout);
}

static void recover_partial_notification(bool allocation_failure)
{
    const char *partial = "{\"type\":\"status\",\"payload\":\"an interrupted message must not swallow the next credit\"}";
    const char *next = "{\"type\":\"speaker.credit\",\"streamId\":17,\"received\":320,\"limit\":96000}";
    if (allocation_failure) fail_allocation_at = 2;
    else fail_notify_at = 2;
    assert(!notify_text(partial));
    assert(received_size == 20 && packet_count == 1);
    assert(s_notify_needs_separator && lock_count == 1 && unlock_count == 1);
    assert(!notification_locked);
    assert(notify_text(next));
    assert(received_size == 20 + 1 + strlen(next) + 1);
    assert(memcmp(received, partial, 20) == 0 && received[20] == '\n');
    assert(memcmp(received + 21, next, strlen(next)) == 0);
    assert(received[received_size - 1] == '\n');
    assert(!s_notify_needs_separator && lock_count == 2 && unlock_count == 2);
    fputs(received, stdout);
}

static void rejected_notifications(void)
{
    const char *message = "{\"type\":\"status\"}";
    char oversized[BLE_MESSAGE_MAX_BYTES + 2];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    assert(!notify_text(NULL) && !notify_text("") && !notify_text(oversized));
    assert(take_attempts == 0 && lock_count == 0);
    refuse_lock = true;
    assert(!notify_text(message));
    assert(take_attempts == 1 && lock_count == 0 && unlock_count == 0);
    refuse_lock = false;
    connected = false;
    assert(!notify_text(message));
    assert(lock_count == 1 && unlock_count == 1 && received_size == 0);
    assert(!s_notify_needs_separator);
    connected = true;
    assert(notify_text(message));
    assert(lock_count == 2 && unlock_count == 2 && !notification_locked);
    fputs(received, stdout);
}

static void verify_framing(const char *status)
{
    const uint16_t mtus[] = {23, 185, 517};
    const size_t length = strlen(status);
    for (unsigned index = 0; index < sizeof(mtus) / sizeof(mtus[0]); ++index) {
        mtu = mtus[index];
        received_size = packet_count = delays = 0;
        memset(received, 0, sizeof(received));
        assert(notify_text(status));
        assert(received_size == length + 1);
        assert(memcmp(received, status, length) == 0 && received[length] == '\n');
        assert(strchr(received, '\n') == received + length);
        const size_t chunk_size = mtu - 3;
        assert(packet_count == (length + 1 + chunk_size - 1) / chunk_size);
        assert(delays + 1 == packet_count);
    }
    received_size = packet_count = delays = 0;
    const unsigned previous_reads = snapshot_reads;
    notify_current_status();
    assert(snapshot_reads == previous_reads + 1);
    assert(received_size == length + 1 && memcmp(received, status, length) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "concurrent") == 0) {
        concurrent_notifications();
        return 0;
    }
    if (strcmp(argv[1], "recover-notify") == 0 || strcmp(argv[1], "recover-allocation") == 0) {
        recover_partial_notification(strcmp(argv[1], "recover-allocation") == 0);
        return 0;
    }
    if (strcmp(argv[1], "rejected") == 0) {
        rejected_notifications();
        return 0;
    }
    battery = (battery_status_t){
        .supported = CONFIG_JUFF_BOARD_WAVESHARE_LCD_154,
        .valid = true, .percent = 40, .millivolts = 3790,
    };
    if (strcmp(argv[1], "charging") == 0) {
        battery.charging = true;
        battery.percent = 66;
        battery.millivolts = 3930;
    } else if (strcmp(argv[1], "missing-percent") == 0) {
        battery.valid = false;
        battery.charging = true;
        battery.percent = -1;
        battery.millivolts = 0;
    } else if (strcmp(argv[1], "invalid") == 0) {
        battery.valid = false;
        battery.percent = -1;
        battery.millivolts = 0;
    } else if (strcmp(argv[1], "empty") == 0) {
        battery.percent = 0;
        battery.millivolts = 3300;
    } else if (strcmp(argv[1], "full") == 0) {
        battery.percent = 100;
        battery.millivolts = 4200;
    } else if (strcmp(argv[1], "maximum") == 0) {
        // Full-width source fields and integer limits exceed realistic battery
        // values, guarding the existing status buffer without new identifiers.
        memset(s_device_name, 'N', sizeof(s_device_name) - 1);
        memset(application.version, 'V', sizeof(application.version) - 1);
        battery.valid = false;
        battery.percent = INT_MAX;
        battery.millivolts = INT_MIN;
    } else assert(strcmp(argv[1], "valid") == 0);

    char status[BLE_MESSAGE_MAX_BYTES];
    const size_t length = build_status(status, sizeof(status));
    assert(snapshot_reads == 1 && length > 0 && length == strlen(status));
    assert(length + 1 <= sizeof(status));
    unsigned char boundary[BLE_MESSAGE_MAX_BYTES + 1];
    memset(boundary, 0xa5, sizeof(boundary));
    assert(build_status((char *)boundary, length) == 0);
    assert(boundary[length] == 0xa5); // Truncation must not write past caller capacity.
    assert(build_status((char *)boundary, length + 1) == length);
    assert(boundary[length + 1] == 0xa5);
    assert(strcmp(status, (char *)boundary) == 0);
    verify_framing(status);
    puts(status);
    return 0;
}

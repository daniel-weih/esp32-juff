#include "audio_io.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "juff_audio";

#if CONFIG_JUFF_AUDIO_ENABLED

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define GATEWAY_INPUT_SAMPLE_RATE 16000
#define CODEC_SAMPLE_RATE 24000
#define CODEC_FRAME_SAMPLES 2400
#define GATEWAY_FRAME_SAMPLES 1600
#define RESPONSE_ID_SIZE 64
#define PLAYBACK_QUEUE_DEPTH 80
#define DIAGNOSTIC_WARMUP_MS 500
#define DIAGNOSTIC_AMBIENT_MS 400
#define DIAGNOSTIC_TONE_CHUNKS 4
#define DIAGNOSTIC_TONE_AMPLITUDE 3500
#define DIAGNOSTIC_TONE_FIRST_HZ 660
#define DIAGNOSTIC_TONE_SECOND_HZ 880

typedef enum {
    AUDIO_ITEM_BEGIN,
    AUDIO_ITEM_PCM,
    AUDIO_ITEM_END,
    AUDIO_ITEM_CLEAR,
} audio_item_type_t;

typedef struct {
    audio_item_type_t type;
    char response_id[RESPONSE_ID_SIZE];
    uint32_t sample_rate;
    uint8_t *data;
    size_t size;
} audio_item_t;

typedef struct {
    uint64_t sum_squares;
    uint32_t sample_count;
    uint32_t peak;
} microphone_stats_t;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_io_expander;
static SemaphoreHandle_t s_io_expander_mutex;
static i2s_chan_handle_t s_rx_channel;
static i2s_chan_handle_t s_tx_channel;
static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_playback_queue;
static audio_pcm_callback_t s_pcm_callback;
static audio_playback_callback_t s_playback_callback;
static void *s_callback_context;
static volatile bool s_capture_enabled;
static volatile bool s_playing;
static bool s_available;
static uint8_t s_io_expander_output;
static uint8_t s_io_expander_configuration;
static bool s_amplifier_enabled;
static portMUX_TYPE s_diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_diagnostic_collecting;
static microphone_stats_t s_diagnostic_stats;

#define TCA9554_INPUT_REGISTER 0x00
#define TCA9554_OUTPUT_REGISTER 0x01
#define TCA9554_POLARITY_REGISTER 0x02
#define TCA9554_CONFIG_REGISTER 0x03

static void copy_response_id(char destination[RESPONSE_ID_SIZE], const char *source)
{
    strlcpy(destination, source == NULL ? "" : source, RESPONSE_ID_SIZE);
}

static void free_item(audio_item_t *item)
{
    free(item->data);
    item->data = NULL;
}

static esp_err_t enqueue_item(audio_item_t *item)
{
    if (s_playback_queue == NULL) {
        free_item(item);
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t wait = item->type == AUDIO_ITEM_PCM
        ? pdMS_TO_TICKS(20)
        : pdMS_TO_TICKS(200);
    if (xQueueSend(s_playback_queue, item, wait) != pdTRUE) {
        ESP_LOGW(TAG, "Playback queue full; dropping item type %d (%u bytes)",
                 item->type,
                 (unsigned)item->size);
        free_item(item);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t codec_status(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed (codec status=%d)", operation, result);
    return ESP_FAIL;
}

static esp_err_t expander_read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_io_expander,
                                       &reg,
                                       sizeof(reg),
                                       value,
                                       sizeof(*value),
                                       100);
}

static esp_err_t expander_write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = { reg, value };
    return i2c_master_transmit(s_io_expander,
                               command,
                               sizeof(command),
                               100);
}

static esp_err_t set_amplifier_enabled(bool enabled)
{
    const esp_err_t error = audio_io_set_expander_pin(
        CONFIG_JUFF_CODEC_PA_EXPANDER_PIN,
        enabled);
    if (error == ESP_OK) {
        s_amplifier_enabled = enabled;
    }
    return error;
}

esp_err_t audio_io_set_expander_pin(unsigned pin, bool high)
{
    if (pin > 7 || s_io_expander == NULL || s_io_expander_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_expander_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t bit = (uint8_t)(1U << pin);
    const uint8_t next_output = high
        ? (uint8_t)(s_io_expander_output | bit)
        : (uint8_t)(s_io_expander_output & (uint8_t)~bit);
    esp_err_t error = ESP_OK;
    if (next_output != s_io_expander_output) {
        error = expander_write_register(TCA9554_OUTPUT_REGISTER, next_output);
        if (error == ESP_OK) {
            s_io_expander_output = next_output;
        }
    }
    if (error == ESP_OK && (s_io_expander_configuration & bit) != 0) {
        const uint8_t next_configuration =
            (uint8_t)(s_io_expander_configuration & (uint8_t)~bit);
        error = expander_write_register(TCA9554_CONFIG_REGISTER,
                                        next_configuration);
        if (error == ESP_OK) {
            s_io_expander_configuration = next_configuration;
        }
    }
    xSemaphoreGive(s_io_expander_mutex);
    return error;
}

static esp_err_t initialize_amplifier_control(void)
{
    s_io_expander_mutex = xSemaphoreCreateMutex();
    if (s_io_expander_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_JUFF_CODEC_PA_EXPANDER_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t error = i2c_master_bus_add_device(s_i2c_bus,
                                                 &device_config,
                                                 &s_io_expander);
    if (error != ESP_OK) {
        return error;
    }

    error = expander_read_register(TCA9554_OUTPUT_REGISTER,
                                   &s_io_expander_output);
    if (error != ESP_OK) {
        return error;
    }
    error = expander_read_register(TCA9554_CONFIG_REGISTER,
                                   &s_io_expander_configuration);
    if (error != ESP_OK) {
        return error;
    }

    error = set_amplifier_enabled(false);
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGI(TAG,
             "TCA9554 amplifier control ready: address=0x%02x EXIO%d (muted)",
             CONFIG_JUFF_CODEC_PA_EXPANDER_ADDRESS,
             CONFIG_JUFF_CODEC_PA_EXPANDER_PIN);
    return ESP_OK;
}

static esp_err_t initialize_i2s(const audio_codec_data_if_t **data_interface)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        CONFIG_JUFF_CODEC_I2S_PORT,
        I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    esp_err_t error = i2s_new_channel(&channel_config, &s_tx_channel, &s_rx_channel);
    if (error != ESP_OK) {
        return error;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = CONFIG_JUFF_CODEC_I2S_MCLK_GPIO,
            .bclk = CONFIG_JUFF_CODEC_I2S_BCLK_GPIO,
            .ws = CONFIG_JUFF_CODEC_I2S_WS_GPIO,
            .dout = CONFIG_JUFF_CODEC_I2S_DOUT_GPIO,
            .din = CONFIG_JUFF_CODEC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    error = i2s_channel_init_std_mode(s_tx_channel, &standard_config);
    if (error != ESP_OK) {
        return error;
    }
    error = i2s_channel_init_std_mode(s_rx_channel, &standard_config);
    if (error != ESP_OK) {
        return error;
    }

    // esp_codec_dev reconfigures both channels by disabling them first. Start
    // them once here so that initial disable is valid and does not emit a
    // misleading driver error during an otherwise successful codec open.
    error = i2s_channel_enable(s_tx_channel);
    if (error != ESP_OK) {
        return error;
    }
    error = i2s_channel_enable(s_rx_channel);
    if (error != ESP_OK) {
        (void)i2s_channel_disable(s_tx_channel);
        return error;
    }

    audio_codec_i2s_cfg_t codec_i2s_config = {
        .port = CONFIG_JUFF_CODEC_I2S_PORT,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
    };
    *data_interface = audio_codec_new_i2s_data(&codec_i2s_config);
    return *data_interface == NULL ? ESP_FAIL : ESP_OK;
}

static void recover_i2c_bus(void)
{
    const gpio_num_t scl = CONFIG_JUFF_CODEC_I2C_SCL_GPIO;
    const gpio_num_t sda = CONFIG_JUFF_CODEC_I2C_SDA_GPIO;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << scl) | (1ULL << sda),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(scl, 1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(sda, 1));
    esp_rom_delay_us(10);

    ESP_LOGI(TAG,
             "I2C levels before recovery: SCL=%d SDA=%d",
             gpio_get_level(scl),
             gpio_get_level(sda));
    for (unsigned pulse = 0; pulse < 18 && gpio_get_level(sda) == 0; ++pulse) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(scl, 0));
        esp_rom_delay_us(10);
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(scl, 1));
        esp_rom_delay_us(10);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(sda, 0));
    esp_rom_delay_us(10);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(scl, 1));
    esp_rom_delay_us(10);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(sda, 1));
    esp_rom_delay_us(10);
    ESP_LOGI(TAG,
             "I2C levels after recovery: SCL=%d SDA=%d",
             gpio_get_level(scl),
             gpio_get_level(sda));
}

static esp_err_t initialize_codec(const audio_codec_data_if_t *data_interface)
{
    recover_i2c_bus();
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = CONFIG_JUFF_CODEC_I2C_PORT,
        .sda_io_num = CONFIG_JUFF_CODEC_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_JUFF_CODEC_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (error != ESP_OK) {
        return error;
    }

    unsigned detected_devices = 0;
    unsigned timed_out_addresses = 0;
    unsigned nack_addresses = 0;
    unsigned other_errors = 0;
    const esp_log_level_t i2c_log_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        const esp_err_t probe_result = i2c_master_probe(s_i2c_bus, address, 20);
        if (probe_result == ESP_OK) {
            ESP_LOGI(TAG, "I2C device detected at 0x%02x", address);
            ++detected_devices;
        } else if (probe_result == ESP_ERR_TIMEOUT) {
            ++timed_out_addresses;
        } else if (probe_result == ESP_ERR_NOT_FOUND) {
            ++nack_addresses;
        } else {
            ++other_errors;
        }
    }
    esp_log_level_set("i2c.master", i2c_log_level);
    ESP_LOGI(TAG,
             "Board I2C scan complete: found=%u, NACK=%u, timeout=%u, other=%u",
             detected_devices,
             nack_addresses,
             timed_out_addresses,
             other_errors);

    error = initialize_amplifier_control();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 amplifier control failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    const audio_codec_gpio_if_t *gpio_interface = audio_codec_new_gpio();
    if (gpio_interface == NULL) {
        return ESP_FAIL;
    }
    audio_codec_i2c_cfg_t i2c_config = {
        .port = CONFIG_JUFF_CODEC_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *control_interface = audio_codec_new_i2c_ctrl(&i2c_config);
    if (control_interface == NULL) {
        return ESP_FAIL;
    }

    const esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_interface,
        .gpio_if = gpio_interface,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = CONFIG_JUFF_CODEC_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
        .no_dac_ref = true,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_interface = es8311_codec_new(&codec_config);
    if (codec_interface == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_interface,
        .data_if = data_interface,
    };
    s_codec = esp_codec_dev_new(&device_config);
    if (s_codec == NULL) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = CODEC_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    error = codec_status(esp_codec_dev_open(s_codec, &sample_info), "open ES8311");
    if (error != ESP_OK) {
        return error;
    }
    error = codec_status(esp_codec_dev_set_out_vol(s_codec, CONFIG_JUFF_CODEC_VOLUME),
                         "set speaker volume");
    if (error != ESP_OK) {
        return error;
    }
    error = codec_status(esp_codec_dev_set_in_gain(s_codec,
                                                   (float)CONFIG_JUFF_CODEC_MIC_GAIN_DB),
                         "set microphone gain");
    if (error != ESP_OK) {
        return error;
    }
    (void)esp_codec_dev_set_out_mute(s_codec, true);
    return ESP_OK;
}

static void resample_24k_to_16k(const int16_t *input, int16_t *output)
{
    for (size_t output_index = 0;
         output_index < GATEWAY_FRAME_SAMPLES;
         ++output_index) {
        const size_t doubled_position = output_index * 3;
        const size_t input_index = doubled_position / 2;
        if ((doubled_position & 1U) == 0) {
            output[output_index] = input[input_index];
        } else {
            output[output_index] = (int16_t)(((int32_t)input[input_index]
                                              + input[input_index + 1]) / 2);
        }
    }
}

static void diagnostic_stats_start(void)
{
    portENTER_CRITICAL(&s_diagnostic_lock);
    memset(&s_diagnostic_stats, 0, sizeof(s_diagnostic_stats));
    s_diagnostic_collecting = true;
    portEXIT_CRITICAL(&s_diagnostic_lock);
}

static microphone_stats_t diagnostic_stats_finish(void)
{
    microphone_stats_t result;
    portENTER_CRITICAL(&s_diagnostic_lock);
    s_diagnostic_collecting = false;
    result = s_diagnostic_stats;
    portEXIT_CRITICAL(&s_diagnostic_lock);
    return result;
}

static void collect_diagnostic_stats(const int16_t *samples, size_t count)
{
    if (!s_diagnostic_collecting) {
        return;
    }

    microphone_stats_t frame = { 0 };
    for (size_t index = 0; index < count; ++index) {
        const int32_t sample = samples[index];
        const uint32_t magnitude = sample < 0
            ? (uint32_t)(-sample)
            : (uint32_t)sample;
        frame.sum_squares += (uint64_t)magnitude * magnitude;
        if (magnitude > frame.peak) {
            frame.peak = magnitude;
        }
    }
    frame.sample_count = count;

    portENTER_CRITICAL(&s_diagnostic_lock);
    if (s_diagnostic_collecting) {
        s_diagnostic_stats.sum_squares += frame.sum_squares;
        s_diagnostic_stats.sample_count += frame.sample_count;
        if (frame.peak > s_diagnostic_stats.peak) {
            s_diagnostic_stats.peak = frame.peak;
        }
    }
    portEXIT_CRITICAL(&s_diagnostic_lock);
}

static uint32_t stats_rms(const microphone_stats_t *stats)
{
    if (stats->sample_count == 0) {
        return 0;
    }
    return (uint32_t)sqrt((double)stats->sum_squares / stats->sample_count);
}

static void log_diagnostic_stats(const char *label,
                                 const microphone_stats_t *stats)
{
    const uint32_t rms = stats_rms(stats);
    const uint32_t rms_tenths_percent = (rms * 1000U + 16384U) / 32768U;
    const uint32_t peak_tenths_percent = (stats->peak * 1000U + 16384U) / 32768U;
    ESP_LOGI(TAG,
             "Microphone %s: samples=%" PRIu32 ", RMS=%" PRIu32
             " (%" PRIu32 ".%" PRIu32 "%% FS), peak=%" PRIu32
             " (%" PRIu32 ".%" PRIu32 "%% FS)",
             label,
             stats->sample_count,
             rms,
             rms_tenths_percent / 10,
             rms_tenths_percent % 10,
             stats->peak,
             peak_tenths_percent / 10,
             peak_tenths_percent % 10);
}

static void capture_task(void *argument)
{
    (void)argument;
    int16_t *codec_pcm = heap_caps_malloc(CODEC_FRAME_SAMPLES * sizeof(*codec_pcm),
                                          MALLOC_CAP_8BIT);
    int16_t *gateway_pcm = heap_caps_malloc(GATEWAY_FRAME_SAMPLES * sizeof(*gateway_pcm),
                                            MALLOC_CAP_8BIT);
    if (codec_pcm == NULL || gateway_pcm == NULL) {
        ESP_LOGE(TAG, "Unable to allocate microphone buffers");
        free(codec_pcm);
        free(gateway_pcm);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        const int result = esp_codec_dev_read(s_codec,
                                              codec_pcm,
                                              CODEC_FRAME_SAMPLES * sizeof(*codec_pcm));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Microphone read failed (codec status=%d)", result);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        collect_diagnostic_stats(codec_pcm, CODEC_FRAME_SAMPLES);
        if (!s_capture_enabled || s_playing || s_pcm_callback == NULL) {
            continue;
        }

        resample_24k_to_16k(codec_pcm, gateway_pcm);
        s_pcm_callback((const uint8_t *)gateway_pcm,
                       GATEWAY_FRAME_SAMPLES * sizeof(*gateway_pcm),
                       s_callback_context);
    }
}

static void notify_playback(const char *event_type, const char *response_id)
{
    if (s_playback_callback != NULL && response_id != NULL && response_id[0] != '\0') {
        s_playback_callback(event_type, response_id, s_callback_context);
    }
}

static void playback_task(void *argument)
{
    (void)argument;
    char current_response[RESPONSE_ID_SIZE] = { 0 };
    bool started = false;

    while (true) {
        audio_item_t item = { 0 };
        if (xQueueReceive(s_playback_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (item.type == AUDIO_ITEM_BEGIN) {
            if (started || current_response[0] != '\0') {
                notify_playback("playback.cancelled", current_response);
            }
            copy_response_id(current_response, item.response_id);
            started = false;
            s_playing = true;
            ESP_ERROR_CHECK_WITHOUT_ABORT(set_amplifier_enabled(true));
            vTaskDelay(pdMS_TO_TICKS(5));
            (void)esp_codec_dev_set_out_mute(s_codec, false);
            if (item.sample_rate != CODEC_SAMPLE_RATE) {
                ESP_LOGW(TAG,
                         "Gateway output is %" PRIu32 " Hz; ES8311 remains at %d Hz",
                         item.sample_rate,
                         CODEC_SAMPLE_RATE);
            }
        } else if (item.type == AUDIO_ITEM_PCM) {
            if (current_response[0] == '\0') {
                free_item(&item);
                continue;
            }
            if (!started) {
                started = true;
                notify_playback("playback.started", current_response);
            }
            const int result = esp_codec_dev_write(s_codec, item.data, (int)item.size);
            if (result != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "Speaker write failed (codec status=%d)", result);
            }
        } else if (item.type == AUDIO_ITEM_END) {
            if (started) {
                notify_playback("playback.ended", current_response);
            }
            (void)esp_codec_dev_set_out_mute(s_codec, true);
            vTaskDelay(pdMS_TO_TICKS(2));
            ESP_ERROR_CHECK_WITHOUT_ABORT(set_amplifier_enabled(false));
            current_response[0] = '\0';
            started = false;
            s_playing = false;
        } else if (item.type == AUDIO_ITEM_CLEAR) {
            if (started || current_response[0] != '\0') {
                notify_playback("playback.cancelled", current_response);
            }
            (void)esp_codec_dev_set_out_mute(s_codec, true);
            vTaskDelay(pdMS_TO_TICKS(2));
            ESP_ERROR_CHECK_WITHOUT_ABORT(set_amplifier_enabled(false));
            current_response[0] = '\0';
            started = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(s_tx_channel));
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_enable(s_tx_channel));
            s_playing = false;
        }
        free_item(&item);
    }
}

esp_err_t audio_io_init(audio_pcm_callback_t pcm_callback,
                        audio_playback_callback_t playback_callback,
                        void *context)
{
    s_pcm_callback = pcm_callback;
    s_playback_callback = playback_callback;
    s_callback_context = context;
    s_playback_queue = xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(audio_item_t));
    if (s_playback_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const audio_codec_data_if_t *data_interface = NULL;
    esp_err_t error = initialize_i2s(&data_interface);
    if (error == ESP_OK) {
        error = initialize_codec(data_interface);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initialization failed: %s", esp_err_to_name(error));
        return error;
    }

    if (xTaskCreatePinnedToCore(capture_task,
                                "juff_capture",
                                4096,
                                NULL,
                                6,
                                NULL,
                                0) != pdPASS
        || xTaskCreatePinnedToCore(playback_task,
                                  "juff_playback",
                                  4096,
                                  NULL,
                                  7,
                                  NULL,
                                  1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_available = true;
    ESP_LOGI(TAG,
             "Waveshare ESP32-S3-Touch-LCD-3.5 ES8311 ready: codec=%d Hz, gateway microphone=%d Hz, volume=%d, gain=%d dB",
             CODEC_SAMPLE_RATE,
             GATEWAY_INPUT_SAMPLE_RATE,
             CONFIG_JUFF_CODEC_VOLUME,
             CONFIG_JUFF_CODEC_MIC_GAIN_DB);
    return ESP_OK;
}

esp_err_t audio_io_run_self_test(void)
{
    if (!s_available || s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playing) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t *tone = heap_caps_malloc(CODEC_FRAME_SAMPLES * sizeof(*tone),
                                     MALLOC_CAP_8BIT);
    if (tone == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "USB audio self-test: measuring ambient microphone, then playing two short tones");
    // The first DMA frames after ES8311 startup can contain a full-scale
    // transient. Discard them so the ambient reference reflects the mic.
    vTaskDelay(pdMS_TO_TICKS(DIAGNOSTIC_WARMUP_MS));
    diagnostic_stats_start();
    vTaskDelay(pdMS_TO_TICKS(DIAGNOSTIC_AMBIENT_MS));
    const microphone_stats_t ambient = diagnostic_stats_finish();
    log_diagnostic_stats("ambient", &ambient);

    diagnostic_stats_start();
    s_playing = true;
    esp_err_t error = set_amplifier_enabled(true);
    if (error == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        error = codec_status(esp_codec_dev_set_out_mute(s_codec, false),
                             "unmute speaker for self-test");
    }

    float phase = 0.0f;
    const float two_pi = 6.2831853071795864769f;
    for (unsigned chunk = 0;
         error == ESP_OK && chunk < DIAGNOSTIC_TONE_CHUNKS;
         ++chunk) {
        const unsigned frequency = chunk < (DIAGNOSTIC_TONE_CHUNKS / 2)
            ? DIAGNOSTIC_TONE_FIRST_HZ
            : DIAGNOSTIC_TONE_SECOND_HZ;
        const float phase_step = two_pi * frequency / CODEC_SAMPLE_RATE;
        if (chunk == DIAGNOSTIC_TONE_CHUNKS / 2) {
            phase = 0.0f;
        }
        for (size_t index = 0; index < CODEC_FRAME_SAMPLES; ++index) {
            tone[index] = (int16_t)(sinf(phase) * DIAGNOSTIC_TONE_AMPLITUDE);
            phase += phase_step;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }
        error = codec_status(esp_codec_dev_write(s_codec,
                                                  tone,
                                                  CODEC_FRAME_SAMPLES
                                                      * sizeof(*tone)),
                             "write speaker self-test tone");
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    (void)esp_codec_dev_set_out_mute(s_codec, true);
    vTaskDelay(pdMS_TO_TICKS(2));
    const esp_err_t amplifier_error = set_amplifier_enabled(false);
    if (error == ESP_OK) {
        error = amplifier_error;
    }
    s_playing = false;
    vTaskDelay(pdMS_TO_TICKS(100));

    const microphone_stats_t during_tone = diagnostic_stats_finish();
    free(tone);
    log_diagnostic_stats("during acoustic tone", &during_tone);

    if (ambient.sample_count == 0 || during_tone.sample_count == 0) {
        ESP_LOGE(TAG, "Microphone self-test captured no samples");
        return error == ESP_OK ? ESP_FAIL : error;
    }
    if (stats_rms(&during_tone) > stats_rms(&ambient) + 50U
        || during_tone.peak > ambient.peak + 100U) {
        ESP_LOGI(TAG,
                 "Audio self-test passed: microphone detected the speaker tones");
    } else {
        ESP_LOGW(TAG,
                 "Tone playback completed, but acoustic pickup was weak; confirm the tones were audible");
    }
    return error;
}

esp_err_t audio_io_begin_response(const char *response_id, uint32_t sample_rate)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_item_t item = {
        .type = AUDIO_ITEM_BEGIN,
        .sample_rate = sample_rate,
    };
    copy_response_id(item.response_id, response_id);
    const esp_err_t error = enqueue_item(&item);
    if (error == ESP_OK) {
        s_playing = true;
    }
    return error;
}

esp_err_t audio_io_push_pcm(const uint8_t *data, size_t size)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || size == 0 || (size % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_item_t item = {
        .type = AUDIO_ITEM_PCM,
        .data = heap_caps_malloc(size, MALLOC_CAP_8BIT),
        .size = size,
    };
    if (item.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(item.data, data, size);
    return enqueue_item(&item);
}

esp_err_t audio_io_end_response(const char *response_id)
{
    audio_item_t item = { .type = AUDIO_ITEM_END };
    copy_response_id(item.response_id, response_id);
    return enqueue_item(&item);
}

void audio_io_clear(const char *reason)
{
    ESP_LOGI(TAG, "Clearing playback (%s)", reason == NULL ? "unspecified" : reason);
    if (s_playback_queue == NULL) {
        return;
    }
    audio_item_t pending = { 0 };
    while (xQueueReceive(s_playback_queue, &pending, 0) == pdTRUE) {
        free_item(&pending);
    }
    audio_item_t clear = { .type = AUDIO_ITEM_CLEAR };
    (void)xQueueSendToFront(s_playback_queue, &clear, pdMS_TO_TICKS(200));
}

void audio_io_set_capture_enabled(bool enabled)
{
    s_capture_enabled = enabled;
}

bool audio_io_is_playing(void)
{
    return s_playing;
}

bool audio_io_is_available(void)
{
    return s_available;
}

i2c_master_bus_handle_t audio_io_i2c_bus(void)
{
    return s_i2c_bus;
}

#else

esp_err_t audio_io_init(audio_pcm_callback_t pcm_callback,
                        audio_playback_callback_t playback_callback,
                        void *context)
{
    (void)pcm_callback;
    (void)playback_callback;
    (void)context;
    ESP_LOGW(TAG, "ES8311 audio is disabled; USB/network diagnostics only");
    return ESP_OK;
}

esp_err_t audio_io_run_self_test(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_io_begin_response(const char *response_id, uint32_t sample_rate)
{
    (void)response_id;
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_io_push_pcm(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_io_end_response(const char *response_id)
{
    (void)response_id;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_io_clear(const char *reason)
{
    (void)reason;
}

void audio_io_set_capture_enabled(bool enabled)
{
    (void)enabled;
}

bool audio_io_is_playing(void)
{
    return false;
}

bool audio_io_is_available(void)
{
    return false;
}

i2c_master_bus_handle_t audio_io_i2c_bus(void)
{
    return NULL;
}

esp_err_t audio_io_set_expander_pin(unsigned pin, bool high)
{
    (void)pin;
    (void)high;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

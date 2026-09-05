#include "audio_io.h"
#include "board_config.h"

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
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "voice_frontend.h"
#include "audio_pcm_format.h"

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
#define CODEC_WRITE_CHUNK_FRAMES 240U
#define CODEC_WRITE_WAIT_MS 20U
#if CONFIG_JUFF_VOICE_BARGE_IN && CONFIG_JUFF_BOARD_WAVESHARE_LCD_35
#define CODEC_TX_CHANNELS 2
#else
#define CODEC_TX_CHANNELS 1
#endif

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
static esp_codec_dev_handle_t s_input_codec;
static SemaphoreHandle_t s_codec_write_mutex;
static portMUX_TYPE s_codec_write_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_codec_write_generation;
static bool s_self_test_running;
static QueueHandle_t s_playback_queue;
static audio_pcm_callback_t s_pcm_callback;
static audio_playback_callback_t s_playback_callback;
static void *s_callback_context;
static volatile bool s_capture_enabled;
static volatile bool s_playing;
static bool s_available;
static uint8_t s_io_expander_output;
static uint8_t s_io_expander_configuration;
static volatile bool s_amplifier_enabled;
static portMUX_TYPE s_diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_diagnostic_collecting;
static microphone_stats_t s_diagnostic_stats;
static bool s_voice_frontend_ready;
static audio_barge_in_callback_t s_barge_in_allowed_callback;
#if CONFIG_JUFF_VOICE_BARGE_IN
static portMUX_TYPE s_frontend_playback_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_frontend_playback_generation;
static bool s_frontend_pcm_started;
static DRAM_ATTR portMUX_TYPE s_rx_overflow_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t s_rx_overflow_count;

static bool IRAM_ATTR frontend_rx_overflow(i2s_chan_handle_t channel,
                                           i2s_event_data_t *event,
                                           void *context)
{
    (void)channel;
    (void)event;
    (void)context;
    portENTER_CRITICAL_ISR(&s_rx_overflow_lock);
    ++s_rx_overflow_count;
    portEXIT_CRITICAL_ISR(&s_rx_overflow_lock);
    return false;
}

static bool recover_frontend_rx_overflow(uint32_t *last_seen)
{
    portENTER_CRITICAL(&s_rx_overflow_lock);
    const uint32_t count = s_rx_overflow_count;
    portEXIT_CRITICAL(&s_rx_overflow_lock);
    if (count == *last_seen) return false;
    const uint32_t dropped = count - *last_seen;
    *last_seen = count;
    if (s_voice_frontend_ready) voice_frontend_reset_stream();
    ESP_LOGW(TAG, "I2S RX overflow: dropped=%" PRIu32 " total=%" PRIu32
             "; discarded capture block and reset voice frontend", dropped, count);
    return true;
}

static void begin_frontend_playback(void)
{
    portENTER_CRITICAL(&s_frontend_playback_lock);
    ++s_frontend_playback_generation;
    s_frontend_pcm_started = false;
    portEXIT_CRITICAL(&s_frontend_playback_lock);
}

static void mark_frontend_pcm_started(void)
{
    portENTER_CRITICAL(&s_frontend_playback_lock);
    s_frontend_pcm_started = true;
    portEXIT_CRITICAL(&s_frontend_playback_lock);
}

static voice_frontend_playback_t snapshot_frontend_playback(void)
{
    portENTER_CRITICAL(&s_frontend_playback_lock);
    const voice_frontend_playback_t playback = {
        .generation = s_frontend_playback_generation,
        // A waiting response blocks upload even before PA is enabled. Once
        // PCM has started, an immediate PA mute releases capture after Stop.
        .playing = s_playing && (!s_frontend_pcm_started || s_amplifier_enabled),
        .pcm_started = s_frontend_pcm_started,
    };
    portEXIT_CRITICAL(&s_frontend_playback_lock);
    return playback;
}

static voice_frontend_playback_t snapshot_frontend_playback_after_read(
    voice_frontend_playback_t before_read)
{
    voice_frontend_playback_t playback = snapshot_frontend_playback();
    // A block captured partly before the first PCM is not speaker warmup.
    playback.pcm_started = playback.pcm_started && before_read.pcm_started
        && playback.generation == before_read.generation;
    return playback;
}
#endif

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

static uint32_t codec_write_generation(void)
{
    portENTER_CRITICAL(&s_codec_write_lock);
    const uint32_t generation = s_codec_write_generation;
    portEXIT_CRITICAL(&s_codec_write_lock);
    return generation;
}

static void cancel_codec_writes(void)
{
    portENTER_CRITICAL(&s_codec_write_lock);
    ++s_codec_write_generation;
    portEXIT_CRITICAL(&s_codec_write_lock);
}

#if CONFIG_JUFF_CODEC_PA_GPIO < 0
static esp_err_t expander_read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_io_expander,
                                       &reg,
                                       sizeof(reg),
                                       value,
                                       sizeof(*value),
                                       100);
}
#endif

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
#if CONFIG_JUFF_CODEC_PA_GPIO >= 0
    const esp_err_t error = gpio_set_level(CONFIG_JUFF_CODEC_PA_GPIO, enabled);
#else
    const esp_err_t error = audio_io_set_expander_pin(
        CONFIG_JUFF_CODEC_PA_EXPANDER_PIN,
        enabled);
#endif
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
#if CONFIG_JUFF_CODEC_PA_GPIO >= 0
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_JUFF_CODEC_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t direct_error = gpio_config(&config);
    if (direct_error == ESP_OK) {
        direct_error = set_amplifier_enabled(false);
    }
    return direct_error;
#else
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
#endif
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
            CODEC_TX_CHANNELS == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
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
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    // ES7210 emits four 16-bit TDM slots. The ADC and DAC share BCLK/WS;
    // esp_codec_dev keeps the ES8311's two 32-bit slots on the same clocks.
    const i2s_tdm_config_t receive_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = CONFIG_JUFF_CODEC_I2S_MCLK_GPIO,
            .bclk = CONFIG_JUFF_CODEC_I2S_BCLK_GPIO,
            .ws = CONFIG_JUFF_CODEC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_JUFF_CODEC_I2S_DIN_GPIO,
        },
    };
    error = i2s_channel_init_tdm_mode(s_rx_channel, &receive_config);
#else
    error = i2s_channel_init_std_mode(s_rx_channel, &standard_config);
#endif
    if (error != ESP_OK) {
        return error;
    }

#if CONFIG_JUFF_VOICE_BARGE_IN
    // Register while RX is READY; callbacks must remain ISR-safe even when
    // flash/cache is unavailable. Recovery and logging belong to capture_task.
    const i2s_event_callbacks_t receive_callbacks = {
        .on_recv_q_ovf = frontend_rx_overflow,
    };
    error = i2s_channel_register_event_callback(s_rx_channel, &receive_callbacks, NULL);
    if (error != ESP_OK) return error;
#endif

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
        ESP_LOGE(TAG, "Amplifier control failed: %s",
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
        .codec_mode = BOARD_HAS_ES7210
            ? ESP_CODEC_DEV_WORK_MODE_DAC : ESP_CODEC_DEV_WORK_MODE_BOTH,
        // JUFF owns amplifier muting, including during interruptions.
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
        .no_dac_ref = CODEC_TX_CHANNELS != 2,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_interface = es8311_codec_new(&codec_config);
    if (codec_interface == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = BOARD_HAS_ES7210
            ? ESP_CODEC_DEV_TYPE_OUT : ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_interface,
        .data_if = data_interface,
    };
    s_codec = esp_codec_dev_new(&device_config);
    if (s_codec == NULL) {
        return ESP_FAIL;
    }

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    i2c_config.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *input_control = audio_codec_new_i2c_ctrl(&i2c_config);
    if (input_control == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es7210_codec_cfg_t input_config = {
        .ctrl_if = input_control,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2
            | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_div = 256,
    };
    const audio_codec_if_t *input_interface = es7210_codec_new(&input_config);
    if (input_interface == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t input_device = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = input_interface,
        .data_if = data_interface,
    };
    s_input_codec = esp_codec_dev_new(&input_device);
    if (s_input_codec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // TDM slot 0 = MIC1, slot 1 = MIC3 (speaker reference). The reference is
    // consumed locally by AEC and must never be uploaded as microphone audio.
    esp_codec_dev_sample_info_t input_info = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)
#if CONFIG_JUFF_VOICE_BARGE_IN
            | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)
#endif
            ,
        .sample_rate = CODEC_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    error = codec_status(esp_codec_dev_open(s_input_codec, &input_info), "open ES7210");
    if (error != ESP_OK) {
        return error;
    }
#else
    s_input_codec = s_codec;
#endif

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        // The 3.5-inch IN_OUT handle configures both directions together:
        // left RX is ADC microphone, right RX is ES8311's DAC reference.
        .channel = CODEC_TX_CHANNELS,
        .channel_mask = CODEC_TX_CHANNELS == 2
            ? ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) : 0,
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
    error = codec_status(esp_codec_dev_set_in_gain(s_input_codec,
                                                   (float)CONFIG_JUFF_CODEC_MIC_GAIN_DB),
                         "set microphone gain");
    if (error != ESP_OK) {
        return error;
    }
#if CONFIG_JUFF_VOICE_BARGE_IN && CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    // Channel gain uses physical ADC numbering, unlike the reordered TDM slots.
    error = codec_status(esp_codec_dev_set_in_channel_gain(
                             s_input_codec, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                             (float)CONFIG_JUFF_CODEC_REFERENCE_GAIN_DB),
                         "set speaker reference gain");
    if (error != ESP_OK) return error;
    ESP_LOGI(TAG, "ES7210 speaker reference: MIC3 / TDM slot 1, gain=%d dB",
             CONFIG_JUFF_CODEC_REFERENCE_GAIN_DB);
#endif
    (void)esp_codec_dev_set_out_mute(s_codec, true);
    return ESP_OK;
}

typedef struct {
    uint32_t generation;
    int64_t deadline_us;
} codec_write_context_t;

static int write_i2s_bytes(void *context, const void *data, size_t size, size_t *written)
{
    const codec_write_context_t *write = context;
    *written = 0;
    if (write->generation != codec_write_generation()) return ESP_ERR_INVALID_STATE;
    if (esp_timer_get_time() >= write->deadline_us) return ESP_ERR_TIMEOUT;
    const esp_err_t error = i2s_channel_write(s_tx_channel, data, size, written,
                                             CODEC_WRITE_WAIT_MS);
    // The driver can time out after consuming a prefix. Continue from that
    // exact byte, subject to the same deadline and cancellation generation.
    return error == ESP_ERR_TIMEOUT && *written != 0 ? ESP_OK : error;
}

static esp_err_t reset_codec_tx_locked(void)
{
    esp_err_t error = i2s_channel_disable(s_tx_channel);
    if (error == ESP_OK) error = i2s_channel_enable(s_tx_channel);
    return error;
}

static esp_err_t write_codec_pcm_mono(const void *pcm, size_t size, uint32_t generation)
{
    if (pcm == NULL || size == 0 || size % sizeof(int16_t) != 0) return ESP_ERR_INVALID_ARG;
    if (s_tx_channel == NULL || s_codec_write_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (generation != codec_write_generation()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_codec_write_mutex, pdMS_TO_TICKS(CODEC_WRITE_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // ES8311 implements hardware volume/mute; no software volume processor is
    // configured. Use its existing TX channel to retain the partial-write count
    // that esp_codec_dev_write otherwise hides. Network/queue data stays mono.
    int16_t scratch[CODEC_TX_CHANNELS == 2 ? CODEC_WRITE_CHUNK_FRAMES * 2 : 1];
    codec_write_context_t context = {
        .generation = generation,
        .deadline_us = esp_timer_get_time() + 250000
            + (int64_t)(size / sizeof(int16_t)) * 1000000 / CODEC_SAMPLE_RATE,
    };
    int result = audio_pcm_write_mono16(pcm, size, CODEC_TX_CHANNELS == 2,
                                       CODEC_TX_CHANNELS == 2 ? scratch : NULL,
                                       CODEC_WRITE_CHUNK_FRAMES, write_i2s_bytes, &context);
    if (result != ESP_OK) {
        // A failed write may end mid-frame. Reset before another writer can
        // append fresh PCM, so left/right alignment is never carried forward.
        ESP_ERROR_CHECK_WITHOUT_ABORT(set_amplifier_enabled(false));
        ESP_ERROR_CHECK_WITHOUT_ABORT(reset_codec_tx_locked());
        if (result < 0) result = ESP_FAIL;
    }
    xSemaphoreGive(s_codec_write_mutex);
    return result;
}

#if !CONFIG_JUFF_VOICE_BARGE_IN
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
#endif

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
#if CONFIG_JUFF_VOICE_BARGE_IN
    // Twenty milliseconds keeps detection responsive while the frontend batches
    // network PCM separately into the existing 100 ms BLE frames.
    enum { CAPTURE_FRAMES = 480, CAPTURE_CHANNELS = 2 };
#else
    enum { CAPTURE_FRAMES = CODEC_FRAME_SAMPLES, CAPTURE_CHANNELS = 1 };
#endif
    int16_t *codec_pcm = heap_caps_malloc(CAPTURE_FRAMES * CAPTURE_CHANNELS * sizeof(*codec_pcm),
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

#if CONFIG_JUFF_VOICE_BARGE_IN
    uint32_t seen_rx_overflows = 0;
#endif
    while (true) {
#if CONFIG_JUFF_VOICE_BARGE_IN
        const voice_frontend_playback_t before_read = snapshot_frontend_playback();
#endif
        const int result = esp_codec_dev_read(s_input_codec,
                                              codec_pcm,
                                              CAPTURE_FRAMES * CAPTURE_CHANNELS * sizeof(*codec_pcm));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Microphone read failed (codec status=%d)", result);
#if CONFIG_JUFF_VOICE_BARGE_IN
            if (s_voice_frontend_ready) voice_frontend_reset_stream();
#endif
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#if CONFIG_JUFF_VOICE_BARGE_IN
        if (recover_frontend_rx_overflow(&seen_rx_overflows)) {
            // This read may straddle a dropped DMA block. Start with the next
            // complete read instead of feeding a discontinuity to AEC/VAD.
            continue;
        }
        // Diagnostic statistics still measure the unprocessed physical mic.
        for (size_t index = 0; index < CAPTURE_FRAMES; ++index) {
            gateway_pcm[index] = codec_pcm[index * CAPTURE_CHANNELS];
        }
        collect_diagnostic_stats(gateway_pcm, CAPTURE_FRAMES);
        if (s_voice_frontend_ready) {
            const voice_frontend_playback_t playback =
                snapshot_frontend_playback_after_read(before_read);
            voice_frontend_process(codec_pcm, CAPTURE_FRAMES,
                                   playback, s_capture_enabled,
                                   !s_diagnostic_collecting
                                       && s_barge_in_allowed_callback != NULL
                                       && s_barge_in_allowed_callback(s_callback_context));
        } else if (s_capture_enabled && !s_playing && s_pcm_callback != NULL) {
            // Allocation failure keeps ordinary half-duplex voice usable.
            for (size_t index = 0; index < CAPTURE_FRAMES * 2 / 3; ++index) {
                const size_t doubled = index * 3;
                const size_t source = doubled / 2 * CAPTURE_CHANNELS;
                gateway_pcm[index] = (doubled & 1U) == 0 ? codec_pcm[source]
                    : (int16_t)(((int32_t)codec_pcm[source] + codec_pcm[source + CAPTURE_CHANNELS]) / 2);
            }
            s_pcm_callback((const uint8_t *)gateway_pcm,
                           CAPTURE_FRAMES * 2 / 3 * sizeof(*gateway_pcm), s_callback_context);
        }
#else
        collect_diagnostic_stats(codec_pcm, CODEC_FRAME_SAMPLES);
        if (!s_capture_enabled || s_playing || s_pcm_callback == NULL) {
            continue;
        }

        resample_24k_to_16k(codec_pcm, gateway_pcm);
        s_pcm_callback((const uint8_t *)gateway_pcm,
                       GATEWAY_FRAME_SAMPLES * sizeof(*gateway_pcm),
                       s_callback_context);
#endif
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
    uint32_t write_generation = codec_write_generation();

    while (true) {
        audio_item_t item = { 0 };
        if (xQueueReceive(s_playback_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (item.type == AUDIO_ITEM_BEGIN) {
            portENTER_CRITICAL(&s_codec_write_lock);
            const bool diagnostic_running = s_self_test_running;
            if (!diagnostic_running) s_playing = true;
            portEXIT_CRITICAL(&s_codec_write_lock);
            if (diagnostic_running) {
                notify_playback("playback.cancelled", item.response_id);
                free_item(&item);
                continue;
            }
            if (started || current_response[0] != '\0') {
                notify_playback("playback.cancelled", current_response);
            }
            copy_response_id(current_response, item.response_id);
            started = false;
            write_generation = codec_write_generation();
#if CONFIG_JUFF_VOICE_BARGE_IN
            begin_frontend_playback();
#endif
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
#if CONFIG_JUFF_VOICE_BARGE_IN
                // Notification delivery may wait; start AEC warmup only now,
                // immediately before the first actual codec write.
                mark_frontend_pcm_started();
#endif
            }
            const esp_err_t result = write_codec_pcm_mono(item.data, item.size, write_generation);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Speaker write stopped: %s", esp_err_to_name(result));
                // An explicit Clear already queued cancellation. Repeating it
                // here could discard a newer response queued in the meantime.
                if (write_generation == codec_write_generation()) {
                    audio_io_clear("speaker write stopped");
                }
            }
        } else if (item.type == AUDIO_ITEM_END) {
            // A BEGIN rejected during diagnostics has no playback to end.
            // Likewise, a delayed END must not mute a newer response.
            if (current_response[0] == '\0'
                || (item.response_id[0] != '\0'
                    && strcmp(item.response_id, current_response) != 0)) {
                free_item(&item);
                continue;
            }
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
            if (xSemaphoreTake(s_codec_write_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(reset_codec_tx_locked());
                xSemaphoreGive(s_codec_write_mutex);
            }
            s_playing = false;
        }
        free_item(&item);
    }
}

esp_err_t audio_io_init(audio_pcm_callback_t pcm_callback,
                        audio_playback_callback_t playback_callback,
                        audio_barge_in_callback_t barge_in_callback,
                        audio_barge_in_callback_t barge_in_allowed_callback,
                        void *context)
{
    s_pcm_callback = pcm_callback;
    s_playback_callback = playback_callback;
    s_barge_in_allowed_callback = barge_in_allowed_callback;
    s_callback_context = context;
    s_playback_queue = xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(audio_item_t));
    s_codec_write_mutex = xSemaphoreCreateMutex();
    if (s_playback_queue == NULL || s_codec_write_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const audio_codec_data_if_t *data_interface = NULL;
    esp_err_t error = initialize_i2s(&data_interface);
    if (error == ESP_OK) {
        error = initialize_codec(data_interface);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec initialization failed: %s", esp_err_to_name(error));
        return error;
    }

#if CONFIG_JUFF_VOICE_BARGE_IN
    error = voice_frontend_init(pcm_callback, barge_in_callback, context);
    s_voice_frontend_ready = error == ESP_OK;
    if (!s_voice_frontend_ready) {
        ESP_LOGE(TAG, "Voice interruption unavailable (%s); retaining half-duplex audio",
                 esp_err_to_name(error));
    }
#else
    (void)barge_in_callback;
#endif

    if (xTaskCreatePinnedToCore(capture_task,
                                "juff_capture",
                                s_voice_frontend_ready ? 8192 : 4096,
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
             JUFF_BOARD_NAME " audio ready: codec=%d Hz, gateway microphone=%d Hz, volume=%d, gain=%d dB",
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
    portENTER_CRITICAL(&s_codec_write_lock);
    const bool busy = s_playing || s_self_test_running;
    if (!busy) s_self_test_running = true;
    portEXIT_CRITICAL(&s_codec_write_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t write_generation = codec_write_generation();

    int16_t *tone = heap_caps_malloc(CODEC_FRAME_SAMPLES * sizeof(*tone),
                                     MALLOC_CAP_8BIT);
    if (tone == NULL) {
        portENTER_CRITICAL(&s_codec_write_lock);
        s_self_test_running = false;
        portEXIT_CRITICAL(&s_codec_write_lock);
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
    esp_err_t error = write_generation == codec_write_generation()
        ? set_amplifier_enabled(true) : ESP_ERR_INVALID_STATE;
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
        error = write_codec_pcm_mono(tone, CODEC_FRAME_SAMPLES * sizeof(*tone),
                                     write_generation);
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
    portENTER_CRITICAL(&s_codec_write_lock);
    s_self_test_running = false;
    portEXIT_CRITICAL(&s_codec_write_lock);

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
    portENTER_CRITICAL(&s_codec_write_lock);
    const bool diagnostic_running = s_self_test_running;
    portEXIT_CRITICAL(&s_codec_write_lock);
    if (diagnostic_running) return ESP_ERR_INVALID_STATE;
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
    cancel_codec_writes();
    // Silence the amplifier immediately, even if a PCM write is still draining.
    ESP_ERROR_CHECK_WITHOUT_ABORT(set_amplifier_enabled(false));
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

bool audio_io_supports_voice_barge_in(void)
{
    return s_voice_frontend_ready;
}

i2c_master_bus_handle_t audio_io_i2c_bus(void)
{
    return s_i2c_bus;
}

#else

esp_err_t audio_io_init(audio_pcm_callback_t pcm_callback,
                        audio_playback_callback_t playback_callback,
                        audio_barge_in_callback_t barge_in_callback,
                        audio_barge_in_callback_t barge_in_allowed_callback,
                        void *context)
{
    (void)pcm_callback;
    (void)playback_callback;
    (void)barge_in_callback;
    (void)barge_in_allowed_callback;
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

bool audio_io_supports_voice_barge_in(void)
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

#include "board_display.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_io.h"
#include "ble_manager.h"
#include "device_client.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "wifi_manager.h"

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 320
#define LCD_HEIGHT 480
#define LCD_DRAW_ROWS 24
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define LCD_PIN_MOSI 1
#define LCD_PIN_CLOCK 5
#define LCD_PIN_DC 3
#define LCD_PIN_BACKLIGHT 6
#define LCD_RESET_EXPANDER_PIN 1
#define TOUCH_ADDRESS 0x38
#define LVGL_TICK_MS 2

#define COLOR_BACKGROUND 0x060914
#define COLOR_BACKGROUND_END 0x101B2F
#define COLOR_SURFACE 0x111A2D
#define COLOR_SURFACE_RAISED 0x192641
#define COLOR_PRIMARY 0x62E7D0
#define COLOR_PRIMARY_DEEP 0x18BFA7
#define COLOR_BLUE 0x55B8FF
#define COLOR_VIOLET 0x8D7CFF
#define COLOR_WARNING 0xFFBE69
#define COLOR_DANGER 0xFF6F87
#define COLOR_TEXT 0xF7FAFF
#define COLOR_MUTED 0x91A1B8
#define COLOR_OFF 0x45546C

typedef struct {
    uint8_t command;
    const uint8_t *data;
    uint8_t data_size;
    uint16_t delay_ms;
} lcd_init_command_t;

typedef enum {
    VOICE_VISUAL_IDLE,
    VOICE_VISUAL_LISTENING,
    VOICE_VISUAL_PROCESSING,
    VOICE_VISUAL_SPEAKING,
} voice_visual_state_t;

static const char *TAG = "juff_display";
static esp_lcd_panel_io_handle_t s_lcd_io;
static i2c_master_dev_handle_t s_touch;
static lv_disp_draw_buf_t s_draw_buffer;
static lv_disp_drv_t s_display_driver;
static lv_indev_drv_t s_touch_driver;
static esp_timer_handle_t s_tick_timer;
static SemaphoreHandle_t s_state_mutex;
static volatile bool s_flush_pending;
static volatile uint32_t s_completed_flushes;
static bool s_ready;
static bool s_touch_reported;
static bool s_audio_test_running;
static uint8_t s_brightness = 100;
static voice_visual_state_t s_voice_visual_state = VOICE_VISUAL_IDLE;
static uint32_t s_animation_phase;
static char s_notice_title[40];
static char s_notice_detail[96];
static uint64_t s_notice_until_ms;

static lv_obj_t *s_connection_pill;
static lv_obj_t *s_connection_label;
static lv_obj_t *s_orb_halo;
static lv_obj_t *s_voice_arc;
static lv_obj_t *s_orb;
static lv_obj_t *s_orb_icon;
static lv_obj_t *s_orb_live_dot;
static lv_obj_t *s_main_title;
static lv_obj_t *s_main_detail;
static lv_obj_t *s_wave_bars[7];
static lv_obj_t *s_qwen_chip;
static lv_obj_t *s_qwen_dot;
static lv_obj_t *s_qwen_label;
static lv_obj_t *s_mic_chip;
static lv_obj_t *s_mic_dot;
static lv_obj_t *s_mic_label;
static lv_obj_t *s_primary_button;
static lv_obj_t *s_primary_button_label;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_connection_overlay;
static lv_obj_t *s_connection_device_label;
static lv_obj_t *s_connection_status_label;
static lv_obj_t *s_connection_detail_label;
static lv_obj_t *s_pairing_button;
static lv_obj_t *s_pairing_button_label;

static const uint8_t s_cmd_colmod[] = { 0x05 };
static const uint8_t s_cmd_unlock_one[] = { 0xC3 };
static const uint8_t s_cmd_unlock_two[] = { 0x96 };
static const uint8_t s_cmd_inversion[] = { 0x01 };
static const uint8_t s_cmd_entry_mode[] = { 0xC6 };
static const uint8_t s_cmd_power_one[] = { 0x80, 0x45 };
static const uint8_t s_cmd_power_two[] = { 0x13 };
static const uint8_t s_cmd_power_three[] = { 0xA7 };
static const uint8_t s_cmd_vcom[] = { 0x0A };
static const uint8_t s_cmd_display_function[] = {
    0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
};
static const uint8_t s_cmd_positive_gamma[] = {
    0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30,
    0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31,
};
static const uint8_t s_cmd_negative_gamma[] = {
    0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F,
    0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32,
};
static const uint8_t s_cmd_lock_one[] = { 0x3C };
static const uint8_t s_cmd_lock_two[] = { 0x69 };
static const uint8_t s_cmd_madctl_base[] = { 0x08 };
static const uint8_t s_cmd_colmod_rgb565[] = { 0x55 };
static const uint8_t s_cmd_madctl[] = { 0x48 };

// This is the vendor sequence shipped for the non-B Waveshare
// ESP32-S3-Touch-LCD-3.5 ST7796 panel.
static const lcd_init_command_t s_lcd_init_commands[] = {
    { 0x11, NULL, 0, 120 },
    { 0x3A, s_cmd_colmod, sizeof(s_cmd_colmod), 0 },
    { 0xF0, s_cmd_unlock_one, sizeof(s_cmd_unlock_one), 0 },
    { 0xF0, s_cmd_unlock_two, sizeof(s_cmd_unlock_two), 0 },
    { 0xB4, s_cmd_inversion, sizeof(s_cmd_inversion), 0 },
    { 0xB7, s_cmd_entry_mode, sizeof(s_cmd_entry_mode), 0 },
    { 0xC0, s_cmd_power_one, sizeof(s_cmd_power_one), 0 },
    { 0xC1, s_cmd_power_two, sizeof(s_cmd_power_two), 0 },
    { 0xC2, s_cmd_power_three, sizeof(s_cmd_power_three), 0 },
    { 0xC5, s_cmd_vcom, sizeof(s_cmd_vcom), 0 },
    { 0xE8, s_cmd_display_function, sizeof(s_cmd_display_function), 0 },
    { 0xE0, s_cmd_positive_gamma, sizeof(s_cmd_positive_gamma), 0 },
    { 0xE1, s_cmd_negative_gamma, sizeof(s_cmd_negative_gamma), 0 },
    { 0xF0, s_cmd_lock_one, sizeof(s_cmd_lock_one), 0 },
    { 0xF0, s_cmd_lock_two, sizeof(s_cmd_lock_two), 120 },
    { 0x21, NULL, 0, 0 },
    { 0x29, NULL, 0, 20 },
};

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static esp_err_t set_brightness(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t duty = ((uint32_t)percent * 1023U) / 100U;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                    LEDC_CHANNEL_0,
                                    duty);
    if (error == ESP_OK) {
        error = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (error == ESP_OK) {
        s_brightness = percent;
        ESP_LOGI(TAG, "Backlight brightness=%u%%", percent);
    }
    return error;
}

static esp_err_t initialize_backlight(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

static esp_err_t lcd_command(uint8_t command,
                             const uint8_t *data,
                             size_t data_size)
{
    return esp_lcd_panel_io_tx_param(s_lcd_io, command, data, data_size);
}

static bool lcd_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *event_data,
                              void *user_context)
{
    (void)panel_io;
    (void)event_data;
    (void)user_context;
    if (s_flush_pending) {
        s_flush_pending = false;
        ++s_completed_flushes;
        lv_disp_flush_ready(&s_display_driver);
    }
    return false;
}

static esp_err_t initialize_lcd(void)
{
    ESP_LOGI(TAG,
             "Initializing ST7796 LCD: 320x480, MOSI=%d CLK=%d DC=%d BL=%d",
             LCD_PIN_MOSI,
             LCD_PIN_CLOCK,
             LCD_PIN_DC,
             LCD_PIN_BACKLIGHT);

    ESP_RETURN_ON_ERROR(audio_io_set_expander_pin(LCD_RESET_EXPANDER_PIN, false),
                        TAG,
                        "assert LCD reset on TCA9554 EXIO1");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(audio_io_set_expander_pin(LCD_RESET_EXPANDER_PIN, true),
                        TAG,
                        "release LCD reset on TCA9554 EXIO1");
    vTaskDelay(pdMS_TO_TICKS(100));

    const spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LCD_PIN_CLOCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_DRAW_ROWS * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG,
                        "initialize LCD SPI bus");

    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_transfer_done,
        .user_ctx = &s_display_driver,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                 &io,
                                 &s_lcd_io),
        TAG,
        "attach ST7796 panel IO");

    ESP_RETURN_ON_ERROR(lcd_command(0x01, NULL, 0), TAG, "software reset LCD");
    vTaskDelay(pdMS_TO_TICKS(20));

    // Match Waveshare's ST7796 driver preamble exactly. The panel needs this
    // wake/configure pass before the vendor sequence below on a cold boot.
    ESP_RETURN_ON_ERROR(lcd_command(0x11, NULL, 0), TAG, "wake ST7796");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(lcd_command(0x36,
                                    s_cmd_madctl_base,
                                    sizeof(s_cmd_madctl_base)),
                        TAG,
                        "set initial ST7796 MADCTL");
    ESP_RETURN_ON_ERROR(lcd_command(0x3A,
                                    s_cmd_colmod_rgb565,
                                    sizeof(s_cmd_colmod_rgb565)),
                        TAG,
                        "set initial ST7796 RGB565 mode");

    for (size_t index = 0;
         index < sizeof(s_lcd_init_commands) / sizeof(s_lcd_init_commands[0]);
         ++index) {
        const lcd_init_command_t *entry = &s_lcd_init_commands[index];
        ESP_RETURN_ON_ERROR(lcd_command(entry->command,
                                        entry->data,
                                        entry->data_size),
                            TAG,
                            "send ST7796 init sequence");
        if (entry->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
        }
    }
    // The official portrait configuration mirrors the panel on X after init.
    ESP_RETURN_ON_ERROR(lcd_command(0x36,
                                    s_cmd_madctl,
                                    sizeof(s_cmd_madctl)),
                        TAG,
                        "set portrait orientation");
    ESP_LOGI(TAG, "ST7796 initialization complete");
    return ESP_OK;
}

static esp_err_t lcd_fill_solid(uint16_t rgb565)
{
    const size_t stripe_pixels = LCD_WIDTH * LCD_DRAW_ROWS;
    uint16_t *stripe = heap_caps_malloc(stripe_pixels * sizeof(*stripe),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (stripe == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // The SPI panel consumes the high byte first.
    const uint16_t wire_color = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    for (size_t index = 0; index < stripe_pixels; ++index) {
        stripe[index] = wire_color;
    }

    const uint8_t column[] = { 0x00, 0x00, 0x01, 0x3F };
    esp_err_t error = ESP_OK;
    for (uint16_t y = 0; y < LCD_HEIGHT && error == ESP_OK; y += LCD_DRAW_ROWS) {
        const uint16_t y_end = (uint16_t)(
            (y + LCD_DRAW_ROWS - 1 < LCD_HEIGHT)
                ? y + LCD_DRAW_ROWS - 1
                : LCD_HEIGHT - 1);
        const uint8_t row[] = {
            (uint8_t)(y >> 8),
            (uint8_t)y,
            (uint8_t)(y_end >> 8),
            (uint8_t)y_end,
        };
        error = lcd_command(0x2A, column, sizeof(column));
        if (error == ESP_OK) {
            error = lcd_command(0x2B, row, sizeof(row));
        }
        if (error == ESP_OK) {
            const size_t pixels = LCD_WIDTH * (size_t)(y_end - y + 1);
            error = esp_lcd_panel_io_tx_color(s_lcd_io,
                                              0x2C,
                                              stripe,
                                              pixels * sizeof(*stripe));
        }
    }
    // A parameter transaction waits for queued color transfers before the DMA
    // buffer is released. NOP does not alter the displayed frame.
    if (error == ESP_OK) {
        error = lcd_command(0x00, NULL, 0);
    }
    free(stripe);
    return error;
}

static void lcd_flush(lv_disp_drv_t *driver,
                      const lv_area_t *area,
                      lv_color_t *color_map)
{
    const uint8_t column[] = {
        (uint8_t)(area->x1 >> 8),
        (uint8_t)area->x1,
        (uint8_t)(area->x2 >> 8),
        (uint8_t)area->x2,
    };
    const uint8_t row[] = {
        (uint8_t)(area->y1 >> 8),
        (uint8_t)area->y1,
        (uint8_t)(area->y2 >> 8),
        (uint8_t)area->y2,
    };
    esp_err_t error = lcd_command(0x2A, column, sizeof(column));
    if (error == ESP_OK) {
        error = lcd_command(0x2B, row, sizeof(row));
    }
    if (error == ESP_OK) {
        const size_t pixels = (size_t)(area->x2 - area->x1 + 1)
            * (size_t)(area->y2 - area->y1 + 1);
        s_flush_pending = true;
        error = esp_lcd_panel_io_tx_color(s_lcd_io,
                                          0x2C,
                                          color_map,
                                          pixels * sizeof(lv_color_t));
    }
    if (error != ESP_OK) {
        s_flush_pending = false;
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(error));
        lv_disp_flush_ready(driver);
    }
}

static esp_err_t touch_read(uint8_t first_register,
                            uint8_t *data,
                            size_t data_size)
{
    return i2c_master_transmit_receive(s_touch,
                                       &first_register,
                                       1,
                                       data,
                                       data_size,
                                       20);
}

static void touch_read_callback(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    uint8_t touch_data[5] = { 0 };
    const esp_err_t error = touch_read(0x02,
                                       touch_data,
                                       sizeof(touch_data));
    if (error != ESP_OK || (touch_data[0] & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    const uint16_t x = (uint16_t)(((touch_data[1] & 0x0F) << 8)
                                  | touch_data[2]);
    const uint16_t y = (uint16_t)(((touch_data[3] & 0x0F) << 8)
                                  | touch_data[4]);
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!s_touch_reported) {
        s_touch_reported = true;
        ESP_LOGI(TAG, "Touch input confirmed at x=%u y=%u", x, y);
    }
}

static esp_err_t initialize_touch(lv_disp_t *display)
{
    i2c_master_bus_handle_t bus = audio_io_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_ADDRESS, 100),
                        TAG,
                        "probe FT6336 touch at 0x38");
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &s_touch),
                        TAG,
                        "attach FT6336 touch");
    uint8_t chip_id = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(touch_read(0xA3, &chip_id, 1));

    lv_indev_drv_init(&s_touch_driver);
    s_touch_driver.type = LV_INDEV_TYPE_POINTER;
    s_touch_driver.disp = display;
    s_touch_driver.read_cb = touch_read_callback;
    lv_indev_drv_register(&s_touch_driver);
    ESP_LOGI(TAG, "FT6336 touch ready at 0x38 (chip id=0x%02x)", chip_id);
    return ESP_OK;
}

static void style_panel(lv_obj_t *object, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void set_button_style(lv_obj_t *button, uint32_t color)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(COLOR_SURFACE_RAISED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(COLOR_OFF),
                              LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_chip_state(lv_obj_t *chip,
                           lv_obj_t *dot,
                           lv_obj_t *label,
                           uint32_t color,
                           const char *text)
{
    lv_obj_set_style_border_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    set_label_text(label, text);
}

static void snapshot_ui_state(char title[40],
                              char detail[96],
                              bool *test_running,
                              voice_visual_state_t *voice_state)
{
    title[0] = '\0';
    detail[0] = '\0';
    *test_running = false;
    *voice_state = VOICE_VISUAL_IDLE;
    if (s_state_mutex == NULL
        || xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    *test_running = s_audio_test_running;
    *voice_state = s_voice_visual_state;
    const uint64_t now = uptime_ms();
    if (s_notice_title[0] != '\0'
        && (s_notice_until_ms == UINT64_MAX || now < s_notice_until_ms)) {
        strlcpy(title, s_notice_title, 40);
        strlcpy(detail, s_notice_detail, 96);
    } else if (s_notice_title[0] != '\0') {
        s_notice_title[0] = '\0';
        s_notice_detail[0] = '\0';
        s_notice_until_ms = 0;
    }
    xSemaphoreGive(s_state_mutex);
}

static void refresh_status_ui(void)
{
    const bool has_wifi = wifi_manager_has_credentials();
    const bool wifi_connected = wifi_manager_is_connected();
    const bool ble_connected = ble_manager_is_connected();
    const bool ble_advertising = ble_manager_is_advertising();
    const bool ble_pairing = ble_manager_is_pairing();
    const bool ble_audio_connected = ble_manager_audio_is_connected();
    const bool ble_voice_ready = ble_manager_is_voice_ready();
    const bool bridge_connected = ble_audio_connected || device_client_is_connected();
    bool voice_ready = ble_voice_ready || device_client_is_ready();
    const bool playing = audio_io_is_playing();
    const bool audio_ready = audio_io_is_available();

    char notice_title[40];
    char notice_detail[96];
    bool test_running = false;
    voice_visual_state_t voice_state = VOICE_VISUAL_IDLE;
    snapshot_ui_state(notice_title,
                      notice_detail,
                      &test_running,
                      &voice_state);
    if (ble_audio_connected
        && (playing || voice_state != VOICE_VISUAL_IDLE)) {
        voice_ready = true;
    }
    if (playing) {
        voice_state = VOICE_VISUAL_SPEAKING;
    } else if (!voice_ready) {
        voice_state = VOICE_VISUAL_IDLE;
    }

    const char *title = notice_title;
    const char *detail = notice_detail;
    const char *icon = LV_SYMBOL_AUDIO;
    const char *primary_text = LV_SYMBOL_AUDIO "  JUST START TALKING";
    uint32_t orb_color = COLOR_PRIMARY;
    uint32_t orb_gradient = COLOR_BLUE;
    uint32_t primary_color = COLOR_SURFACE_RAISED;

    if (title[0] == '\0') {
        if (!voice_ready) {
            icon = LV_SYMBOL_BLUETOOTH;
            primary_text = LV_SYMBOL_BLUETOOTH "  OPEN MAC";
            if (ble_audio_connected || bridge_connected) {
                title = "Waking up Qwen";
                detail = "Your private audio link is ready. Just a moment...";
                orb_color = COLOR_BLUE;
                orb_gradient = COLOR_VIOLET;
            } else if (ble_connected) {
                title = "Mac found";
                detail = "Opening the realtime voice connection";
                orb_color = COLOR_BLUE;
                orb_gradient = COLOR_PRIMARY;
            } else if (ble_advertising || !has_wifi) {
                title = "Meet JUFF";
                detail = "Open the JUFF service on your Mac to begin";
                orb_color = COLOR_BLUE;
                orb_gradient = COLOR_VIOLET;
            } else if (!wifi_connected) {
                title = "Getting online";
                detail = "Connecting with your saved network";
                orb_color = COLOR_WARNING;
                orb_gradient = COLOR_DANGER;
            } else {
                title = "Almost there";
                detail = "Waiting for the voice service";
                orb_color = COLOR_BLUE;
                orb_gradient = COLOR_VIOLET;
            }
        } else {
            switch (voice_state) {
            case VOICE_VISUAL_LISTENING:
                title = "I'm listening";
                detail = "Go ahead - you don't need to press anything";
                icon = LV_SYMBOL_BARS;
                primary_text = LV_SYMBOL_AUDIO "  I'M LISTENING";
                primary_color = COLOR_PRIMARY_DEEP;
                orb_color = COLOR_PRIMARY;
                orb_gradient = COLOR_BLUE;
                break;
            case VOICE_VISUAL_PROCESSING:
                title = "Thinking it through";
                detail = "Qwen is preparing a helpful answer";
                icon = LV_SYMBOL_REFRESH;
                primary_text = LV_SYMBOL_STOP "  CANCEL REQUEST";
                primary_color = COLOR_VIOLET;
                orb_color = COLOR_VIOLET;
                orb_gradient = COLOR_BLUE;
                break;
            case VOICE_VISUAL_SPEAKING:
                title = "Here's what I found";
                detail = "Tap the orb or Stop anytime to interrupt";
                icon = LV_SYMBOL_VOLUME_MAX;
                primary_text = LV_SYMBOL_STOP "  STOP RESPONSE";
                primary_color = COLOR_DANGER;
                orb_color = COLOR_PRIMARY;
                orb_gradient = COLOR_VIOLET;
                break;
            case VOICE_VISUAL_IDLE:
            default: {
                static const char *const hints[] = {
                    "Just speak naturally - no button needed",
                    "Ask a follow-up whenever you like",
                    "Try: Help me plan the rest of my day",
                };
                title = "Ready when you are";
                detail = hints[(s_animation_phase / 42U) % 3U];
                break;
            }
        }
        }
    } else if (test_running) {
        orb_color = COLOR_WARNING;
        orb_gradient = COLOR_DANGER;
        icon = LV_SYMBOL_AUDIO;
        primary_text = LV_SYMBOL_AUDIO "  AUDIO CHECK";
        primary_color = COLOR_WARNING;
    }

    set_label_text(s_main_title, title);
    set_label_text(s_main_detail, detail);
    set_label_text(s_orb_icon, icon);
    set_label_text(s_primary_button_label, primary_text);
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(orb_color), 0);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(orb_gradient), 0);
    lv_obj_set_style_shadow_color(s_orb, lv_color_hex(orb_color), 0);
    lv_obj_set_style_border_color(s_orb_halo, lv_color_hex(orb_color), 0);
    lv_obj_set_style_arc_color(s_voice_arc,
                               lv_color_hex(orb_color),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_orb_live_dot,
                              lv_color_hex(voice_ready ? COLOR_PRIMARY
                                                       : COLOR_WARNING),
                              0);
    lv_obj_set_style_bg_color(s_primary_button,
                              lv_color_hex(primary_color),
                              0);

    const uint8_t pulse = (uint8_t)(45U
                                    + ((s_animation_phase % 12U) < 6U
                                           ? (s_animation_phase % 6U) * 18U
                                           : (11U - (s_animation_phase % 12U)) * 18U));
    lv_obj_set_style_border_opa(s_orb_halo, pulse, 0);
    lv_arc_set_rotation(s_voice_arc,
                        (uint16_t)((s_animation_phase *
                                    (voice_state == VOICE_VISUAL_PROCESSING ? 17U : 7U))
                                   % 360U));

    for (unsigned index = 0; index < 7; ++index) {
        unsigned height = 5;
        if (!voice_ready) {
            height += (index + s_animation_phase / 5U) % 3U;
        } else if (voice_state == VOICE_VISUAL_LISTENING) {
            height = 8U + ((s_animation_phase * 11U + index * 17U) % 23U);
        } else if (voice_state == VOICE_VISUAL_PROCESSING) {
            const unsigned wave = (s_animation_phase + index * 2U) % 12U;
            height = 7U + (wave < 6U ? wave : 12U - wave) * 4U;
        } else if (voice_state == VOICE_VISUAL_SPEAKING) {
            height = 9U + ((s_animation_phase * 13U + index * 19U) % 22U);
        } else {
            height = 5U + ((index + s_animation_phase / 4U) % 4U) * 2U;
        }
        lv_obj_set_height(s_wave_bars[index], (lv_coord_t)height);
        lv_obj_set_y(s_wave_bars[index], (lv_coord_t)(237 - height));
        lv_obj_set_style_bg_color(s_wave_bars[index],
                                  lv_color_hex(orb_color),
                                  0);
    }

    if (ble_pairing) {
        set_label_text(s_connection_label, LV_SYMBOL_BLUETOOTH "  OPEN");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_WARNING),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_WARNING),
                                    0);
    } else if (ble_voice_ready) {
        set_label_text(s_connection_label, LV_SYMBOL_BLUETOOTH "  LIVE");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_PRIMARY),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_PRIMARY),
                                    0);
    } else if (wifi_connected) {
        set_label_text(s_connection_label, LV_SYMBOL_WIFI "  LIVE");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_PRIMARY),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_PRIMARY),
                                    0);
    } else if (ble_connected) {
        set_label_text(s_connection_label, LV_SYMBOL_BLUETOOTH "  MAC");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_BLUE),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_BLUE),
                                    0);
    } else if (ble_advertising) {
        set_label_text(s_connection_label, LV_SYMBOL_BLUETOOTH "  PAIR");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_BLUE),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_BLUE),
                                    0);
    } else {
        set_label_text(s_connection_label, LV_SYMBOL_USB "  USB");
        lv_obj_set_style_border_color(s_connection_pill,
                                      lv_color_hex(COLOR_OFF),
                                      0);
        lv_obj_set_style_text_color(s_connection_label,
                                    lv_color_hex(COLOR_MUTED),
                                    0);
    }

    set_chip_state(s_qwen_chip,
                   s_qwen_dot,
                   s_qwen_label,
                   voice_ready ? COLOR_PRIMARY
                               : (bridge_connected ? COLOR_BLUE : COLOR_OFF),
                   voice_ready ? "QWEN READY"
                               : (bridge_connected ? "QWEN LINK" : "QWEN OFF"));
    set_chip_state(s_mic_chip,
                   s_mic_dot,
                   s_mic_label,
                   !audio_ready ? COLOR_DANGER
                                : (voice_ready ? COLOR_PRIMARY : COLOR_BLUE),
                   !audio_ready ? "MIC ERROR"
                                : (voice_state == VOICE_VISUAL_LISTENING
                                       ? "HEARING YOU"
                                       : (voice_ready ? "MIC LIVE" : "MIC READY")));
    lv_label_set_text_fmt(s_brightness_label,
                          LV_SYMBOL_EYE_OPEN "\n%u%%",
                          s_brightness);

    if (s_connection_device_label != NULL) {
        set_label_text(s_connection_device_label, ble_manager_device_name());
        if (ble_pairing) {
            const uint32_t seconds = ble_manager_pairing_seconds_remaining();
            char button_text[48];
            snprintf(button_text,
                     sizeof(button_text),
                     LV_SYMBOL_CLOSE "  CANCEL  %u:%02u",
                     (unsigned)(seconds / 60U),
                     (unsigned)(seconds % 60U));
            set_label_text(s_pairing_button_label, button_text);
            set_label_text(s_connection_status_label,
                           "DISCOVERABLE NOW");
            if (strcmp(notice_title, "Pair with JUFF") == 0) {
                set_label_text(s_connection_detail_label, notice_detail);
            } else {
                set_label_text(s_connection_detail_label,
                               "Open JUFF on the new Mac. The pairing code will appear here.");
            }
            lv_obj_set_style_text_color(s_connection_status_label,
                                        lv_color_hex(COLOR_WARNING),
                                        0);
            lv_obj_set_style_bg_color(s_pairing_button,
                                      lv_color_hex(COLOR_DANGER),
                                      0);
        } else {
            set_label_text(s_pairing_button_label,
                           "+  PAIR A NEW MAC");
            set_label_text(s_connection_status_label,
                           ble_connected ? "MAC CONNECTED" : "READY TO CONNECT");
            set_label_text(s_connection_detail_label,
                           ble_connected
                               ? "This Mac is active. Pairing another Mac keeps this one remembered."
                               : "Tap below, then open JUFF on the Mac you want to use.");
            lv_obj_set_style_text_color(s_connection_status_label,
                                        lv_color_hex(ble_connected
                                                         ? COLOR_PRIMARY
                                                         : COLOR_BLUE),
                                        0);
            lv_obj_set_style_bg_color(s_pairing_button,
                                      lv_color_hex(COLOR_PRIMARY_DEEP),
                                      0);
        }
    }
    ++s_animation_phase;
}

static void audio_test_task(void *argument)
{
    (void)argument;
    const esp_err_t result = audio_io_run_self_test();
    if (s_state_mutex != NULL
        && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_audio_test_running = false;
        xSemaphoreGive(s_state_mutex);
    }
    if (result == ESP_OK) {
        board_display_set_notice("Sound check passed",
                                 "Speaker and microphone are working",
                                 3500);
    } else {
        board_display_set_notice("Sound needs attention",
                                 "Open the Mac dashboard for details",
                                 5000);
    }
    vTaskDelete(NULL);
}

void board_display_start_audio_test(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    const bool already_running = s_audio_test_running;
    s_audio_test_running = true;
    xSemaphoreGive(s_state_mutex);
    if (already_running) {
        return;
    }
    ESP_LOGI(TAG, "Touch action: audio self-test");
    board_display_set_notice("A quick sound check",
                             "You'll hear two soft tones",
                             0);
    if (xTaskCreate(audio_test_task,
                    "display_audio_test",
                    4096,
                    NULL,
                    5,
                    NULL) != pdPASS) {
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_audio_test_running = false;
            xSemaphoreGive(s_state_mutex);
        }
        board_display_set_notice("Sound check unavailable",
                                 "Please try again in a moment",
                                 4000);
            }
        }

static void primary_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    voice_visual_state_t voice_state = VOICE_VISUAL_IDLE;
    if (s_state_mutex != NULL
        && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        voice_state = s_voice_visual_state;
        xSemaphoreGive(s_state_mutex);
    }
    const bool voice_ready = ble_manager_is_voice_ready()
        || device_client_is_ready();
    const bool can_interrupt = audio_io_is_playing()
        || voice_state == VOICE_VISUAL_LISTENING
        || voice_state == VOICE_VISUAL_PROCESSING
        || voice_state == VOICE_VISUAL_SPEAKING;

    if (voice_ready && can_interrupt) {
        ESP_LOGI(TAG, "Touch action: interrupt active voice turn");
        if (ble_manager_is_voice_ready()) {
            ble_manager_send_interrupt();
        } else {
            device_client_send_interrupt();
        }
        board_display_set_notice("Got it",
                                 "Stopping this turn",
                                 1400);
    } else if (voice_ready) {
        board_display_set_notice("No button needed",
                                 "Just start speaking whenever you're ready",
                                 2200);
    } else {
        board_display_set_notice("Let's connect",
                                 "Open the JUFF service on your Mac",
                                 2500);
    }
}

void board_display_cycle_brightness(void)
{
    uint8_t next = 100;
    if (s_brightness > 80) {
        next = 65;
    } else if (s_brightness > 40) {
        next = 30;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(set_brightness(next));
    board_display_set_notice("Display brightness",
                             next == 100 ? "Bright"
                                         : (next == 65 ? "Comfortable" : "Dim"),
                             1200);
}

static void brightness_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        board_display_cycle_brightness();
    }
}

static void connection_pill_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED
        || s_connection_overlay == NULL) {
        return;
    }
    ESP_LOGI(TAG, "Touch action: open connection manager");
    lv_obj_clear_flag(s_connection_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_connection_overlay);
    refresh_status_ui();
}

static void connection_close_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED
        && s_connection_overlay != NULL) {
        lv_obj_add_flag(s_connection_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pairing_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (ble_manager_is_pairing()) {
        ESP_LOGI(TAG, "Touch action: cancel pair-new-Mac window");
        ble_manager_cancel_pairing();
        board_display_set_notice("Pairing cancelled",
                                 "Your remembered Mac can reconnect",
                                 2200);
    } else {
        ESP_LOGI(TAG, "Touch action: pair a new Mac");
        const esp_err_t error = ble_manager_start_pairing();
        if (error != ESP_OK) {
            board_display_set_notice("Can't start pairing",
                                     "Please try again in a moment",
                                     3000);
        }
    }
    refresh_status_ui();
}

static lv_obj_t *create_button(lv_obj_t *screen,
                               int x,
                               int y,
                               int width,
                               int height,
                               const char *text,
                               uint32_t color,
                               lv_event_cb_t callback)
{
    lv_obj_t *button = lv_btn_create(screen);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    set_button_style(button, color);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = make_label(button,
                                 text,
                                 &lv_font_montserrat_14,
                                 COLOR_TEXT);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *create_status_chip(lv_obj_t *screen,
                                    int x,
                                    const char *text,
                                    lv_obj_t **dot,
                                    lv_obj_t **label)
{
    lv_obj_t *chip = lv_obj_create(screen);
    style_panel(chip, COLOR_SURFACE, 15);
    lv_obj_set_pos(chip, x, 328);
    lv_obj_set_size(chip, 112, 30);
    lv_obj_set_style_bg_opa(chip, (lv_opa_t)170, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(COLOR_OFF), 0);
    lv_obj_set_style_border_opa(chip, LV_OPA_40, 0);

    *dot = lv_obj_create(chip);
    style_panel(*dot, COLOR_OFF, LV_RADIUS_CIRCLE);
    lv_obj_set_size(*dot, 8, 8);
    lv_obj_set_pos(*dot, 12, 11);
    lv_obj_set_style_shadow_width(*dot, 0, 0);

    *label = make_label(chip,
                        text,
                        &lv_font_montserrat_12,
                        COLOR_MUTED);
    lv_obj_set_pos(*label, 29, 8);
    return chip;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_grad_color(screen,
                                   lv_color_hex(COLOR_BACKGROUND_END),
                                   0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand_mark = lv_obj_create(screen);
    style_panel(brand_mark, COLOR_PRIMARY, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(brand_mark, 16, 14);
    lv_obj_set_size(brand_mark, 38, 38);
    lv_obj_set_style_bg_grad_color(brand_mark, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_dir(brand_mark, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(brand_mark, 0, 0);
    lv_obj_t *brand_mark_label = make_label(brand_mark,
                                            "J",
                                            &lv_font_montserrat_20,
                                            COLOR_BACKGROUND);
    lv_obj_center(brand_mark_label);

    lv_obj_t *brand = make_label(screen,
                                 "JUFF",
                                 &lv_font_montserrat_20,
                                 COLOR_TEXT);
    lv_obj_set_pos(brand, 64, 11);
    lv_obj_t *product = make_label(screen,
                                   "YOUR VOICE COMPANION",
                                   &lv_font_montserrat_12,
                                   COLOR_MUTED);
    lv_obj_set_pos(product, 65, 37);

    s_connection_pill = lv_obj_create(screen);
    style_panel(s_connection_pill, COLOR_SURFACE, 15);
    lv_obj_set_pos(s_connection_pill, 218, 18);
    lv_obj_set_size(s_connection_pill, 86, 30);
    lv_obj_set_style_bg_opa(s_connection_pill, (lv_opa_t)145, 0);
    lv_obj_set_style_border_width(s_connection_pill, 1, 0);
    lv_obj_set_style_border_color(s_connection_pill,
                                  lv_color_hex(COLOR_OFF),
                                  0);
    s_connection_label = make_label(s_connection_pill,
                                    LV_SYMBOL_USB "  USB",
                                    &lv_font_montserrat_12,
                                    COLOR_MUTED);
    lv_obj_center(s_connection_label);
    lv_obj_add_flag(s_connection_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_connection_pill,
                        connection_pill_clicked,
                        LV_EVENT_CLICKED,
                        NULL);

    s_orb_halo = lv_obj_create(screen);
    style_panel(s_orb_halo, COLOR_BACKGROUND, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_orb_halo, 80, 63);
    lv_obj_set_size(s_orb_halo, 160, 160);
    lv_obj_set_style_bg_opa(s_orb_halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_halo, 2, 0);
    lv_obj_set_style_border_color(s_orb_halo,
                                  lv_color_hex(COLOR_PRIMARY),
                                  0);
    lv_obj_set_style_border_opa(s_orb_halo, LV_OPA_30, 0);

    s_voice_arc = lv_arc_create(screen);
    lv_obj_set_pos(s_voice_arc, 87, 70);
    lv_obj_set_size(s_voice_arc, 146, 146);
    lv_arc_set_bg_angles(s_voice_arc, 0, 360);
    lv_arc_set_range(s_voice_arc, 0, 100);
    lv_arc_set_value(s_voice_arc, 27);
    lv_obj_remove_style(s_voice_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_voice_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_voice_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(s_voice_arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_voice_arc,
                               lv_color_hex(COLOR_OFF),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_voice_arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_voice_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_voice_arc,
                               lv_color_hex(COLOR_PRIMARY),
                               LV_PART_INDICATOR);

    s_orb = lv_obj_create(screen);
    style_panel(s_orb, COLOR_PRIMARY, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_orb, 104, 87);
    lv_obj_set_size(s_orb, 112, 112);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(s_orb, 0, 0);
    lv_obj_add_flag(s_orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_orb, primary_clicked, LV_EVENT_CLICKED, NULL);
    s_orb_icon = make_label(s_orb,
                            LV_SYMBOL_AUDIO,
                            &lv_font_montserrat_24,
                            COLOR_TEXT);
    lv_obj_center(s_orb_icon);

    s_orb_live_dot = lv_obj_create(s_orb);
    style_panel(s_orb_live_dot, COLOR_WARNING, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_orb_live_dot, 82, 82);
    lv_obj_set_size(s_orb_live_dot, 16, 16);
    lv_obj_set_style_border_width(s_orb_live_dot, 3, 0);
    lv_obj_set_style_border_color(s_orb_live_dot,
                                  lv_color_hex(COLOR_TEXT),
                                  0);

    for (unsigned index = 0; index < 7; ++index) {
        s_wave_bars[index] = lv_obj_create(screen);
        style_panel(s_wave_bars[index], COLOR_PRIMARY, LV_RADIUS_CIRCLE);
        lv_obj_set_pos(s_wave_bars[index], 128 + (int)index * 10, 230);
        lv_obj_set_size(s_wave_bars[index], 5, 7);
    }

    s_main_title = make_label(screen,
                              "Meet JUFF",
                              &lv_font_montserrat_24,
                              COLOR_TEXT);
    lv_obj_set_width(s_main_title, 288);
    lv_obj_set_style_text_align(s_main_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_main_title, 16, 249);

    s_main_detail = make_label(screen,
                               "Open the JUFF service on your Mac to begin",
                               &lv_font_montserrat_14,
                               COLOR_MUTED);
    lv_label_set_long_mode(s_main_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_main_detail, 286);
    lv_obj_set_style_text_align(s_main_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_main_detail, 17, 284);

    s_qwen_chip = create_status_chip(screen,
                                     42,
                                     "QWEN OFFLINE",
                                     &s_qwen_dot,
                                     &s_qwen_label);
    s_mic_chip = create_status_chip(screen,
                                    166,
                                    "MIC READY",
                                    &s_mic_dot,
                                    &s_mic_label);

    s_primary_button = create_button(screen,
                                     16,
                                     376,
                                     208,
                                     62,
                                     "",
                                     COLOR_SURFACE_RAISED,
                                     primary_clicked);
    s_primary_button_label = make_label(s_primary_button,
                                        LV_SYMBOL_AUDIO "  JUST START TALKING",
                                        &lv_font_montserrat_14,
                                        COLOR_TEXT);
    lv_obj_center(s_primary_button_label);

    lv_obj_t *brightness = create_button(screen,
                                         236,
                                         376,
                                         68,
                                         62,
                                         "",
                                         COLOR_SURFACE_RAISED,
                                         brightness_clicked);
    s_brightness_label = make_label(brightness,
                                    LV_SYMBOL_EYE_OPEN "\n100%",
                                    &lv_font_montserrat_12,
                                    COLOR_MUTED);
    lv_obj_set_style_text_align(s_brightness_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_brightness_label);

    lv_obj_t *footer = make_label(screen,
                                  "PRIVATE BLE AUDIO  |  JUFF 0.5",
                                  &lv_font_montserrat_12,
                                  COLOR_OFF);
    lv_obj_set_width(footer, 288);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(footer, 16, 455);

    s_connection_overlay = lv_obj_create(screen);
    style_panel(s_connection_overlay, 0x000000, 0);
    lv_obj_set_pos(s_connection_overlay, 0, 0);
    lv_obj_set_size(s_connection_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(s_connection_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(s_connection_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *sheet = lv_obj_create(s_connection_overlay);
    style_panel(sheet, COLOR_SURFACE_RAISED, 26);
    lv_obj_set_pos(sheet, 12, 58);
    lv_obj_set_size(sheet, 296, 378);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_border_opa(sheet, LV_OPA_30, 0);

    lv_obj_t *sheet_eyebrow = make_label(sheet,
                                         LV_SYMBOL_BLUETOOTH "  CONNECTIONS",
                                         &lv_font_montserrat_12,
                                         COLOR_BLUE);
    lv_obj_set_pos(sheet_eyebrow, 18, 18);

    create_button(sheet,
                  244,
                  10,
                  38,
                  38,
                  "X",
                  COLOR_SURFACE,
                  connection_close_clicked);

    s_connection_device_label = make_label(sheet,
                                            "JUFF",
                                            &lv_font_montserrat_20,
                                            COLOR_TEXT);
    lv_obj_set_pos(s_connection_device_label, 18, 52);

    s_connection_status_label = make_label(sheet,
                                            "READY TO CONNECT",
                                            &lv_font_montserrat_12,
                                            COLOR_BLUE);
    lv_obj_set_pos(s_connection_status_label, 18, 84);

    lv_obj_t *divider = lv_obj_create(sheet);
    style_panel(divider, COLOR_OFF, 1);
    lv_obj_set_pos(divider, 18, 111);
    lv_obj_set_size(divider, 260, 1);
    lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);

    lv_obj_t *pair_title = make_label(sheet,
                                      "Pair another Mac",
                                      &lv_font_montserrat_16,
                                      COLOR_TEXT);
    lv_obj_set_pos(pair_title, 18, 131);

    s_connection_detail_label = make_label(
        sheet,
        "Tap below, then open JUFF on the Mac you want to use.",
        &lv_font_montserrat_14,
        COLOR_MUTED);
    lv_label_set_long_mode(s_connection_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_connection_detail_label, 260);
    lv_obj_set_pos(s_connection_detail_label, 18, 165);

    lv_obj_t *privacy_note = lv_obj_create(sheet);
    style_panel(privacy_note, COLOR_SURFACE, 14);
    lv_obj_set_pos(privacy_note, 18, 226);
    lv_obj_set_size(privacy_note, 260, 48);
    lv_obj_set_style_bg_opa(privacy_note, (lv_opa_t)180, 0);
    lv_obj_t *privacy_label = make_label(
        privacy_note,
        LV_SYMBOL_OK "  Old Mac stays remembered\n     One active Mac at a time",
        &lv_font_montserrat_12,
        COLOR_MUTED);
    lv_obj_set_pos(privacy_label, 12, 9);

    s_pairing_button = create_button(sheet,
                                     18,
                                     292,
                                     260,
                                     56,
                                     "",
                                     COLOR_PRIMARY_DEEP,
                                     pairing_clicked);
    s_pairing_button_label = make_label(s_pairing_button,
                                        "+  PAIR A NEW MAC",
                                        &lv_font_montserrat_14,
                                        COLOR_TEXT);
    lv_obj_center(s_pairing_button_label);

    refresh_status_ui();
}

static void lvgl_tick(void *argument)
{
    (void)argument;
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *argument)
{
    (void)argument;
    uint64_t next_status_refresh = 0;
    bool first_frame_reported = false;
    while (true) {
        if (!first_frame_reported && s_completed_flushes > 0) {
            first_frame_reported = true;
            ESP_LOGI(TAG,
                     "First LVGL frame transferred to the ST7796 panel");
        }
        const uint64_t now = uptime_ms();
        if (now >= next_status_refresh) {
            refresh_status_ui();
            next_status_refresh = now + 120;
        }
        uint32_t delay = lv_timer_handler();
        if (delay < 2) {
            delay = 2;
        } else if (delay > 20) {
            delay = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

esp_err_t board_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(initialize_backlight(), TAG, "initialize backlight");

    lv_init();
    ESP_RETURN_ON_ERROR(initialize_lcd(), TAG, "initialize LCD");
    ESP_RETURN_ON_ERROR(set_brightness(s_brightness), TAG, "enable backlight");
    ESP_RETURN_ON_ERROR(lcd_fill_solid(0x047F), TAG, "draw LCD startup color");
    ESP_LOGI(TAG, "High-visibility LCD startup frame transferred");

    const size_t pixel_count = LCD_WIDTH * LCD_DRAW_ROWS;
    lv_color_t *first = heap_caps_malloc(pixel_count * sizeof(*first),
                                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *second = heap_caps_malloc(pixel_count * sizeof(*second),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buffer,
                          first,
                          second,
                          pixel_count);
    lv_disp_drv_init(&s_display_driver);
    s_display_driver.hor_res = LCD_WIDTH;
    s_display_driver.ver_res = LCD_HEIGHT;
    s_display_driver.flush_cb = lcd_flush;
    s_display_driver.draw_buf = &s_draw_buffer;
    lv_disp_t *display = lv_disp_drv_register(&s_display_driver);
    if (display == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t touch_error = initialize_touch(display);
    if (touch_error != ESP_OK) {
        ESP_LOGW(TAG,
                 "Display is active but touch initialization failed: %s",
                 esp_err_to_name(touch_error));
    }
    create_ui();

    const esp_timer_create_args_t tick_config = {
        .callback = lvgl_tick,
        .name = "juff_lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_config, &s_tick_timer),
                        TAG,
                        "create LVGL tick");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer,
                                                 LVGL_TICK_MS * 1000),
                        TAG,
                        "start LVGL tick");
    if (xTaskCreatePinnedToCore(lvgl_task,
                                "juff_lvgl",
                                8192,
                                NULL,
                                3,
                                NULL,
                                1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_LOGI(TAG, "JUFF product UI ready at 320x480");
    return ESP_OK;
}

bool board_display_is_ready(void)
{
    return s_ready;
}

void board_display_set_voice_state(const char *state)
{
    voice_visual_state_t next = VOICE_VISUAL_IDLE;
    if (state != NULL) {
        if (strcmp(state, "listening") == 0) {
            next = VOICE_VISUAL_LISTENING;
        } else if (strcmp(state, "processing") == 0) {
            next = VOICE_VISUAL_PROCESSING;
        } else if (strcmp(state, "speaking") == 0) {
            next = VOICE_VISUAL_SPEAKING;
        }
    }
    if (s_state_mutex == NULL
        || xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_voice_visual_state = next;
    xSemaphoreGive(s_state_mutex);
}

void board_display_set_notice(const char *title,
                              const char *detail,
                              uint32_t duration_ms)
{
    if (s_state_mutex == NULL
        || xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (title == NULL || title[0] == '\0') {
        s_notice_title[0] = '\0';
        s_notice_detail[0] = '\0';
        s_notice_until_ms = 0;
    } else {
        strlcpy(s_notice_title, title, sizeof(s_notice_title));
        strlcpy(s_notice_detail,
                detail == NULL ? "" : detail,
                sizeof(s_notice_detail));
        s_notice_until_ms = duration_ms == 0
            ? UINT64_MAX
            : uptime_ms() + duration_ms;
    }
    xSemaphoreGive(s_state_mutex);
}

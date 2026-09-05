#include "board_display.h"
#include "board_config.h"
#include "companion_face.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_io.h"
#include "ble_manager.h"
#include "device_client.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "wifi_manager.h"

#define LVGL_TICK_MS 2

#define COLOR_BACKGROUND 0xF3EEDC
#define COLOR_SURFACE 0xE7E5D5
#define COLOR_SURFACE_RAISED 0xD7DBC8
#define COLOR_PRIMARY 0xD3E2BA
#define COLOR_PRIMARY_DEEP 0x63875E
#define COLOR_BLUE 0x547A70
#define COLOR_WARNING 0xB85C45
#define COLOR_DANGER 0xEDC4AF
#define COLOR_TEXT 0x203B32
#define COLOR_MUTED 0x67766C
#define COLOR_OFF 0xC7CEBB

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
static lv_obj_t *s_face;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_main_title;
static lv_obj_t *s_main_detail;
static lv_obj_t *s_primary_button;
static lv_obj_t *s_primary_button_label;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_connection_overlay;
static lv_obj_t *s_connection_device_label;
static lv_obj_t *s_connection_status_label;
static lv_obj_t *s_connection_detail_label;
static lv_obj_t *s_pairing_button;
static lv_obj_t *s_pairing_button_label;

#if !CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
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
#endif

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

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_35
static esp_err_t initialize_st7796(void)
{
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
#endif

static esp_err_t initialize_lcd(void)
{
    ESP_LOGI(TAG,
             "Initializing LCD: %dx%d, MOSI=%d CLK=%d DC=%d BL=%d",
             LCD_WIDTH,
             LCD_HEIGHT,
             LCD_PIN_MOSI,
             LCD_PIN_CLOCK,
             LCD_PIN_DC,
             LCD_PIN_BACKLIGHT);

#if !CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    ESP_RETURN_ON_ERROR(audio_io_set_expander_pin(LCD_RESET_EXPANDER_PIN, false),
                        TAG,
                        "assert LCD reset on TCA9554 EXIO1");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(audio_io_set_expander_pin(LCD_RESET_EXPANDER_PIN, true),
                        TAG,
                        "release LCD reset on TCA9554 EXIO1");
    vTaskDelay(pdMS_TO_TICKS(100));

#endif
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
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = LCD_SPI_MODE,
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
        "attach LCD panel IO");

#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &panel),
                        TAG, "create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "initialize ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "invert ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "enable ST7789");
    ESP_LOGI(TAG, "ST7789 initialization complete");
#else
    ESP_RETURN_ON_ERROR(initialize_st7796(), TAG, "initialize ST7796");
#endif
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

    const uint8_t column[] = {
        0, 0, (uint8_t)((LCD_WIDTH - 1) >> 8), (uint8_t)(LCD_WIDTH - 1),
    };
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
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_RESET,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "configure touch reset");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_PIN_RESET, 0), TAG, "assert touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_PIN_RESET, 1), TAG, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_ADDRESS, 100),
                        TAG,
                        "probe " TOUCH_NAME " touch");
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDRESS,
        .scl_speed_hz = TOUCH_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &s_touch),
                        TAG,
                        "attach " TOUCH_NAME " touch");
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
    // Keep CST816 awake so LVGL polling can read press and release events.
    const uint8_t disable_auto_sleep[] = { 0xFE, 0x01 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_touch, disable_auto_sleep,
                                           sizeof(disable_auto_sleep), 100),
                        TAG, "disable CST816 auto sleep");
#endif
    uint8_t chip_id = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(touch_read(TOUCH_CHIP_ID_REGISTER, &chip_id, 1));

    lv_indev_drv_init(&s_touch_driver);
    s_touch_driver.type = LV_INDEV_TYPE_POINTER;
    s_touch_driver.disp = display;
    s_touch_driver.read_cb = touch_read_callback;
    lv_indev_drv_register(&s_touch_driver);
    ESP_LOGI(TAG, TOUCH_NAME " touch ready at 0x%02x (chip id=0x%02x)",
             TOUCH_ADDRESS, chip_id);
    return ESP_OK;
}

static void style_panel(lv_obj_t *object, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(COLOR_SURFACE_RAISED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(COLOR_OFF),
                              LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
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
    const bool wifi_connected = wifi_manager_is_connected();
    const bool ble_connected = ble_manager_is_connected();
    const bool ble_advertising = ble_manager_is_advertising();
    const bool ble_pairing = ble_manager_is_pairing();
    const bool ble_audio_connected = ble_manager_audio_is_connected();
    const bool ble_voice_active = ble_manager_is_voice_active();
    const bool bridge_connected = ble_audio_connected || device_client_is_connected();
    const bool voice_ready = ble_voice_active || device_client_is_voice_active();
    const bool playing = audio_io_is_playing();
    const bool audio_ready = audio_io_is_available();
    const bool voice_interrupt_allowed = audio_io_supports_voice_barge_in()
        && (ble_voice_active ? ble_manager_allows_voice_interrupt()
                             : device_client_allows_voice_interrupt());
    const bool mic_ready = voice_interrupt_allowed || (!playing && (ble_voice_active
        ? ble_manager_is_voice_ready() : device_client_is_ready()));

    char notice_title[40];
    char notice_detail[96];
    bool test_running = false;
    voice_visual_state_t voice_state = VOICE_VISUAL_IDLE;
    snapshot_ui_state(notice_title, notice_detail, &test_running, &voice_state);
    if (playing) voice_state = VOICE_VISUAL_SPEAKING;
    else if (!voice_ready) voice_state = VOICE_VISUAL_IDLE;

    const char *title = "hey, you.";
    const char *detail = "say something. I'm here.";
    companion_mood_t mood = COMPANION_MOOD_READY;
    if (!audio_ready) {
        mood = COMPANION_MOOD_ALERT;
        title = "a little hiccup.";
        detail = "check audio on your Mac";
    } else if (!voice_ready) {
        mood = bridge_connected || ble_connected ? COMPANION_MOOD_THINKING
                                                : COMPANION_MOOD_DOZING;
        title = bridge_connected ? "waking up..." : "let's connect.";
        detail = bridge_connected ? "just a little moment" : "open JUFF on your Mac";
    } else if (voice_state == VOICE_VISUAL_SPEAKING) {
        mood = COMPANION_MOOD_SPEAKING;
        title = "here's a thought.";
        detail = voice_interrupt_allowed ? "speak to interrupt" : "tap my face to stop";
    } else if (voice_state == VOICE_VISUAL_PROCESSING) {
        mood = COMPANION_MOOD_THINKING;
        title = "hmm...";
        detail = "give me a little moment";
    } else if (!mic_ready) {
        mood = COMPANION_MOOD_MUTED;
        title = "a quiet moment.";
        detail = "microphone is paused";
    } else if (voice_state == VOICE_VISUAL_LISTENING) {
        mood = COMPANION_MOOD_LISTENING;
        title = "all ears.";
        detail = "go on. I'm listening.";
    }
    if (test_running) mood = COMPANION_MOOD_ALERT;
    if (notice_title[0] != '\0') {
        title = notice_title;
        detail = notice_detail;
    }

    // A notice can change the caption, but must never hide a live stop action.
    const bool can_interrupt = voice_ready && (playing
        || voice_state == VOICE_VISUAL_LISTENING
        || voice_state == VOICE_VISUAL_PROCESSING
        || voice_state == VOICE_VISUAL_SPEAKING);
    const char *action = can_interrupt
        ? (voice_state == VOICE_VISUAL_PROCESSING ? LV_SYMBOL_CLOSE "  cancel"
                                                 : LV_SYMBOL_STOP "  stop")
        : LV_SYMBOL_BLUETOOTH "  connection";
    set_label_text(s_main_title, title);
    set_label_text(s_main_detail, detail);
    set_label_text(s_primary_button_label, action);
    lv_obj_set_style_bg_color(s_primary_button,
                              lv_color_hex(can_interrupt ? COLOR_DANGER : COLOR_PRIMARY), 0);
    companion_face_set(s_face, mood, s_animation_phase);

    uint32_t connection_color = COLOR_MUTED;
    const char *connection_icon = LV_SYMBOL_BLUETOOTH;
    if (ble_pairing) connection_color = COLOR_WARNING;
    else if (voice_ready) connection_color = COLOR_TEXT;
    else if (wifi_connected) {
        connection_color = COLOR_TEXT;
        connection_icon = LV_SYMBOL_WIFI;
    } else if (ble_connected || ble_advertising) connection_color = COLOR_BLUE;
    set_label_text(s_connection_label, connection_icon);
    lv_obj_set_style_text_color(s_connection_label, lv_color_hex(connection_color), 0);
    lv_obj_set_style_bg_color(s_status_dot,
                              lv_color_hex(!audio_ready ? COLOR_WARNING
                                           : (voice_ready ? COLOR_PRIMARY_DEEP : COLOR_OFF)), 0);
    lv_label_set_text_fmt(s_brightness_label, LV_SYMBOL_EYE_OPEN " %u", s_brightness);

    if (s_connection_device_label != NULL) {
        set_label_text(s_connection_device_label, ble_manager_device_name());
        const bool showing_passkey = ble_pairing
            && strcmp(notice_title, "Pair with JUFF") == 0;
        lv_obj_set_style_text_font(s_connection_detail_label,
                                  showing_passkey ? &lv_font_montserrat_24
                                      : (UI_COMPACT ? &lv_font_montserrat_14
                                                    : &lv_font_montserrat_16), 0);
        lv_obj_set_style_text_align(s_connection_detail_label,
                                   showing_passkey ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        if (ble_pairing) {
            const uint32_t seconds = ble_manager_pairing_seconds_remaining();
            char button_text[48];
            snprintf(button_text, sizeof(button_text), LV_SYMBOL_CLOSE "  cancel  %u:%02u",
                     (unsigned)(seconds / 60U), (unsigned)(seconds % 60U));
            set_label_text(s_pairing_button_label, button_text);
            set_label_text(s_connection_status_label, "LOOKING FOR A MAC");
            set_label_text(s_connection_detail_label, showing_passkey ? notice_detail
                : "Open JUFF on your Mac. Your pairing code will appear here.");
            lv_obj_set_style_text_color(s_connection_status_label, lv_color_hex(COLOR_WARNING), 0);
            lv_obj_set_style_bg_color(s_pairing_button, lv_color_hex(COLOR_DANGER), 0);
        } else {
            set_label_text(s_pairing_button_label, "+  pair a new Mac");
            set_label_text(s_connection_status_label, ble_connected ? "MAC CONNECTED" : "READY TO CONNECT");
            set_label_text(s_connection_detail_label, ble_connected
                ? "Together at last. You can pair another Mac below."
                : "Tap below, then open JUFF on the Mac you want to use.");
            lv_obj_set_style_text_color(s_connection_status_label,
                                       lv_color_hex(ble_connected ? COLOR_PRIMARY_DEEP : COLOR_BLUE), 0);
            lv_obj_set_style_bg_color(s_pairing_button, lv_color_hex(COLOR_PRIMARY), 0);
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
    const bool ble_voice_active = ble_manager_is_voice_active();
    const bool voice_ready = ble_voice_active || device_client_is_voice_active();
    const bool can_interrupt = audio_io_is_playing()
        || voice_state == VOICE_VISUAL_LISTENING
        || voice_state == VOICE_VISUAL_PROCESSING
        || voice_state == VOICE_VISUAL_SPEAKING;

    if (voice_ready && can_interrupt) {
        ESP_LOGI(TAG, "Touch action: interrupt active voice turn");
        if (ble_voice_active) {
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
    if (text[0] != '\0') {
        lv_obj_t *label = make_label(button,
                                     text,
                                     &lv_font_montserrat_14,
                                     COLOR_TEXT);
        lv_obj_center(label);
    }
    return button;
}

static void place(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static void main_action_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    voice_visual_state_t state = VOICE_VISUAL_IDLE;
    char title[40], detail[96];
    bool testing;
    snapshot_ui_state(title, detail, &testing, &state);
    const bool active = ble_manager_is_voice_active() || device_client_is_voice_active();
    if (active && (audio_io_is_playing() || state == VOICE_VISUAL_LISTENING
                  || state == VOICE_VISUAL_PROCESSING || state == VOICE_VISUAL_SPEAKING)) {
        primary_clicked(event);
    } else {
        connection_pill_clicked(event);
    }
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const int margin = UI_COMPACT ? 14 : 24;
    const int header_y = UI_COMPACT ? 9 : 21;
    lv_obj_t *brand = make_label(screen, "juff.", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_pos(brand, margin + 12, header_y + 7);
    s_status_dot = lv_obj_create(screen);
    style_panel(s_status_dot, COLOR_PRIMARY_DEEP, LV_RADIUS_CIRCLE);
    place(s_status_dot, margin, header_y + 14, 5, 5);

    s_connection_pill = create_button(screen, LCD_WIDTH - margin - 40, header_y,
                                      40, 34, "", COLOR_SURFACE, connection_pill_clicked);
    s_connection_label = make_label(s_connection_pill, LV_SYMBOL_BLUETOOTH,
                                    &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_center(s_connection_label);

    s_face = companion_face_create(screen);
    place(s_face, UI_COMPACT ? 10 : 20, UI_COMPACT ? 44 : 82,
          LCD_WIDTH - (UI_COMPACT ? 20 : 40), UI_COMPACT ? 106 : 206);
    lv_obj_add_flag(s_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_face, primary_clicked, LV_EVENT_CLICKED, NULL);

    s_main_title = make_label(screen, "hey, you.",
                              UI_COMPACT ? &lv_font_montserrat_20 : &lv_font_montserrat_24,
                              COLOR_TEXT);
    place(s_main_title, margin, UI_COMPACT ? 150 : 300, LCD_WIDTH - 2 * margin,
          UI_COMPACT ? 25 : 32);
    lv_obj_set_style_text_align(s_main_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_main_title, LV_LABEL_LONG_DOT);
    s_main_detail = make_label(screen, "say something. I'm here.",
                               UI_COMPACT ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                               COLOR_MUTED);
    place(s_main_detail, margin, UI_COMPACT ? 179 : 339, LCD_WIDTH - 2 * margin,
          UI_COMPACT ? 16 : 40);
    lv_obj_set_style_text_align(s_main_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_main_detail, LV_LABEL_LONG_DOT);

    const int controls_y = UI_COMPACT ? 201 : 403;
    const int control_height = UI_COMPACT ? 32 : 46;
    const int brightness_width = UI_COMPACT ? 58 : 68;
    s_primary_button = create_button(screen, margin, controls_y,
                                      LCD_WIDTH - 3 * margin - brightness_width,
                                      control_height, "", COLOR_PRIMARY, main_action_clicked);
    s_primary_button_label = make_label(s_primary_button, LV_SYMBOL_BLUETOOTH "  connection",
                                        &lv_font_montserrat_12, COLOR_TEXT);
    lv_obj_center(s_primary_button_label);
    lv_obj_t *brightness = create_button(screen, LCD_WIDTH - margin - brightness_width,
                                         controls_y, brightness_width, control_height, "",
                                         COLOR_SURFACE, brightness_clicked);
    s_brightness_label = make_label(brightness, LV_SYMBOL_EYE_OPEN " 100",
                                    &lv_font_montserrat_12, COLOR_TEXT);
    lv_obj_center(s_brightness_label);
    if (!UI_COMPACT) {
        lv_obj_t *footer = make_label(screen, "a little presence.", &lv_font_montserrat_12, COLOR_MUTED);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -12);
    }

    s_connection_overlay = lv_obj_create(screen);
    style_panel(s_connection_overlay, COLOR_TEXT, 0);
    place(s_connection_overlay, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(s_connection_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_connection_overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sheet = lv_obj_create(s_connection_overlay);
    style_panel(sheet, COLOR_BACKGROUND, UI_COMPACT ? 18 : 24);
    place(sheet, UI_COMPACT ? 6 : 16, UI_COMPACT ? 6 : 76,
          UI_COMPACT ? 228 : 288, UI_COMPACT ? 228 : 328);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    const int inner = UI_COMPACT ? 12 : 20;
    const int content_width = UI_COMPACT ? 204 : 248;
    lv_obj_t *heading = make_label(sheet, "a little connection.", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_pos(heading, inner, UI_COMPACT ? 16 : 22);
    create_button(sheet, UI_COMPACT ? 182 : 232, UI_COMPACT ? 6 : 12,
                  36, 36, LV_SYMBOL_CLOSE, COLOR_SURFACE, connection_close_clicked);

    s_connection_device_label = make_label(sheet, "JUFF", &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_set_pos(s_connection_device_label, inner, UI_COMPACT ? 53 : 72);
    s_connection_status_label = make_label(sheet, "READY TO CONNECT", &lv_font_montserrat_12, COLOR_BLUE);
    lv_obj_set_pos(s_connection_status_label, inner, UI_COMPACT ? 80 : 105);
    lv_obj_t *divider = lv_obj_create(sheet);
    style_panel(divider, COLOR_OFF, 0);
    place(divider, inner, UI_COMPACT ? 104 : 138, content_width, 1);
    s_connection_detail_label = make_label(sheet, "Open JUFF on your Mac.",
                                            UI_COMPACT ? &lv_font_montserrat_14 : &lv_font_montserrat_16,
                                            COLOR_MUTED);
    place(s_connection_detail_label, inner, UI_COMPACT ? 118 : 155,
          content_width, UI_COMPACT ? 52 : 74);
    lv_label_set_long_mode(s_connection_detail_label, LV_LABEL_LONG_WRAP);
    if (!UI_COMPACT) {
        lv_obj_t *note = make_label(sheet, "Your other Mac stays remembered.",
                                    &lv_font_montserrat_12, COLOR_MUTED);
        lv_obj_set_pos(note, inner, 242);
    }
    s_pairing_button = create_button(sheet, inner, UI_COMPACT ? 181 : 270,
                                     content_width, 36, "", COLOR_PRIMARY, pairing_clicked);
    s_pairing_button_label = make_label(s_pairing_button, "+  pair a new Mac",
                                        &lv_font_montserrat_12, COLOR_TEXT);
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
                     "First LVGL frame transferred to the LCD panel");
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
    ESP_RETURN_ON_ERROR(lcd_fill_solid(0xF77B), TAG, "draw LCD startup color");
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
    ESP_LOGI(TAG, "JUFF product UI ready at %dx%d", LCD_WIDTH, LCD_HEIGHT);
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

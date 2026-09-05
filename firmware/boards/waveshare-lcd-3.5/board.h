#pragma once

// Waveshare ESP32-S3-Touch-LCD-3.5 (non-B) wiring.
#define JUFF_BOARD_ID "waveshare-lcd-3.5"
#define JUFF_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-3.5"
#define BOARD_HAS_ES7210 0
#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 320
#define LCD_HEIGHT 480
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define LCD_SPI_MODE 0
#define LCD_PIN_MOSI 1
#define LCD_PIN_CLOCK 5
#define LCD_PIN_DC 3
#define LCD_PIN_CS -1
#define LCD_PIN_BACKLIGHT 6
#define LCD_RESET_EXPANDER_PIN 1
#define TOUCH_ADDRESS 0x38
#define TOUCH_NAME "FT6336"
#define TOUCH_CHIP_ID_REGISTER 0xA3
#define TOUCH_I2C_SPEED_HZ 400000

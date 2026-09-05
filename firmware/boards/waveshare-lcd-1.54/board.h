#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.54 wiring.
#define JUFF_BOARD_ID "waveshare-lcd-1.54"
#define JUFF_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-1.54"
#define BOARD_HAS_ES7210 1
#define LCD_HOST SPI3_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_SPI_MODE 3
#define LCD_PIN_MOSI 39
#define LCD_PIN_CLOCK 38
#define LCD_PIN_DC 45
#define LCD_PIN_CS 21
#define LCD_PIN_RESET 40
#define LCD_PIN_BACKLIGHT 46
#define TOUCH_ADDRESS 0x15
#define TOUCH_NAME "CST816"
#define TOUCH_PIN_RESET 47
#define TOUCH_CHIP_ID_REGISTER 0xA7
#define TOUCH_I2C_SPEED_HZ 100000

#pragma once

#include "sdkconfig.h"

// Board wiring from Waveshare's schematics. These profiles also select the
// controller initialization and UI; screen dimensions alone are insufficient.
#if CONFIG_JUFF_BOARD_WAVESHARE_LCD_154
#include "../boards/waveshare-lcd-1.54/board.h"
#elif CONFIG_JUFF_BOARD_WAVESHARE_LCD_35
#include "../boards/waveshare-lcd-3.5/board.h"
#else
#error "Select a supported JUFF board profile"
#endif

#define LCD_DRAW_ROWS 24
#define UI_COMPACT (LCD_HEIGHT <= 240)

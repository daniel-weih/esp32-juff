#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POWER_BUTTON_DEBOUNCE_MS 50U
#define POWER_BUTTON_HOLD_MS 3000U

typedef enum {
    POWER_BUTTON_NONE,
    POWER_BUTTON_PREPARE,
    POWER_BUTTON_OFF,
} power_button_event_t;

typedef struct {
    bool initialized;
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool requested;
    bool finished;
    uint32_t changed_at;
    uint32_t pressed_at;
} power_button_t;

// Start zeroed. A released key must first be observed so the press used to
// power on (or wake) the board can never immediately shut it down again.
static inline power_button_event_t power_button_update(power_button_t *button,
                                                       bool pressed,
                                                       uint32_t now_ms)
{
    if (button->finished) {
        return POWER_BUTTON_NONE;
    }
    if (!button->initialized) {
        button->initialized = true;
        button->raw_pressed = pressed;
        button->changed_at = now_ms;
        return POWER_BUTTON_NONE;
    }
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed_at = now_ms;
    }
    if (now_ms - button->changed_at < POWER_BUTTON_DEBOUNCE_MS) {
        return POWER_BUTTON_NONE;
    }
    if (pressed != button->stable_pressed) {
        button->stable_pressed = pressed;
        if (pressed) {
            button->pressed_at = now_ms;
        }
    }
    if (!button->stable_pressed) {
        if (button->requested) {
            button->finished = true;
            return POWER_BUTTON_OFF;
        }
        button->armed = true;
    } else if (button->armed && !button->requested &&
               now_ms - button->pressed_at >= POWER_BUTTON_HOLD_MS) {
        button->requested = true;
        return POWER_BUTTON_PREPARE;
    }
    return POWER_BUTTON_NONE;
}

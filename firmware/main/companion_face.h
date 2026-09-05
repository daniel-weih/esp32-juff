#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMPANION_MOOD_DOZING,
    COMPANION_MOOD_READY,
    COMPANION_MOOD_LISTENING,
    COMPANION_MOOD_THINKING,
    COMPANION_MOOD_SPEAKING,
    COMPANION_MOOD_MUTED,
    COMPANION_MOOD_ALERT,
} companion_mood_t;

// Transparent, childless LVGL object. The caller owns its size and click action.
lv_obj_t *companion_face_create(lv_obj_t *parent);

// Call from the LVGL task. Phase advances once per 120 ms; unchanged poses do
// not invalidate the object. All drawing uses its current size and position.
void companion_face_set(lv_obj_t *object, companion_mood_t mood, uint32_t phase);

#ifdef __cplusplus
}
#endif

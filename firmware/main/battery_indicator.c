#include "battery_indicator.h"

#include <stdio.h>

#define BATTERY_INK 0x203B32
#define BATTERY_TRACK 0xD7DBC8
#define BATTERY_MUTED 0x879485
#define BATTERY_CHARGING 0x34794F
#define BATTERY_LOW 0xBB4438
#define BATTERY_PAPER 0xF3EEDC

typedef struct {
    lv_obj_t object;
    bool valid;
    bool charging;
    uint8_t percent;
} battery_indicator_t;

static void battery_event(const lv_obj_class_t *class_p, lv_event_t *event);

static const lv_obj_class_t battery_indicator_class = {
    .base_class = &lv_obj_class,
    .event_cb = battery_event,
    .width_def = 48,
    .height_def = 22,
    .instance_size = sizeof(battery_indicator_t),
};

static void draw_battery(lv_obj_t *object, lv_draw_ctx_t *context)
{
    const battery_indicator_t *battery = (const battery_indicator_t *)object;
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    const lv_opa_t opacity = lv_obj_get_style_opa_recursive(object, LV_PART_MAIN);
    const uint32_t ink = battery->charging ? BATTERY_CHARGING
        : (battery->valid && battery->percent <= 20 ? BATTERY_LOW : BATTERY_INK);
    const lv_area_t body = {bounds.x1 + 10, bounds.y1 + 2,
                            bounds.x1 + 43, bounds.y1 + 19};
    const lv_area_t tip = {bounds.x1 + 45, bounds.y1 + 7,
                           bounds.x1 + 47, bounds.y1 + 14};
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = 4;
    rect.bg_color = lv_color_hex(BATTERY_TRACK);
    rect.bg_opa = opacity;
    lv_draw_rect(context, &rect, &body);
    rect.radius = 1;
    rect.bg_color = lv_color_hex(battery->valid ? ink : BATTERY_MUTED);
    lv_draw_rect(context, &rect, &tip);

    lv_area_t fill = body;
    const int width = battery->valid ? (34 * battery->percent + 50) / 100 : 0;
    fill.x2 = fill.x1 + width - 1;
    if (width > 0) {
        // Clip a full rounded battery shape, keeping the far fill edge flat.
        const lv_area_t *saved_clip = context->clip_area;
        lv_area_t clipped;
        if (_lv_area_intersect(&clipped, saved_clip, &fill)) {
            context->clip_area = &clipped;
            rect.radius = 4;
            lv_draw_rect(context, &rect, &body);
            context->clip_area = saved_clip;
        }
    }

    char number[4];
    const char *caption = "--";
    if (battery->valid) {
        snprintf(number, sizeof(number), "%u", battery->percent);
        caption = number;
    }
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &lv_font_montserrat_12;
    label.color = lv_color_hex(battery->valid ? ink : BATTERY_MUTED);
    label.opa = opacity;
    label.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t text = body;
    text.y1 += (18 - lv_font_get_line_height(label.font)) / 2;
    lv_draw_label(context, &label, &text, caption, NULL);

    // Invert the text only over the filled portion so every percentage stays
    // readable, including when the fill edge passes through the digits.
    if (width > 0) {
        const lv_area_t *saved_clip = context->clip_area;
        lv_area_t clipped;
        if (_lv_area_intersect(&clipped, saved_clip, &fill)) {
            context->clip_area = &clipped;
            label.color = lv_color_hex(BATTERY_PAPER);
            lv_draw_label(context, &label, &text, caption, NULL);
            context->clip_area = saved_clip;
        }
    }
    if (battery->charging) {
        const lv_area_t bolt = {bounds.x1, bounds.y1 + 4,
                                bounds.x1 + 7, bounds.y1 + 19};
        label.color = lv_color_hex(BATTERY_CHARGING);
        lv_draw_label(context, &label, &bolt, LV_SYMBOL_CHARGE, NULL);
    }
}

static void battery_event(const lv_obj_class_t *class_p, lv_event_t *event)
{
    LV_UNUSED(class_p);
    if (lv_obj_event_base(&battery_indicator_class, event) != LV_RES_OK) return;
    if (lv_event_get_code(event) == LV_EVENT_DRAW_MAIN) {
        draw_battery(lv_event_get_target(event), lv_event_get_draw_ctx(event));
    }
}

lv_obj_t *battery_indicator_create(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_class_create_obj(&battery_indicator_class, parent);
    if (object == NULL) return NULL;
    lv_obj_class_init_obj(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    return object;
}

void battery_indicator_set(lv_obj_t *object, bool valid, int percent, bool charging)
{
    if (object == NULL) return;
    LV_ASSERT_OBJ(object, &battery_indicator_class);
    battery_indicator_t *battery = (battery_indicator_t *)object;
    // A published percentage stays visible during charging and estimator
    // transitions. An unavailable estimate must not get clamped to "0".
    const bool numeric = valid && percent >= 0;
    const uint8_t level = numeric ? LV_CLAMP(0, percent, 100) : 0;
    if (battery->valid == numeric && battery->percent == level
        && battery->charging == charging) return;
    battery->valid = numeric;
    battery->percent = level;
    battery->charging = charging;
    lv_obj_invalidate(object);
}

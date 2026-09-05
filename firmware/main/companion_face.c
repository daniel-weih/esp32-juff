#include "companion_face.h"

#define FACE_WIDTH 220
#define FACE_HEIGHT 120
#define FACE_INK 0x203B32
#define FACE_ACCENT 0xD77459
#define SCALE_ONE 256

typedef struct {
    lv_obj_t object;
    companion_mood_t mood;
    int8_t gaze;
    int8_t bob;
    uint8_t blink;
    uint8_t movement;
} companion_face_t;

typedef struct {
    lv_draw_ctx_t *context;
    lv_coord_t left;
    lv_coord_t top;
    int32_t scale;
    lv_opa_t opacity;
} face_canvas_t;

static void face_event(const lv_obj_class_t *class_p, lv_event_t *event);

static const lv_obj_class_t companion_face_class = {
    .base_class = &lv_obj_class,
    .event_cb = face_event,
    .width_def = FACE_WIDTH,
    .height_def = FACE_HEIGHT,
    .instance_size = sizeof(companion_face_t),
};

static lv_coord_t scaled(const face_canvas_t *canvas, int value)
{
    const int32_t amount = value * canvas->scale;
    return (lv_coord_t)(amount >= 0
        ? (amount + SCALE_ONE / 2) / SCALE_ONE
        : -((-amount + SCALE_ONE / 2) / SCALE_ONE));
}

static lv_point_t point(const face_canvas_t *canvas, int x, int y)
{
    const lv_point_t result = {
        .x = canvas->left + scaled(canvas, x),
        .y = canvas->top + scaled(canvas, y),
    };
    return result;
}

static void stroke(const face_canvas_t *canvas, int x1, int y1, int x2, int y2,
                    int width, bool accent)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(accent ? FACE_ACCENT : FACE_INK);
    line.width = LV_MAX(1, scaled(canvas, width));
    line.opa = canvas->opacity;
    line.round_start = true;
    line.round_end = true;
    const lv_point_t start = point(canvas, x1, y1);
    const lv_point_t end = point(canvas, x2, y2);
    lv_draw_line(canvas->context, &line, &start, &end);
}

static void capsule(const face_canvas_t *canvas, int x, int y, int width,
                     int height, bool hollow)
{
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = LV_RADIUS_CIRCLE;
    rect.bg_color = lv_color_hex(FACE_INK);
    rect.bg_opa = hollow ? LV_OPA_TRANSP : canvas->opacity;
    rect.border_color = lv_color_hex(FACE_INK);
    rect.border_opa = canvas->opacity;
    rect.border_width = hollow ? LV_MAX(1, scaled(canvas, 4)) : 0;
    const lv_point_t start = point(canvas, x, y);
    const lv_area_t area = {
        .x1 = start.x,
        .y1 = start.y,
        .x2 = start.x + LV_MAX(1, scaled(canvas, width)) - 1,
        .y2 = start.y + LV_MAX(1, scaled(canvas, height)) - 1,
    };
    lv_draw_rect(canvas->context, &rect, &area);
}

static void spark(const face_canvas_t *canvas, int x, int y, int radius)
{
    // Three strokes, six tips: a small typographic asterisk, not a face outline.
    stroke(canvas, x, y - radius, x, y + radius, 3, true);
    stroke(canvas, x - radius, y - radius / 2,
           x + radius, y + radius / 2, 3, true);
    stroke(canvas, x - radius, y + radius / 2,
           x + radius, y - radius / 2, 3, true);
}

static void angle_eyes(const face_canvas_t *canvas, bool blink)
{
    if (blink) {
        stroke(canvas, 34, 57, 58, 57, 7, false);
        stroke(canvas, 164, 54, 186, 54, 7, false);
        return;
    }
    stroke(canvas, 34, 41, 57, 55, 7, false);
    stroke(canvas, 57, 55, 34, 69, 7, false);
    stroke(canvas, 186, 43, 164, 55, 7, false);
    stroke(canvas, 164, 55, 186, 66, 7, false);
}

static void draw_face(lv_obj_t *object, lv_draw_ctx_t *context)
{
    const companion_face_t *face = (const companion_face_t *)object;
    lv_area_t bounds;
    lv_obj_get_content_coords(object, &bounds);
    const lv_coord_t width = lv_area_get_width(&bounds);
    const lv_coord_t height = lv_area_get_height(&bounds);
    if (width <= 0 || height <= 0) return;
    face_canvas_t canvas = {
        .context = context,
        .scale = LV_MIN((int32_t)width * SCALE_ONE / FACE_WIDTH,
                        (int32_t)height * SCALE_ONE / FACE_HEIGHT),
        .opacity = lv_obj_get_style_opa_recursive(object, LV_PART_MAIN),
    };
    if (canvas.scale == 0 || canvas.opacity <= LV_OPA_MIN) return;
    canvas.left = bounds.x1 + (width - scaled(&canvas, FACE_WIDTH)) / 2
        + scaled(&canvas, face->gaze);
    canvas.top = bounds.y1 + (height - scaled(&canvas, FACE_HEIGHT)) / 2
        + scaled(&canvas, face->bob);

    switch (face->mood) {
    case COMPANION_MOOD_DOZING:
        stroke(&canvas, 33, 60, 59, 60, 7, false);
        stroke(&canvas, 164, 57, 189, 57, 7, false);
        stroke(&canvas, 101, 78, 119, 78, 6, false);
        break;
    case COMPANION_MOOD_LISTENING:
        if (face->blink) {
            stroke(&canvas, 38, 57, 60, 57, 7, false);
            stroke(&canvas, 163, 54, 185, 54, 7, false);
        } else {
            capsule(&canvas, 42, 35, 16, 40, false);
            capsule(&canvas, 165, 32, 16, 43, false);
        }
        stroke(&canvas, 101, 84, 120, 84, 6, false);
        stroke(&canvas, 25, 80, 34, 78, 3, true);
        stroke(&canvas, 187, 77, 196, 79, 3, true);
        break;
    case COMPANION_MOOD_THINKING:
        if (face->blink) {
            stroke(&canvas, 35, 56, 59, 56, 7, false);
            stroke(&canvas, 163, 52, 186, 52, 7, false);
        } else {
            stroke(&canvas, 37, 64, 58, 42, 7, false);
            if (face->movement == 1)
                stroke(&canvas, 174, 39, 174, 63, 7, false);
            else
                stroke(&canvas, 161, 51, 185, 51, 7, false);
        }
        stroke(&canvas, 102, 79, 121, 75, 6, false);
        spark(&canvas, 196, 26, face->movement == 2 ? 6 : 8);
        break;
    case COMPANION_MOOD_SPEAKING: {
        static const uint8_t mouth_height[] = {10, 20, 28, 18};
        static const uint8_t mouth_width[] = {18, 21, 22, 20};
        angle_eyes(&canvas, face->blink);
        const int w = mouth_width[face->movement];
        const int h = mouth_height[face->movement];
        capsule(&canvas, 111 - w / 2, 76 - h / 2, w, h, true);
        stroke(&canvas, 25, 79, 34, 77, 3, true);
        stroke(&canvas, 187, 76, 196, 78, 3, true);
        break;
    }
    case COMPANION_MOOD_MUTED:
        stroke(&canvas, 34, 53, 60, 53, 7, false);
        stroke(&canvas, 163, 53, 189, 53, 7, false);
        stroke(&canvas, 105, 75, 116, 75, 5, false);
        break;
    case COMPANION_MOOD_ALERT:
        stroke(&canvas, 35, 41, 59, 65, 7, false);
        stroke(&canvas, 35, 65, 59, 41, 7, false);
        stroke(&canvas, 163, 41, 187, 65, 7, false);
        stroke(&canvas, 163, 65, 187, 41, 7, false);
        stroke(&canvas, 99, 80, 122, 80, 6, false);
        spark(&canvas, 199, 26, 7);
        break;
    case COMPANION_MOOD_READY:
    default:
        angle_eyes(&canvas, face->blink);
        stroke(&canvas, 97, 77, 124, 75, 6, false);
        spark(&canvas, 195, 27, 6);
        break;
    }
}

static void face_event(const lv_obj_class_t *class_p, lv_event_t *event)
{
    LV_UNUSED(class_p);
    if (lv_obj_event_base(&companion_face_class, event) != LV_RES_OK) return;
    if (lv_event_get_code(event) == LV_EVENT_DRAW_MAIN)
        draw_face(lv_event_get_target(event), lv_event_get_draw_ctx(event));
}

lv_obj_t *companion_face_create(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_class_create_obj(&companion_face_class, parent);
    if (object == NULL) return NULL;
    lv_obj_class_init_obj(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    ((companion_face_t *)object)->mood = COMPANION_MOOD_READY;
    return object;
}

void companion_face_set(lv_obj_t *object, companion_mood_t mood, uint32_t phase)
{
    if (object == NULL) return;
    LV_ASSERT_OBJ(object, &companion_face_class);
    if ((unsigned)mood > COMPANION_MOOD_ALERT) mood = COMPANION_MOOD_READY;
    companion_face_t *face = (companion_face_t *)object;
    const bool moving = mood != COMPANION_MOOD_MUTED && mood != COMPANION_MOOD_ALERT;
    static const int8_t gaze_steps[] = {0, 1, 0, -1};
    static const int8_t bob_steps[] = {0, -1, 0, 1};
    const int8_t gaze = moving ? gaze_steps[(phase / 16U) % 4U] : 0;
    const int8_t bob = moving ? bob_steps[(phase / 12U) % 4U] : 0;
    // One short blink every 5.04 seconds, rather than rapid repeating winks.
    const uint8_t blink = moving && mood != COMPANION_MOOD_DOZING
        && (phase % 42U) >= (mood == COMPANION_MOOD_LISTENING ? 41U : 40U);
    const uint8_t movement = mood == COMPANION_MOOD_SPEAKING ? (phase / 2U) % 4U
        : (mood == COMPANION_MOOD_THINKING ? (phase / 8U) % 3U : 0);
    if (face->mood == mood && face->gaze == gaze && face->bob == bob
        && face->blink == blink && face->movement == movement) return;
    face->mood = mood;
    face->gaze = gaze;
    face->bob = bob;
    face->blink = blink;
    face->movement = movement;
    lv_obj_invalidate(object);
}

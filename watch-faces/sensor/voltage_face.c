#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// ---- Tuning knobs ----
#define REQUIRED_HITS          4
#define WINDOW_SECONDS         4
#define MIN_HIT_GAP_TICKS      2   // at 4Hz: 2 ticks ~= 0.5s

typedef struct {
    uint8_t hit_count;          // 0..4
    uint8_t last_hit_subsecond; // 0..3
    watch_date_time_t first_hit_time;
} wave_detect_ctx_t;

static void _beep(void) {
    watch_enable_buzzer();
    watch_buzzer_play_note(BUZZER_NOTE_A5, 120);
}

static void _reset_hits(wave_detect_ctx_t *ctx) {
    ctx->hit_count = 0;
    ctx->last_hit_subsecond = 0;
    memset(&ctx->first_hit_time, 0, sizeof(ctx->first_hit_time));
}

// In your accelerometer_status_face:
//   if (HAL_GPIO_A4_read()) => "Still"
//   else => "Active"
// So Active == (A4 == 0)
static bool _is_active_motion(void) {
    return !HAL_GPIO_A4_read();
}

// Clear the exact LCD regions we use so old segments don’t linger.
static void _clear_regions(void) {
    // These are fixed-width fields; spaces clear old segments.
    watch_display_text(WATCH_POSITION_TOP_LEFT,  "    ");
    watch_display_text(WATCH_POSITION_TOP,       "     "); // top center area
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");    // small field
    watch_display_text(WATCH_POSITION_BOTTOM,    "     "); // bottom area

    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CLASSIC) {
        watch_display_text(WATCH_POSITION_SECONDS, "  ");
    }
    watch_clear_decimal_if_available();
}

static void _update_display(wave_detect_ctx_t *ctx) {
    _clear_regions();

    // Title (use fixed length; fallback is 2 chars)
    watch_display_text_with_fallback(WATCH_POSITION_TOP, "WAVE ", "WV");

    // Motion status
    if (_is_active_motion()) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "ACTV ", "ACt");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "STIL ", "St");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }

    // Show hit count in the top-right as 0..4 (always overwrite with 2 chars)
    // e.g. "H0", "H1", ...
    char buf[3];
    buf[0] = 'H';
    buf[1] = (char)('0' + (ctx->hit_count <= 9 ? ctx->hit_count : 9));
    buf[2] = '\0';
    watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
}

void voltage_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(wave_detect_ctx_t));
        memset(*context_ptr, 0, sizeof(wave_detect_ctx_t));
    }
}

void voltage_face_activate(void *context) {
    wave_detect_ctx_t *ctx = (wave_detect_ctx_t *)context;

    // 4 Hz updates while this face is active
    movement_request_tick_frequency(4);

    _reset_hits(ctx);

    if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
    _update_display(ctx);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wave_detect_ctx_t *ctx = (wave_detect_ctx_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
            _reset_hits(ctx);
            _update_display(ctx);
            break;

        case EVENT_TICK: {
            bool active = _is_active_motion();

            if (active) {
                if (ctx->hit_count == 0) {
                    ctx->first_hit_time = movement_get_local_date_time();
                    ctx->last_hit_subsecond = event.subsecond;
                    ctx->hit_count = 1;
                } else {
                    uint8_t delta = (uint8_t)((event.subsecond + 4 - ctx->last_hit_subsecond) % 4);
                    if (delta >= MIN_HIT_GAP_TICKS) {
                        ctx->last_hit_subsecond = event.subsecond;
                        if (ctx->hit_count < 255) ctx->hit_count++;
                    }
                }

                if (ctx->hit_count >= REQUIRED_HITS) {
                    watch_date_time_t now = movement_get_local_date_time();

                    int32_t first_s = (int32_t)ctx->first_hit_time.unit.hour * 3600
                                    + (int32_t)ctx->first_hit_time.unit.minute * 60
                                    + (int32_t)ctx->first_hit_time.unit.second;

                    int32_t now_s   = (int32_t)now.unit.hour * 3600
                                    + (int32_t)now.unit.minute * 60
                                    + (int32_t)now.unit.second;

                    if ((now_s - first_s) <= WINDOW_SECONDS) {
                        _beep();
                    }

                    _reset_hits(ctx);
                }
            } else {
                _reset_hits(ctx);
            }

            _update_display(ctx);
            break;
        }

        case EVENT_LOW_ENERGY_UPDATE:
            if (!watch_sleep_animation_is_running()) watch_start_sleep_animation(1000);
            _update_display(ctx);
            break;

        default:
            movement_default_loop_handler(event);
            break;
    }

    return true;
}

void voltage_face_resign(void *context) {
    (void) context;
}

#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// ---- Detection tuning ----
#define REQUIRED_HITS     4
#define WINDOW_SECONDS    4
#define MIN_GAP_TICKS     2   // at 4 Hz = ~0.5 s

typedef struct {
    uint8_t hit_count;
    uint8_t last_subsecond;
    watch_date_time_t first_time;
} wave_ctx_t;

// ------------------------------------------------------------

static void beep(void) {
    watch_enable_buzzer();
    watch_buzzer_play_note(BUZZER_NOTE_A5, 120);
}

static void reset_hits(wave_ctx_t *ctx) {
    ctx->hit_count = 0;
    ctx->last_subsecond = 0;
    memset(&ctx->first_time, 0, sizeof(ctx->first_time));
}

// From accelerometer_status_face:
// A4 HIGH = Still, A4 LOW = Active
static bool motion_active(void) {
    return !HAL_GPIO_A4_read();
}

// Clear all LCD regions we touch (prevents ghost segments)
static void clear_display(void) {
    watch_display_text(WATCH_POSITION_TOP_LEFT,  "  ");
    watch_display_text(WATCH_POSITION_TOP,       "     ");
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
    watch_display_text(WATCH_POSITION_BOTTOM,    "     ");

    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CLASSIC) {
        watch_display_text(WATCH_POSITION_SECONDS, "  ");
    }
    watch_clear_decimal_if_available();
}

static void draw(wave_ctx_t *ctx) {
    clear_display();

    // Title
    watch_display_text(WATCH_POSITION_TOP_LEFT, "WV");

    // Status
    if (motion_active()) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "ACTV ", "ACt");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "STIL ", "St");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }

    // Hit counter (single digit is safest on LCD)
    char d[3] = { ' ', (char)('0' + ctx->hit_count), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);
}

// ------------------------------------------------------------

void voltage_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(wave_ctx_t));
        memset(*context_ptr, 0, sizeof(wave_ctx_t));
    }
}

void voltage_face_activate(void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    // Faster updates for motion detection
    movement_request_tick_frequency(4);

    // 🔑 TURN ACCELEROMETER ON (background mode)
    movement_enable_tap_detection_if_available();
    movement_set_accelerometer_background_rate(LIS2DW_DATA_RATE_50_HZ);


    reset_hits(ctx);

    if (watch_sleep_animation_is_running()) {
        watch_stop_sleep_animation();
    }
    draw(ctx);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    switch (event.event_type) {

        case EVENT_ACTIVATE:
            reset_hits(ctx);
            draw(ctx);
            break;

        case EVENT_TICK: {
            bool active = motion_active();

            if (active) {
                if (ctx->hit_count == 0) {
                    ctx->first_time = movement_get_local_date_time();
                    ctx->last_subsecond = event.subsecond;
                    ctx->hit_count = 1;
                } else {
                    uint8_t delta =
                        (event.subsecond + 4 - ctx->last_subsecond) % 4;

                    if (delta >= MIN_GAP_TICKS) {
                        ctx->last_subsecond = event.subsecond;
                        if (ctx->hit_count < 9) ctx->hit_count++;
                    }
                }

                if (ctx->hit_count >= REQUIRED_HITS) {
                    watch_date_time_t now =
                        movement_get_local_date_time();

                    int32_t t0 =
                        ctx->first_time.unit.hour * 3600 +
                        ctx->first_time.unit.minute * 60 +
                        ctx->first_time.unit.second;

                    int32_t t1 =
                        now.unit.hour * 3600 +
                        now.unit.minute * 60 +
                        now.unit.second;

                    if ((t1 - t0) <= WINDOW_SECONDS) {
                        beep();
                    }
                    reset_hits(ctx);
                }
            } else {
                reset_hits(ctx);
            }

            draw(ctx);
            break;
        }

        case EVENT_LOW_ENERGY_UPDATE:
            if (!watch_sleep_animation_is_running()) {
                watch_start_sleep_animation(1000);
            }
            draw(ctx);
            break;

        default:
            movement_default_loop_handler(event);
            break;
    }

    return true;
}

void voltage_face_resign(void *context) {
    (void) context;

    // 🔑 TURN ACCELEROMETER OFF when leaving the face
    movement_disable_tap_detection_if_available();

}

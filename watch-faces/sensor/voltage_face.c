#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// ---- Tuning ----
#define REQUIRED_HITS      2
#define WINDOW_SECONDS     4

// How long the LED stays on after a detection.
// At 4 Hz ticks, 2 ticks ≈ 0.5 sec, 4 ticks ≈ 1 sec.
#define LED_ON_TICKS       2

typedef struct {
    uint8_t hit_count;
    watch_date_time_t first_time;
    bool prev_active;
    uint8_t led_ticks;
} wave_ctx_t;

static void reset_hits(wave_ctx_t *ctx) {
    ctx->hit_count = 0;
    ctx->prev_active = false;
    memset(&ctx->first_time, 0, sizeof(ctx->first_time));
}

static void beep(void) {
    watch_enable_buzzer();
    watch_buzzer_play_note(BUZZER_NOTE_A5, 120);
}

static void led_on(void) {
    watch_enable_leds();
    watch_set_led_green();   // change to watch_set_led_red() if you prefer
}

static void led_off(void) {
    watch_set_led_off();
    watch_disable_leds();
}

// In your accelerometer_status_face:
// A4 HIGH = Still, A4 LOW = Active
static bool motion_active(void) {
    return !HAL_GPIO_A4_read();
}

// Clear fields we use so old segments don't linger.
static void clear_display(void) {
    watch_display_text(WATCH_POSITION_TOP_LEFT,  "  ");
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
    watch_display_text(WATCH_POSITION_BOTTOM,    "     ");

    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CLASSIC) {
        watch_display_text(WATCH_POSITION_SECONDS, "  ");
    }
    watch_clear_decimal_if_available();
}

static void draw(wave_ctx_t *ctx) {
    clear_display();

    // Title: "WV"
    watch_display_text(WATCH_POSITION_TOP_LEFT, "WV");

    // Status: "AC" or "St" (short = reliable on segment LCD)
    if (motion_active()) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "ACTV ", "AC");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "STIL ", "St");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }

    // Counter in top-right: single digit 0..4
    char d[3] = { ' ', (char)('0' + (ctx->hit_count <= 9 ? ctx->hit_count : 9)), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);
}

void voltage_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(wave_ctx_t));
        memset(*context_ptr, 0, sizeof(wave_ctx_t));
    }
}

void voltage_face_activate(void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    // Faster ticks so we can see multiple events inside 4 seconds
    movement_request_tick_frequency(4);

    // Ensure accelerometer is actually running
    movement_enable_tap_detection_if_available();
    movement_set_accelerometer_background_rate(LIS2DW_DATA_RATE_50_HZ);

    reset_hits(ctx);
    ctx->led_ticks = 0;

    if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
    draw(ctx);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            reset_hits(ctx);
            ctx->led_ticks = 0;
            led_off();
            draw(ctx);
            break;

        case EVENT_TICK: {
            bool active = motion_active();

            // Count hits on Still -> Active edges (much more reliable than counting "active samples")
            if (active && !ctx->prev_active) {
                watch_date_time_t now = movement_get_local_date_time();

                if (ctx->hit_count == 0) {
                    ctx->first_time = now;
                    ctx->hit_count = 1;
                } else {
                    ctx->hit_count++;
                }

                // Check window
                int32_t t0 =
                    (int32_t)ctx->first_time.unit.hour * 3600 +
                    (int32_t)ctx->first_time.unit.minute * 60 +
                    (int32_t)ctx->first_time.unit.second;

                int32_t t1 =
                    (int32_t)now.unit.hour * 3600 +
                    (int32_t)now.unit.minute * 60 +
                    (int32_t)now.unit.second;

                // Too slow? restart window at this edge
                if ((t1 - t0) > WINDOW_SECONDS) {
                    ctx->first_time = now;
                    ctx->hit_count = 1;
                }

                // Trigger
                if (ctx->hit_count >= REQUIRED_HITS) {
                    beep();
                    led_on();
                    ctx->led_ticks = LED_ON_TICKS;

                    // Start fresh after a trigger
                    reset_hits(ctx);
                }
            }

            ctx->prev_active = active;

            // LED timeout (non-blocking)
            if (ctx->led_ticks) {
                ctx->led_ticks--;
                if (ctx->led_ticks == 0) {
                    led_off();
                }
            }

            draw(ctx);
            break;
        }

        case EVENT_LOW_ENERGY_UPDATE:
            if (!watch_sleep_animation_is_running()) watch_start_sleep_animation(1000);
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
    led_off();
    movement_disable_tap_detection_if_available();
    // Optional: if you want to reduce accel use further, you can set a lower background rate here.
}

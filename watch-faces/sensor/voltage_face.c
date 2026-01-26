#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// =====================
// TUNING
// =====================
#define TICK_HZ                 16
#define ACCEL_RATE              LIS2DW_DATA_RATE_50_HZ

// Cycle period limits (in ticks)
// At 16 Hz: 3 ticks = 0.187s, 24 ticks = 1.5s
#define MIN_CYCLE_TICKS         3
#define MAX_CYCLE_TICKS         24

#define REQUIRED_CYCLES         4
#define WINDOW_SECONDS          4

#define LED_ON_TICKS            8   // at 16 Hz ≈ 0.5 sec

// How big the oscillation must be to count (noise gate)
#define AMP_THRESHOLD           1200

// =====================

typedef struct {
    int32_t baseline;

    // filtered signed high-pass signal
    int32_t hp_filt;

    // cycle detection state
    int32_t last_hp_filt;
    uint16_t tick_counter;
    uint16_t last_cross_tick;

    uint8_t cycle_count;
    watch_date_time_t first_cycle_time;

    uint8_t led_ticks;
} wave_ctx_t;

static void reset_cycles(wave_ctx_t *ctx) {
    ctx->cycle_count = 0;
    memset(&ctx->first_cycle_time, 0, sizeof(ctx->first_cycle_time));
    ctx->last_cross_tick = 0;
}

static void beep(void) {
    watch_enable_buzzer();
    watch_buzzer_play_note(BUZZER_NOTE_A5, 120);
}

static void led_on(void) {
    watch_enable_leds();
    watch_set_led_green();
}

static void led_off(void) {
    watch_set_led_off();
    watch_disable_leds();
}

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
    watch_display_text(WATCH_POSITION_TOP_LEFT, "WV");

    // show cycle count
    char d[3] = { ' ', (char)('0' + (ctx->cycle_count <= 9 ? ctx->cycle_count : 9)), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);

    watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "CYCL ", "Cy");
    watch_set_indicator(WATCH_INDICATOR_SIGNAL);
}

static int32_t to_seconds(watch_date_time_t t) {
    return (int32_t)t.unit.hour * 3600 + (int32_t)t.unit.minute * 60 + (int32_t)t.unit.second;
}

// Read accel and update baseline + filtered signed hp
static bool read_hp_filtered(wave_ctx_t *ctx, int32_t *out_hp_filt_abs) {
    if (!lis2dw_have_new_data()) return false;

    lis2dw_reading_t r = lis2dw_get_raw_reading();
    int32_t mag = (int32_t)abs(r.x) + (int32_t)abs(r.y) + (int32_t)abs(r.z);

    if (ctx->baseline == 0) ctx->baseline = mag;

    // baseline LP: /16
    ctx->baseline += (mag - ctx->baseline) >> 4;

    // signed high-pass
    int32_t hp = mag - ctx->baseline;

    // light smoothing on hp to reduce jitter: /4
    ctx->hp_filt += (hp - ctx->hp_filt) >> 2;

    int32_t a = ctx->hp_filt;
    if (a < 0) a = -a;
    *out_hp_filt_abs = a;
    return true;
}

void voltage_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(wave_ctx_t));
        memset(*context_ptr, 0, sizeof(wave_ctx_t));
    }
}

void voltage_face_activate(void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    movement_request_tick_frequency(TICK_HZ);
    movement_enable_tap_detection_if_available();
    movement_set_accelerometer_background_rate(ACCEL_RATE);

    memset(ctx, 0, sizeof(*ctx));
    ctx->tick_counter = 0;
    reset_cycles(ctx);
    ctx->led_ticks = 0;

    if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
    draw(ctx);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            memset(ctx, 0, sizeof(*ctx));
            reset_cycles(ctx);
            ctx->led_ticks = 0;
            led_off();
            draw(ctx);
            break;

        case EVENT_TICK: {
            ctx->tick_counter++;

            // LED timeout
            if (ctx->led_ticks) {
                ctx->led_ticks--;
                if (ctx->led_ticks == 0) led_off();
            }

            // consume a few samples if available
            for (int i = 0; i < 6; i++) {
                int32_t amp = 0;
                if (!read_hp_filtered(ctx, &amp)) break;

                // Only detect cycles when there is enough oscillation (noise gate)
                if (amp < AMP_THRESHOLD) {
                    ctx->last_hp_filt = ctx->hp_filt;
                    continue;
                }

                // Detect a NEG->POS zero crossing of filtered hp
                bool crossed = (ctx->last_hp_filt < 0 && ctx->hp_filt >= 0);

                if (crossed) {
                    uint16_t now_tick = ctx->tick_counter;
                    uint16_t dt_ticks = (ctx->last_cross_tick == 0) ? 0 : (uint16_t)(now_tick - ctx->last_cross_tick);

                    // Accept only if within reasonable period band
                    if (dt_ticks >= MIN_CYCLE_TICKS && dt_ticks <= MAX_CYCLE_TICKS) {
                        watch_date_time_t now = movement_get_local_date_time();

                        if (ctx->cycle_count == 0) {
                            ctx->first_cycle_time = now;
                            ctx->cycle_count = 1;
                        } else {
                            ctx->cycle_count++;
                        }

                        // Window check
                        int32_t dt_s = to_seconds(now) - to_seconds(ctx->first_cycle_time);
                        if (dt_s > WINDOW_SECONDS) {
                            ctx->first_cycle_time = now;
                            ctx->cycle_count = 1;
                        }

                        if (ctx->cycle_count >= REQUIRED_CYCLES) {
                            beep();
                            led_on();
                            ctx->led_ticks = LED_ON_TICKS;
                            reset_cycles(ctx);
                        }
                    }

                    ctx->last_cross_tick = now_tick;
                }

                ctx->last_hp_filt = ctx->hp_filt;
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
}

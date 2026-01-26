#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// =====================
// HANDWASH DETECTOR TUNING
// =====================

// Face tick rate (how often our loop runs). Higher helps frequency detection.
#define TICK_HZ                 16

// Accelerometer data rate.
#define ACCEL_RATE              LIS2DW_DATA_RATE_50_HZ

// Handwashing scrub is often ~2–5 Hz.
// At 16 Hz tick rate, that’s ~3–8 ticks per cycle.
#define MIN_CYCLE_TICKS         3
#define MAX_CYCLE_TICKS         9

// How many cycles within the window to declare "washing".
#define REQUIRED_CYCLES         6
#define WINDOW_SECONDS          4

// Noise gate: increase to ignore casual movement, decrease if it never triggers.
#define AMP_THRESHOLD           1400

// LED flash duration after a detection
#define LED_ON_TICKS            8   // ~0.5s at 16 Hz

// How long to show the daily count after pressing Light
#define SHOW_COUNT_TICKS        (TICK_HZ * 3) // ~3 seconds

// =====================

typedef struct {
    // magnitude baseline (low-pass) for removing gravity/bias
    int32_t baseline;

    // filtered signed high-pass
    int32_t hp_filt;
    int32_t last_hp_filt;

    // cycle timing
    uint16_t tick_counter;
    uint16_t last_cross_tick;

    // cycle counting window
    uint8_t cycle_count;
    watch_date_time_t first_cycle_time;

    // LED timing
    uint8_t led_ticks;

    // Daily count of triggers ("LED activations")
    uint16_t daily_count;
    uint8_t day;
    uint8_t month;
    uint16_t year;

    // UI mode
    uint16_t show_count_ticks;
} wash_ctx_t;

static void reset_cycles(wash_ctx_t *ctx) {
    ctx->cycle_count = 0;
    memset(&ctx->first_cycle_time, 0, sizeof(ctx->first_cycle_time));
    ctx->last_cross_tick = 0;
}

static int32_t to_seconds(watch_date_time_t t) {
    return (int32_t)t.unit.hour * 3600 + (int32_t)t.unit.minute * 60 + (int32_t)t.unit.second;
}

static void beep(void) {
    watch_enable_buzzer();
    watch_buzzer_play_note(BUZZER_NOTE_A5, 120);
}

static void led_on(void) {
    watch_enable_leds();
    watch_set_led_green(); // change to watch_set_led_red() if you prefer
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

// Shows either status or daily count depending on ctx->show_count_ticks
static void draw(wash_ctx_t *ctx) {
    clear_display();
    watch_display_text(WATCH_POSITION_TOP_LEFT, "HW"); // HandWash

    if (ctx->show_count_ticks) {
        // Show daily count. LCD fields are limited; we display last 2 digits on top-right,
        // and show "CNT" on bottom.
        uint8_t last2 = (uint8_t)(ctx->daily_count % 100);
        char tr[3];
        snprintf(tr, sizeof(tr), "%02u", (unsigned)last2);
        watch_display_text(WATCH_POSITION_TOP_RIGHT, tr);
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "CNT  ", "Ct");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
        return;
    }

    // Normal mode: show how many cycles collected toward detection
    char d[3] = { ' ', (char)('0' + (ctx->cycle_count <= 9 ? ctx->cycle_count : 9)), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);
    watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "WASH ", "WA");
    watch_set_indicator(WATCH_INDICATOR_SIGNAL);
}

// Read accel sample and update baseline + filtered signed high-pass.
// Returns true if a new sample was consumed.
static bool read_hp_filtered(wash_ctx_t *ctx, int32_t *out_abs_amp) {
    if (!lis2dw_have_new_data()) return false;

    lis2dw_reading_t r = lis2dw_get_raw_reading();
    int32_t mag = (int32_t)abs(r.x) + (int32_t)abs(r.y) + (int32_t)abs(r.z);

    if (ctx->baseline == 0) ctx->baseline = mag;

    // baseline low-pass: /16
    ctx->baseline += (mag - ctx->baseline) >> 4;

    // signed high-pass
    int32_t hp = mag - ctx->baseline;

    // smooth hp a bit: /4
    ctx->hp_filt += (hp - ctx->hp_filt) >> 2;

    int32_t a = ctx->hp_filt;
    if (a < 0) a = -a;
    *out_abs_amp = a;
    return true;
}

// Reset daily count if date changed
static void maybe_roll_day(wash_ctx_t *ctx) {
    watch_date_time_t now = movement_get_local_date_time();

    // Some builds store year as years since 2000; some store full year.
    // We just store whatever is in now.unit.year and compare consistently.
    if (ctx->day == 0 && ctx->month == 0 && ctx->year == 0) {
        ctx->day = now.unit.day;
        ctx->month = now.unit.month;
        ctx->year = now.unit.year;
        return;
    }

    if (now.unit.day != ctx->day || now.unit.month != ctx->month || now.unit.year != ctx->year) {
        ctx->daily_count = 0;
        ctx->day = now.unit.day;
        ctx->month = now.unit.month;
        ctx->year = now.unit.year;
    }
}

void voltage_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(wash_ctx_t));
        memset(*context_ptr, 0, sizeof(wash_ctx_t));
    }
}

void voltage_face_activate(void *context) {
    wash_ctx_t *ctx = (wash_ctx_t *)context;

    movement_request_tick_frequency(TICK_HZ);

    // Ensure accelerometer is running in this firmware environment
    movement_enable_tap_detection_if_available();
    movement_set_accelerometer_background_rate(ACCEL_RATE);

    // Keep daily_count across activations of this face (don’t memset the whole struct).
    // But reset signal processing.
    ctx->baseline = 0;
    ctx->hp_filt = 0;
    ctx->last_hp_filt = 0;
    ctx->tick_counter = 0;
    ctx->last_cross_tick = 0;
    ctx->led_ticks = 0;
    ctx->show_count_ticks = 0;
    reset_cycles(ctx);

    maybe_roll_day(ctx);

    if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
    draw(ctx);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wash_ctx_t *ctx = (wash_ctx_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            // keep daily count; just reset detection state
            ctx->baseline = 0;
            ctx->hp_filt = 0;
            ctx->last_hp_filt = 0;
            ctx->tick_counter = 0;
            ctx->last_cross_tick = 0;
            ctx->led_ticks = 0;
            ctx->show_count_ticks = 0;
            reset_cycles(ctx);
            maybe_roll_day(ctx);
            led_off();
            draw(ctx);
            break;

        case EVENT_LIGHT_BUTTON_DOWN:
            // Show today’s count for a few seconds
            maybe_roll_day(ctx);
            ctx->show_count_ticks = SHOW_COUNT_TICKS;
            draw(ctx);
            break;

        case EVENT_TICK: {
            maybe_roll_day(ctx);

            ctx->tick_counter++;

            // LED timeout
            if (ctx->led_ticks) {
                ctx->led_ticks--;
                if (ctx->led_ticks == 0) led_off();
            }

            // Show-count timeout
            if (ctx->show_count_ticks) {
                ctx->show_count_ticks--;
                draw(ctx);
                // Keep detecting in background even while showing count.
                // (So you don’t “pause” detection.)
            }

            // Consume a few samples each tick to keep up
            for (int i = 0; i < 6; i++) {
                int32_t amp = 0;
                if (!read_hp_filtered(ctx, &amp)) break;

                // Noise gate
                if (amp < AMP_THRESHOLD) {
                    ctx->last_hp_filt = ctx->hp_filt;
                    continue;
                }

                // Count NEG->POS zero crossings as cycles
                bool crossed = (ctx->last_hp_filt < 0 && ctx->hp_filt >= 0);
                if (crossed) {
                    uint16_t now_tick = ctx->tick_counter;
                    uint16_t dt_ticks = (ctx->last_cross_tick == 0) ? 0 : (uint16_t)(now_tick - ctx->last_cross_tick);

                    if (dt_ticks >= MIN_CYCLE_TICKS && dt_ticks <= MAX_CYCLE_TICKS) {
                        watch_date_time_t now = movement_get_local_date_time();

                        if (ctx->cycle_count == 0) {
                            ctx->first_cycle_time = now;
                            ctx->cycle_count = 1;
                        } else {
                            ctx->cycle_count++;
                        }

                        // window check
                        int32_t dt_s = to_seconds(now) - to_seconds(ctx->first_cycle_time);
                        if (dt_s > WINDOW_SECONDS) {
                            ctx->first_cycle_time = now;
                            ctx->cycle_count = 1;
                        }

                        if (ctx->cycle_count >= REQUIRED_CYCLES) {
                            // Trigger: count an "LED activation"
                            ctx->daily_count++;

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

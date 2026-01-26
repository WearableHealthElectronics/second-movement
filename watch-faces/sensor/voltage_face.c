#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"

// =====================
// TUNING KNOBS
// =====================

// How fast our face loop runs. Higher = better peak detection.
// 8 is a good balance. If your build supports 16, even better.
// (If you want, try 16 later.)
#define TICK_HZ                 8

// Accelerometer sample rate (actual sensor). 25 Hz is plenty for waving.
// (50 Hz also works, but costs more power.)
#define ACCEL_RATE              LIS2DW_DATA_RATE_25_HZ

// Peak detection threshold in "high-pass magnitude units".
// If it never triggers, lower this. If it triggers too easily, raise it.
#define PEAK_THRESHOLD          1800

// Minimum ticks between peaks so we don’t double-count one wave.
// At 8 Hz, 3 ticks ≈ 0.375s. For waving, 2–5 is typical.
#define PEAK_REFRACTORY_TICKS   3

// How many peaks we require and the time window.
#define REQUIRED_PEAKS          2
#define WINDOW_SECONDS          4

// Peaks must be similar: max - min <= tolerance
#define PEAK_TOLERANCE          1200

// LED flash duration after detection (ticks at TICK_HZ)
#define LED_ON_TICKS            6   // at 8 Hz ≈ 0.75 sec

// =====================

typedef struct {
    // baseline (low-pass) of magnitude to remove gravity / bias
    int32_t baseline;

    // peak detector state
    bool was_above;
    uint8_t refractory;

    // collected peaks
    int16_t peaks[REQUIRED_PEAKS];
    uint8_t peak_count;
    watch_date_time_t first_peak_time;

    // UI / effects
    uint8_t led_ticks;
} wave_ctx_t;

static void reset_peaks(wave_ctx_t *ctx) {
    ctx->was_above = false;
    ctx->refractory = 0;
    ctx->peak_count = 0;
    memset(&ctx->first_peak_time, 0, sizeof(ctx->first_peak_time));
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

static void draw(wave_ctx_t *ctx, int16_t hp_mag) {
    (void)hp_mag;

    clear_display();
    watch_display_text(WATCH_POSITION_TOP_LEFT, "WV");

    // show peak count 0..4 in top right
    char d[3] = { ' ', (char)('0' + (ctx->peak_count <= 9 ? ctx->peak_count : 9)), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);

    // show simple status at bottom: "RUN" when we’re seeing motion, else "----"
    // (we call it RUN if we’re recently above threshold)
    if (ctx->was_above) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "RUN  ", "rN");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "---- ", "--");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }
}

// Return seconds-since-midnight (simple timing)
static int32_t to_seconds(watch_date_time_t t) {
    return (int32_t)t.unit.hour * 3600 + (int32_t)t.unit.minute * 60 + (int32_t)t.unit.second;
}

static bool peaks_similar(const int16_t p[REQUIRED_PEAKS]) {
    int16_t mn = p[0], mx = p[0];
    for (int i = 1; i < REQUIRED_PEAKS; i++) {
        if (p[i] < mn) mn = p[i];
        if (p[i] > mx) mx = p[i];
    }
    return (mx - mn) <= PEAK_TOLERANCE;
}

// Read one accel sample and return a "high-pass magnitude" value.
// Uses: lis2dw_have_new_data() and lis2dw_get_raw_reading().
static bool read_hp_mag(wave_ctx_t *ctx, int16_t *out_hp_mag) {
    if (!lis2dw_have_new_data()) return false;

    lis2dw_reading_t r = lis2dw_get_raw_reading();

    // magnitude-ish (no sqrt): abs(x)+abs(y)+abs(z)
    int32_t mag = (int32_t)abs(r.x) + (int32_t)abs(r.y) + (int32_t)abs(r.z);

    // init baseline the first time
    if (ctx->baseline == 0) ctx->baseline = mag;

    // low-pass baseline: baseline += (mag - baseline)/16
    ctx->baseline += (mag - ctx->baseline) >> 4;

    // high-pass magnitude = |mag - baseline|
    int32_t hp = mag - ctx->baseline;
    if (hp < 0) hp = -hp;

    // clamp to int16 range
    if (hp > 32767) hp = 32767;

    *out_hp_mag = (int16_t)hp;
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

    // Ensure accelerometer is enabled/configured in this firmware environment
    movement_enable_tap_detection_if_available();
    movement_set_accelerometer_background_rate(ACCEL_RATE);

    // Reset state
    ctx->baseline = 0;
    reset_peaks(ctx);
    ctx->led_ticks = 0;

    if (watch_sleep_animation_is_running()) watch_stop_sleep_animation();
    draw(ctx, 0);
}

bool voltage_face_loop(movement_event_t event, void *context) {
    wave_ctx_t *ctx = (wave_ctx_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            ctx->baseline = 0;
            reset_peaks(ctx);
            ctx->led_ticks = 0;
            led_off();
            draw(ctx, 0);
            break;

        case EVENT_TICK: {
            // handle LED timeout
            if (ctx->led_ticks) {
                ctx->led_ticks--;
                if (ctx->led_ticks == 0) led_off();
            }

            // Try to consume accel samples. At higher accel rates, there may be more than one
            // sample ready between ticks; we’ll read up to a few to keep up.
            for (int i = 0; i < 4; i++) {
                int16_t hp_mag = 0;
                if (!read_hp_mag(ctx, &hp_mag)) break;

                // refractory countdown
                if (ctx->refractory) ctx->refractory--;

                bool above = (hp_mag >= PEAK_THRESHOLD);

                // detect an upward threshold crossing as a "peak event"
                if (above && !ctx->was_above && ctx->refractory == 0) {
                    watch_date_time_t now = movement_get_local_date_time();

                    if (ctx->peak_count == 0) {
                        ctx->first_peak_time = now;
                    }

                    // record peak magnitude (hp_mag)
                    if (ctx->peak_count < REQUIRED_PEAKS) {
                        ctx->peaks[ctx->peak_count++] = hp_mag;
                    }

                    ctx->refractory = PEAK_REFRACTORY_TICKS;

                    // time window check
                    int32_t dt = to_seconds(now) - to_seconds(ctx->first_peak_time);
                    if (dt > WINDOW_SECONDS) {
                        // restart window using this as the first peak
                        ctx->first_peak_time = now;
                        ctx->peak_count = 1;
                        ctx->peaks[0] = hp_mag;
                    }

                    // trigger
                    if (ctx->peak_count >= REQUIRED_PEAKS) {
                        if (peaks_similar(ctx->peaks)) {
                            beep();
                            led_on();
                            ctx->led_ticks = LED_ON_TICKS;
                        }
                        reset_peaks(ctx);
                    }
                }

                ctx->was_above = above;
            }

            // redraw UI (not too heavy)
            // show latest hp value indirectly via RUN/-- and peak count
            draw(ctx, 0);
            break;
        }

        case EVENT_LOW_ENERGY_UPDATE:
            if (!watch_sleep_animation_is_running()) watch_start_sleep_animation(1000);
            draw(ctx, 0);
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

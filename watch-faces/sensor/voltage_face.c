#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"
#include "lis2dw.h"   // keep, since your accel face includes it

// ---- Tuning ----
#define REQUIRED_HITS          4
#define WINDOW_SECONDS         4
#define MIN_HIT_GAP_TICKS      2   // at 4Hz: 2 ticks ~= 0.5s

typedef struct {
    uint8_t hit_count;          // 0..4
    uint8_t last_hit_subsecond; // 0..3 (at 4Hz)
    watch_date_time_t first_hit_time;
    bool accel_enabled;
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

// From the official docs + your accelerometer_status_face:
// A4 high => Still, A4 low => Active. :contentReference[oaicite:3]{index=3}
static bool _is_active_motion(void) {
    return !HAL_GPIO_A4_read();
}

static void _clear_regions(void) {
    // Clear fields we use (fixed-width blanks clear leftover segments)
    watch_display_text(WATCH_POSITION_TOP_LEFT,  "  ");
    watch_display_text(WATCH_POSITION_TOP,       "     ");
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
    watch_display_text(WATCH_POSITION_BOTTOM,    "     ");

    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CLASSIC) {
        watch_display_text(WATCH_POSITION_SECONDS, "  ");
    }

    watch_clear_decimal_if_available();
}

static void _update_display(wave_detect_ctx_t *ctx) {
    _clear_regions();

    // Title: 2 chars in top-left so it won't smear across the top region
    watch_display_text(WATCH_POSITION_TOP_LEFT, "WV");

    // Status (use short fixed text)
    if (_is_active_motion()) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "ACTV ", "ACt");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "STIL ", "St");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }

    // Top-right: show 0..4 as a single digit (more reliable on segment LCD)
    char d[3] = { ' ', '0', '\0' };
    d[1] = (char)('0' + (ctx->hit_count <= 9 ? ctx->hit_count : 9));
    watch_display_text(WATCH_POSITION_TOP_RIGHT, d);
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

    movement_request_tick_frequency(4);
    _reset_hits(ctx);

    // ---- THIS IS THE IMPORTANT PART ----
    // Your accelerometer is powered down unless a face requests it. :contentReference[oaicite:4]{index=4}
    // Open movement.h and find the exact signature/constants, then replace the line below.
    //
    // Example possibilities you might see in movement.h:
    //   movement_set_accelerometer_background_rate(MOVEMENT_ACCELEROMETER_RATE_1_6_HZ);
    //   movement_set_accelerometer_background_rate(1.6f);
    //   movement_set_accelerometer_background_rate(ACCEL_BG_RATE_1_6_HZ);
    //
    // Replace this call with the one that matches YOUR movement.h:
    ctx->accel_enabled = movement_set_accelerometer_background_rate(/* TODO: 1.6 Hz constant/value */);

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
            // If accel isn't enabled (board missing / call failed), just show it
            if (!ctx->accel_enabled) {
                _clear_regions();
                watch_display_text(WATCH_POSITION_TOP_LEFT, "NV"); // "No accel" vibe
                watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "NOACC", "NO");
                watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
                break;
            }

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
    wave_detect_ctx_t *ctx = (wave_detect_ctx_t *)context;

    // If your movement.h provides a way to disable background rate, do it here.
    // Often it's something like setting the rate to 0 / OFF.
    // Example:
    //   movement_set_accelerometer_background_rate(MOVEMENT_ACCELEROMETER_RATE_OFF);
    (void)ctx;
}

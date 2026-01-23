#include <stdlib.h>
#include <string.h>
#include "voltage_face.h"
#include "watch.h"

// We include lis2dw.h because your existing accelerometer face includes it.
// Even if we don't directly call LIS2DW functions here, it helps keep build
// environments consistent with the accelerometer board enabled.
#include "lis2dw.h"

// ---- Tuning knobs ----
// Tick frequency is 4 Hz (set in activate). event.subsecond will be 0..3.
// Require a little spacing so one continuous "Active" doesn't count as multiple hits.
#define REQUIRED_HITS          4
#define WINDOW_SECONDS         4
#define MIN_HIT_GAP_TICKS      2   // at 4Hz: 2 ticks ~= 0.5s

typedef struct {
    uint8_t hit_count;
    uint8_t last_hit_subsecond;     // 0..3
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

// In your accelerometer_status_face.c you display:
//   if (HAL_GPIO_A4_read()) => "Still"
//   else => "Active"
// So: Active == (HAL_GPIO_A4_read() == 0)
static bool _is_active_motion(void) {
    return !HAL_GPIO_A4_read();
}

static void _update_display(wave_detect_ctx_t *ctx) {
    // Title
    watch_display_text_with_fallback(WATCH_POSITION_TOP, "WAVE", "WV");

    // Motion status
    if (_is_active_motion()) {
        watch_display_text_with_fallback(WATCH_POSITION_BOTTOM, "Active", " ACtiv");
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    } else {
        watch_display_text(WATCH_POSITION_BOTTOM, "Still ");
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
    }

    // Show hit count (optional, helps tuning/seeing it work)
    // Use the top-right area if available.
    // Display "0".."4"
    char c[2] = { (char)('0' + (ctx->hit_count <= 9 ? ctx->hit_count : 9)), '\0' };
    watch_display_text(WATCH_POSITION_TOP_RIGHT, c);

    // Seconds area: clear on classic to avoid clutter with animations
    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CLASSIC) {
        watch_display_text(WATCH_POSITION_SECONDS, "  ");
    }
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

    // Faster tick so we can detect repeated motion reliably
    movement_request_tick_frequency(4);

    _reset_hits(ctx);

    // Update display immediately
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
            // Update display each tick so you can see Active/Still quickly
            _update_display(ctx);

            bool active = _is_active_motion();

            if (active) {
                // First hit starts the window
                if (ctx->hit_count == 0) {
                    ctx->first_hit_time = movement_get_local_date_time();
                    ctx->last_hit_subsecond = event.subsecond;
                    ctx->hit_count = 1;
                } else {
                    // Only count a new hit if enough ticks have passed since last hit
                    // event.subsecond is 0..3; compute modular distance
                    uint8_t delta = (uint8_t)((event.subsecond + 4 - ctx->last_hit_subsecond) % 4);
                    if (delta >= MIN_HIT_GAP_TICKS) {
                        ctx->last_hit_subsecond = event.subsecond;
                        if (ctx->hit_count < 255) ctx->hit_count++;
                    }
                }

                // If we have enough hits, check window and beep
                if (ctx->hit_count >= REQUIRED_HITS) {
                    watch_date_time_t now = movement_get_local_date_time();

                    // Compute seconds-since-midnight for simple difference.
                    // Note: if you cross midnight during the 4-second window, this simple math breaks,
                    // but that's extremely unlikely for this use case.
                    int32_t first_s = (int32_t)ctx->first_hit_time.unit.hour * 3600
                                    + (int32_t)ctx->first_hit_time.unit.minute * 60
                                    + (int32_t)ctx->first_hit_time.unit.second;

                    int32_t now_s   = (int32_t)now.unit.hour * 3600
                                    + (int32_t)now.unit.minute * 60
                                    + (int32_t)now.unit.second;

                    if ((now_s - first_s) <= WINDOW_SECONDS) {
                        _beep();
                    }

                    // Reset after a detection attempt
                    _reset_hits(ctx);
                }
            } else {
                // If it goes still, reset the sequence (keeps it "consecutive")
                _reset_hits(ctx);
            }
            break;
        }

        case EVENT_LOW_ENERGY_UPDATE:
            // If low energy update happens, show status but don't try to beep aggressively.
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

    // Optional: you could request slower ticks again, but Movement usually manages it per-face.
    // movement_request_tick_frequency(1);
}

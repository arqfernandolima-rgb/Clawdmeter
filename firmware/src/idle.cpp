#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"
#include "hal/board_caps.h"

enum IdleState {
    STATE_AWAKE,
    STATE_FADING_OUT,
    STATE_ASLEEP,
    STATE_FADING_IN,
};

static IdleState state = STATE_AWAKE;
static uint32_t last_activity_ms = 0;
static uint32_t fade_started_ms  = 0;
static uint32_t fade_last_step_ms = 0;
static uint8_t  fade_from = DISPLAY_DEFAULT_BRIGHTNESS;
static uint8_t  fade_to   = 0;
static uint8_t  awake_brightness = DISPLAY_DEFAULT_BRIGHTNESS;  // user-set "full" level (brightness.cpp)

static uint8_t applied_awake = DISPLAY_DEFAULT_BRIGHTNESS;     // what AWAKE currently shows

static void apply_brightness(uint8_t b) {
    display_hal_set_brightness(b);
}

bool idle_on_battery(void) {
    return board_caps().has_battery && !power_hal_is_vbus_in();
}

// The awake level for the current power source (dimmer on battery).
static uint8_t awake_level(void) {
    if (!idle_on_battery()) return awake_brightness;
    uint16_t b = (uint16_t)awake_brightness * IDLE_BATTERY_BRIGHTNESS_PCT / 100;
    return (uint8_t)(b < 20 ? 20 : b);
}

static void begin_fade(uint8_t to, uint32_t now) {
    fade_from = (to == 0) ? applied_awake : 0;
    fade_to   = to;
    fade_started_ms = now;
    fade_last_step_ms = now;
}

void idle_init(void) {
    state = STATE_AWAKE;
    last_activity_ms = millis();
    applied_awake = awake_brightness;
    apply_brightness(awake_brightness);
}

void idle_sleep_now(void) {
    if (state == STATE_AWAKE || state == STATE_FADING_IN) {
        begin_fade(0, millis());
        state = STATE_FADING_OUT;
    }
}

void idle_set_awake_brightness(uint8_t level) {
    awake_brightness = level;
    // Apply now if fully awake so a button press is visible immediately;
    // during fades/sleep the next fade-in picks it up.
    if (state == STATE_AWAKE) { applied_awake = awake_level(); apply_brightness(applied_awake); }
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    if (state == STATE_FADING_IN) return;
    if (state == STATE_AWAKE) return;
    // Asleep/fading-out shouldn't reach here in normal flow (callers gate via
    // idle_consume_wake_press first), but if it does: trigger a wake.
    begin_fade(awake_level(), last_activity_ms);
    state = STATE_FADING_IN;
}

bool idle_consume_wake_press(void) {
    if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
        uint32_t now = millis();
        last_activity_ms = now;
        begin_fade(awake_level(), now);
        state = STATE_FADING_IN;
        return true;
    }
    if (state == STATE_FADING_IN) {
        // Mid-wake — still swallow this press; the user shouldn't get a
        // half-wake half-action surprise.
        last_activity_ms = millis();
        return true;
    }
    last_activity_ms = millis();
    return false;
}

bool idle_is_asleep(void) {
    return state == STATE_ASLEEP || state == STATE_FADING_OUT;
}

void idle_tick(void) {
    uint32_t now = millis();

    // While on USB power (if configured), don't sleep — and wake from sleep
    // when power comes back. Treats USB-in as continuous activity.
    if (!IDLE_SLEEP_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = now;
        if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
            begin_fade(awake_level(), now);
            state = STATE_FADING_IN;
        }
    }

    switch (state) {
    case STATE_AWAKE: {
        // Plugging in / unplugging changes the awake level: follow it.
        uint8_t want = awake_level();
        if (want != applied_awake) { applied_awake = want; apply_brightness(want); }
        uint32_t timeout = idle_on_battery() ? IDLE_BATTERY_TIMEOUT_MS : IDLE_TIMEOUT_MS;
        if (now - last_activity_ms >= timeout) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
        }
        break;
    }

    case STATE_FADING_OUT:
    case STATE_FADING_IN: {
        if (now - fade_last_step_ms < IDLE_FADE_STEP_MS) break;
        fade_last_step_ms = now;
        uint32_t dur = (state == STATE_FADING_OUT) ? IDLE_FADE_OUT_MS : IDLE_FADE_IN_MS;
        uint32_t elapsed = now - fade_started_ms;
        if (elapsed >= dur) {
            if (state == STATE_FADING_IN) applied_awake = fade_to;
            apply_brightness(fade_to);
            state = (state == STATE_FADING_OUT) ? STATE_ASLEEP : STATE_AWAKE;
        } else {
            // Linear interpolation fade_from -> fade_to over dur ms.
            int32_t span = (int32_t)fade_to - (int32_t)fade_from;
            int32_t b = (int32_t)fade_from + (span * (int32_t)elapsed) / (int32_t)dur;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            apply_brightness((uint8_t)b);
        }
        break;
    }

    case STATE_ASLEEP:
        break;
    }
}

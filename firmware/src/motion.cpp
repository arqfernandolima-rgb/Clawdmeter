#include "motion.h"
#include <Arduino.h>
#include <math.h>
#include "hal/imu_hal.h"

// Gesture detection from raw acceleration (in g), sampled at ~25 Hz:
//
//  * face-down: gravity along the screen normal for 1.5 s. Which sign of z
//    means "screen down" depends on how the IMU sits on the PCB; the host can
//    flip it with the `facedown_sign` config if it comes out backwards.
//  * shake: three or more strong jolts (|a| more than 0.9 g away from 1 g)
//    within 1.2 s.
//  * pick-up: a clear movement after at least 30 s of sitting still.

#define SAMPLE_MS        40
#define FACEDOWN_HOLD_MS 1500
#define FACEUP_HOLD_MS   800
#define SHAKE_WINDOW_MS  1200
#define SHAKE_PEAKS      3
#define SHAKE_COOLDOWN   3000
#define STILL_MS         30000
#define PICKUP_COOLDOWN  15000

static bool     have_imu = true;
static uint32_t last_sample = 0;
static int8_t   fz_sign = 1;

static bool     dnd = false, dnd_changed = false;
static uint32_t down_since = 0, up_since = 0;

static uint32_t peak_ms[SHAKE_PEAKS];
static uint8_t  peak_n = 0;
static uint32_t last_peak = 0, last_shake = 0;
static bool     shake_evt = false;

static uint32_t still_since = 0, last_pickup = 0;
static bool     pickup_evt = false;

void motion_set_facedown_sign(int8_t s) { fz_sign = (s < 0) ? -1 : 1; }
bool motion_dnd(void) { return dnd; }
bool motion_take_shake(void)      { bool e = shake_evt;   shake_evt = false;   return e; }
bool motion_take_pickup(void)     { bool e = pickup_evt;  pickup_evt = false;  return e; }
bool motion_take_dnd_change(void) { bool e = dnd_changed; dnd_changed = false; return e; }

void motion_tick(void) {
    if (!have_imu) return;
    uint32_t now = millis();
    if (now - last_sample < SAMPLE_MS) return;
    last_sample = now;

    float x, y, z;
    if (!imu_hal_read_accel(&x, &y, &z)) {
        static uint8_t misses = 0;
        if (++misses > 50) have_imu = false;   // board has no accelerometer path
        return;
    }
    float mag = sqrtf(x * x + y * y + z * z);
    float dev = fabsf(mag - 1.0f);

    // ---- face-down (do not disturb)
    float zn = z * fz_sign;
    if (zn > 0.8f && dev < 0.25f) {
        up_since = 0;
        if (!down_since) down_since = now;
        if (!dnd && now - down_since >= FACEDOWN_HOLD_MS) { dnd = true; dnd_changed = true; }
    } else {
        down_since = 0;
        if (dnd && zn < 0.5f) {
            if (!up_since) up_since = now;
            if (now - up_since >= FACEUP_HOLD_MS) { dnd = false; dnd_changed = true; up_since = 0; }
        }
    }

    // ---- shake
    if (dev > 0.9f && now - last_peak > 80) {
        last_peak = now;
        peak_ms[peak_n % SHAKE_PEAKS] = now;
        peak_n++;
        if (peak_n >= SHAKE_PEAKS) {
            uint32_t oldest = peak_ms[peak_n % SHAKE_PEAKS];
            if (now - oldest <= SHAKE_WINDOW_MS && now - last_shake > SHAKE_COOLDOWN) {
                shake_evt = true;
                last_shake = now;
                peak_n = 0;
            }
        }
    }

    // ---- pick-up after sitting still
    if (dev < 0.06f) {
        if (!still_since) still_since = now;
    } else if (dev > 0.25f) {
        if (still_since && now - still_since >= STILL_MS && !dnd
            && now - last_pickup > PICKUP_COOLDOWN && now - last_shake > SHAKE_COOLDOWN) {
            pickup_evt = true;
            last_pickup = now;
        }
        still_since = 0;
    }
}

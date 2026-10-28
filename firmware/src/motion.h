#pragma once
#include <stdint.h>

// Accelerometer gestures for the desk buddy (see motion.cpp). All no-ops on
// boards whose imu_hal_read_accel() returns false.
void motion_tick(void);
bool motion_dnd(void);                 // lying face-down = do not disturb
bool motion_take_shake(void);          // one-shot: board was shaken
bool motion_take_pickup(void);         // one-shot: picked up after sitting still
bool motion_take_dnd_change(void);     // one-shot: DND flipped (read motion_dnd())
void motion_set_facedown_sign(int8_t s);

// Weak defaults for optional HAL entry points added after the original HAL
// contract. Boards that implement them (currently the C6 AMOLED-2.16) provide
// strong definitions that win at link time; every other board keeps building
// unchanged and simply doesn't get the feature.
#include "hal/sound_hal.h"
#include "hal/imu_hal.h"

__attribute__((weak)) void sound_hal_play(uint8_t id) {
    if (id == 0) sound_hal_play_reset();
}
__attribute__((weak)) void sound_hal_set_volume(uint8_t volume) { (void)volume; }
__attribute__((weak)) bool imu_hal_read_accel(float* x, float* y, float* z) {
    (void)x; (void)y; (void)z;
    return false;
}

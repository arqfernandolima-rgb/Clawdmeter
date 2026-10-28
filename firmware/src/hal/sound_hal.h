#pragma once

// Optional audio output (a passive piezo buzzer driven by LEDC PWM). Used to
// chime when the Claude session limit resets. Boards without a buzzer — the
// AMOLED-1.8 and the C6, whose stock hardware has no speaker output — no-op on
// init/tick and ignore play requests.
//
// Playback is non-blocking: sound_hal_play_reset() only *queues* the chime and
// returns immediately; sound_hal_tick() (called every loop) advances the notes
// so the LVGL render loop never stalls.

#include <stdint.h>

void sound_hal_init(void);
void sound_hal_tick(void);
void sound_hal_play_reset(void);

// Play one of the CHIME_* cues (see chime.h) and set the speaker volume (0..100).
// Weak no-op defaults live in hal_defaults.cpp; boards with audio override them.
void sound_hal_play(uint8_t id);
void sound_hal_set_volume(uint8_t volume);

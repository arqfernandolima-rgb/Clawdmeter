#include "../../hal/sound_hal.h"
#include "board.h"
#include <Arduino.h>
#include "../../chime.h"

// C6 AMOLED-2.16: ES8311 codec + onboard speaker. Upstream left this board
// silent; pins come from Waveshare's own config for this exact board
// (XiaoZhi esp32-c6-touch-amoled-2.16/config.h and the Arduino 07_Audio_Test
// board_cfg): I2S MCLK 19, BCLK 20, WS 22, DOUT 23, DIN 21; ES8311 at 0x18 on
// the shared I2C bus; the power amp has no MCU control (always enabled), so
// the chime engine mutes the codec DAC between sounds instead.

#define SND_I2S_MCLK      19
#define SND_I2S_BCLK      20
#define SND_I2S_WS        22
#define SND_I2S_DOUT      23
#define SND_I2S_DIN       21
#define SND_ES8311_ADDR   0x18
#define SND_SAMPLE_RATE   44100   // matches the embedded bell clip

void sound_hal_init(void) {
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 70, nullptr
    };
    chime_init(cfg);
}

void sound_hal_tick(void)                 { chime_tick(); }
void sound_hal_play_reset(void)           { chime_play(); }
void sound_hal_play(uint8_t id)           { chime_play_id(id); }
void sound_hal_set_volume(uint8_t volume) { chime_set_volume(volume); }

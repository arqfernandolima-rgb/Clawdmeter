#include "chime.h"
#include <Arduino.h>
#include <math.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "bell_pcm.h"   // const uint8_t bell_pcm[] / bell_pcm_len — 44.1 kHz 16-bit stereo

// Shared ES8311 chime engine. See chime.h. Adapted from the original 2.16
// sound.cpp so the 2.16, 1.8 (and any future ES8311 board) share one copy of
// the codec setup, the embedded PCM, and the non-blocking playback task.
//
// Besides the embedded bell clip, it synthesizes a few short cues (attention,
// ding, fanfare, boing, blip) from a 256-entry integer sine table: the C6 has
// no FPU, so everything per-sample is fixed-point.

static I2SClass        i2s;
static ChimeConfig     cfg;
static es8311_handle_t es      = nullptr;
static bool            ready   = false;
static volatile bool   playing = false;
static volatile uint8_t pending_id = 0;

static bool es8311_setup(void) {
    es = es8311_create(0, cfg.es8311_addr);   // I2C port 0 (shared Wire bus)
    if (!es) return false;
    // mclk_inverted, sclk_inverted, mclk_from_mclk_pin, mclk_frequency, sample_frequency
    const es8311_clock_config_t clk = {
        false, false, true, cfg.sample_rate * 256, cfg.sample_rate
    };
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(es, false);
    es8311_voice_volume_set(es, cfg.volume, NULL);
    es8311_voice_mute(es, true);             // quiet between sounds (amp may be always-on)
    return true;
}

// ---------------- synthesizer ----------------
static int16_t sine_tab[256];
static void build_sine(void) {
    for (int i = 0; i < 256; i++) sine_tab[i] = (int16_t)(sinf(i * 6.2831853f / 256.0f) * 32767.0f);
}

#define FRAMES 256
static int16_t frame_buf[FRAMES * 2];

// One note: frequency glides f0 -> f1 over `ms`; 4 ms attack, then exponential
// decay with time constant `tau_ms` (0 = flat, with a 15 ms release instead).
// `h2` adds the 2nd harmonic at h2/256 of the level (a softer, bell-ish tone).
static void synth_note(float f0, float f1, int ms, int level, int tau_ms, int h2) {
    const int sr = cfg.sample_rate;
    const int total = sr * ms / 1000;
    const int attack = sr * 4 / 1000;
    const int release = sr * 15 / 1000;
    uint32_t phase = 0, phase2 = 0;
    // amplitude in Q15; decay multiplier per sample in Q15
    int32_t decay_q15 = 32768;
    if (tau_ms > 0) decay_q15 = (int32_t)(expf(-1000.0f / (tau_ms * (float)sr)) * 32768.0f);
    int32_t env = 32767;
    int done = 0;
    while (done < total) {
        int n = total - done < FRAMES ? total - done : FRAMES;
        float frac = (float)done / (float)total;
        float f = f0 + (f1 - f0) * frac;
        uint32_t inc = (uint32_t)(f * 4294967296.0f / sr);
        uint32_t inc2 = inc * 2;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            int32_t a;
            if (k < attack) a = (int32_t)((int64_t)32767 * k / attack);
            else {
                if (tau_ms > 0) env = (int32_t)(((int64_t)env * decay_q15) >> 15);
                a = env;
                if (tau_ms == 0 && total - k < release) a = (int32_t)((int64_t)32767 * (total - k) / release);
            }
            int32_t s = sine_tab[phase >> 24];
            if (h2) s += (sine_tab[phase2 >> 24] * h2) >> 8;
            phase += inc; phase2 += inc2;
            int32_t v = (int32_t)(((int64_t)s * a >> 15) * level >> 8);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            frame_buf[2 * i] = frame_buf[2 * i + 1] = (int16_t)v;
        }
        i2s.write((uint8_t*)frame_buf, n * 4);
        done += n;
    }
}

static void silence(int ms) {
    memset(frame_buf, 0, sizeof(frame_buf));
    int total = cfg.sample_rate * ms / 1000;
    while (total > 0) {
        int n = total < FRAMES ? total : FRAMES;
        i2s.write((uint8_t*)frame_buf, n * 4);
        total -= n;
    }
}

static void play_id(uint8_t id) {
    switch (id) {
    case CHIME_ATTENTION:                 // two rising notes, twice: "hey — hey"
        for (int r = 0; r < 2; r++) {
            synth_note(659.3f, 659.3f, 120, 150, 0, 40);
            silence(25);
            synth_note(880.0f, 880.0f, 170, 150, 0, 40);
            if (r == 0) silence(180);
        }
        break;
    case CHIME_DING:                      // soft single bell: "your turn"
        synth_note(1046.5f, 1046.5f, 900, 140, 260, 70);
        break;
    case CHIME_FANFARE:                   // C-E-G-C arpeggio: big finish
        synth_note(523.3f, 523.3f, 120, 140, 0, 50);
        synth_note(659.3f, 659.3f, 120, 140, 0, 50);
        synth_note(784.0f, 784.0f, 120, 140, 0, 50);
        synth_note(1046.5f, 1046.5f, 650, 150, 300, 60);
        break;
    case CHIME_BOING:                     // shake reaction: falling wobble
        synth_note(620.0f, 180.0f, 300, 150, 180, 0);
        break;
    case CHIME_BLIP:                      // pick-up / small acknowledgements
        synth_note(1318.5f, 1318.5f, 45, 110, 0, 0);
        break;
    case CHIME_PREVIEW:                   // what "Claude needs you" will sound like
        synth_note(659.3f, 659.3f, 120, 150, 0, 40);
        silence(25);
        synth_note(880.0f, 880.0f, 170, 150, 0, 40);
        break;
    case CHIME_BELL:
    default:
        i2s.write((uint8_t*)bell_pcm, bell_pcm_len);
        break;
    }
}

static void chime_task(void* arg) {
    uint8_t id = pending_id;
    if (cfg.amp_enable) cfg.amp_enable(true);
    if (es) es8311_voice_mute(es, false);
    delay(8);                                  // let the amp settle (avoids turn-on pop)
    play_id(id);
    silence(30);
    if (es) es8311_voice_mute(es, true);
    if (cfg.amp_enable) cfg.amp_enable(false);
    playing = false;
    vTaskDelete(nullptr);
}

bool chime_init(const ChimeConfig& c) {
    cfg = c;
    if (cfg.amp_enable) cfg.amp_enable(false);   // amp off until we play
    build_sine();

    i2s.setPins(cfg.bclk, cfg.ws, cfg.dout, cfg.din, cfg.mclk);
    if (!i2s.begin(I2S_MODE_STD, cfg.sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("chime: I2S init failed");
        return false;
    }
    if (!es8311_setup()) {
        Serial.println("chime: ES8311 init failed");
        return false;
    }
    ready = true;
    Serial.println("chime: ES8311 ready");
    return true;
}

void chime_play_id(uint8_t id) {
    if (!ready || playing) return;
    playing = true;
    pending_id = id;
    if (xTaskCreatePinnedToCore(chime_task, "chime", 4096, nullptr, 1, nullptr, 0) != pdPASS)
        playing = false;   // couldn't spawn — stay silent rather than wedge the flag
}

void chime_play(void) { chime_play_id(CHIME_BELL); }

void chime_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    if (!ready || !es || volume == cfg.volume) return;
    cfg.volume = volume;
    es8311_voice_volume_set(es, volume, NULL);
}

void chime_tick(void) {}   // playback runs in chime_task; nothing to poll

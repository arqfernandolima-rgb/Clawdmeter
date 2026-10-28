#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "charge_anim.h"
#include "motion.h"
#include "chime.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

// Abstecher vom Splash auf die Auslastungszahlen: alle EVERY fuer SHOW lang.
#define USAGE_PEEK_EVERY_MS  (5UL * 60UL * 1000UL)
#define USAGE_PEEK_SHOW_MS   (60UL * 1000UL)

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    // Letzter Streifen eines Durchlaufs: der Splash wartet darauf, bevor er
    // nach einem Screenwechsel wieder direkt auf den Panel malt.
    if (lv_display_flush_is_last(disp)) splash_note_refresh_done();
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- Speaker volume (right button hold) ----
#define VOLUME_HOLD_MS 700
static uint8_t  s_volume = 70;
static uint32_t vol_local_ms = 0;

static void volume_step(void) {
    static const uint8_t levels[4] = {0, 45, 65, 85};
    static const char* names[4] = {"Sound: off", "Sound: low", "Sound: medium", "Sound: high"};
    int cur = 0, best = 1000;
    for (int i = 0; i < 4; i++) {
        int d = abs((int)levels[i] - (int)s_volume);
        if (d < best) { best = d; cur = i; }
    }
    int nxt = (cur + 1) % 4;
    s_volume = levels[nxt];
    vol_local_ms = millis();
    if (vol_local_ms == 0) vol_local_ms = 1;
    sound_hal_set_volume(s_volume);
    if (s_volume) sound_hal_play(CHIME_PREVIEW);   // hear the new level
    ble_send_volume(s_volume);                     // host saves it to its config
    ui_toast(names[nxt], 1800);
    Serial.printf("volume -> %u\n", (unsigned)s_volume);
}

// Three message kinds keep every BLE write under the ~250-byte ATT MTU:
//   usage  - the 60 s poll (numbers, clock, settings)       -> full refresh of usage fields
//   status - "so":1, Claude Code state/approval/attention    -> only the keys present change
//   stats  - "hs":1, month histogram + today's counters      -> stats fields only
//   max    - "hm":1, the second (personal Max) account        -> max_* fields only
enum msg_kind_t { MSG_BAD = -1, MSG_USAGE = 0, MSG_STATUS = 1, MSG_STATS = 2, MSG_MAX = 3 };

static void parse_status_keys(JsonDocument& doc, UsageData* out) {
    if (doc["cs"].is<const char*>()) {
        out->cc_live = true;
        strlcpy(out->cc_status, doc["cs"] | "", sizeof(out->cc_status));
    }
    if (doc["a"].is<const char*>())  strlcpy(out->anim, doc["a"] | "", sizeof(out->anim));
    if (doc["tt"].is<int>()) {       // seconds into the turn at send time -> local start
        int tt = doc["tt"] | -1;
        out->turn_start_ms = tt >= 0 ? millis() - (uint32_t)tt * 1000u : 0;
        if (tt >= 0 && out->turn_start_ms == 0) out->turn_start_ms = 1;
    }
    if (doc["pj"].is<const char*>()) strlcpy(out->project, doc["pj"] | "", sizeof(out->project));
    if (doc["ns"].is<int>())         out->sessions = doc["ns"] | 0;
    if (doc["att"].is<int>()) {
        out->att = doc["att"] | 0;
        out->att_seq = doc["aq"] | 0;
        out->att_secs = doc["atd"] | -1;
        out->att_rx_ms = millis();
        strlcpy(out->att_project, doc["atp"] | "", sizeof(out->att_project));
    }
    if (doc["snd"].is<int>()) {
        out->snd = doc["snd"] | 0;
        out->snd_seq = doc["sq"] | 0;
    }
    if (doc["ai"].is<unsigned int>()) {
        out->ap_id = doc["ai"] | 0u;
        strlcpy(out->ap_title, doc["at"] | "", sizeof(out->ap_title));
        strlcpy(out->ap_head, doc["ah"] | "", sizeof(out->ap_head));
        strlcpy(out->ap_detail, doc["ad"] | "", sizeof(out->ap_detail));
        out->ap_parts = doc["an"] | 0;
        out->ap_got = 0;
    }
    // Detail chunk for the pending approval, appended in arrival order.
    if (doc["ac"].is<unsigned int>() && out->ap_id && (doc["ac"] | 0u) == out->ap_id) {
        size_t n = strlen(out->ap_detail);
        strlcpy(out->ap_detail + n, doc["d"] | "", sizeof(out->ap_detail) - n);
        if (out->ap_got < 255) out->ap_got++;
    }
}

static int parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return MSG_BAD;
    }

    if (doc["hs"].is<int>()) {                         // ---- stats
        JsonArrayConst a = doc["dh"].as<JsonArrayConst>();
        uint8_t n = 0;
        for (JsonVariantConst v : a) {
            if (n >= sizeof(out->hist)) break;
            int x = v | 0;
            out->hist[n++] = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x));
        }
        out->hist_n = n;
        out->hist_today = doc["di"] | -1;
        out->hist_max_usd = doc["dm"] | 0.0f;
        out->k_tools = doc["kt"] | -1;
        out->k_files = doc["kf"] | -1;
        out->k_prompts = doc["kp"] | -1;
        out->k_approvals = doc["ka"] | -1;
        out->k_minutes = doc["km"] | -1;
        out->k_streak = doc["kr"] | -1;
        out->stats_valid = true;
        out->status_only = true;
        return MSG_STATS;
    }
    if (doc["hm"].is<int>()) {                         // ---- second account (Max)
        out->max_err = doc["e"] | 0;
        out->max_s   = doc["s"] | 0.0f;
        out->max_sr  = doc["sr"] | -1;
        out->max_w   = doc["w"] | 0.0f;
        out->max_wr  = doc["wr"] | -1;
        out->max_m   = doc["m"] | -1;
        out->max_mr  = doc["mr"] | -1;
        strlcpy(out->max_mn, doc["mn"] | "", sizeof(out->max_mn));
        out->max_rx_ms = millis();
        out->max_valid = true;
        out->status_only = true;
        return MSG_MAX;
    }
    if (doc["so"] | 0) {                               // ---- status (partial)
        parse_status_keys(doc, out);
        out->status_only = true;
        out->valid = true;
        return MSG_STATUS;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->spent_usd = doc["sd"] | -1.0f;
    out->limit_usd = doc["sl"] | -1.0f;
    out->today_usd = doc["td"] | -1.0f;
    out->volume = doc["vol"] | 70;
    out->facedown_sign = doc["fz"] | 1;
    out->status_only = false;
    parse_status_keys(doc, out);                       // tolerate status keys on usage too
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;
    out->valid = true;
    return MSG_USAGE;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // Play the charge overlay without touching the cable — the real
            // trigger needs a USB transition, which is awkward to produce on a
            // device that is being flashed over that same cable.
            else if (strcmp(cmd_buf, "charge") == 0)   charge_anim_play(true);
            else if (strcmp(cmd_buf, "uncharge") == 0) charge_anim_play(false);
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Motion gestures (accelerometer) ----
    motion_tick();
    if (motion_take_dnd_change()) {
        bool d = motion_dnd();
        ble_send_dnd(d);                       // host routes approvals to the keyboard
        Serial.printf("motion: %s\n", d ? "face-down, quiet" : "face-up again");
        if (!d) { idle_note_activity(); sound_hal_play(CHIME_BLIP); }
    }
    if (motion_take_shake()) {
        idle_note_activity();
        if (!motion_dnd()) sound_hal_play(CHIME_BOING);
        ui_flash_splash("expression surprise", 4000);
    }
    if (motion_take_pickup()) {
        idle_note_activity();
        if (!motion_dnd()) sound_hal_play(CHIME_BLIP);
        if (ui_get_current_screen() == SCREEN_SPLASH) splash_play_temp("expression wink", 3000);
    }

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        // SECONDARY: a tap sends Shift+Tab (on release, so a hold can mean
        // something else); holding ~0.7 s steps the speaker volume instead.
        if (board_caps().button_count >= 2) {
            static bool     secondary_was = false;
            static bool     secondary_wake_swallowed = false;
            static bool     secondary_long_done = false;
            static uint32_t secondary_down_ms = 0;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now && !secondary_was) {                 // pressed
                secondary_wake_swallowed = idle_consume_wake_press();
                secondary_down_ms = millis();
                secondary_long_done = false;
            }
            if (secondary_now && !secondary_wake_swallowed && !secondary_long_done
                && millis() - secondary_down_ms >= VOLUME_HOLD_MS) {
                secondary_long_done = true;
                volume_step();
            }
            if (!secondary_now && secondary_was) {                 // released
                if (!secondary_wake_swallowed && !secondary_long_done) {
                    ble_keyboard_press(0x2B, 0x02);                // HID Tab + LEFT_SHIFT
                    delay(15);
                    ble_keyboard_release();
                }
                secondary_wake_swallowed = false;
            }
            secondary_was = secondary_now;
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. On the usage view: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }

        pair_tick();
    }

    // ---- Zwischendurch die Zahlen zeigen ----
    // Wer den Clawd als Animation laufen laesst, will trotzdem ab und zu
    // sehen wie es um die Auslastung steht. Alle USAGE_PEEK_EVERY_MS also
    // fuer USAGE_PEEK_SHOW_MS auf den Usage-Screen und wieder zurueck.
    //
    // Bewusst nur aus dem Splash heraus und nur solange der Nutzer nicht
    // selbst umschaltet: ein Tastendruck waehrend des Abstechers beendet ihn,
    // und wer laenger freiwillig auf den Zahlen steht, faengt danach mit
    // vollem Abstand wieder an - sonst wechselt das Ding vor der Nase hin
    // und her.
    {
        static uint32_t peek_ref_ms = 0;
        static bool     peeking     = false;
        const uint32_t now_ms = millis();
        const screen_t cur    = ui_get_current_screen();

        if (idle_is_asleep() || ui_overlay_active()) {
            peek_ref_ms = now_ms;          // im Schlaf laeuft die Uhr nicht
            peeking = false;               // (and never flip away from an approval)
        } else if (peeking) {
            if (cur != SCREEN_USAGE) {     // Nutzer hat selbst umgeschaltet
                peeking = false;
                peek_ref_ms = now_ms;
            } else if (now_ms - peek_ref_ms >= USAGE_PEEK_SHOW_MS) {
                peeking = false;
                peek_ref_ms = now_ms;
                ui_show_screen(SCREEN_SPLASH);
                Serial.println("peek: zurueck zu den Animationen");
            }
        } else if (cur != SCREEN_SPLASH) {
            peek_ref_ms = now_ms;          // steht ohnehin auf den Zahlen
        } else if (now_ms - peek_ref_ms >= USAGE_PEEK_EVERY_MS) {
            peeking = true;
            peek_ref_ms = now_ms;
            ui_show_screen(SCREEN_USAGE);
            Serial.println("peek: zeige kurz die Auslastung");
        }
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Cable in / out gets a short animation. Driven by VBUS rather than the
    // charging flag, which also drops when the battery reaches full with the
    // cable still in — that would play the unplug sequence for nothing.
    // The first reading only records the state: booting on USB is not an event.
    static bool vbus_known = false;
    static bool last_vbus  = false;
    bool vbus = power_hal_is_vbus_in();
    if (!vbus_known) {
        vbus_known = true;
        last_vbus  = vbus;
    } else if (vbus != last_vbus) {
        last_vbus = vbus;
        Serial.printf("USB %s\n", vbus ? "in" : "out");
        charge_anim_play(vbus);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        int kind = parse_json(ble_get_data(), &usage);
        if (kind != MSG_BAD) {
            // Status/stats pushes arrive every few seconds; feeding them to the
            // 5-minute rate ring would shrink its window, so only usage counts.
            int g_before = usage_rate_group();
            bool session_reset = (kind == MSG_USAGE) ? usage_rate_sample(usage.session_pct)
                                                     : false;
            int g_after = usage_rate_group();
            if (kind == MSG_USAGE) {
                // The host's volume wins, except right after a local change
                // (it takes the host a moment to save the new level).
                if (vol_local_ms == 0 || millis() - vol_local_ms > 15000) {
                    s_volume = usage.volume;
                    sound_hal_set_volume(s_volume);
                }
                motion_set_facedown_sign(usage.facedown_sign);
                if (motion_dnd()) ble_send_dnd(true);   // re-assert after a host restart
            }
            // Sound requests from the host: play once per sequence bump, never
            // while the board lies face-down (do-not-disturb).
            static uint16_t last_snd_seq = 0;
            if (usage.snd_seq != last_snd_seq) {
                last_snd_seq = usage.snd_seq;
                if (usage.snd && !motion_dnd() && s_volume > 0) sound_hal_play(usage.snd);
            }
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            // Host-driven animation. Sent only when the host is configured to
            // mirror its desktop buddy; absent → "" → device keeps deciding.
            splash_set_anim(usage.anim);
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}

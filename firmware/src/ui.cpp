#include "ui.h"
#include "splash.h"
#include "charge_anim.h"
#include <lvgl.h>
#include <time.h>
#include <math.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "ble.h"
#include "idle.h"
#include "chime.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* today_font;     // enterprise "$x.xx today" (one step under reset_font)
    const lv_font_t* anim_font;      // status line text (geometric)
    const lv_font_t* anim_glyph_font;// status line spinner glyph (mono: has the dingbats)
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.today_font   = &font_styrene_24;
    L.anim_font    = &font_styrene_28;
    L.anim_glyph_font = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.today_font   = &font_styrene_12;
        L.anim_font    = &font_styrene_14;
        L.anim_glyph_font = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* stats_container = nullptr;
static lv_obj_t* max_container = nullptr;   // second account (personal Max plan)
static bool      s_max_seen = false;        // host sent an "hm" -> screen joins the tap cycle
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_weekly_pct_sym = nullptr;   // same treatment for the second number
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_spending_today = nullptr;    // "$4.30 today", right-aligned on the dollar line
static lv_obj_t* lbl_anim;      // status line text: connection state / Claude Code status
static lv_obj_t* lbl_anim_glyph;// status line spinner glyph (separate label, mono font)
static lv_obj_t* anim_row;      // centers glyph + text as one unit

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static int       logo_mode = -1;        // 0 float (idle), 1 bob (Claude busy); -1 stopped

static void logo_y_cb(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }

// Moving the logo only invalidates its own 80x84 box, and only when the
// integer y changes (a few times a second), so this is cheap even on the C6.
// It's stopped on the splash: there the C6 draws straight to the panel and an
// LVGL repaint under the logo would scribble over Clawd.
static void logo_anim_apply(int mode) {
    if (!logo_img) return;
    if (mode == logo_mode) return;
    lv_anim_delete(logo_img, logo_y_cb);
    lv_obj_set_y(logo_img, L.logo_y);
    logo_mode = mode;
    if (mode < 0) return;
    const int amp = L.small_icons ? 2 : (mode == 1 ? 5 : 3);
    const uint32_t half = mode == 1 ? 380 : 1400;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, logo_img);
    lv_anim_set_exec_cb(&a, logo_y_cb);
    lv_anim_set_values(&a, L.logo_y, L.logo_y - amp);
    lv_anim_set_duration(&a, half);
    lv_anim_set_reverse_duration(&a, half);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static int logo_wanted_mode(void);   // defined after the status statics
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)
static bool     s_cc_live = false;         // host sends real Claude Code status ("cs")
static char     s_cc_status[24] = "";      // last status text from the host
static uint32_t s_turn_start_ms = 0;       // local millis() when the current turn began
static char     s_project[24] = "";        // most recent session's folder name
static uint8_t  s_sessions = 0;            // active Claude Code sessions

static int logo_wanted_mode(void) {
    if (current_screen == SCREEN_SPLASH) return -1;
    return (s_cc_live && strstr(s_cc_status, "\xE2\x80\xA6") != NULL) ? 1 : 0;
}

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// Status line setter. Styrene's subset is ASCII-only, so a trailing "…"
// (U+2026) is drawn as "..."; identical updates are skipped (the C6 repaints
// in strips, so needless redraws cost real time).
static void set_status_line(const char* glyph, const char* text) {
    static char last_glyph[8] = "";
    static char last_text[48] = "";
    char t[48];
    size_t j = 0;
    for (size_t i = 0; text[i] && j < sizeof(t) - 4; ) {
        if ((uint8_t)text[i] == 0xE2 && (uint8_t)text[i + 1] == 0x80 && (uint8_t)text[i + 2] == 0xA6) {
            t[j++] = '.'; t[j++] = '.'; t[j++] = '.';
            i += 3;
        } else {
            t[j++] = text[i++];
        }
    }
    t[j] = '\0';
    if (strcmp(glyph, last_glyph) != 0) {
        strlcpy(last_glyph, glyph, sizeof(last_glyph));
        lv_label_set_text(lbl_anim_glyph, glyph);
    }
    if (strcmp(t, last_text) != 0) {
        strlcpy(last_text, t, sizeof(last_text));
        lv_label_set_text(lbl_anim, t);
    }
}

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void build_approval(lv_obj_t* parent);
static void approval_update(const UsageData* data);
static void approval_hide(void);
static void build_attention(lv_obj_t* parent);
static void attention_update(const UsageData* data);
static void attention_tick(void);
static void build_spark(lv_obj_t* panel);
static void spark_update(const UsageData* data, lv_color_t pace);
static void init_stats_screen(lv_obj_t* scr);
static void stats_update(const UsageData* data);
static void init_max_screen(lv_obj_t* scr);
static void max_update(const UsageData* data);
static void max_tick(void);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--");
    lv_obj_set_style_text_font(*out_pct, L.ent_pct_font, 0);   // serif headline number
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_today = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_today, "");
    lv_obj_set_style_text_font(lbl_spending_today, L.today_font, 0);
    lv_obj_set_style_text_color(lbl_spending_today, COL_ACCENT, 0);
    // Smaller face than the dollar line: drop it a few px so the baselines meet.
    lv_obj_align(lbl_spending_today, LV_ALIGN_TOP_RIGHT, 0,
                 L.usage_reset_y + (lv_font_get_line_height(L.reset_font) - lv_font_get_line_height(L.today_font)) * 3 / 4);
    lv_obj_add_flag(lbl_spending_today, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_spark(panel_weekly);

    lbl_weekly_pct_sym = lv_label_create(panel_weekly);
    lv_label_set_text(lbl_weekly_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_weekly_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_weekly_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_weekly_pct_sym, LV_OBJ_FLAG_HIDDEN);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    // Text in the geometric face; the spinner glyph keeps the mono font because
    // Styrene has no dingbats. A flex row keeps the pair centered as one unit.
    anim_row = lv_obj_create(usage_container);
    lv_obj_set_size(anim_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(anim_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(anim_row, 0, 0);
    lv_obj_set_style_pad_all(anim_row, 0, 0);
    lv_obj_set_style_pad_column(anim_row, L.small_icons ? 6 : 10, 0);
    lv_obj_set_flex_flow(anim_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(anim_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(anim_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(anim_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(anim_row, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(anim_row, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);

    lbl_anim_glyph = lv_label_create(anim_row);
    lv_label_set_text(lbl_anim_glyph, "");
    lv_obj_set_style_text_font(lbl_anim_glyph, L.anim_glyph_font, 0);
    lv_obj_set_style_text_color(lbl_anim_glyph, COL_ACCENT, 0);

    lbl_anim = lv_label_create(anim_row);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);

    build_attention(usage_container);
    build_approval(usage_container);
}

// ======== Claude Code approval prompt ========
//
// The daemon forwards a pending Claude Code permission request ("ai" id,
// "at" title, "ad" detail) only when the whole request fits on this screen.
// Tapping Allow/Deny notifies "A<id>:1|0" back to the daemon, which hands the
// answer to the waiting PermissionRequest hook. When the request goes away on
// the host side (answered at the keyboard, or timed out) the daemon sends
// ai=0 and the prompt closes. Touch only: the side buttons stay HID keys so a
// stray press can never approve anything.
static lv_obj_t* ap_box = nullptr;
static lv_obj_t* ap_kicker = nullptr;   // "Run a command" (or "Claude Code wants to")
static lv_obj_t* ap_title = nullptr;    // Claude's description, or the action in big type
static lv_obj_t* ap_panel = nullptr;    // scrolls when the command is long
static lv_obj_t* ap_detail = nullptr;
static lv_obj_t* ap_hint = nullptr;
static lv_obj_t* ap_allow_btn = nullptr;
static lv_obj_t* ap_allow_lbl = nullptr;
static bool      ap_locked = false;     // Allow waits until the whole command was on screen
static int       ap_hint_y = 0;
static uint32_t  ap_shown_id = 0;      // request currently on screen
static uint32_t  ap_answered_id = 0;   // answered here; ignore until the host clears it
static bool      ap_visible = false;
static bool      att_visible = false;       // attention screen (defined further down)
static screen_t  ap_prev_screen = SCREEN_USAGE;

static lv_obj_t* make_ap_button(lv_obj_t* parent, int x, int y, int w, int h,
                                const char* text, lv_color_t bg, lv_color_t fg, bool allow);

static void ap_set_locked(bool locked) {
    ap_locked = locked;
    if (!ap_allow_btn) return;
    lv_obj_set_style_bg_color(ap_allow_btn, locked ? COL_BAR_BG : COL_GREEN, 0);
    lv_obj_set_style_text_color(ap_allow_lbl, locked ? COL_DIM : COL_TEXT, 0);
    lv_label_set_text(ap_hint, locked ? "Showing the rest - tap it to skip ahead" : "or answer at your keyboard");
    lv_obj_set_style_text_color(ap_hint, locked ? COL_AMBER : COL_DIM, 0);
    lv_obj_align(ap_hint, LV_ALIGN_TOP_MID, 0, ap_hint_y);
}

// Long commands page through on their own: a screenful at a time, as instant
// jumps (smooth scrolling repaints every frame, which the C6 can't keep up
// with). Allow unlocks once the last page is on screen. A tap on the command
// skips to the next page right away; dragging still works too.
#define AP_PAGE_MS 1400
static lv_timer_t* ap_page_timer = nullptr;

static void ap_page_stop(void) {
    if (ap_page_timer) { lv_timer_delete(ap_page_timer); ap_page_timer = nullptr; }
}

static void ap_page_step(void) {
    if (!ap_panel) return;
    int32_t bottom = lv_obj_get_scroll_bottom(ap_panel);
    if (bottom <= 4) { ap_page_stop(); ap_set_locked(false); return; }
    // keep two lines of overlap so you don't lose your place between pages
    int32_t page = lv_obj_get_content_height(ap_panel) - 2 * (lv_font_get_line_height(&font_mono_18) + 3);
    if (page < 40) page = 40;
    int32_t dy = bottom < page ? bottom : page;
    lv_obj_scroll_to_y(ap_panel, lv_obj_get_scroll_y(ap_panel) + dy, LV_ANIM_OFF);
    if (lv_obj_get_scroll_bottom(ap_panel) <= 4) { ap_page_stop(); ap_set_locked(false); }
}

static void ap_page_timer_cb(lv_timer_t* t) { (void)t; ap_page_step(); }

static void ap_panel_click_cb(lv_event_t* e) {
    (void)e;
    if (!ap_locked) return;
    ap_page_step();
    if (ap_page_timer) lv_timer_reset(ap_page_timer);
}

static void ap_scroll_cb(lv_event_t* e) {
    (void)e;
    if (ap_locked && lv_obj_get_scroll_bottom(ap_panel) <= 4) { ap_page_stop(); ap_set_locked(false); }
}

static void ap_button_cb(lv_event_t* e) {
    bool allow = (bool)(uintptr_t)lv_event_get_user_data(e);
    if (!ap_visible || ap_shown_id == 0) return;
    if (allow && ap_locked) return;      // read it all first
    ble_send_approval(ap_shown_id, allow);
    ap_answered_id = ap_shown_id;
    strlcpy(s_cc_status, allow ? "Allowed" : "Denied", sizeof(s_cc_status));
    approval_hide();
}

static lv_obj_t* make_ap_button(lv_obj_t* parent, int x, int y, int w, int h,
                                const char* text, lv_color_t bg, lv_color_t fg, bool allow) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, ap_button_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)allow);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, L.pct_font, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    if (allow) { ap_allow_btn = b; ap_allow_lbl = l; }
    return b;
}

static void build_approval(lv_obj_t* parent) {
    const int gap = L.small_icons ? 8 : 16;
    const int btn_h = L.small_icons ? 50 : 100;
    const int btn_w = (L.content_w - gap) / 2;
    const int btn_y = L.scr_h - L.margin - btn_h;
    const int top = L.small_icons ? 6 : L.title_y + 6;
    const int title_y = top + (L.small_icons ? 18 : 32);
    const int head_h = 2 * lv_font_get_line_height(L.reset_font) + 4;    // two lines
    const int panel_y = title_y + head_h + (L.small_icons ? 6 : 10);
    const int panel_h = btn_y - panel_y - (L.small_icons ? 22 : 36);

    ap_box = lv_obj_create(parent);
    lv_obj_set_pos(ap_box, 0, 0);
    lv_obj_set_size(ap_box, L.scr_w, L.scr_h);
    lv_obj_set_style_bg_color(ap_box, COL_BG, 0);
    lv_obj_set_style_bg_opa(ap_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ap_box, 0, 0);
    lv_obj_set_style_pad_all(ap_box, 0, 0);
    lv_obj_set_style_radius(ap_box, 0, 0);
    lv_obj_clear_flag(ap_box, LV_OBJ_FLAG_SCROLLABLE);   // stays clickable: swallows stray taps

    ap_kicker = lv_label_create(ap_box);
    lv_label_set_text(ap_kicker, "Claude Code wants to");
    lv_obj_set_style_text_font(ap_kicker, L.today_font, 0);
    lv_obj_set_style_text_color(ap_kicker, COL_DIM, 0);
    lv_obj_align(ap_kicker, LV_ALIGN_TOP_MID, 0, top);

    // Headline: Claude's own description of the command (two lines max), or
    // the action itself in big type when there's no description.
    ap_title = lv_label_create(ap_box);
    lv_label_set_text(ap_title, "");
    lv_obj_set_style_text_font(ap_title, L.reset_font, 0);
    lv_obj_set_style_text_color(ap_title, COL_ACCENT, 0);
    lv_obj_set_style_text_align(ap_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ap_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(ap_title, L.content_w, head_h);
    lv_obj_set_pos(ap_title, L.margin, title_y);

    // The full command in monospace, wrapped; drag to scroll when it's long.
    ap_panel = make_panel(ap_box, L.margin, panel_y, L.content_w, panel_h);
    lv_obj_add_flag(ap_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ap_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ap_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(ap_panel, COL_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ap_panel, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(ap_panel, 4, LV_PART_SCROLLBAR);
    lv_obj_clear_flag(ap_panel, LV_OBJ_FLAG_SCROLL_ELASTIC);   // fewer repaints when dragged
    lv_obj_clear_flag(ap_panel, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(ap_panel, ap_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(ap_panel, ap_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(ap_panel, ap_panel_click_cb, LV_EVENT_CLICKED, NULL);
    ap_detail = lv_label_create(ap_panel);
    lv_label_set_text(ap_detail, "");
    lv_obj_set_style_text_font(ap_detail, &font_mono_18, 0);
    lv_obj_set_style_text_color(ap_detail, COL_TEXT, 0);
    lv_obj_set_style_text_line_space(ap_detail, 3, 0);
    lv_label_set_long_mode(ap_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ap_detail, L.content_w - 2 * L.panel_pad_x - 8);   // room for the scrollbar
    lv_obj_set_pos(ap_detail, 0, 0);

    ap_hint_y = panel_y + panel_h + (L.small_icons ? 4 : 8);
    ap_hint = lv_label_create(ap_box);
    lv_label_set_text(ap_hint, "or answer at your keyboard");
    lv_obj_set_style_text_font(ap_hint, L.pace_font, 0);
    lv_obj_set_style_text_color(ap_hint, COL_DIM, 0);
    lv_obj_align(ap_hint, LV_ALIGN_TOP_MID, 0, ap_hint_y);

    make_ap_button(ap_box, L.margin, btn_y, btn_w, btn_h, "Deny", COL_PANEL, COL_TEXT, false);
    make_ap_button(ap_box, L.margin + btn_w + gap, btn_y, btn_w, btn_h, "Allow", COL_GREEN, COL_TEXT, true);

    lv_obj_add_flag(ap_box, LV_OBJ_FLAG_HIDDEN);
}

static void approval_hide(void) {
    if (!ap_box || !ap_visible) return;
    ap_page_stop();
    lv_obj_add_flag(ap_box, LV_OBJ_FLAG_HIDDEN);
    ap_visible = false;
    ap_shown_id = 0;
    if (ap_prev_screen == SCREEN_SPLASH && current_screen != SCREEN_SPLASH && !att_visible) {
        ui_show_screen(SCREEN_SPLASH);   // go back to where the user was
    }
}

static void approval_update(const UsageData* data) {
    if (!ap_box) return;
    if (data->ap_id == 0) {              // host has no pending request
        ap_answered_id = 0;
        approval_hide();
        return;
    }
    if (data->ap_id == ap_answered_id) return;           // answered here, awaiting clear
    if (ap_visible && data->ap_id == ap_shown_id) return; // already showing it
    if (data->ap_parts && data->ap_got < data->ap_parts) return;   // detail still arriving

    const char* action = data->ap_title[0] ? data->ap_title : "Use a tool";
    if (data->ap_head[0]) {                // "Run a command" / Claude's description
        lv_label_set_text(ap_kicker, action);
        lv_obj_set_style_text_font(ap_title, L.reset_font, 0);
        lv_label_set_text(ap_title, data->ap_head);
    } else {                               // "Claude Code wants to" / "Edit a file"
        lv_label_set_text(ap_kicker, "Claude Code wants to");
        lv_obj_set_style_text_font(ap_title, L.pct_font, 0);
        lv_label_set_text(ap_title, action);
    }
    lv_obj_align(ap_kicker, LV_ALIGN_TOP_MID, 0, L.small_icons ? 6 : L.title_y + 6);
    lv_label_set_text(ap_detail, data->ap_detail);
    ap_shown_id = data->ap_id;
    if (!ap_visible) ap_prev_screen = current_screen;
    if (current_screen != SCREEN_USAGE) ui_show_screen(SCREEN_USAGE);
    lv_obj_clear_flag(ap_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ap_box);
    ap_visible = true;
    // Long command? Start at the top and keep Allow locked until the end was shown.
    lv_obj_scroll_to_y(ap_panel, 0, LV_ANIM_OFF);
    lv_obj_update_layout(ap_box);
    ap_page_stop();
    ap_set_locked(lv_obj_get_scroll_bottom(ap_panel) > 4);
    if (ap_locked) ap_page_timer = lv_timer_create(ap_page_timer_cb, AP_PAGE_MS, NULL);
    idle_note_activity();                // wake the panel for it
    Serial.printf("approval: showing %lu (%s)\n", (unsigned long)ap_shown_id, data->ap_title);
}

bool ui_approval_active(void) {
    return ap_visible;
}

// ======== Attention screen ("Claude needs you" / "Your turn" / "All done!") ========
//
// Shown when Claude Code is blocked on you (a permission prompt that didn't go
// to the touch approval) or has just finished a long turn. Borrows the idle
// screen's mini Clawd. Tap anywhere to dismiss; the host clears it on its own
// when you answer at the keyboard or start the next prompt.
static lv_obj_t* att_box = nullptr;
static lv_obj_t* att_title = nullptr;
static lv_obj_t* att_sub = nullptr;
static lv_obj_t* att_sprite_slot = nullptr;   // where the mini Clawd sits on this screen
static uint8_t   att_kind = 0;
static uint16_t  att_seq_shown = 0, att_seq_dismissed = 0xFFFF;
static int32_t   att_secs = -1;
static uint32_t  att_rx_ms = 0;
static screen_t  att_prev_screen = SCREEN_USAGE;
static char      att_project[24] = "";
static lv_obj_t* att_sprite_home = nullptr;   // the mini's normal parent (idle screen)

static void fmt_dur(char* out, size_t n, int32_t secs) {
    if (secs < 0) { out[0] = '\0'; return; }
    if (secs >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", (long)(secs / 3600), (long)(secs / 60 % 60), (long)(secs % 60));
    else              snprintf(out, n, "%ld:%02ld", (long)(secs / 60), (long)(secs % 60));
}

static void attention_refresh_sub(void) {
    if (!att_sub) return;
    char d[16], line[64];
    if (att_kind == 1) {
        int32_t waited = att_secs < 0 ? -1 : att_secs + (int32_t)((millis() - att_rx_ms) / 1000);
        fmt_dur(d, sizeof(d), waited);
        if (att_project[0]) snprintf(line, sizeof(line), "%s - waiting %s", att_project, d);
        else                snprintf(line, sizeof(line), "waiting %s", d);
    } else {
        fmt_dur(d, sizeof(d), att_secs);
        if (att_project[0] && d[0]) snprintf(line, sizeof(line), "%s - took %s", att_project, d);
        else if (d[0])              snprintf(line, sizeof(line), "took %s", d);
        else                        strlcpy(line, att_project, sizeof(line));
    }
    lv_label_set_text(att_sub, line);
}

static void attention_hide(bool by_tap) {
    if (!att_visible) return;
    att_visible = false;
    if (by_tap) att_seq_dismissed = att_seq_shown;
    lv_obj_add_flag(att_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* mini = splash_mini_obj();
    if (mini && att_sprite_home) {
        lv_obj_set_parent(mini, att_sprite_home);
        lv_obj_align(mini, LV_ALIGN_CENTER, 0, -20);
        splash_mini_set_anim("expression sleep");
    }
    if (att_prev_screen != SCREEN_USAGE && !ap_visible) ui_show_screen(att_prev_screen);
}

static void att_click_cb(lv_event_t* e) {
    (void)e;
    attention_hide(true);
}

static void build_attention(lv_obj_t* parent) {
    att_box = lv_obj_create(parent);
    lv_obj_set_pos(att_box, 0, 0);
    lv_obj_set_size(att_box, L.scr_w, L.scr_h);
    lv_obj_set_style_bg_color(att_box, COL_BG, 0);
    lv_obj_set_style_bg_opa(att_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(att_box, 0, 0);
    lv_obj_set_style_pad_all(att_box, 0, 0);
    lv_obj_set_style_radius(att_box, 0, 0);
    lv_obj_clear_flag(att_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(att_box, att_click_cb, LV_EVENT_CLICKED, NULL);

    att_sprite_slot = lv_obj_create(att_box);           // invisible anchor for the mini Clawd
    lv_obj_set_size(att_sprite_slot, L.idle_px, L.idle_px);
    lv_obj_set_style_bg_opa(att_sprite_slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(att_sprite_slot, 0, 0);
    lv_obj_set_style_pad_all(att_sprite_slot, 0, 0);
    lv_obj_clear_flag(att_sprite_slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(att_sprite_slot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(att_sprite_slot, LV_ALIGN_TOP_MID, 0, L.small_icons ? 20 : 60);

    att_title = lv_label_create(att_box);
    lv_label_set_text(att_title, "");
    lv_obj_set_style_text_font(att_title, L.pct_font, 0);
    lv_obj_set_style_text_color(att_title, COL_ACCENT, 0);
    lv_obj_align(att_title, LV_ALIGN_TOP_MID, 0, (L.small_icons ? 20 : 60) + L.idle_px + (L.small_icons ? 8 : 20));

    att_sub = lv_label_create(att_box);
    lv_label_set_text(att_sub, "");
    lv_obj_set_style_text_font(att_sub, L.reset_font, 0);
    lv_obj_set_style_text_color(att_sub, COL_TEXT, 0);
    lv_obj_align_to(att_sub, att_title, LV_ALIGN_OUT_BOTTOM_MID, 0, L.small_icons ? 4 : 14);

    lv_obj_t* hint = lv_label_create(att_box);
    lv_label_set_text(hint, "tap to dismiss");
    lv_obj_set_style_text_font(hint, L.pace_font, 0);
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, L.small_icons ? -8 : -24);

    lv_obj_add_flag(att_box, LV_OBJ_FLAG_HIDDEN);
}

static void attention_update(const UsageData* data) {
    if (!att_box) return;
    if (data->att == 0) { attention_hide(false); att_kind = 0; return; }
    if (data->att_seq == att_seq_dismissed) return;            // dismissed by a tap
    bool fresh = !att_visible || data->att_seq != att_seq_shown || data->att != att_kind;
    att_kind = data->att;
    att_seq_shown = data->att_seq;
    att_secs = data->att_secs;
    att_rx_ms = data->att_rx_ms;
    strlcpy(att_project, data->att_project, sizeof(att_project));
    if (!fresh) { attention_refresh_sub(); return; }

    const char* title = "Claude needs you";
    const char* anim  = "allow";
    lv_color_t color  = COL_ACCENT;
    if (att_kind == 2) { title = "Your turn";  anim = "done";         color = COL_TEXT; }
    if (att_kind == 3) { title = "All done!";  anim = "dance bounce"; color = COL_GREEN; }
    lv_label_set_text(att_title, title);
    lv_obj_set_style_text_color(att_title, color, 0);
    lv_obj_align(att_title, LV_ALIGN_TOP_MID, 0, (L.small_icons ? 20 : 60) + L.idle_px + (L.small_icons ? 8 : 20));
    attention_refresh_sub();
    lv_obj_align_to(att_sub, att_title, LV_ALIGN_OUT_BOTTOM_MID, 0, L.small_icons ? 4 : 14);

    lv_obj_t* mini = splash_mini_obj();
    if (mini) {
        if (!att_sprite_home) att_sprite_home = lv_obj_get_parent(mini);
        lv_obj_set_parent(mini, att_sprite_slot);
        lv_obj_center(mini);
        splash_mini_set_anim(anim);
    }
    if (!att_visible) att_prev_screen = current_screen;
    if (current_screen != SCREEN_USAGE) ui_show_screen(SCREEN_USAGE);
    lv_obj_clear_flag(att_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(att_box);
    if (ap_visible && ap_box) lv_obj_move_foreground(ap_box);   // approval stays on top
    att_visible = true;
    idle_note_activity();
    Serial.printf("attention: %u (%s)\n", (unsigned)att_kind, title);
}

static void attention_tick(void) {
    if (!att_visible) return;
    splash_mini_tick();
    static uint32_t last = 0;
    if (att_kind == 1 && millis() - last >= 1000) {   // the "waiting 0:42" counter
        last = millis();
        attention_refresh_sub();
    }
}

bool ui_overlay_active(void) {
    return ap_visible || att_visible;
}

// ======== Month chart in the Period panel (Enterprise) ========
//
// One column per day of the billing month, height = that day's Claude Code
// spend relative to the busiest day. Days so far are drawn in the pace color
// (today brightest); days still to come are short gray stubs, so the row also
// shows how far into the month you are.
#define SPARK_MAX 31
static lv_obj_t* spark_box = nullptr;
static lv_obj_t* spark_col[SPARK_MAX];
static int16_t   spark_h = 0;

static void build_spark(lv_obj_t* panel) {
    int w = L.content_w - 2 * L.panel_pad_x;
    spark_h = L.bar_h + (L.small_icons ? 4 : 12);
    spark_box = lv_obj_create(panel);
    lv_obj_set_size(spark_box, w, spark_h);
    lv_obj_set_pos(spark_box, 0, L.usage_bar_y - (spark_h - L.bar_h) / 2);
    lv_obj_set_style_bg_opa(spark_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spark_box, 0, 0);
    lv_obj_set_style_pad_all(spark_box, 0, 0);
    lv_obj_clear_flag(spark_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(spark_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spark_box, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < SPARK_MAX; i++) {
        spark_col[i] = lv_obj_create(spark_box);
        lv_obj_set_style_border_width(spark_col[i], 0, 0);
        lv_obj_set_style_pad_all(spark_col[i], 0, 0);
        lv_obj_set_style_radius(spark_col[i], 2, 0);
        lv_obj_clear_flag(spark_col[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(spark_col[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(spark_col[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(spark_col[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(spark_box, LV_OBJ_FLAG_HIDDEN);
}

static void spark_update(const UsageData* data, lv_color_t pace) {
    if (!spark_box) return;
    if (!data || !data->enterprise || !data->stats_valid || data->hist_n == 0) {
        lv_obj_add_flag(spark_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_weekly, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(bar_weekly, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(spark_box, LV_OBJ_FLAG_HIDDEN);
    int n = data->hist_n > SPARK_MAX ? SPARK_MAX : data->hist_n;
    int w = L.content_w - 2 * L.panel_pad_x;
    int slot = w / n;
    int gap = slot >= 8 ? 3 : 1;
    int today = data->hist_today;
    for (int i = 0; i < SPARK_MAX; i++) {
        if (i >= n) { lv_obj_add_flag(spark_col[i], LV_OBJ_FLAG_HIDDEN); continue; }
        bool future = today >= 0 && i > today;
        int h = future ? 3 : 3 + (spark_h - 3) * data->hist[i] / 100;
        lv_obj_set_size(spark_col[i], slot - gap, h);
        lv_obj_set_pos(spark_col[i], i * slot, spark_h - h);
        lv_obj_set_style_bg_color(spark_col[i], future ? COL_BAR_BG : pace, 0);
        lv_obj_set_style_bg_opa(spark_col[i], (future || i == today) ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_clear_flag(spark_col[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ======== Today screen (tap: splash -> usage -> today -> splash) ========
static lv_obj_t* st_num[4];
static lv_obj_t* st_foot = nullptr;

static void init_stats_screen(lv_obj_t* scr) {
    stats_container = lv_obj_create(scr);
    lv_obj_set_size(stats_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(stats_container, 0, 0);
    lv_obj_set_style_bg_color(stats_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(stats_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stats_container, 0, 0);
    lv_obj_set_style_pad_all(stats_container, 0, 0);
    lv_obj_set_style_radius(stats_container, 0, 0);
    lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(stats_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(stats_container);
    lv_label_set_text(title, "Today");
    lv_obj_set_style_text_font(title, L.title_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    static const char* labels[4] = {"tool calls", "files edited", "prompts", "approved here"};
    const int gap = L.small_icons ? 6 : 16;
    const int pw = (L.content_w - gap) / 2;
    const int ph = L.small_icons ? 70 : 130;
    for (int i = 0; i < 4; i++) {
        int x = L.margin + (i % 2) * (pw + gap);
        int y = L.content_y + (i / 2) * (ph + gap);
        lv_obj_t* p = make_panel(stats_container, x, y, pw, ph);
        lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
        st_num[i] = lv_label_create(p);
        lv_label_set_text(st_num[i], "--");
        lv_obj_set_style_text_font(st_num[i], L.ent_pct_font, 0);
        lv_obj_set_style_text_color(st_num[i], COL_TEXT, 0);
        lv_obj_set_pos(st_num[i], 0, 0);
        lv_obj_t* l = lv_label_create(p);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, L.today_font, 0);
        lv_obj_set_style_text_color(l, COL_DIM, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    st_foot = lv_label_create(stats_container);
    lv_label_set_text(st_foot, "");
    lv_obj_set_style_text_font(st_foot, L.reset_font, 0);
    lv_obj_set_style_text_color(st_foot, COL_ACCENT, 0);
    lv_obj_align(st_foot, LV_ALIGN_TOP_MID, 0, L.content_y + 2 * ph + gap + (L.small_icons ? 8 : 22));

    lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);
}

static void stats_update(const UsageData* data) {
    if (!stats_container || !data->stats_valid) return;
    const int32_t v[4] = {data->k_tools, data->k_files, data->k_prompts, data->k_approvals};
    for (int i = 0; i < 4; i++) {
        if (v[i] < 0) lv_label_set_text(st_num[i], "--");
        else          lv_label_set_text_fmt(st_num[i], "%ld", (long)v[i]);
    }
    char foot[64] = "";
    if (data->k_minutes >= 0) {
        long m = data->k_minutes;
        if (m >= 60) snprintf(foot, sizeof(foot), "%ldh %02ldm with Claude", m / 60, m % 60);
        else         snprintf(foot, sizeof(foot), "%ldm with Claude", m);
    }
    if (data->k_streak > 1) {
        size_t n = strlen(foot);
        snprintf(foot + n, sizeof(foot) - n, "%s%ld-day streak", n ? " - " : "", (long)data->k_streak);
    }
    lv_label_set_text(st_foot, foot);
    lv_obj_align(st_foot, LV_ALIGN_TOP_MID, 0,
                 L.content_y + 2 * (L.small_icons ? 70 : 130) + (L.small_icons ? 6 : 16) + (L.small_icons ? 8 : 22));
}

// ======== Max screen (tap: splash -> usage -> max -> today -> splash) ========
// The personal Max plan, polled by the host with a second Claude Code login.
// Same panels as the Pro/Max usage view, plus a thin marker on each bar showing
// how far into the window you are: fill past the marker = ahead of pace.
struct MaxPanel {
    lv_obj_t* pct;
    lv_obj_t* sym;
    lv_obj_t* pill;
    lv_obj_t* bar;
    lv_obj_t* reset;
    lv_obj_t* tick;
};
static MaxPanel mx[2];
static lv_obj_t* mx_foot = nullptr;
static uint32_t  mx_last_rx = 0;
static int       mx_foot_min = -2;          // last minute value the footer was drawn for
static uint8_t   mx_err = 0;
static int16_t   mx_m = -1;
static char      mx_mn[12] = "";

static void init_max_screen(lv_obj_t* scr) {
    max_container = lv_obj_create(scr);
    lv_obj_set_size(max_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(max_container, 0, 0);
    lv_obj_set_style_bg_color(max_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(max_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(max_container, 0, 0);
    lv_obj_set_style_pad_all(max_container, 0, 0);
    lv_obj_set_style_radius(max_container, 0, 0);
    lv_obj_clear_flag(max_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(max_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(max_container);
    lv_label_set_text(title, "Max");
    lv_obj_set_style_text_font(title, L.title_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    static const char* pills[2] = {"Current", "Weekly"};
    for (int i = 0; i < 2; i++) {
        int y = L.content_y + i * (L.usage_panel_h + L.usage_panel_gap);
        lv_obj_t* p = make_usage_panel(max_container, y, pills[i],
                                       &mx[i].pct, &mx[i].pill, &mx[i].bar, &mx[i].reset);
        mx[i].sym = lv_label_create(p);
        lv_label_set_text(mx[i].sym, "%");
        lv_obj_set_style_text_font(mx[i].sym, L.reset_font, 0);
        lv_obj_set_style_text_color(mx[i].sym, COL_TEXT, 0);
        lv_obj_add_flag(mx[i].sym, LV_OBJ_FLAG_HIDDEN);

        mx[i].tick = lv_obj_create(p);           // pace marker, drawn over the bar
        lv_obj_set_size(mx[i].tick, 3, L.bar_h + 10);
        lv_obj_set_style_bg_color(mx[i].tick, COL_TEXT, 0);
        lv_obj_set_style_bg_opa(mx[i].tick, LV_OPA_80, 0);
        lv_obj_set_style_border_width(mx[i].tick, 0, 0);
        lv_obj_set_style_radius(mx[i].tick, 1, 0);
        lv_obj_set_style_pad_all(mx[i].tick, 0, 0);
        lv_obj_clear_flag(mx[i].tick, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mx[i].tick, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(mx[i].tick, LV_OBJ_FLAG_HIDDEN);
    }

    mx_foot = lv_label_create(max_container);
    lv_label_set_text(mx_foot, "");
    lv_obj_set_style_text_font(mx_foot, L.today_font, 0);
    lv_obj_set_style_text_color(mx_foot, COL_DIM, 0);
    lv_obj_align(mx_foot, LV_ALIGN_TOP_MID, 0,
                 L.content_y + 2 * L.usage_panel_h + L.usage_panel_gap + (L.small_icons ? 4 : 16));

    lv_obj_add_flag(max_container, LV_OBJ_FLAG_HIDDEN);
}

// One panel: number + "%", colored bar, reset line, pace marker at
// (window - mins_left) / window along the bar.
static void max_panel_set(MaxPanel& m, bool ok, float pct, int reset_mins, int window_mins) {
    char buf[40];
    if (!ok) {
        lv_label_set_text(m.pct, "--");
        lv_obj_add_flag(m.sym, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(m.bar, 0, LV_ANIM_OFF);
        lv_label_set_text(m.reset, "---");
        lv_obj_add_flag(m.tick, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int p = (int)(pct + 0.5f);
    if (p > 100) p = 100;
    lv_label_set_text_fmt(m.pct, "%d", p);
    lv_obj_clear_flag(m.sym, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(m.sym, m.pct, LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    lv_bar_set_value(m.bar, p, LV_ANIM_ON);
    lv_obj_set_style_bg_color(m.bar, pct_color(pct), LV_PART_INDICATOR);
    format_reset_time(reset_mins, buf, sizeof(buf));
    lv_label_set_text(m.reset, buf);
    if (reset_mins >= 0 && reset_mins <= window_mins) {
        const int bar_w = L.content_w - 2 * L.panel_pad_x;
        int x = (int)((long)bar_w * (window_mins - reset_mins) / window_mins);
        if (x < 2) x = 2;
        if (x > bar_w - 2) x = bar_w - 2;
        lv_obj_set_pos(m.tick, x - 1, L.usage_bar_y - 5);
        lv_obj_clear_flag(m.tick, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m.tick, LV_OBJ_FLAG_HIDDEN);
    }
}

static void max_update(const UsageData* data) {
    if (!max_container || !data->max_valid || data->max_rx_ms == mx_last_rx) return;
    mx_last_rx = data->max_rx_ms;
    s_max_seen = true;
    mx_err = data->max_err;
    mx_m = data->max_m;
    strlcpy(mx_mn, data->max_mn, sizeof(mx_mn));
    bool ok = data->max_err == 0;
    max_panel_set(mx[0], ok, data->max_s, data->max_sr, 300);       // 5-hour window
    max_panel_set(mx[1], ok, data->max_w, data->max_wr, 7 * 1440);  // 7-day window
    mx_foot_min = -2;   // redraw the footer on the next tick
    max_tick();
}

// Footer: errors first, then staleness, then the optional per-model limit.
// Only relabels when the minute-age bucket changes.
static void max_tick(void) {
    if (!mx_foot || !s_max_seen) return;
    int age_min = (int)((millis() - mx_last_rx) / 60000);
    int bucket = age_min >= 5 ? age_min : -1;
    if (bucket == mx_foot_min) return;
    mx_foot_min = bucket;
    char buf[48];
    lv_color_t col = COL_DIM;
    if (mx_err == 1)      { strlcpy(buf, "Not signed in - run connect-max", sizeof(buf)); col = COL_AMBER; }
    else if (mx_err == 2) { strlcpy(buf, "Sign-in expired - run connect-max", sizeof(buf)); col = COL_AMBER; }
    else if (mx_err == 3) { strlcpy(buf, "Can't reach Claude - retrying", sizeof(buf)); }
    else if (bucket >= 0) {
        if (age_min >= 60) snprintf(buf, sizeof(buf), "No update for %dh %dm", age_min / 60, age_min % 60);
        else               snprintf(buf, sizeof(buf), "No update for %dm", age_min);
        col = COL_AMBER;
    }
    else if (mx_m >= 0)   snprintf(buf, sizeof(buf), "%s only: %d%% this week", mx_mn[0] ? mx_mn : "Model", (int)mx_m);
    else                  strlcpy(buf, "Personal plan - all Claude apps", sizeof(buf));
    lv_label_set_text(mx_foot, buf);
    lv_obj_set_style_text_color(mx_foot, col, 0);
    lv_obj_align(mx_foot, LV_ALIGN_TOP_MID, 0,
                 L.content_y + 2 * L.usage_panel_h + L.usage_panel_gap + (L.small_icons ? 4 : 16));
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, LOGO_SMALL_WIDTH, LOGO_SMALL_HEIGHT, logo_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_stats_screen(scr);
    init_max_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.logo_y);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }

    // Last, so the charge overlay covers everything else when it plays.
    charge_anim_init(scr);
}

void ui_update(const UsageData* data) {
    max_update(data);               // its own message kind; may land before the first usage
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    s_cc_live = data->cc_live;
    strlcpy(s_cc_status, data->cc_status, sizeof(s_cc_status));
    s_turn_start_ms = data->turn_start_ms;
    strlcpy(s_project, data->project, sizeof(s_project));
    s_sessions = data->sessions;
    stats_update(data);
    logo_anim_apply(logo_wanted_mode());

    // Enterprise: show real dollars under the Spending % when the host knows them.
    if (lbl_spending_desc) {
        char dbuf[40];
        long cents = lroundf(data->spent_usd * 100.0f);
        if (data->spent_usd >= 0.0f && data->limit_usd > 0.0f) {
            snprintf(dbuf, sizeof(dbuf), "$%ld.%02ld / $%d",
                     cents / 100, cents % 100, (int)(data->limit_usd + 0.5f));
        } else if (data->spent_usd >= 0.0f) {
            snprintf(dbuf, sizeof(dbuf), "$%ld.%02ld this period", cents / 100, cents % 100);
        } else {
            strlcpy(dbuf, "of your monthly budget", sizeof(dbuf));
        }
        lv_label_set_text(lbl_spending_desc, dbuf);
    }
    if (lbl_spending_today) {
        if (data->enterprise && data->today_usd >= 0.0f) {
            long tc = lroundf(data->today_usd * 100.0f);
            lv_label_set_text_fmt(lbl_spending_today, "$%ld.%02ld today", tc / 100, tc % 100);
            lv_obj_clear_flag(lbl_spending_today, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_spending_today, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: serif number + small "%" symbol + desc + pace
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    // Both headline numbers: serif digits with a small geometric "%" beside them.
    if (data->enterprise) {
        // Live spend estimate: one decimal (tenths as integers, no float printf).
        int tenths = (int)lroundf(data->session_pct * 10.0f);
        lv_label_set_text_fmt(lbl_session_pct, "%d.%d", tenths / 10, tenths % 10);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
    }
    lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct, LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    if (!data->enterprise) {
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        spark_update(data, bar_pace);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        spark_update(nullptr, COL_GREEN);   // Pro/Max: keep the plain bar
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
    lv_obj_clear_flag(lbl_weekly_pct_sym, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(lbl_weekly_pct_sym, lbl_weekly_pct, LV_ALIGN_OUT_RIGHT_TOP, 4, 12);

    attention_update(data);
    approval_update(data);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

static uint32_t flash_until = 0;          // ui_flash_splash()/ui_toast(): when to go back
static screen_t flash_prev = SCREEN_USAGE;
static screen_t flash_shown = SCREEN_SPLASH;
static uint32_t toast_until = 0;
static char     toast_text[32] = "";

void ui_tick_anim(void) {
    if (flash_until && (int32_t)(lv_tick_get() - flash_until) >= 0) {
        flash_until = 0;
        if (current_screen == flash_shown && flash_prev != flash_shown && !ui_overlay_active())
            ui_show_screen(flash_prev);
    }
    attention_tick();
    if (current_screen == SCREEN_MAX) max_tick();
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        // Alternate the clock with the "Usage" title every 6 s; only touch the
        // label when what it shows actually changes.
        static int title_phase = -1;
        int phase = (int)((now / 6000) % 2);
        if (phase == 1) {
            if (title_phase != 1) { lv_label_set_text(lbl_title, "Usage"); title_phase = 1; }
        } else if (tmv.tm_min != clock_last_min || title_phase != 0) {
            title_phase = 0;
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else if (toast_until && (int32_t)(now - toast_until) < 0) {
        set_status_line(spinner_frames[0], toast_text);  // e.g. "Sound: medium"
        return;
    } else if (s_cc_live) {
        // Real Claude Code state from the host's hooks. The host appends "…" to
        // states that are in progress; settled ones (Done, Idle, Needs you) get a
        // still glyph instead of the spinner so the line doesn't look busy.
        // While busy the line also carries the turn timer; every other 4 s
        // slot it shows which project (and how many other sessions) instead.
        const char* st = s_cc_status[0] ? s_cc_status : "Idle";
        bool busy = strstr(st, "\xE2\x80\xA6") != NULL;
        static char line[48];
        if ((anim_msg_idx & 1) && s_project[0]) {
            if (s_sessions > 1) snprintf(line, sizeof(line), "%s +%u", s_project, (unsigned)(s_sessions - 1));
            else                strlcpy(line, s_project, sizeof(line));
        } else if (busy && s_turn_start_ms) {
            uint32_t secs = (millis() - s_turn_start_ms) / 1000;
            if (secs >= 3600) snprintf(line, sizeof(line), "%s %lu:%02lu:%02lu", st,
                                       (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
            else              snprintf(line, sizeof(line), "%s %lu:%02lu", st,
                                       (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        } else {
            strlcpy(line, st, sizeof(line));
        }
        set_status_line(busy ? spinner_frames[anim_spinner_idx] : spinner_frames[0], line);
        return;
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>..."
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s...", text);
    set_status_line(spinner_frames[anim_spinner_idx], buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (ui_overlay_active()) return;     // never flip screens under a prompt
    flash_until = 0;
    if (current_screen == SCREEN_SPLASH)     ui_show_screen(SCREEN_USAGE);
    else if (current_screen == SCREEN_USAGE) ui_show_screen(s_max_seen ? SCREEN_MAX : SCREEN_STATS);
    else if (current_screen == SCREEN_MAX)   ui_show_screen(SCREEN_STATS);
    else                                     ui_show_screen(SCREEN_SPLASH);
}

void ui_flash_splash(const char* anim, uint32_t ms) {
    if (ui_overlay_active()) return;
    if (current_screen != SCREEN_SPLASH) {
        flash_prev = current_screen;
        flash_shown = SCREEN_SPLASH;
        flash_until = lv_tick_get() + ms;
        ui_show_screen(SCREEN_SPLASH);
    }
    splash_play_temp(anim, ms);
}

void ui_toast(const char* text, uint32_t ms) {
    strlcpy(toast_text, text, sizeof(toast_text));
    toast_until = lv_tick_get() + ms;
    if (ui_overlay_active()) return;
    if (current_screen != SCREEN_USAGE) {
        if (!flash_until) flash_prev = current_screen;   // keep the original screen on repeats
        flash_shown = SCREEN_USAGE;
        flash_until = lv_tick_get() + ms;
        ui_show_screen(SCREEN_USAGE);
    } else if (flash_until && flash_shown == SCREEN_USAGE) {
        flash_until = lv_tick_get() + ms;               // repeated presses extend the toast
    }
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (stats_container) lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);
    if (max_container) lv_obj_add_flag(max_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_MAX:     if (max_container) lv_obj_clear_flag(max_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_STATS:   if (stats_container) lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
    logo_anim_apply(logo_wanted_mode());
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    if (!s_ble_connected) approval_hide();   // can't answer without a link
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}

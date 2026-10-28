#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_MAX,      // second account (personal Max plan); in the cycle once seen
    SCREEN_STATS,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
bool ui_approval_active(void);   // an approval prompt is on screen
bool ui_overlay_active(void);    // approval or attention screen is up (don't flip screens)
// Show `anim` on the splash for `ms` (switching to the splash and back if
// needed) — used for motion reactions. Ignored while an overlay is up.
void ui_flash_splash(const char* anim, uint32_t ms);
// Briefly show `text` on the status line (switching to the Usage screen and
// back if needed) — e.g. "Sound: medium" after a volume change.
void ui_toast(const char* text, uint32_t ms);

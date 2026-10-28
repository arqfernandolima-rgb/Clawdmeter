#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    char anim[24];           // splash animation the host wants shown ("" = host
                             // has no opinion, device picks by usage rate)
    char cc_status[24];      // live Claude Code status text from the host
                             // ("" = host sent none; see ui_tick_anim)
    bool cc_live;            // host speaks the status protocol ("cs" key present)
    float spent_usd;         // Enterprise: dollars spent this period (<0 = unknown)
    float limit_usd;         // Enterprise: period spend limit in dollars (<0 = unknown)
    float today_usd;         // Enterprise: today's Claude Code spend estimate (<0 = unknown)
    bool status_only;        // payload is a status push, not a fresh usage sample
    uint32_t ap_id;          // pending Claude Code approval request id (0 = none)
    char ap_title[32];       // e.g. "Run a command"
    char ap_detail[160];     // e.g. "npm test" (only sent when it fits the screen)
    // ---- live session info (status messages) ----
    uint32_t turn_start_ms;  // millis() when the current Claude Code turn began (0 = none)
    char project[24];        // folder of the most recent session, e.g. "nexus-v2"
    uint8_t sessions;        // Claude Code sessions active right now
    uint8_t att;             // attention screen: 0 none, 1 needs you, 2 your turn, 3 celebrate
    uint16_t att_seq;        // bumps per new attention event (dismissal is per seq)
    int32_t att_secs;        // needs-you: waited so far / your-turn: how long the turn took
    uint32_t att_rx_ms;      // millis() when att_secs arrived (needs-you counts up from it)
    char att_project[24];    // the session the attention screen is about
    uint8_t snd;             // sound to play: 1 attention, 2 ding, 3 fanfare
    uint16_t snd_seq;        // bumps per sound request
    // ---- stats messages ----
    bool stats_valid;
    uint8_t hist[31];        // daily spend this month, 0..100 relative to the busiest day
    uint8_t hist_n;          // days in the period
    int8_t hist_today;       // index of today in hist (-1 unknown)
    float hist_max_usd;      // dollars on the busiest day (scale for hist)
    int32_t k_tools, k_files, k_prompts, k_approvals, k_minutes, k_streak;
    // ---- second account (personal Max plan), "hm" messages ----
    bool max_valid;          // the host has a Max account configured (any "hm" seen)
    uint8_t max_err;         // 0 ok, 1 not signed in, 2 sign-in expired, 3 offline
    float max_s;             // 5-hour session utilization 0-100
    int max_sr;              // minutes until the session resets
    float max_w;             // weekly utilization 0-100
    int max_wr;              // minutes until the weekly reset
    int16_t max_m;           // optional per-model weekly limit 0-100 (-1 = none)
    int max_mr;              // minutes until that one resets
    char max_mn[12];         // its name, e.g. "Sonnet"
    uint32_t max_rx_ms;      // millis() when the last "hm" landed
    // ---- settings carried on usage messages ----
    uint8_t volume;          // 0..100 speaker volume
    int8_t facedown_sign;    // +1/-1: accelerometer z sign that means "screen down"
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

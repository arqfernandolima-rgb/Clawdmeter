#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);
// Answer a Claude Code approval request: notifies "A<id>:1" (allow) or "A<id>:0"
// (deny) on the request characteristic the daemon already subscribes to.
void ble_send_approval(uint32_t id, bool allow, bool always = false);   // "A<id>:0|1|2"
// Tell the host the board went face-down ("D1") or back up ("D0").
void ble_send_dnd(bool on);
// Tell the host the speaker volume was changed on the board ("V0".."V100").
void ble_send_volume(uint8_t volume);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);

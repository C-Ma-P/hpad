#ifndef HPADV2_BLE_HID_H_
#define HPADV2_BLE_HID_H_

#include <stdbool.h>
#include <stdint.h>

enum ble_hid_state {
	BLE_HID_STATE_INACTIVE = 0,
	BLE_HID_STATE_STARTING,
	BLE_HID_STATE_ADVERTISING,
	BLE_HID_STATE_CONNECTED,
	BLE_HID_STATE_SECURITY_FAILED,
	BLE_HID_STATE_STOPPING,
	BLE_HID_STATE_ERROR,
};

int ble_hid_start(void);
int ble_hid_stop(void);
int ble_hid_forget_pairing(void);
int ble_hid_send_key_state(uint8_t keys);
int ble_hid_send_key_tap(uint8_t usage);
bool ble_hid_is_connected(void);
enum ble_hid_state ble_hid_get_state(void);

#define BLE_HID_USAGE_KEYBOARD_ENTER 0x28
#define BLE_HID_USAGE_KEYBOARD_UP_ARROW 0x52
#define BLE_HID_USAGE_KEYBOARD_DOWN_ARROW 0x51

#endif

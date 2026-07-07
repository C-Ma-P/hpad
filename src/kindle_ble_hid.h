#ifndef HPADV2_KINDLE_BLE_HID_H_
#define HPADV2_KINDLE_BLE_HID_H_

#include <stdbool.h>
#include <stdint.h>

enum kindle_ble_hid_state {
	KINDLE_BLE_HID_STATE_INACTIVE = 0,
	KINDLE_BLE_HID_STATE_STARTING,
	KINDLE_BLE_HID_STATE_ADVERTISING,
	KINDLE_BLE_HID_STATE_CONNECTED,
	KINDLE_BLE_HID_STATE_SECURITY_FAILED,
	KINDLE_BLE_HID_STATE_STOPPING,
	KINDLE_BLE_HID_STATE_ERROR,
};

int kindle_ble_hid_start(void);
int kindle_ble_hid_stop(void);
int kindle_ble_hid_forget_pairing(void);
int kindle_ble_hid_send_key_state(uint8_t keys);
int kindle_ble_hid_send_key_tap(uint8_t usage);
bool kindle_ble_hid_is_connected(void);
enum kindle_ble_hid_state kindle_ble_hid_get_state(void);

#define KINDLE_BLE_HID_USAGE_KEYBOARD_ENTER 0x28
#define KINDLE_BLE_HID_USAGE_KEYBOARD_UP_ARROW 0x52
#define KINDLE_BLE_HID_USAGE_KEYBOARD_DOWN_ARROW 0x51

#endif

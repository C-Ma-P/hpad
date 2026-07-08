#ifndef HPADV2_DESKTOP_BLE_CONSUMER_HID_H_
#define HPADV2_DESKTOP_BLE_CONSUMER_HID_H_

#include <stdbool.h>
#include <stdint.h>

enum desktop_ble_consumer_hid_state {
	DESKTOP_BLE_CONSUMER_HID_STATE_INACTIVE = 0,
	DESKTOP_BLE_CONSUMER_HID_STATE_STARTING,
	DESKTOP_BLE_CONSUMER_HID_STATE_ADVERTISING,
	DESKTOP_BLE_CONSUMER_HID_STATE_CONNECTED,
	DESKTOP_BLE_CONSUMER_HID_STATE_SECURITY_FAILED,
	DESKTOP_BLE_CONSUMER_HID_STATE_STOPPING,
	DESKTOP_BLE_CONSUMER_HID_STATE_ERROR,
};

int desktop_ble_consumer_hid_start(void);
int desktop_ble_consumer_hid_stop(void);
int desktop_ble_consumer_hid_forget_pairing(void);
int desktop_ble_consumer_hid_trigger_action(uint16_t action_id);
bool desktop_ble_consumer_hid_is_connected(void);
enum desktop_ble_consumer_hid_state desktop_ble_consumer_hid_get_state(void);

#endif

#ifndef HPADV2_BLE_HID_H_
#define HPADV2_BLE_HID_H_

#include <stdbool.h>
#include <stdint.h>

int ble_hid_start(void);
int ble_hid_stop(void);
int ble_hid_send_key_state(uint8_t keys);
bool ble_hid_is_connected(void);

#endif

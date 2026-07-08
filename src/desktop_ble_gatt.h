#ifndef HPADV2_DESKTOP_BLE_GATT_H_
#define HPADV2_DESKTOP_BLE_GATT_H_

#include <stdbool.h>
#include <stdint.h>

#include "wire_protocol.h"

enum desktop_ble_gatt_state {
	DESKTOP_BLE_GATT_STATE_INACTIVE = 0,
	DESKTOP_BLE_GATT_STATE_STARTING,
	DESKTOP_BLE_GATT_STATE_ADVERTISING,
	DESKTOP_BLE_GATT_STATE_CONNECTED,
	DESKTOP_BLE_GATT_STATE_STOPPING,
	DESKTOP_BLE_GATT_STATE_ERROR,
};

typedef void (*desktop_ble_gatt_config_handler_t)(const macropad_config_t *config);

void desktop_ble_gatt_set_config_handler(desktop_ble_gatt_config_handler_t handler);
int desktop_ble_gatt_start(void);
int desktop_ble_gatt_stop(void);
int desktop_ble_gatt_send_report(const macropad_report_t *report);
bool desktop_ble_gatt_is_connected(void);
enum desktop_ble_gatt_state desktop_ble_gatt_get_state(void);

#endif

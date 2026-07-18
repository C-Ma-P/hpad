#ifndef HPADV2_DESKTOP_BLE_TRANSPORT_H_
#define HPADV2_DESKTOP_BLE_TRANSPORT_H_

#include <stdbool.h>
#include <stdint.h>

#include "wire_protocol.h"

enum desktop_ble_transport_state {
	DESKTOP_BLE_TRANSPORT_STATE_INACTIVE = 0,
	DESKTOP_BLE_TRANSPORT_STATE_STARTING,
	DESKTOP_BLE_TRANSPORT_STATE_ADVERTISING,
	DESKTOP_BLE_TRANSPORT_STATE_CONNECTED,
	DESKTOP_BLE_TRANSPORT_STATE_STOPPING,
	DESKTOP_BLE_TRANSPORT_STATE_ERROR,
};

typedef void (*desktop_ble_transport_config_handler_t)(const macropad_config_t *config);

void desktop_ble_transport_set_config_handler(desktop_ble_transport_config_handler_t handler);
int desktop_ble_transport_start(void);
int desktop_ble_transport_stop(void);
int desktop_ble_transport_forget_pairing(void);
int desktop_ble_transport_send_report(const macropad_report_t *report);
bool desktop_ble_transport_is_connected(void);
bool desktop_ble_transport_get_retry_status(uint8_t *attempt, uint8_t *seconds_remaining);
enum desktop_ble_transport_state desktop_ble_transport_get_state(void);

#endif

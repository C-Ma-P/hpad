#ifndef HPADV2_DESKTOP_BLE_PROTOCOL_H_
#define HPADV2_DESKTOP_BLE_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>

#include "wire_protocol.h"

#define HPAD_DESKTOP_BLE_PROTOCOL_VERSION 1U
#define HPAD_DESKTOP_BLE_CAPABILITY_LED_CONFIG BIT(0)
#define HPAD_DESKTOP_BLE_CAPABILITIES HPAD_DESKTOP_BLE_CAPABILITY_LED_CONFIG
#define HPAD_DESKTOP_BLE_PROTOCOL_SIZE 2U
#define HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE HPAD_PROTOCOL_MACROPAD_REPORT_SIZE
#define HPAD_DESKTOP_BLE_CONFIG_SIZE HPAD_PROTOCOL_CONFIG_REPORT_SIZE

#define HPAD_DESKTOP_BLE_SERVICE_UUID \
	"6f7d7a10-5d57-4a0d-9f0d-484150440001"
#define HPAD_DESKTOP_BLE_PROTOCOL_UUID \
	"6f7d7a11-5d57-4a0d-9f0d-484150440001"
#define HPAD_DESKTOP_BLE_INPUT_UUID \
	"6f7d7a12-5d57-4a0d-9f0d-484150440001"
#define HPAD_DESKTOP_BLE_CONFIG_UUID \
	"6f7d7a13-5d57-4a0d-9f0d-484150440001"

#define HPAD_DESKTOP_BLE_SERVICE_UUID_VAL \
	BT_UUID_128_ENCODE(0x6f7d7a10, 0x5d57, 0x4a0d, 0x9f0d, 0x484150440001)
#define HPAD_DESKTOP_BLE_PROTOCOL_UUID_VAL \
	BT_UUID_128_ENCODE(0x6f7d7a11, 0x5d57, 0x4a0d, 0x9f0d, 0x484150440001)
#define HPAD_DESKTOP_BLE_INPUT_UUID_VAL \
	BT_UUID_128_ENCODE(0x6f7d7a12, 0x5d57, 0x4a0d, 0x9f0d, 0x484150440001)
#define HPAD_DESKTOP_BLE_CONFIG_UUID_VAL \
	BT_UUID_128_ENCODE(0x6f7d7a13, 0x5d57, 0x4a0d, 0x9f0d, 0x484150440001)

void hpad_desktop_ble_encode_protocol(uint8_t payload[HPAD_DESKTOP_BLE_PROTOCOL_SIZE]);
void hpad_desktop_ble_encode_input_report(const macropad_report_t *report,
					  uint8_t payload[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE]);
bool hpad_desktop_ble_decode_config(const uint8_t *payload, size_t len,
				    macropad_config_t *config);

#endif

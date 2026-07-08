#include "desktop_ble_protocol.h"

#include <string.h>

void hpad_desktop_ble_encode_protocol(uint8_t payload[HPAD_DESKTOP_BLE_PROTOCOL_SIZE])
{
	payload[0] = HPAD_DESKTOP_BLE_PROTOCOL_VERSION;
	payload[1] = HPAD_DESKTOP_BLE_CAPABILITIES;
}

void hpad_desktop_ble_encode_input_report(const macropad_report_t *report,
					  uint8_t payload[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE])
{
	payload[0] = report->keys;
	payload[1] = (uint8_t)report->encoder_delta;
	payload[2] = report->encoder_pressed != 0U ? 1U : 0U;
	payload[3] = (uint8_t)(report->battery_mv & 0xffU);
	payload[4] = (uint8_t)(report->battery_mv >> 8);
	payload[5] = report->usb_power_present != 0U ? 1U : 0U;
}

bool hpad_desktop_ble_decode_config(const uint8_t *payload, size_t len,
				    macropad_config_t *config)
{
	if ((payload == NULL) || (config == NULL) || (len != HPAD_DESKTOP_BLE_CONFIG_SIZE)) {
		return false;
	}
	if (payload[0] != HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS) {
		return false;
	}

	memset(config, 0, sizeof(*config));
	config->kind = HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS;
	for (size_t index = 0U; index < HPAD_PROTOCOL_KEY_COUNT; ++index) {
		const size_t offset = 1U + (index * HPAD_PROTOCOL_KEY_LED_CONFIG_SIZE);

		config->keys[index].r = payload[offset];
		config->keys[index].g = payload[offset + 1U];
		config->keys[index].b = payload[offset + 2U];
		config->keys[index].brightness = payload[offset + 3U];
	}

	return true;
}

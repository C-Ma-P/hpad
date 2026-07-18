#ifndef HPADV2_MACROPAD_CONFIG_H_
#define HPADV2_MACROPAD_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#include "wire_protocol.h"

#define DISPLAY_BRIGHTNESS_DEFAULT 0x8FU
#define DISPLAY_BRIGHTNESS_MIN 0x08U
#define DISPLAY_BRIGHTNESS_MAX UINT8_MAX

enum macropad_operating_mode {
	MACROPAD_MODE_DESKTOP_DONGLE = 0,
	MACROPAD_MODE_KINDLE_BLE = 1,
	MACROPAD_MODE_DESKTOP_BLE = 2,

	MACROPAD_OPERATING_MODE_DONGLE = MACROPAD_MODE_DESKTOP_DONGLE,
	MACROPAD_OPERATING_MODE_BLE = MACROPAD_MODE_KINDLE_BLE,
};

enum macropad_ble_feedback {
	MACROPAD_BLE_FEEDBACK_BUZZ = 0,
	MACROPAD_BLE_FEEDBACK_LED_LOW = 1,
	MACROPAD_BLE_FEEDBACK_LED_MED = 2,
	MACROPAD_BLE_FEEDBACK_LED_HIGH = 3,
};

int macropad_config_init(void);
const macropad_config_t *macropad_config_get(void);
int macropad_config_store(const macropad_config_t *config);
enum macropad_operating_mode macropad_config_get_operating_mode(void);
int macropad_config_store_operating_mode(enum macropad_operating_mode mode);
enum macropad_ble_feedback macropad_config_get_ble_feedback(void);
int macropad_config_store_ble_feedback(enum macropad_ble_feedback feedback);
bool macropad_config_get_keys_locked(void);
int macropad_config_store_keys_locked(bool locked);
uint8_t macropad_config_get_display_brightness(void);
int macropad_config_store_display_brightness(uint8_t brightness);

#endif

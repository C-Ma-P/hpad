#ifndef HPADV2_MACROPAD_CONFIG_H_
#define HPADV2_MACROPAD_CONFIG_H_

#include <stdint.h>

#include "wire_protocol.h"

enum macropad_operating_mode {
	MACROPAD_OPERATING_MODE_DONGLE = 0,
	MACROPAD_OPERATING_MODE_BLE = 1,
};

int macropad_config_init(void);
const macropad_config_t *macropad_config_get(void);
int macropad_config_store(const macropad_config_t *config);
enum macropad_operating_mode macropad_config_get_operating_mode(void);
int macropad_config_store_operating_mode(enum macropad_operating_mode mode);

#endif

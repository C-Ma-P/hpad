#ifndef HPADV2_MACROPAD_CONFIG_H_
#define HPADV2_MACROPAD_CONFIG_H_

#include "wire_protocol.h"

int macropad_config_init(void);
const macropad_config_t *macropad_config_get(void);
int macropad_config_store(const macropad_config_t *config);

#endif
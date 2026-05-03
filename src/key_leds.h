#ifndef HPADV2_KEY_LEDS_H_
#define HPADV2_KEY_LEDS_H_

#include <stdint.h>

#include "wire_protocol.h"

int key_leds_init(void);
int key_leds_update(uint8_t keys);
void key_leds_apply_config(const macropad_config_t *config);

#endif
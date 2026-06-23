#ifndef HPADV2_KEY_LEDS_H_
#define HPADV2_KEY_LEDS_H_

#include <stdint.h>

#include "macropad_config.h"

int key_leds_init(void);
int key_leds_set_all(uint8_t r, uint8_t g, uint8_t b);
int key_leds_all_on(void);
int key_leds_update(uint8_t keys);
int key_leds_update_ble_feedback(uint8_t keys, enum macropad_ble_feedback feedback);
int key_leds_preview_ble_feedback(enum macropad_ble_feedback feedback);
void key_leds_apply_config(const macropad_config_t *config);

#endif

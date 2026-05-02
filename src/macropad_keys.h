#ifndef HPADV2_MACROPAD_KEYS_H_
#define HPADV2_MACROPAD_KEYS_H_

#include <stdint.h>

#include <stdbool.h>

#define MACROPAD_KEY_COUNT 6U

typedef void (*macropad_keys_callback_t)(uint8_t key_mask, bool pressed, uint8_t keys);

int macropad_keys_init(macropad_keys_callback_t callback);
uint8_t macropad_keys_get_pressed_mask(void);

#endif
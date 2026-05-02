#ifndef HPADV2_WIRE_PROTOCOL_H_
#define HPADV2_WIRE_PROTOCOL_H_

#include <stdint.h>

#include <zephyr/toolchain.h>

#define WIRE_PROTOCOL_KEY_COUNT 6U
#define MACROPAD_CONFIG_KIND_KEY_COLORS 1U

typedef struct __packed {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t brightness;
} macropad_key_led_config_t;

typedef struct __packed {
	uint8_t keys;
	int8_t encoder_delta;
	uint8_t encoder_pressed;
	uint16_t battery_mv;
	uint8_t charging;
} macropad_report_t;

typedef struct __packed {
	uint8_t kind;
	macropad_key_led_config_t keys[WIRE_PROTOCOL_KEY_COUNT];
} macropad_config_t;

#endif
#ifndef HPADV2_WIRE_PROTOCOL_H_
#define HPADV2_WIRE_PROTOCOL_H_

#include <stdint.h>

#include <zephyr/toolchain.h>

typedef struct __packed {
	uint8_t keys;
	int8_t encoder_delta;
	uint8_t encoder_pressed;
	uint16_t battery_mv;
	uint8_t charging;
} macropad_report_t;

#endif
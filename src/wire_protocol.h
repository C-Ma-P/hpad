#ifndef HPADV2_WIRE_PROTOCOL_H_
#define HPADV2_WIRE_PROTOCOL_H_

#include <stdint.h>

#include <zephyr/toolchain.h>

#define HPAD_PROTOCOL_KEY_COUNT 6U
#define HPAD_PROTOCOL_KEY_LED_CONFIG_SIZE 4U
#define HPAD_PROTOCOL_MACROPAD_REPORT_SIZE 6U
#define HPAD_PROTOCOL_HOST_MACROPAD_REPORT_SIZE 7U
#define HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS 1U
#define HPAD_PROTOCOL_CONFIG_REPORT_SIZE \
	(1U + (HPAD_PROTOCOL_KEY_COUNT * HPAD_PROTOCOL_KEY_LED_CONFIG_SIZE))
#define HPAD_USB_VENDOR_INPUT_REPORT_SIZE HPAD_PROTOCOL_HOST_MACROPAD_REPORT_SIZE
#define HPAD_USB_VENDOR_OUTPUT_REPORT_SIZE HPAD_PROTOCOL_CONFIG_REPORT_SIZE

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
	uint8_t usb_power_present;
} macropad_report_t;

typedef struct __packed {
	uint8_t kind;
	macropad_key_led_config_t keys[HPAD_PROTOCOL_KEY_COUNT];
} macropad_config_t;

_Static_assert(sizeof(macropad_key_led_config_t) == HPAD_PROTOCOL_KEY_LED_CONFIG_SIZE,
	"macropad_key_led_config_t size must match wire format");
_Static_assert(sizeof(macropad_report_t) == HPAD_PROTOCOL_MACROPAD_REPORT_SIZE,
	"macropad_report_t size must match wire format");
_Static_assert(sizeof(macropad_config_t) == HPAD_PROTOCOL_CONFIG_REPORT_SIZE,
	"macropad_config_t size must match wire format");

#endif
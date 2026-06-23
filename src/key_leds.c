#include "key_leds.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "macropad_config.h"

LOG_MODULE_REGISTER(key_leds, LOG_LEVEL_INF);

#define LED_STRIP_NODE DT_ALIAS(led_strip)

#if DT_NODE_EXISTS(LED_STRIP_NODE) && DT_NODE_HAS_STATUS(LED_STRIP_NODE, okay)
#define HAVE_MACROPAD_LED_STRIP 1
#define MACROPAD_LED_STRIP_LENGTH DT_PROP(LED_STRIP_NODE, chain_length)
static const struct device *const macropad_led_strip = DEVICE_DT_GET(LED_STRIP_NODE);
static struct led_rgb macropad_led_strip_pixels[MACROPAD_LED_STRIP_LENGTH];
#else
#define HAVE_MACROPAD_LED_STRIP 0
#endif

static uint8_t scale_led_channel(uint8_t value, uint8_t brightness)
{
	return (uint8_t)(((uint16_t)value * (uint16_t)brightness + 127U) / 255U);
}

static uint8_t ble_feedback_brightness(enum macropad_ble_feedback feedback)
{
	switch (feedback) {
	case MACROPAD_BLE_FEEDBACK_LED_LOW:
		return 24U;
	case MACROPAD_BLE_FEEDBACK_LED_MED:
		return 72U;
	case MACROPAD_BLE_FEEDBACK_LED_HIGH:
		return 160U;
	case MACROPAD_BLE_FEEDBACK_BUZZ:
	default:
		return 0U;
	}
}

int key_leds_update(uint8_t keys)
{
#if HAVE_MACROPAD_LED_STRIP
	const macropad_config_t *config = macropad_config_get();

	for (size_t index = 0; index < MACROPAD_LED_STRIP_LENGTH; ++index) {
		struct led_rgb color = { 0 };

		if ((index < HPAD_PROTOCOL_KEY_COUNT) && ((keys & BIT(index)) != 0U)) {
			const macropad_key_led_config_t *key_config = &config->keys[index];

			color = (struct led_rgb){
				.r = scale_led_channel(key_config->r, key_config->brightness),
				.g = scale_led_channel(key_config->g, key_config->brightness),
				.b = scale_led_channel(key_config->b, key_config->brightness),
			};
		}

		macropad_led_strip_pixels[index] = color;
	}

	return led_strip_update_rgb(macropad_led_strip,
					macropad_led_strip_pixels,
					MACROPAD_LED_STRIP_LENGTH);
#else
	ARG_UNUSED(keys);
	return -ENOTSUP;
#endif
}

int key_leds_update_ble_feedback(uint8_t keys, enum macropad_ble_feedback feedback)
{
#if HAVE_MACROPAD_LED_STRIP
	const uint8_t brightness = ble_feedback_brightness(feedback);

	for (size_t index = 0; index < MACROPAD_LED_STRIP_LENGTH; ++index) {
		struct led_rgb color = { 0 };

		if ((brightness != 0U) && (index < HPAD_PROTOCOL_KEY_COUNT) &&
		    ((keys & BIT(index)) != 0U)) {
			color = (struct led_rgb){
				.r = brightness,
				.g = brightness,
				.b = brightness,
			};
		}

		macropad_led_strip_pixels[index] = color;
	}

	return led_strip_update_rgb(macropad_led_strip,
				    macropad_led_strip_pixels,
				    MACROPAD_LED_STRIP_LENGTH);
#else
	ARG_UNUSED(keys);
	ARG_UNUSED(feedback);
	return -ENOTSUP;
#endif
}

int key_leds_preview_ble_feedback(enum macropad_ble_feedback feedback)
{
	uint8_t brightness = ble_feedback_brightness(feedback);

	return key_leds_set_all(brightness, brightness, brightness);
}

int key_leds_init(void)
{
#if HAVE_MACROPAD_LED_STRIP
	if (!device_is_ready(macropad_led_strip)) {
		return -ENODEV;
	}

	return key_leds_update(0U);
#else
	return -ENOTSUP;
#endif
}

int key_leds_set_all(uint8_t r, uint8_t g, uint8_t b)
{
#if HAVE_MACROPAD_LED_STRIP
	const struct led_rgb color = { .r = r, .g = g, .b = b };

	for (size_t index = 0; index < MACROPAD_LED_STRIP_LENGTH; ++index) {
		macropad_led_strip_pixels[index] = color;
	}

	return led_strip_update_rgb(macropad_led_strip,
				    macropad_led_strip_pixels,
				    MACROPAD_LED_STRIP_LENGTH);
#else
	ARG_UNUSED(r);
	ARG_UNUSED(g);
	ARG_UNUSED(b);
	return -ENOTSUP;
#endif
}

int key_leds_all_on(void)
{
	return key_leds_set_all(64U, 64U, 64U);
}

void key_leds_apply_config(const macropad_config_t *config)
{
	int rc;

	if ((config == NULL) || (config->kind != HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS)) {
		return;
	}

	rc = macropad_config_store(config);
	if (rc != 0) {
		LOG_WRN("Failed to persist macropad LED config: %d", rc);
	}
}

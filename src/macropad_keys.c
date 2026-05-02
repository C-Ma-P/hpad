#include "macropad_keys.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(macropad_keys, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(macro_key_0) && DT_HAS_ALIAS(macro_key_1) && DT_HAS_ALIAS(macro_key_2) && \
	DT_HAS_ALIAS(macro_key_3) && DT_HAS_ALIAS(macro_key_4) && DT_HAS_ALIAS(macro_key_5)
#define MACROPAD_KEYS_SCAN_INTERVAL_MS 5
#define MACROPAD_KEYS_DEBOUNCE_SAMPLES 4

struct macropad_key_context {
	const struct gpio_dt_spec *spec;
	uint8_t mask;
	bool stable_pressed;
	bool sampled_pressed;
	uint8_t stable_samples;
};

static const struct gpio_dt_spec macropad_key_specs[MACROPAD_KEY_COUNT] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_3), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_4), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(macro_key_5), gpios),
};

static struct macropad_key_context macropad_keys[MACROPAD_KEY_COUNT];
static macropad_keys_callback_t macropad_keys_callback;
static uint8_t macropad_keys_pressed_mask;
static struct k_work_delayable macropad_keys_scan_work;

static void macropad_keys_notify(uint8_t key_mask, bool pressed)
{
	if ((key_mask != 0U) && (macropad_keys_callback != NULL)) {
		macropad_keys_callback(key_mask, pressed, macropad_keys_pressed_mask);
	}
}

static void macropad_keys_scan_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	for (size_t index = 0; index < ARRAY_SIZE(macropad_keys); ++index) {
		struct macropad_key_context *key_context = &macropad_keys[index];
		bool pressed;
		int state;

		state = gpio_pin_get_dt(key_context->spec);
		if (state < 0) {
			LOG_ERR("Failed to sample macro key %u: %d", (unsigned int)index, state);
			continue;
		}

		pressed = (state != 0);
		if (pressed == key_context->sampled_pressed) {
			if (key_context->stable_samples < MACROPAD_KEYS_DEBOUNCE_SAMPLES) {
				key_context->stable_samples++;
			}
		} else {
			key_context->sampled_pressed = pressed;
			key_context->stable_samples = 1U;
		}

		if ((key_context->stable_samples < MACROPAD_KEYS_DEBOUNCE_SAMPLES) ||
		    (key_context->stable_pressed == key_context->sampled_pressed)) {
			continue;
		}

		key_context->stable_pressed = key_context->sampled_pressed;
		if (key_context->stable_pressed) {
			macropad_keys_pressed_mask |= key_context->mask;
		} else {
			macropad_keys_pressed_mask &= (uint8_t)~key_context->mask;
		}

		macropad_keys_notify(key_context->mask, key_context->stable_pressed);
	}

	(void)k_work_reschedule(&macropad_keys_scan_work,
				K_MSEC(MACROPAD_KEYS_SCAN_INTERVAL_MS));
}

int macropad_keys_init(macropad_keys_callback_t callback)
{
	int rc;

	macropad_keys_callback = callback;
	macropad_keys_pressed_mask = 0U;
	k_work_init_delayable(&macropad_keys_scan_work, macropad_keys_scan_work_handler);

	for (size_t index = 0; index < ARRAY_SIZE(macropad_keys); ++index) {
		int state;
		bool pressed;
		struct macropad_key_context *key_context = &macropad_keys[index];
		const struct gpio_dt_spec *spec = &macropad_key_specs[index];

		if (!gpio_is_ready_dt(spec)) {
			return -ENODEV;
		}

		key_context->spec = spec;
		key_context->mask = BIT(index);

		rc = gpio_pin_configure_dt(spec, GPIO_INPUT);
		if (rc != 0) {
			LOG_ERR("Failed to configure macro key %u: %d", (unsigned int)index, rc);
			return rc;
		}

		state = gpio_pin_get_dt(spec);
		if (state < 0) {
			LOG_ERR("Failed to read initial macro key %u: %d", (unsigned int)index, state);
			return state;
		}

		pressed = (state != 0);
		key_context->stable_pressed = pressed;
		key_context->sampled_pressed = pressed;
		key_context->stable_samples = MACROPAD_KEYS_DEBOUNCE_SAMPLES;
		if (pressed) {
			macropad_keys_pressed_mask |= key_context->mask;
		}
	}

	(void)k_work_reschedule(&macropad_keys_scan_work,
				K_MSEC(MACROPAD_KEYS_SCAN_INTERVAL_MS));

	LOG_INF("Initialized %u macro keys", (unsigned int)ARRAY_SIZE(macropad_keys));
	return 0;
}

uint8_t macropad_keys_get_pressed_mask(void)
{
	return macropad_keys_pressed_mask;
}
#else
int macropad_keys_init(macropad_keys_callback_t callback)
{
	ARG_UNUSED(callback);
	return -ENOTSUP;
}

uint8_t macropad_keys_get_pressed_mask(void)
{
	return 0U;
}
#endif
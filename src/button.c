#include "button.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(sw0)
#define BUTTON_NODE DT_ALIAS(sw0)
#define BUTTON_DEBOUNCE_MS 20

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback button_gpio_cb;
static struct k_work_delayable button_poll_work;
static button_state_callback_t button_state_callback;
static bool button_pressed_latched;

static void button_poll_work_handler(struct k_work *work)
{
	bool pressed;
	int state;

	ARG_UNUSED(work);

	state = gpio_pin_get_dt(&button);
	if (state < 0) {
		LOG_ERR("Failed to sample button state: %d", state);
		return;
	}

	LOG_INF("Button state=%d on port=%s pin=%u", state,
		button.port->name, button.pin);

	pressed = (state != 0);
	if (pressed != button_pressed_latched) {
		button_pressed_latched = pressed;
		LOG_INF("Button %s detected on port=%s pin=%u",
			pressed ? "press" : "release", button.port->name, button.pin);
		if (button_state_callback != NULL) {
			button_state_callback(pressed);
		}
	}
}

static void button_gpio_handler(const struct device *port, struct gpio_callback *cb,
				uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&button_poll_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

int button_init(button_state_callback_t callback)
{
	int rc;

	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}

	button_state_callback = callback;
	k_work_init_delayable(&button_poll_work, button_poll_work_handler);

	rc = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (rc != 0) {
		LOG_ERR("Failed to configure button GPIO: %d", rc);
		return rc;
	}

	gpio_init_callback(&button_gpio_cb, button_gpio_handler, BIT(button.pin));
	rc = gpio_add_callback(button.port, &button_gpio_cb);
	if (rc != 0) {
		LOG_ERR("Failed to add button GPIO callback: %d", rc);
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		LOG_ERR("Failed to configure button interrupt: %d", rc);
		return rc;
	}

	(void)k_work_reschedule(&button_poll_work, K_NO_WAIT);

	LOG_INF("Button interrupt initialized on port=%s pin=%u", button.port->name,
		button.pin);
	return 0;
}
#else
int button_init(button_state_callback_t callback)
{
	ARG_UNUSED(callback);
	return -ENOTSUP;
}
#endif
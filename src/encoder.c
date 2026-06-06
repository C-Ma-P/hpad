#include "encoder.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(encoder, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(encoder_a) && DT_HAS_ALIAS(encoder_b) && DT_HAS_ALIAS(encoder_button)
#define ENCODER_A_NODE DT_ALIAS(encoder_a)
#define ENCODER_B_NODE DT_ALIAS(encoder_b)
#define ENCODER_BUTTON_NODE DT_ALIAS(encoder_button)
#define ENCODER_STEPS_PER_DETENT 4
#define ENCODER_BUTTON_DEBOUNCE_MS 20

static const struct gpio_dt_spec encoder_a = GPIO_DT_SPEC_GET(ENCODER_A_NODE, gpios);
static const struct gpio_dt_spec encoder_b = GPIO_DT_SPEC_GET(ENCODER_B_NODE, gpios);
static const struct gpio_dt_spec encoder_button = GPIO_DT_SPEC_GET(ENCODER_BUTTON_NODE, gpios);

static struct gpio_callback encoder_a_gpio_cb;
static struct gpio_callback encoder_b_gpio_cb;
static struct gpio_callback encoder_button_gpio_cb;
static struct k_work encoder_work;
static struct k_work_delayable encoder_button_work;
static encoder_delta_callback_t encoder_delta_callback;
static encoder_button_callback_t encoder_button_callback;
static uint8_t encoder_state;
static int8_t encoder_accumulator;
static bool encoder_button_pressed;

static const int8_t encoder_transition_delta[16] = {
	0, -1, 1, 0,
	1, 0, 0, -1,
	-1, 0, 0, 1,
	0, 1, -1, 0,
};

static int encoder_sample(void)
{
	int a;
	int b;

	a = gpio_pin_get_dt(&encoder_a);
	if (a < 0) {
		return a;
	}

	b = gpio_pin_get_dt(&encoder_b);
	if (b < 0) {
		return b;
	}

	return ((a != 0) << 1) | (b != 0);
}

static void encoder_emit_delta(int8_t delta)
{
	if ((delta != 0) && (encoder_delta_callback != NULL)) {
		encoder_delta_callback(delta);
	}
}

static void encoder_emit_button(bool pressed)
{
	if (encoder_button_callback != NULL) {
		encoder_button_callback(pressed);
	}
}

static void encoder_work_handler(struct k_work *work)
{
	uint8_t current_state;
	uint8_t transition;
	int rc;

	ARG_UNUSED(work);

	rc = encoder_sample();
	if (rc < 0) {
		LOG_ERR("Failed to sample encoder state: %d", rc);
		return;
	}
	current_state = (uint8_t)rc;

	transition = (uint8_t)((encoder_state << 2) | current_state);
	encoder_accumulator += encoder_transition_delta[transition];
	encoder_state = current_state;

	while (encoder_accumulator >= ENCODER_STEPS_PER_DETENT) {
		encoder_accumulator -= ENCODER_STEPS_PER_DETENT;
		encoder_emit_delta(1);
	}

	while (encoder_accumulator <= -ENCODER_STEPS_PER_DETENT) {
		encoder_accumulator += ENCODER_STEPS_PER_DETENT;
		encoder_emit_delta(-1);
	}
}

static void encoder_gpio_handler(const struct device *port, struct gpio_callback *cb,
				 uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	(void)k_work_submit(&encoder_work);
}

static void encoder_button_work_handler(struct k_work *work)
{
	bool pressed;
	int raw_state;

	ARG_UNUSED(work);

	raw_state = gpio_pin_get_raw(encoder_button.port, encoder_button.pin);
	if (raw_state < 0) {
		LOG_ERR("Failed to sample encoder button: %d", raw_state);
		return;
	}

	/* Encoder switch is wired to short the pin to GND when pressed. */
	pressed = (raw_state == 0);
	if (pressed != encoder_button_pressed) {
		encoder_button_pressed = pressed;
		encoder_emit_button(pressed);
	}
}

static void encoder_button_gpio_handler(const struct device *port, struct gpio_callback *cb,
					uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&encoder_button_work, K_MSEC(ENCODER_BUTTON_DEBOUNCE_MS));
}

int encoder_init(encoder_delta_callback_t delta_callback,
		 encoder_button_callback_t button_callback)
{
	int rc;

	if (!gpio_is_ready_dt(&encoder_a) || !gpio_is_ready_dt(&encoder_b) ||
	    !gpio_is_ready_dt(&encoder_button)) {
		return -ENODEV;
	}

	encoder_delta_callback = delta_callback;
	encoder_button_callback = button_callback;
	encoder_accumulator = 0;
	k_work_init(&encoder_work, encoder_work_handler);
	k_work_init_delayable(&encoder_button_work, encoder_button_work_handler);

	rc = gpio_pin_configure_dt(&encoder_a, GPIO_INPUT);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder A: %d", rc);
		return rc;
	}

	rc = gpio_pin_configure_dt(&encoder_b, GPIO_INPUT);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder B: %d", rc);
		return rc;
	}

	rc = gpio_pin_configure(encoder_button.port, encoder_button.pin,
				       GPIO_INPUT | GPIO_PULL_UP);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder button: %d", rc);
		return rc;
	}

	rc = encoder_sample();
	if (rc < 0) {
		LOG_ERR("Failed to read initial encoder state: %d", rc);
		return rc;
	}
	encoder_state = (uint8_t)rc;

	gpio_init_callback(&encoder_a_gpio_cb, encoder_gpio_handler, BIT(encoder_a.pin));
	rc = gpio_add_callback(encoder_a.port, &encoder_a_gpio_cb);
	if (rc != 0) {
		LOG_ERR("Failed to add encoder A callback: %d", rc);
		return rc;
	}

	gpio_init_callback(&encoder_b_gpio_cb, encoder_gpio_handler, BIT(encoder_b.pin));
	rc = gpio_add_callback(encoder_b.port, &encoder_b_gpio_cb);
	if (rc != 0) {
		LOG_ERR("Failed to add encoder B callback: %d", rc);
		return rc;
	}

	gpio_init_callback(&encoder_button_gpio_cb, encoder_button_gpio_handler,
			   BIT(encoder_button.pin));
	rc = gpio_add_callback(encoder_button.port, &encoder_button_gpio_cb);
	if (rc != 0) {
		LOG_ERR("Failed to add encoder button callback: %d", rc);
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(&encoder_a, GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder A interrupt: %d", rc);
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(&encoder_b, GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder B interrupt: %d", rc);
		return rc;
	}

	rc = gpio_pin_interrupt_configure(encoder_button.port, encoder_button.pin,
				        GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		LOG_ERR("Failed to configure encoder button interrupt: %d", rc);
		return rc;
	}

	(void)k_work_reschedule(&encoder_button_work, K_NO_WAIT);

	LOG_INF("Encoder initialized on A=%s pin=%u, B=%s pin=%u, SW=%s pin=%u",
		encoder_a.port->name, encoder_a.pin, encoder_b.port->name, encoder_b.pin,
		encoder_button.port->name, encoder_button.pin);
	return 0;
}
#else
int encoder_init(encoder_delta_callback_t delta_callback,
		 encoder_button_callback_t button_callback)
{
	ARG_UNUSED(delta_callback);
	ARG_UNUSED(button_callback);
	return -ENOTSUP;
}
#endif
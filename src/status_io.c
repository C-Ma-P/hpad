#include "status_io.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <hal/nrf_power.h>

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_STATUS_LED 1
#else
#define HAVE_STATUS_LED 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(buzzer), okay)
static const struct gpio_dt_spec buzzer = GPIO_DT_SPEC_GET(DT_ALIAS(buzzer), gpios);
#define HAVE_BUZZER 1
#else
#define HAVE_BUZZER 0
#endif

static struct k_work_delayable status_led_off_work;

static void set_status_led(bool on)
{
#if HAVE_STATUS_LED
	(void)gpio_pin_set_dt(&status_led, on);
#else
	ARG_UNUSED(on);
#endif
}

static void status_led_off_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	set_status_led(false);
}

int status_led_init(void)
{
#if HAVE_STATUS_LED
	if (!gpio_is_ready_dt(&status_led)) {
		return -ENODEV;
	}

	k_work_init_delayable(&status_led_off_work, status_led_off_work_handler);
	return gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
#else
	return -ENOTSUP;
#endif
}

void status_led_blink(uint8_t count, uint32_t pulse_ms, uint32_t gap_ms)
{
	for (uint8_t idx = 0; idx < count; ++idx) {
		set_status_led(true);
		k_msleep(pulse_ms);
		set_status_led(false);
		k_msleep(gap_ms);
	}
}

void status_led_pulse(uint32_t pulse_ms)
{
#if HAVE_STATUS_LED
	set_status_led(true);
	(void)k_work_reschedule(&status_led_off_work, K_MSEC(pulse_ms));
#else
	ARG_UNUSED(pulse_ms);
#endif
}

int status_buzzer_init(void)
{
#if HAVE_BUZZER
	if (!gpio_is_ready_dt(&buzzer)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&buzzer, GPIO_OUTPUT_INACTIVE);
#else
	return -ENOTSUP;
#endif
}

void status_buzzer_set(bool on)
{
#if HAVE_BUZZER
	(void)gpio_pin_set_dt(&buzzer, on ? 1 : 0);
#else
	ARG_UNUSED(on);
#endif
}

bool status_usb_power_present(void)
{
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}
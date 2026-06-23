/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Enter the installed Adafruit nRF52 UF2 bootloader's BLE OTA DFU mode.
 */

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(adafruit_dfu_entry, LOG_LEVEL_INF);

#define ADAFRUIT_DFU_MAGIC_OTA_RESET 0xA8U
#define ADAFRUIT_DFU_HOLD_MS 4000U

static const struct gpio_dt_spec encoder_button =
	GPIO_DT_SPEC_GET(DT_ALIAS(encoder_button), gpios);

static struct gpio_callback encoder_button_callback;
static struct k_work_delayable dfu_hold_work;

static bool encoder_button_pressed(void)
{
	return gpio_pin_get_dt(&encoder_button) > 0;
}

static void enter_ble_dfu(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!encoder_button_pressed()) {
		return;
	}

	LOG_WRN("Encoder held for %u ms; entering BLE DFU", ADAFRUIT_DFU_HOLD_MS);
	NRF_POWER->GPREGRET = ADAFRUIT_DFU_MAGIC_OTA_RESET;
	sys_reboot(SYS_REBOOT_COLD);
}

static void encoder_button_changed(const struct device *port,
				   struct gpio_callback *callback,
				   uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);

	if ((pins & BIT(encoder_button.pin)) == 0U) {
		return;
	}

	if (encoder_button_pressed()) {
		(void)k_work_reschedule(&dfu_hold_work, K_MSEC(ADAFRUIT_DFU_HOLD_MS));
	} else {
		(void)k_work_cancel_delayable(&dfu_hold_work);
	}
}

static int adafruit_dfu_entry_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&encoder_button)) {
		LOG_WRN("Encoder button GPIO is not ready; BLE DFU entry disabled");
		return 0;
	}

	k_work_init_delayable(&dfu_hold_work, enter_ble_dfu);

	rc = gpio_pin_configure_dt(&encoder_button, GPIO_INPUT);
	if (rc != 0) {
		LOG_WRN("Failed to configure encoder button for BLE DFU: %d", rc);
		return 0;
	}

	gpio_init_callback(&encoder_button_callback, encoder_button_changed,
		BIT(encoder_button.pin));
	rc = gpio_add_callback(encoder_button.port, &encoder_button_callback);
	if (rc != 0) {
		LOG_WRN("Failed to add BLE DFU encoder callback: %d", rc);
		return 0;
	}

	rc = gpio_pin_interrupt_configure_dt(&encoder_button, GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		(void)gpio_remove_callback(encoder_button.port, &encoder_button_callback);
		LOG_WRN("Failed to configure BLE DFU encoder interrupt: %d", rc);
		return 0;
	}

	LOG_INF("Hold encoder for %u ms to enter BLE DFU", ADAFRUIT_DFU_HOLD_MS);
	return 0;
}

SYS_INIT(adafruit_dfu_entry_init, APPLICATION, 0);

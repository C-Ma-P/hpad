#include "status_display.h"

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_display, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_ICON_X 0U
#define DISPLAY_STATUS_Y 0U
#define DISPLAY_ACK_X 24U
#define DISPLAY_BATT_X 88U
#define DISPLAY_BATT_Y 0U
#define DISPLAY_VALUE_X 0U
#define DISPLAY_VALUE_Y 16U
#define DISPLAY_CONNECTED_ICON "[+]"
#define DISPLAY_DISCONNECTED_ICON "[x]"
#define DISPLAY_ACK_TEXT "ACK"

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);

#define BATTERY_WARNING_PCT 20U

static uint8_t battery_mv_to_pct(uint16_t mv)
{
	if (mv >= 4200U) {
		return 100U;
	}
	if (mv <= 3000U) {
		return 0U;
	}
	return (uint8_t)((uint32_t)(mv - 3000U) * 100U / 1200U);
}

int status_display_init(void)
{
	int rc;

	if (!device_is_ready(display)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	rc = display_set_pixel_format(display, PIXEL_FORMAT_MONO10);
	if (rc != 0) {
		rc = display_set_pixel_format(display, PIXEL_FORMAT_MONO01);
	}
	LOG_DBG("display_set_pixel_format() -> %d", rc);
	if (rc != 0) {
		return rc;
	}

	rc = display_blanking_off(display);
	LOG_DBG("display_blanking_off() -> %d", rc);
	if ((rc != 0) && (rc != -ENOSYS)) {
		return rc;
	}

	rc = cfb_framebuffer_init(display);
	LOG_DBG("cfb_framebuffer_init() -> %d", rc);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

int status_display_render(bool connected, bool show_ack, bool usb_power_present, int32_t value,
			  uint16_t battery_mv)
{
	int rc;
	char value_text[16];
	char batt_text[8];
	uint8_t pct = battery_mv_to_pct(battery_mv);
	const char *icon = connected ? DISPLAY_CONNECTED_ICON : DISPLAY_DISCONNECTED_ICON;

	(void)snprintf(value_text, sizeof(value_text), "%ld", (long)value);
	if (usb_power_present) {
		(void)snprintf(batt_text, sizeof(batt_text), "+%u%%", pct);
	} else if (pct < BATTERY_WARNING_PCT) {
		(void)snprintf(batt_text, sizeof(batt_text), "!%u%%", pct);
	} else {
		(void)snprintf(batt_text, sizeof(batt_text), "%u%%", pct);
	}

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	rc = cfb_print(display, icon, DISPLAY_ICON_X, DISPLAY_STATUS_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print icon failed: %d", rc);
		return rc;
	}

	if (show_ack) {
		rc = cfb_print(display, DISPLAY_ACK_TEXT, DISPLAY_ACK_X, DISPLAY_STATUS_Y);
		if (rc != 0) {
			LOG_ERR("cfb_print ACK failed: %d", rc);
			return rc;
		}
	}

	rc = cfb_print(display, batt_text, DISPLAY_BATT_X, DISPLAY_BATT_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print battery failed: %d", rc);
		return rc;
	}

	rc = cfb_print(display, value_text, DISPLAY_VALUE_X, DISPLAY_VALUE_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print value failed: %d", rc);
		return rc;
	}

	rc = cfb_framebuffer_finalize(display);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
	}

	return rc;
}

int status_display_blank(void)
{
	return display_blanking_on(display);
}

int status_display_unblank(void)
{
	return display_blanking_off(display);
}
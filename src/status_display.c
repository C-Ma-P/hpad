#include "status_display.h"

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_display, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_TOP_ROW_Y 0U
#define DISPLAY_FRAME_MARGIN_X 2U
#define DISPLAY_BATTERY_GROUP_GAP 4U
#define DISPLAY_BATTERY_FIELD_DIGITS 3U
#define DISPLAY_DEFAULT_WIDTH 128U
#define DISPLAY_DEFAULT_FONT_WIDTH 8U
#define DISPLAY_DEFAULT_FONT_HEIGHT 16U
#define DISPLAY_CONNECTED_TEXT "LINK"
#define DISPLAY_DISCONNECTED_TEXT "WAIT"

#define BATTERY_ICON_WIDTH 8U
#define BATTERY_ICON_HEIGHT 9U
#define BOLT_ICON_WIDTH 8U
#define BOLT_ICON_HEIGHT 9U
#define USB_ICON_WIDTH 8U
#define USB_ICON_HEIGHT 8U

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static uint16_t display_width_px = DISPLAY_DEFAULT_WIDTH;
static uint8_t display_font_width = DISPLAY_DEFAULT_FONT_WIDTH;
static uint8_t display_font_height = DISPLAY_DEFAULT_FONT_HEIGHT;

static const uint16_t battery_icon_rows[BATTERY_ICON_HEIGHT] = {
	0x18U,
	0x18U,
	0x7EU,
	0x42U,
	0x42U,
	0x42U,
	0x42U,
	0x42U,
	0x7EU,
};

static const uint16_t charging_bolt_rows[BOLT_ICON_HEIGHT] = {
	0x18U,
	0x3CU,
	0x38U,
	0x7CU,
	0x3EU,
	0x1EU,
	0x3CU,
	0x38U,
	0x30U,
};

static const uint16_t usb_icon_rows[USB_ICON_HEIGHT] = {
	0x24U,
	0x7EU,
	0x7EU,
	0x5AU,
	0x5AU,
	0x3CU,
	0x18U,
	0x18U,
};

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

static int draw_bitmap(uint16_t x, uint16_t y, uint8_t width, uint8_t height,
		      const uint16_t *rows)
{
	for (uint8_t row = 0U; row < height; ++row) {
		uint16_t row_bits = rows[row];

		for (uint8_t col = 0U; col < width; ++col) {
			struct cfb_position pos;

			if (((row_bits >> (width - 1U - col)) & 0x1U) == 0U) {
				continue;
			}

			pos.x = x + col;
			pos.y = y + row;

			int rc = cfb_draw_point(display, &pos);

			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

int status_display_init(void)
{
	int rc;
	int value;

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

	value = cfb_get_display_parameter(display, CFB_DISPLAY_WIDTH);
	if (value > 0) {
		display_width_px = (uint16_t)value;
	}

	rc = cfb_get_font_size(display, 0U, &display_font_width, &display_font_height);
	if (rc != 0) {
		LOG_WRN("cfb_get_font_size failed: %d, using defaults", rc);
		display_font_width = DISPLAY_DEFAULT_FONT_WIDTH;
		display_font_height = DISPLAY_DEFAULT_FONT_HEIGHT;
	}

	return 0;
}

int status_display_render(bool connected, bool usb_power_present, uint16_t battery_mv)
{
	int rc;
	char batt_text[8];
	uint8_t pct = battery_mv_to_pct(battery_mv);
	const char *status_text = connected ? DISPLAY_CONNECTED_TEXT : DISPLAY_DISCONNECTED_TEXT;
	uint16_t bottom_group_width = (uint16_t)(DISPLAY_BATTERY_FIELD_DIGITS * display_font_width) +
		DISPLAY_BATTERY_GROUP_GAP +
		(usb_power_present ? BOLT_ICON_WIDTH : BATTERY_ICON_WIDTH);
	uint16_t bottom_group_x = (display_width_px > bottom_group_width) ?
		(uint16_t)((display_width_px - bottom_group_width) / 2U) : 0U;
	uint16_t bottom_icon_x = bottom_group_x +
		(DISPLAY_BATTERY_FIELD_DIGITS * display_font_width) + DISPLAY_BATTERY_GROUP_GAP;
	uint16_t bottom_row_y = display_font_height;
	uint16_t bottom_icon_y = bottom_row_y +
		((display_font_height - BATTERY_ICON_HEIGHT) / 2U);
	uint16_t usb_icon_x = (display_width_px > (USB_ICON_WIDTH + DISPLAY_FRAME_MARGIN_X)) ?
		(uint16_t)(display_width_px - USB_ICON_WIDTH - DISPLAY_FRAME_MARGIN_X) : 0U;
	uint16_t usb_icon_y = (display_font_height > USB_ICON_HEIGHT) ?
		(uint16_t)((display_font_height - USB_ICON_HEIGHT) / 2U) : 0U;

	(void)snprintf(batt_text, sizeof(batt_text), "%3u", pct);

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	rc = cfb_print(display, status_text, 0U, DISPLAY_TOP_ROW_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print status failed: %d", rc);
		return rc;
	}

	if (usb_power_present) {
		rc = draw_bitmap(usb_icon_x, usb_icon_y, USB_ICON_WIDTH, USB_ICON_HEIGHT,
			usb_icon_rows);
		if (rc != 0) {
			LOG_ERR("draw USB icon failed: %d", rc);
			return rc;
		}
	}

	rc = cfb_print(display, batt_text, bottom_group_x, bottom_row_y);
	if (rc != 0) {
		LOG_ERR("cfb_print battery failed: %d", rc);
		return rc;
	}

	if (usb_power_present) {
		rc = draw_bitmap(bottom_icon_x, bottom_icon_y, BOLT_ICON_WIDTH, BOLT_ICON_HEIGHT,
			charging_bolt_rows);
	} else {
		rc = draw_bitmap(bottom_icon_x, bottom_icon_y, BATTERY_ICON_WIDTH,
			BATTERY_ICON_HEIGHT, battery_icon_rows);
	}
	if (rc != 0) {
		LOG_ERR("draw battery state icon failed: %d", rc);
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
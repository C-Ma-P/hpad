#include "status_display.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(status_display, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_DEFAULT_WIDTH 128U
#define DISPLAY_DEFAULT_HEIGHT 32U
#define DISPLAY_MARGIN_X 2U
#define TOP_ICON_Y 1U
#define DONGLE_ICON_X 2U
#define DONGLE_ICON_WIDTH 11U
#define DONGLE_ICON_HEIGHT 7U
#define USB_ICON_WIDTH 9U
#define USB_ICON_HEIGHT 7U
#define MATRIX_ORIGIN_X 3U
#define MATRIX_ORIGIN_Y 10U
#define MATRIX_CELL_WIDTH 10U
#define MATRIX_CELL_HEIGHT 9U
#define MATRIX_CELL_GAP_X 2U
#define MATRIX_CELL_GAP_Y 2U
#define BATTERY_WARNING_X 42U
#define BATTERY_WARNING_Y 17U
#define BATTERY_WARNING_WIDTH 7U
#define BATTERY_WARNING_HEIGHT 7U
#define BATTERY_BODY_X 52U
#define BATTERY_BODY_Y 16U
#define BATTERY_BODY_WIDTH 42U
#define BATTERY_BODY_HEIGHT 9U
#define BATTERY_CAP_WIDTH 2U
#define BATTERY_CAP_HEIGHT 5U
#define BATTERY_FILL_INSET 2U
#define BATTERY_PERCENT_X 101U
#define BATTERY_PERCENT_Y 18U
#define CHARGE_BOLT_WIDTH 5U
#define CHARGE_BOLT_HEIGHT 5U
#define TINY_GLYPH_WIDTH 4U
#define TINY_GLYPH_HEIGHT 5U
#define TINY_GLYPH_GAP 1U
#define MODE_LABEL_DONGLE_X 18U
#define MODE_LABEL_BLE_X 2U
#define MODE_LABEL_Y 1U
#define LOCKED_LABEL_X 67U
#define COMPACT_GLYPH_WIDTH 5U
#define COMPACT_GLYPH_HEIGHT 7U
#define COMPACT_GLYPH_GAP 1U
#define COMPACT_LINE_HEIGHT (COMPACT_GLYPH_HEIGHT + 2U)
#define MENU_TITLE_X 2U
#define MENU_TITLE_Y 0U
#define MENU_ITEM_X 2U
#define MENU_MARKER_WIDTH (COMPACT_GLYPH_WIDTH + COMPACT_GLYPH_GAP)
#define MENU_TITLE_BOTTOM_GAP 3U
#define MENU_FIRST_ITEM_Y (MENU_TITLE_Y + COMPACT_GLYPH_HEIGHT + MENU_TITLE_BOTTOM_GAP)
#define MENU_SCROLL_CUE_WIDTH (COMPACT_GLYPH_WIDTH + COMPACT_GLYPH_GAP)
#define MESSAGE_LINE_GAP 3U

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static uint16_t display_width_px = DISPLAY_DEFAULT_WIDTH;
static uint16_t display_height_px = DISPLAY_DEFAULT_HEIGHT;

static const uint16_t usb_icon_rows[USB_ICON_HEIGHT] = {
	0x024U,
	0x024U,
	0x03CU,
	0x07EU,
	0x018U,
	0x018U,
	0x018U,
};

static const uint8_t warning_icon_rows[BATTERY_WARNING_HEIGHT] = {
	0x08U,
	0x1CU,
	0x1CU,
	0x3EU,
	0x3EU,
	0x08U,
	0x08U,
};

static const uint8_t charge_bolt_rows[CHARGE_BOLT_HEIGHT] = {
	0x06U,
	0x04U,
	0x0EU,
	0x0CU,
	0x08U,
};

static int draw_small_glyph(uint16_t x, uint16_t y, uint8_t width, const uint8_t *rows);
static int draw_small_text(uint16_t x, uint16_t y, const char *text);
static int draw_compact_text_clipped(uint16_t x, uint16_t y, const char *text,
				     uint16_t max_width);

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

static uint8_t clamp_u8(uint16_t value, uint8_t max_value)
{
	if (value > max_value) {
		return max_value;
	}

	return (uint8_t)value;
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

static int draw_pixel(uint16_t x, uint16_t y)
{
	struct cfb_position pos = {
		.x = x,
		.y = y,
	};

	if ((x >= display_width_px) || (y >= display_height_px)) {
		return 0;
	}

	return cfb_draw_point(display, &pos);
}

static int draw_hline(uint16_t x, uint16_t y, uint8_t width)
{
	for (uint8_t offset = 0U; offset < width; ++offset) {
		int rc = draw_pixel(x + offset, y);

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int draw_vline(uint16_t x, uint16_t y, uint8_t height)
{
	for (uint8_t offset = 0U; offset < height; ++offset) {
		int rc = draw_pixel(x, y + offset);

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int fill_rect(uint16_t x, uint16_t y, uint8_t width, uint8_t height)
{
	for (uint8_t row = 0U; row < height; ++row) {
		int rc = draw_hline(x, y + row, width);

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int draw_rounded_rect(uint16_t x, uint16_t y, uint8_t width, uint8_t height)
{
	int rc;

	if ((width < 4U) || (height < 4U)) {
		return -EINVAL;
	}

	rc = draw_hline(x + 2U, y, width - 4U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_hline(x + 2U, y + height - 1U, width - 4U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_hline(x + 1U, y + 1U, width - 2U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_hline(x + 1U, y + height - 2U, width - 2U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_vline(x, y + 2U, height - 4U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_vline(x + width - 1U, y + 2U, height - 4U);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

static int fill_rounded_rect(uint16_t x, uint16_t y, uint8_t width, uint8_t height)
{
	int rc;

	if ((width < 4U) || (height < 4U)) {
		return -EINVAL;
	}

	rc = fill_rect(x + 2U, y, width - 4U, 1U);
	if (rc != 0) {
		return rc;
	}

	rc = fill_rect(x + 1U, y + 1U, width - 2U, 1U);
	if (rc != 0) {
		return rc;
	}

	rc = fill_rect(x, y + 2U, width, height - 4U);
	if (rc != 0) {
		return rc;
	}

	rc = fill_rect(x + 1U, y + height - 2U, width - 2U, 1U);
	if (rc != 0) {
		return rc;
	}

	return fill_rect(x + 2U, y + height - 1U, width - 4U, 1U);
}

static int draw_dongle_icon(bool connected, bool activity)
{
	const uint16_t x = DONGLE_ICON_X;
	const uint16_t y = TOP_ICON_Y;
	int rc;

	rc = draw_vline(x + 5U, y + 2U, 5U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_hline(x + 3U, y + 6U, 5U);
	if (rc != 0) {
		return rc;
	}

	rc = draw_pixel(x + 5U, y + 1U);
	if (rc != 0) {
		return rc;
	}

	if (!connected) {
		for (uint8_t offset = 0U; offset < 6U; ++offset) {
			rc = draw_pixel(x + 2U + offset, y + offset);
			if (rc != 0) {
				return rc;
			}
		}

		return 0;
	}

	rc = draw_pixel(x + 3U, y + 3U);
	if (rc != 0) {
		return rc;
	}
	rc = draw_pixel(x + 7U, y + 3U);
	if (rc != 0) {
		return rc;
	}
	rc = draw_pixel(x + 2U, y + 2U);
	if (rc != 0) {
		return rc;
	}
	rc = draw_pixel(x + 8U, y + 2U);
	if (rc != 0) {
		return rc;
	}

	if (!activity) {
		return 0;
	}

	rc = draw_pixel(x + 1U, y + 1U);
	if (rc != 0) {
		return rc;
	}
	rc = draw_pixel(x + 9U, y + 1U);
	if (rc != 0) {
		return rc;
	}
	rc = draw_pixel(x, y);
	if (rc != 0) {
		return rc;
	}

	return draw_pixel(x + 10U, y);
}

static int draw_key_matrix(uint8_t keys_pressed)
{
	for (uint8_t index = 0U; index < 6U; ++index) {
		uint16_t x = MATRIX_ORIGIN_X +
			(index % 3U) * (MATRIX_CELL_WIDTH + MATRIX_CELL_GAP_X);
		uint16_t y = MATRIX_ORIGIN_Y +
			(index / 3U) * (MATRIX_CELL_HEIGHT + MATRIX_CELL_GAP_Y);
		int rc;

		if ((keys_pressed & BIT(index)) != 0U) {
			rc = fill_rounded_rect(x, y, MATRIX_CELL_WIDTH, MATRIX_CELL_HEIGHT);
		} else {
			rc = draw_rounded_rect(x, y, MATRIX_CELL_WIDTH, MATRIX_CELL_HEIGHT);
		}

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int draw_battery_warning(uint16_t x, uint16_t y)
{
	uint16_t rows[BATTERY_WARNING_HEIGHT];

	for (uint8_t row = 0U; row < BATTERY_WARNING_HEIGHT; ++row) {
		rows[row] = warning_icon_rows[row];
	}

	return draw_bitmap(x, y, BATTERY_WARNING_WIDTH, BATTERY_WARNING_HEIGHT, rows);
}

static int draw_usb_icon(void)
{
	uint16_t rows[USB_ICON_HEIGHT];
	uint16_t x = (display_width_px > (USB_ICON_WIDTH + DISPLAY_MARGIN_X)) ?
		(uint16_t)(display_width_px - USB_ICON_WIDTH - DISPLAY_MARGIN_X) : 0U;

	for (uint8_t row = 0U; row < USB_ICON_HEIGHT; ++row) {
		rows[row] = usb_icon_rows[row];
	}

	return draw_bitmap(x, TOP_ICON_Y, USB_ICON_WIDTH, USB_ICON_HEIGHT, rows);
}

static const char *ble_status_text(enum ble_hid_state state)
{
	switch (state) {
	case BLE_HID_STATE_INACTIVE:
		return "BLE OFF";
	case BLE_HID_STATE_STARTING:
		return "BLE START";
	case BLE_HID_STATE_ADVERTISING:
		return "BLE READY";
	case BLE_HID_STATE_CONNECTED:
		return "BLE LINKED";
	case BLE_HID_STATE_SECURITY_FAILED:
	case BLE_HID_STATE_ERROR:
		return "BLE ERR";
	case BLE_HID_STATE_STOPPING:
		return "BLE STOP";
	default:
		return "BLE ?";
	}
}

static int draw_mode_label(enum macropad_operating_mode mode)
{
	if (mode == MACROPAD_OPERATING_MODE_BLE) {
		return draw_small_text(MODE_LABEL_BLE_X, MODE_LABEL_Y, "BLE");
	}

	return draw_small_text(MODE_LABEL_DONGLE_X, MODE_LABEL_Y, "DONGLE");
}

static int draw_locked_label(void)
{
	return draw_compact_text_clipped(LOCKED_LABEL_X, MODE_LABEL_Y, "LOCKED",
		(display_width_px > LOCKED_LABEL_X) ?
			(uint16_t)(display_width_px - LOCKED_LABEL_X) : 0U);
}

static void tiny_digit_rows(uint8_t digit, uint8_t rows[TINY_GLYPH_HEIGHT])
{
	switch (digit) {
	case 0U:
		rows[0] = 0x06U;
		rows[1] = 0x09U;
		rows[2] = 0x09U;
		rows[3] = 0x09U;
		rows[4] = 0x06U;
		break;
	case 1U:
		rows[0] = 0x02U;
		rows[1] = 0x06U;
		rows[2] = 0x02U;
		rows[3] = 0x02U;
		rows[4] = 0x07U;
		break;
	case 2U:
		rows[0] = 0x0EU;
		rows[1] = 0x01U;
		rows[2] = 0x06U;
		rows[3] = 0x08U;
		rows[4] = 0x0FU;
		break;
	case 3U:
		rows[0] = 0x0EU;
		rows[1] = 0x01U;
		rows[2] = 0x06U;
		rows[3] = 0x01U;
		rows[4] = 0x0EU;
		break;
	case 4U:
		rows[0] = 0x09U;
		rows[1] = 0x09U;
		rows[2] = 0x0FU;
		rows[3] = 0x01U;
		rows[4] = 0x01U;
		break;
	case 5U:
		rows[0] = 0x0FU;
		rows[1] = 0x08U;
		rows[2] = 0x0EU;
		rows[3] = 0x01U;
		rows[4] = 0x0EU;
		break;
	case 6U:
		rows[0] = 0x07U;
		rows[1] = 0x08U;
		rows[2] = 0x0EU;
		rows[3] = 0x09U;
		rows[4] = 0x06U;
		break;
	case 7U:
		rows[0] = 0x0FU;
		rows[1] = 0x01U;
		rows[2] = 0x02U;
		rows[3] = 0x04U;
		rows[4] = 0x04U;
		break;
	case 8U:
		rows[0] = 0x06U;
		rows[1] = 0x09U;
		rows[2] = 0x06U;
		rows[3] = 0x09U;
		rows[4] = 0x06U;
		break;
	case 9U:
	default:
		rows[0] = 0x06U;
		rows[1] = 0x09U;
		rows[2] = 0x07U;
		rows[3] = 0x01U;
		rows[4] = 0x0EU;
		break;
	}
}

static void tiny_percent_rows(uint8_t rows[TINY_GLYPH_HEIGHT])
{
	rows[0] = 0x09U;
	rows[1] = 0x02U;
	rows[2] = 0x04U;
	rows[3] = 0x08U;
	rows[4] = 0x09U;
}

static void tiny_letter_rows(char ch, uint8_t rows[TINY_GLYPH_HEIGHT])
{
	switch (ch) {
	case 'B':
		rows[0] = 0x0EU;
		rows[1] = 0x09U;
		rows[2] = 0x0EU;
		rows[3] = 0x09U;
		rows[4] = 0x0EU;
		break;
	case 'C':
		rows[0] = 0x06U;
		rows[1] = 0x08U;
		rows[2] = 0x08U;
		rows[3] = 0x08U;
		rows[4] = 0x06U;
		break;
	case 'D':
		rows[0] = 0x0EU;
		rows[1] = 0x09U;
		rows[2] = 0x09U;
		rows[3] = 0x09U;
		rows[4] = 0x0EU;
		break;
	case 'E':
		rows[0] = 0x0FU;
		rows[1] = 0x08U;
		rows[2] = 0x0EU;
		rows[3] = 0x08U;
		rows[4] = 0x0FU;
		break;
	case 'G':
		rows[0] = 0x06U;
		rows[1] = 0x08U;
		rows[2] = 0x0BU;
		rows[3] = 0x09U;
		rows[4] = 0x07U;
		break;
	case 'H':
		rows[0] = 0x09U;
		rows[1] = 0x09U;
		rows[2] = 0x0FU;
		rows[3] = 0x09U;
		rows[4] = 0x09U;
		break;
	case 'L':
		rows[0] = 0x08U;
		rows[1] = 0x08U;
		rows[2] = 0x08U;
		rows[3] = 0x08U;
		rows[4] = 0x0FU;
		break;
	case 'N':
		rows[0] = 0x09U;
		rows[1] = 0x0DU;
		rows[2] = 0x0BU;
		rows[3] = 0x09U;
		rows[4] = 0x09U;
		break;
	case 'O':
		rows[0] = 0x06U;
		rows[1] = 0x09U;
		rows[2] = 0x09U;
		rows[3] = 0x09U;
		rows[4] = 0x06U;
		break;
	default:
		rows[0] = 0x00U;
		rows[1] = 0x00U;
		rows[2] = 0x00U;
		rows[3] = 0x00U;
		rows[4] = 0x00U;
		break;
	}
}

static int draw_small_glyph(uint16_t x, uint16_t y, uint8_t width, const uint8_t *rows)
{
	for (uint8_t row = 0U; row < TINY_GLYPH_HEIGHT; ++row) {
		for (uint8_t col = 0U; col < width; ++col) {
			int rc;

			if (((rows[row] >> (width - 1U - col)) & 0x1U) == 0U) {
				continue;
			}

			rc = draw_pixel(x + col, y + row);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

static void compact_glyph_rows(char ch, uint8_t rows[COMPACT_GLYPH_HEIGHT])
{
	if ((ch >= 'a') && (ch <= 'z')) {
		ch = (char)(ch - ('a' - 'A'));
	}

	switch (ch) {
	case 'A':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'B':
		memcpy(rows, (uint8_t[]){0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'C':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'D':
		memcpy(rows, (uint8_t[]){0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'E':
		memcpy(rows, (uint8_t[]){0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'F':
		memcpy(rows, (uint8_t[]){0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'G':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'H':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'I':
		memcpy(rows, (uint8_t[]){0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'J':
		memcpy(rows, (uint8_t[]){0x01U, 0x01U, 0x01U, 0x01U, 0x11U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'K':
		memcpy(rows, (uint8_t[]){0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'L':
		memcpy(rows, (uint8_t[]){0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'M':
		memcpy(rows, (uint8_t[]){0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'N':
		memcpy(rows, (uint8_t[]){0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'O':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'P':
		memcpy(rows, (uint8_t[]){0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'Q':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'R':
		memcpy(rows, (uint8_t[]){0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'S':
		memcpy(rows, (uint8_t[]){0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'T':
		memcpy(rows, (uint8_t[]){0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'U':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'V':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x0AU, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'W':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x15U, 0x0AU}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'X':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'Y':
		memcpy(rows, (uint8_t[]){0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	case 'Z':
		memcpy(rows, (uint8_t[]){0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '0':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '1':
		memcpy(rows, (uint8_t[]){0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '2':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '3':
		memcpy(rows, (uint8_t[]){0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '4':
		memcpy(rows, (uint8_t[]){0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '5':
		memcpy(rows, (uint8_t[]){0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '6':
		memcpy(rows, (uint8_t[]){0x06U, 0x08U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '7':
		memcpy(rows, (uint8_t[]){0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '8':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '9':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x02U, 0x0CU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '>':
		memcpy(rows, (uint8_t[]){0x10U, 0x08U, 0x04U, 0x02U, 0x04U, 0x08U, 0x10U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '<':
		memcpy(rows, (uint8_t[]){0x01U, 0x02U, 0x04U, 0x08U, 0x04U, 0x02U, 0x01U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '.':
		memcpy(rows, (uint8_t[]){0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x0CU}, COMPACT_GLYPH_HEIGHT);
		break;
	case ',':
		memcpy(rows, (uint8_t[]){0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x04U, 0x08U}, COMPACT_GLYPH_HEIGHT);
		break;
	case ':':
		memcpy(rows, (uint8_t[]){0x00U, 0x0CU, 0x0CU, 0x00U, 0x0CU, 0x0CU, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case ';':
		memcpy(rows, (uint8_t[]){0x00U, 0x0CU, 0x0CU, 0x00U, 0x0CU, 0x04U, 0x08U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '!':
		memcpy(rows, (uint8_t[]){0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x00U, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '?':
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x00U, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '-':
		memcpy(rows, (uint8_t[]){0x00U, 0x00U, 0x00U, 0x1FU, 0x00U, 0x00U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '_':
		memcpy(rows, (uint8_t[]){0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1FU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '/':
		memcpy(rows, (uint8_t[]){0x01U, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x10U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '\\':
		memcpy(rows, (uint8_t[]){0x10U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U, 0x01U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '+':
		memcpy(rows, (uint8_t[]){0x00U, 0x04U, 0x04U, 0x1FU, 0x04U, 0x04U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '*':
		memcpy(rows, (uint8_t[]){0x00U, 0x15U, 0x0EU, 0x1FU, 0x0EU, 0x15U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '(':
		memcpy(rows, (uint8_t[]){0x02U, 0x04U, 0x08U, 0x08U, 0x08U, 0x04U, 0x02U}, COMPACT_GLYPH_HEIGHT);
		break;
	case ')':
		memcpy(rows, (uint8_t[]){0x08U, 0x04U, 0x02U, 0x02U, 0x02U, 0x04U, 0x08U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '[':
		memcpy(rows, (uint8_t[]){0x0EU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case ']':
		memcpy(rows, (uint8_t[]){0x0EU, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x0EU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '\'':
		memcpy(rows, (uint8_t[]){0x04U, 0x04U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '"':
		memcpy(rows, (uint8_t[]){0x0AU, 0x0AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '%':
		memcpy(rows, (uint8_t[]){0x18U, 0x19U, 0x02U, 0x04U, 0x08U, 0x13U, 0x03U}, COMPACT_GLYPH_HEIGHT);
		break;
	case '#':
		memcpy(rows, (uint8_t[]){0x0AU, 0x0AU, 0x1FU, 0x0AU, 0x1FU, 0x0AU, 0x0AU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '&':
		memcpy(rows, (uint8_t[]){0x0CU, 0x12U, 0x14U, 0x08U, 0x15U, 0x12U, 0x0DU}, COMPACT_GLYPH_HEIGHT);
		break;
	case '=':
		memcpy(rows, (uint8_t[]){0x00U, 0x00U, 0x1FU, 0x00U, 0x1FU, 0x00U, 0x00U}, COMPACT_GLYPH_HEIGHT);
		break;
	case ' ':
		memset(rows, 0, COMPACT_GLYPH_HEIGHT);
		break;
	default:
		memcpy(rows, (uint8_t[]){0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x00U, 0x04U}, COMPACT_GLYPH_HEIGHT);
		break;
	}
}

static uint16_t compact_char_advance(void)
{
	return COMPACT_GLYPH_WIDTH + COMPACT_GLYPH_GAP;
}

static uint16_t compact_text_width_chars(size_t char_count)
{
	if (char_count == 0U) {
		return 0U;
	}

	return (uint16_t)(char_count * compact_char_advance() - COMPACT_GLYPH_GAP);
}

static uint16_t compact_text_width(const char *text)
{
	return compact_text_width_chars(strlen(text));
}

static size_t compact_text_capacity(uint16_t max_width)
{
	const uint16_t advance = compact_char_advance();

	if (max_width < COMPACT_GLYPH_WIDTH) {
		return 0U;
	}

	return (size_t)((max_width + COMPACT_GLYPH_GAP) / advance);
}

static int draw_compact_glyph(uint16_t x, uint16_t y, char ch)
{
	uint8_t rows[COMPACT_GLYPH_HEIGHT];

	compact_glyph_rows(ch, rows);
	for (uint8_t row = 0U; row < COMPACT_GLYPH_HEIGHT; ++row) {
		for (uint8_t col = 0U; col < COMPACT_GLYPH_WIDTH; ++col) {
			int rc;

			if (((rows[row] >> (COMPACT_GLYPH_WIDTH - 1U - col)) & 0x1U) == 0U) {
				continue;
			}

			rc = draw_pixel(x + col, y + row);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

static int draw_compact_text_count(uint16_t x, uint16_t y, const char *text, size_t count)
{
	for (size_t index = 0U; index < count; ++index) {
		int rc = draw_compact_glyph(x, y, text[index]);

		if (rc != 0) {
			return rc;
		}
		x += compact_char_advance();
	}

	return 0;
}

static int draw_compact_text_clipped(uint16_t x, uint16_t y, const char *text,
				     uint16_t max_width)
{
	const size_t text_len = strlen(text);
	const size_t capacity = compact_text_capacity(max_width);
	size_t visible_count;
	int rc;

	if (capacity == 0U) {
		return 0;
	}

	if (text_len <= capacity) {
		return draw_compact_text_count(x, y, text, text_len);
	}

	if (capacity <= 3U) {
		return draw_compact_text_count(x, y, "...", capacity);
	}

	visible_count = capacity - 3U;
	rc = draw_compact_text_count(x, y, text, visible_count);
	if (rc != 0) {
		return rc;
	}

	return draw_compact_text_count(x + (uint16_t)(visible_count * compact_char_advance()),
		y, "...", 3U);
}

static int draw_scroll_cue(uint16_t y, bool up)
{
	uint8_t rows[COMPACT_GLYPH_HEIGHT];
	uint16_t x = (display_width_px > (MENU_SCROLL_CUE_WIDTH + DISPLAY_MARGIN_X)) ?
		(uint16_t)(display_width_px - MENU_SCROLL_CUE_WIDTH - DISPLAY_MARGIN_X) : 0U;

	if (up) {
		memcpy(rows, (uint8_t[]){0x04U, 0x0EU, 0x15U, 0x04U, 0x04U, 0x04U, 0x00U},
			COMPACT_GLYPH_HEIGHT);
	} else {
		memcpy(rows, (uint8_t[]){0x00U, 0x04U, 0x04U, 0x04U, 0x15U, 0x0EU, 0x04U},
			COMPACT_GLYPH_HEIGHT);
	}

	for (uint8_t row = 0U; row < COMPACT_GLYPH_HEIGHT; ++row) {
		for (uint8_t col = 0U; col < COMPACT_GLYPH_WIDTH; ++col) {
			int rc;

			if (((rows[row] >> (COMPACT_GLYPH_WIDTH - 1U - col)) & 0x1U) == 0U) {
				continue;
			}

			rc = draw_pixel(x + col, y + row);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

static int draw_small_percent(uint16_t x, uint16_t y, uint8_t pct)
{
	uint8_t digits[3];
	uint8_t digit_count = 0U;
	uint8_t rows[TINY_GLYPH_HEIGHT];
	int rc;

	if (pct >= 100U) {
		digits[digit_count++] = 1U;
		digits[digit_count++] = 0U;
		digits[digit_count++] = 0U;
	} else if (pct >= 10U) {
		digits[digit_count++] = pct / 10U;
		digits[digit_count++] = pct % 10U;
	} else {
		digits[digit_count++] = pct;
	}

	for (uint8_t index = 0U; index < digit_count; ++index) {
		tiny_digit_rows(digits[index], rows);
		rc = draw_small_glyph(x, y, TINY_GLYPH_WIDTH, rows);
		if (rc != 0) {
			return rc;
		}
		x += TINY_GLYPH_WIDTH + TINY_GLYPH_GAP;
	}

	tiny_percent_rows(rows);
	return draw_small_glyph(x, y, TINY_GLYPH_WIDTH, rows);
}

static int draw_small_text(uint16_t x, uint16_t y, const char *text)
{
	uint8_t rows[TINY_GLYPH_HEIGHT];
	int rc;

	for (uint8_t index = 0U; text[index] != '\0'; ++index) {
		tiny_letter_rows(text[index], rows);
		rc = draw_small_glyph(x, y, TINY_GLYPH_WIDTH, rows);
		if (rc != 0) {
			return rc;
		}
		x += TINY_GLYPH_WIDTH + TINY_GLYPH_GAP;
	}

	return 0;
}

static int draw_charging_bolt(void)
{
	const uint16_t x = BATTERY_BODY_X + ((BATTERY_BODY_WIDTH - CHARGE_BOLT_WIDTH) / 2U);
	const uint16_t y = BATTERY_BODY_Y + ((BATTERY_BODY_HEIGHT - CHARGE_BOLT_HEIGHT) / 2U);

	return draw_small_glyph(x, y, CHARGE_BOLT_WIDTH, charge_bolt_rows);
}

static int draw_battery_meter(uint16_t battery_mv, bool usb_power_present)
{
	const uint8_t pct = battery_mv_to_pct(battery_mv);
	const uint8_t body_inner_width = BATTERY_BODY_WIDTH - (BATTERY_FILL_INSET * 2U);
	const uint8_t body_inner_height = BATTERY_BODY_HEIGHT - (BATTERY_FILL_INSET * 2U);
	const uint8_t fill_width = clamp_u8(
		((uint16_t)body_inner_width * pct + 99U) / 100U,
		body_inner_width);
	const uint16_t cap_x = BATTERY_BODY_X + BATTERY_BODY_WIDTH;
	const uint16_t cap_y = BATTERY_BODY_Y + ((BATTERY_BODY_HEIGHT - BATTERY_CAP_HEIGHT) / 2U);
	int rc;

	rc = draw_rounded_rect(BATTERY_BODY_X, BATTERY_BODY_Y,
		BATTERY_BODY_WIDTH, BATTERY_BODY_HEIGHT);
	if (rc != 0) {
		return rc;
	}

	rc = fill_rect(cap_x, cap_y, BATTERY_CAP_WIDTH, BATTERY_CAP_HEIGHT);
	if (rc != 0) {
		return rc;
	}

	if (usb_power_present) {
		rc = draw_charging_bolt();
		if (rc != 0) {
			return rc;
		}

		return draw_small_text(BATTERY_PERCENT_X, BATTERY_PERCENT_Y, "CHG");
	}

	if (fill_width > 0U) {
		rc = fill_rect(BATTERY_BODY_X + BATTERY_FILL_INSET,
			BATTERY_BODY_Y + BATTERY_FILL_INSET,
			fill_width,
			body_inner_height);
		if (rc != 0) {
			return rc;
		}
	}

	return draw_small_percent(BATTERY_PERCENT_X, BATTERY_PERCENT_Y, pct);
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

	rc = cfb_framebuffer_set_font(display, 0);
	if (rc != 0) {
		LOG_WRN("cfb_framebuffer_set_font failed: %d", rc);
	}

	value = cfb_get_display_parameter(display, CFB_DISPLAY_WIDTH);
	if (value > 0) {
		display_width_px = (uint16_t)value;
	}

	value = cfb_get_display_parameter(display, CFB_DISPLAY_HEIGHT);
	if (value > 0) {
		display_height_px = (uint16_t)value;
	}

	return 0;
}

int status_display_render(const struct status_display_state *state)
{
	int rc;

	if (state == NULL) {
		return -EINVAL;
	}

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	if (state->operating_mode == MACROPAD_OPERATING_MODE_DONGLE) {
		rc = draw_dongle_icon(state->connected, state->dongle_activity);
		if (rc != 0) {
			LOG_ERR("draw dongle icon failed: %d", rc);
			return rc;
		}
		rc = draw_mode_label(state->operating_mode);
		if (rc != 0) {
			LOG_ERR("draw mode label failed: %d", rc);
			return rc;
		}
	} else {
		rc = draw_compact_text_clipped(MODE_LABEL_BLE_X, MODE_LABEL_Y,
			ble_status_text(state->ble_state),
			(display_width_px > MODE_LABEL_BLE_X) ?
				(uint16_t)(display_width_px - MODE_LABEL_BLE_X) : 0U);
		if (rc != 0) {
			LOG_ERR("draw BLE status label failed: %d", rc);
			return rc;
		}
	}

	rc = draw_key_matrix(state->keys_pressed);
	if (rc != 0) {
		LOG_ERR("draw key matrix failed: %d", rc);
		return rc;
	}

	if (state->usb_power_present) {
		rc = draw_usb_icon();
		if (rc != 0) {
			LOG_ERR("draw USB icon failed: %d", rc);
			return rc;
		}
	}

	if (state->keys_locked) {
		rc = draw_locked_label();
		if (rc != 0) {
			LOG_ERR("draw locked label failed: %d", rc);
			return rc;
		}
	}

	if (state->show_battery_warning) {
		rc = draw_battery_warning(BATTERY_WARNING_X, BATTERY_WARNING_Y);
		if (rc != 0) {
			LOG_ERR("draw battery warning failed: %d", rc);
			return rc;
		}
	}

	rc = draw_battery_meter(state->battery_mv, state->usb_power_present);
	if (rc != 0) {
		LOG_ERR("draw battery meter failed: %d", rc);
		return rc;
	}

	rc = cfb_framebuffer_finalize(display);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
	}

	return rc;
}

size_t status_display_menu_visible_rows(void)
{
	if (display_height_px <= MENU_FIRST_ITEM_Y) {
		return 1U;
	}

	return MAX(1U, (size_t)((display_height_px - MENU_FIRST_ITEM_Y) / COMPACT_LINE_HEIGHT));
}

void status_display_menu_clamp_viewport(size_t item_count, size_t selected_index,
					size_t *first_visible_index)
{
	size_t visible_rows;
	size_t max_first;

	if ((first_visible_index == NULL) || (item_count == 0U)) {
		return;
	}

	visible_rows = status_display_menu_visible_rows();
	if (visible_rows >= item_count) {
		*first_visible_index = 0U;
		return;
	}

	max_first = item_count - visible_rows;
	if (*first_visible_index > max_first) {
		*first_visible_index = max_first;
	}
	if (selected_index < *first_visible_index) {
		*first_visible_index = selected_index;
	} else if (selected_index >= (*first_visible_index + visible_rows)) {
		*first_visible_index = selected_index - visible_rows + 1U;
	}
}

int status_display_render_menu(const char *title, const char *const *items,
			       size_t item_count, size_t selected_index,
			       size_t first_visible_index)
{
	size_t visible_rows;
	size_t last_visible_index;
	int rc;

	if ((title == NULL) || ((items == NULL) && (item_count != 0U)) ||
	    ((item_count != 0U) && (selected_index >= item_count))) {
		return -EINVAL;
	}

	status_display_menu_clamp_viewport(item_count, selected_index, &first_visible_index);
	visible_rows = status_display_menu_visible_rows();
	last_visible_index = MIN(item_count, first_visible_index + visible_rows);

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	rc = draw_compact_text_clipped(MENU_TITLE_X, MENU_TITLE_Y, title,
		(display_width_px > MENU_TITLE_X) ?
			(uint16_t)(display_width_px - MENU_TITLE_X) : 0U);
	if (rc != 0) {
		return rc;
	}

	if (first_visible_index > 0U) {
		rc = draw_scroll_cue(MENU_TITLE_Y, true);
		if (rc != 0) {
			return rc;
		}
	}
	if (last_visible_index < item_count) {
		rc = draw_scroll_cue((uint16_t)(MENU_FIRST_ITEM_Y +
			((visible_rows - 1U) * COMPACT_LINE_HEIGHT)), false);
		if (rc != 0) {
			return rc;
		}
	}

	for (size_t index = first_visible_index; index < last_visible_index; ++index) {
		uint16_t y = MENU_FIRST_ITEM_Y +
			(uint16_t)((index - first_visible_index) * COMPACT_LINE_HEIGHT);
		const bool selected = (index == selected_index);

		rc = draw_compact_glyph(MENU_ITEM_X, y, selected ? '>' : ' ');
		if (rc != 0) {
			return rc;
		}

		rc = draw_compact_text_clipped(MENU_ITEM_X + MENU_MARKER_WIDTH, y,
			items[index],
			(display_width_px > (MENU_ITEM_X + MENU_MARKER_WIDTH +
					     MENU_SCROLL_CUE_WIDTH + DISPLAY_MARGIN_X)) ?
				(uint16_t)(display_width_px - MENU_ITEM_X -
					MENU_MARKER_WIDTH - MENU_SCROLL_CUE_WIDTH -
					DISPLAY_MARGIN_X) : 0U);
		if (rc != 0) {
			return rc;
		}

		if (selected) {
			rc = cfb_invert_area(display, 0U, y,
				display_width_px, COMPACT_LINE_HEIGHT);
			if (rc != 0) {
				return rc;
			}
		}
	}

	rc = cfb_framebuffer_finalize(display);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
	}

	return rc;
}

int status_display_render_info(const char *title, const char *const *lines,
			       size_t line_count, size_t first_visible_index)
{
	size_t visible_rows;
	size_t last_visible_index;
	size_t max_first;
	int rc;

	if ((title == NULL) || ((lines == NULL) && (line_count != 0U))) {
		return -EINVAL;
	}

	visible_rows = status_display_menu_visible_rows();
	if (visible_rows >= line_count) {
		first_visible_index = 0U;
	} else {
		max_first = line_count - visible_rows;
		if (first_visible_index > max_first) {
			first_visible_index = max_first;
		}
	}
	last_visible_index = MIN(line_count, first_visible_index + visible_rows);

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	rc = draw_compact_text_clipped(MENU_TITLE_X, MENU_TITLE_Y, title,
		(display_width_px > MENU_TITLE_X) ?
			(uint16_t)(display_width_px - MENU_TITLE_X) : 0U);
	if (rc != 0) {
		return rc;
	}

	if (first_visible_index > 0U) {
		rc = draw_scroll_cue(MENU_TITLE_Y, true);
		if (rc != 0) {
			return rc;
		}
	}
	if (last_visible_index < line_count) {
		rc = draw_scroll_cue((uint16_t)(MENU_FIRST_ITEM_Y +
			((visible_rows - 1U) * COMPACT_LINE_HEIGHT)), false);
		if (rc != 0) {
			return rc;
		}
	}

	for (size_t index = first_visible_index; index < last_visible_index; ++index) {
		uint16_t y = MENU_FIRST_ITEM_Y +
			(uint16_t)((index - first_visible_index) * COMPACT_LINE_HEIGHT);

		rc = draw_compact_text_clipped(MENU_ITEM_X, y, lines[index],
			(display_width_px > (MENU_ITEM_X + MENU_SCROLL_CUE_WIDTH +
					     DISPLAY_MARGIN_X)) ?
				(uint16_t)(display_width_px - MENU_ITEM_X -
					MENU_SCROLL_CUE_WIDTH - DISPLAY_MARGIN_X) : 0U);
		if (rc != 0) {
			return rc;
		}
	}

	rc = cfb_framebuffer_finalize(display);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
	}

	return rc;
}

int status_display_render_message(const char *line1, const char *line2)
{
	uint16_t y;
	int rc;

	if (line1 == NULL) {
		return -EINVAL;
	}

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	if (line2 == NULL) {
		y = (display_height_px > COMPACT_GLYPH_HEIGHT) ?
			(uint16_t)((display_height_px - COMPACT_GLYPH_HEIGHT) / 2U) : 0U;
	} else {
		const uint16_t block_height = (COMPACT_GLYPH_HEIGHT * 2U) + MESSAGE_LINE_GAP;

		y = (display_height_px > block_height) ?
			(uint16_t)((display_height_px - block_height) / 2U) : 0U;
	}

	for (uint8_t line = 0U; line < 2U; ++line) {
		const char *text = (line == 0U) ? line1 : line2;
		uint16_t width;
		uint16_t x;

		if (text == NULL) {
			break;
		}

		width = compact_text_width(text);
		x = (display_width_px > width) ? (uint16_t)((display_width_px - width) / 2U) : 0U;
		rc = draw_compact_text_clipped(x, y, text, display_width_px);
		if (rc != 0) {
			return rc;
		}

		y += COMPACT_GLYPH_HEIGHT + MESSAGE_LINE_GAP;
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

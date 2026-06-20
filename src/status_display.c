#include "status_display.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(status_display, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_DEFAULT_WIDTH 128U
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

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static uint16_t display_width_px = DISPLAY_DEFAULT_WIDTH;

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
	case 'C':
		rows[0] = 0x06U;
		rows[1] = 0x08U;
		rows[2] = 0x08U;
		rows[3] = 0x08U;
		rows[4] = 0x06U;
		break;
	case 'H':
		rows[0] = 0x09U;
		rows[1] = 0x09U;
		rows[2] = 0x0FU;
		rows[3] = 0x09U;
		rows[4] = 0x09U;
		break;
	case 'G':
	default:
		rows[0] = 0x06U;
		rows[1] = 0x08U;
		rows[2] = 0x0BU;
		rows[3] = 0x09U;
		rows[4] = 0x07U;
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

	value = cfb_get_display_parameter(display, CFB_DISPLAY_WIDTH);
	if (value > 0) {
		display_width_px = (uint16_t)value;
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

	rc = draw_dongle_icon(state->connected, state->dongle_activity);
	if (rc != 0) {
		LOG_ERR("draw dongle icon failed: %d", rc);
		return rc;
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

int status_display_blank(void)
{
	return display_blanking_on(display);
}

int status_display_unblank(void)
{
	return display_blanking_off(display);
}
#ifndef HPADV2_STATUS_DISPLAY_H_
#define HPADV2_STATUS_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "macropad_config.h"
#include "macropad_mode.h"

#define STATUS_DISPLAY_REFRESH_INTERVAL_MS 40

struct status_display_state {
	enum macropad_operating_mode operating_mode;
	enum macropad_ble_link_state ble_state;
	uint8_t ble_retry_attempt;
	uint8_t ble_retry_seconds_remaining;
	bool connected;
	bool dongle_activity;
	bool usb_power_present;
	bool show_battery_warning;
	bool keys_locked;
	uint16_t battery_mv;
	uint8_t keys_pressed;
};

int status_display_init(void);
int status_display_render(const struct status_display_state *state);
int status_display_render_menu(const char *title, const char *const *items,
			       size_t item_count, size_t selected_index,
			       size_t first_visible_index);
int status_display_render_info(const char *title, const char *const *lines,
			       size_t line_count, size_t first_visible_index);
int status_display_render_message(const char *line1, const char *line2);
int status_display_render_brightness(uint8_t brightness);
size_t status_display_menu_visible_rows(void);
void status_display_menu_clamp_viewport(size_t item_count, size_t selected_index,
					size_t *first_visible_index);
int status_display_set_brightness(uint8_t brightness);
uint8_t status_display_get_brightness(void);
int status_display_blank(void);
int status_display_unblank(void);

#endif

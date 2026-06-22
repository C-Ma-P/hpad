#ifndef HPADV2_STATUS_DISPLAY_H_
#define HPADV2_STATUS_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "macropad_config.h"

#define STATUS_DISPLAY_REFRESH_INTERVAL_MS 40

struct status_display_state {
	enum macropad_operating_mode operating_mode;
	bool connected;
	bool dongle_activity;
	bool usb_power_present;
	bool show_battery_warning;
	uint16_t battery_mv;
	uint8_t keys_pressed;
};

int status_display_init(void);
int status_display_render(const struct status_display_state *state);
int status_display_render_menu(const char *title, const char *const *items,
			       size_t item_count, size_t selected_index);
int status_display_blank(void);
int status_display_unblank(void);

#endif

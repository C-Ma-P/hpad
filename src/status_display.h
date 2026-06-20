#ifndef HPADV2_STATUS_DISPLAY_H_
#define HPADV2_STATUS_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#define STATUS_DISPLAY_REFRESH_INTERVAL_MS 40

struct status_display_state {
	bool connected;
	bool dongle_activity;
	bool usb_power_present;
	bool show_battery_warning;
	uint16_t battery_mv;
	uint8_t keys_pressed;
};

int status_display_init(void);
int status_display_render(const struct status_display_state *state);
int status_display_blank(void);
int status_display_unblank(void);

#endif
#ifndef HPADV2_STATUS_DISPLAY_H_
#define HPADV2_STATUS_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#define STATUS_DISPLAY_REFRESH_INTERVAL_MS 40

int status_display_init(void);
int status_display_render(bool connected, bool usb_power_present, uint16_t battery_mv);
int status_display_blank(void);
int status_display_unblank(void);

#endif
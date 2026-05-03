#ifndef HPADV2_STATUS_DISPLAY_H_
#define HPADV2_STATUS_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#define STATUS_DISPLAY_REFRESH_INTERVAL_MS 40

int status_display_init(void);
int status_display_render(bool connected, bool show_ack, bool usb_power_present, int32_t value);

#endif
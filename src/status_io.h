#ifndef HPADV2_STATUS_IO_H_
#define HPADV2_STATUS_IO_H_

#include <stdbool.h>
#include <stdint.h>

int status_led_init(void);
void status_led_blink(uint8_t count, uint32_t pulse_ms, uint32_t gap_ms);
void status_led_pulse(uint32_t pulse_ms);

int status_buzzer_init(void);
void status_buzzer_set(bool on);

bool status_usb_power_present(void);

#endif
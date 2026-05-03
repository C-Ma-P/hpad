#ifndef HPADV2_BATTERY_H_
#define HPADV2_BATTERY_H_

#include <stdint.h>

int battery_init(void);
uint16_t battery_sample_mv(void);

#endif
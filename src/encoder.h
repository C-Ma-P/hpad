#ifndef HPADV2_ENCODER_H_
#define HPADV2_ENCODER_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*encoder_delta_callback_t)(int8_t delta);
typedef void (*encoder_button_callback_t)(bool pressed);

int encoder_init(encoder_delta_callback_t delta_callback,
		 encoder_button_callback_t button_callback);

#endif
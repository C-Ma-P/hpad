#ifndef HPADV2_BUTTON_H_
#define HPADV2_BUTTON_H_

#include <stdbool.h>

typedef void (*button_state_callback_t)(bool pressed);

int button_init(button_state_callback_t callback);

#endif
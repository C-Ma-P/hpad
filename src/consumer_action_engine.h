#ifndef HPADV2_CONSUMER_ACTION_ENGINE_H_
#define HPADV2_CONSUMER_ACTION_ENGINE_H_

#include <stdbool.h>
#include <stdint.h>

#include "wire_protocol.h"

#define CONSUMER_ACTION_NONE 0U

enum consumer_action_id {
	CONSUMER_ACTION_VOLUME_UP = 1,
	CONSUMER_ACTION_VOLUME_DOWN = 2,
	CONSUMER_ACTION_MUTE = 3,
	CONSUMER_ACTION_NEXT_TRACK = 4,
	CONSUMER_ACTION_PREVIOUS_TRACK = 5,
	CONSUMER_ACTION_STOP = 6,
	CONSUMER_ACTION_PLAY_PAUSE = 7,
};

struct consumer_action_engine {
	uint8_t previous_keys;
	bool previous_encoder_pressed;
};

typedef uint16_t (*consumer_action_engine_key_action_t)(uint8_t key_index,
							void *user_data);
typedef int (*consumer_action_engine_trigger_action_t)(uint16_t action_id,
						       void *user_data);

void consumer_action_engine_reset(struct consumer_action_engine *engine);
bool consumer_action_engine_input_activity(const struct consumer_action_engine *engine,
					   const macropad_report_t *report);
int consumer_action_engine_process_report(struct consumer_action_engine *engine,
					  const macropad_report_t *report,
					  consumer_action_engine_key_action_t key_action,
					  consumer_action_engine_trigger_action_t trigger_action,
					  void *user_data);

#endif

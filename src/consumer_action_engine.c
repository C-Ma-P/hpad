#include "consumer_action_engine.h"

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(consumer_action_engine, LOG_LEVEL_INF);

#define MACROPAD_KEY_MASK BIT_MASK(HPAD_PROTOCOL_KEY_COUNT)

void consumer_action_engine_reset(struct consumer_action_engine *engine)
{
	if (engine == NULL) {
		return;
	}

	engine->previous_keys = 0U;
	engine->previous_encoder_pressed = false;
}

bool consumer_action_engine_input_activity(const struct consumer_action_engine *engine,
					   const macropad_report_t *report)
{
	uint8_t current_keys;

	if ((engine == NULL) || (report == NULL)) {
		return false;
	}

	current_keys = report->keys & MACROPAD_KEY_MASK;
	return (current_keys != engine->previous_keys) ||
		(report->encoder_delta != 0) ||
		(engine->previous_encoder_pressed != (report->encoder_pressed != 0U));
}

static void remember_first_error(int rc, int *first_error)
{
	if ((rc != 0) && (*first_error == 0)) {
		*first_error = rc;
	}
}

int consumer_action_engine_process_report(struct consumer_action_engine *engine,
					  const macropad_report_t *report,
					  consumer_action_engine_key_action_t key_action,
					  consumer_action_engine_trigger_action_t trigger_action,
					  void *user_data)
{
	uint8_t current_keys;
	uint8_t newly_pressed;
	bool encoder_pressed_now;
	int first_error = 0;
	int rc;

	if ((engine == NULL) || (report == NULL) || (trigger_action == NULL)) {
		return -EINVAL;
	}

	current_keys = report->keys & MACROPAD_KEY_MASK;
	newly_pressed = current_keys & ~engine->previous_keys;
	encoder_pressed_now = (report->encoder_pressed != 0U);

	for (uint8_t i = 0; i < HPAD_PROTOCOL_KEY_COUNT; i++) {
		if (newly_pressed & BIT(i)) {
			uint16_t action = CONSUMER_ACTION_NONE;

			if (key_action != NULL) {
				action = key_action(i, user_data);
			}

			if (action != CONSUMER_ACTION_NONE) {
				rc = trigger_action(action, user_data);
				if (rc != 0) {
					LOG_WRN("Consumer action key=%u action=%u failed: %d",
						i, action, rc);
				}
				remember_first_error(rc, &first_error);
			}
		}
	}

	if (report->encoder_delta != 0) {
		uint16_t action = (report->encoder_delta > 0)
			? CONSUMER_ACTION_VOLUME_UP
			: CONSUMER_ACTION_VOLUME_DOWN;
		int steps = (report->encoder_delta > 0)
			? report->encoder_delta
			: -report->encoder_delta;

		for (int s = 0; s < steps; s++) {
			rc = trigger_action(action, user_data);
			if (rc != 0) {
				LOG_WRN("Consumer encoder volume action=%u failed: %d",
					action, rc);
			}
			remember_first_error(rc, &first_error);
		}
	}

	if (!engine->previous_encoder_pressed && encoder_pressed_now) {
		rc = trigger_action(CONSUMER_ACTION_MUTE, user_data);
		if (rc != 0) {
			LOG_WRN("Consumer encoder mute action failed: %d", rc);
		}
		remember_first_error(rc, &first_error);
	}

	engine->previous_keys = current_keys;
	engine->previous_encoder_pressed = encoder_pressed_now;
	return first_error;
}

/*
 * Copyright (c) 2018 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "encoder.h"
#include "key_leds.h"
#include "macropad_config.h"
#include "macropad_keys.h"
#include "radio_esb.h"
#include "status_display.h"
#include "status_io.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define ACK_VISIBLE_MS 50
#define HEARTBEAT_INTERVAL_MS 1000
#define CONNECTION_TIMEOUT_MS 2500
#define INPUT_ACTIVITY_LED_PULSE_MS 80
#define APP_EVENT_QUEUE_LEN 8

static struct k_spinlock input_state_lock;

enum app_event_type {
	APP_EVENT_ENCODER_DELTA = 0,
	APP_EVENT_ENCODER_BUTTON = 1,
	APP_EVENT_TX_RESULT = 2,
	APP_EVENT_MACROPAD_KEYS = 3,
	APP_EVENT_MACROPAD_CONFIG = 4,
};

struct app_event {
	uint8_t type;
	uint8_t tx_kind;
	uint8_t acked;
	uint8_t pressed;
	uint8_t keys;
	uint8_t key_mask;
	int8_t encoder_delta;
	macropad_config_t config;
};

struct app_ui_state {
	bool connected;
	bool usb_power_present;
	bool show_ack;
	int64_t ack_visible_until_ms;
	int64_t last_delivery_success_ms;
	int32_t value;
};

struct macropad_input_state {
	uint8_t keys;
	int8_t encoder_delta;
	uint8_t encoder_pressed;
	uint16_t battery_mv;
	uint8_t usb_power_present;
};

K_MSGQ_DEFINE(app_event_queue, sizeof(struct app_event), APP_EVENT_QUEUE_LEN, 4);

static struct macropad_input_state macropad_input_state;
static int32_t pending_encoder_delta;
static bool encoder_delta_event_pending;

static int app_queue_event(const struct app_event *event)
{
	int rc = k_msgq_put(&app_event_queue, event, K_NO_WAIT);

	if (rc != 0) {
		LOG_WRN("Dropping app event type=%u rc=%d", event->type, rc);
	}

	return rc;
}

static void refresh_battery_state(struct macropad_input_state *state)
{
	state->battery_mv = battery_sample_mv();
	state->usb_power_present = status_usb_power_present() ? 1U : 0U;
}

static int macropad_send_report(void)
{
	macropad_report_t report;

	refresh_battery_state(&macropad_input_state);
	report = (macropad_report_t){
		.keys = macropad_input_state.keys,
		.encoder_delta = macropad_input_state.encoder_delta,
		.encoder_pressed = macropad_input_state.encoder_pressed,
		.battery_mv = macropad_input_state.battery_mv,
		.usb_power_present = macropad_input_state.usb_power_present,
	};

	return radio_esb_send_macropad_report(&report);
}

static int macropad_send_heartbeat(void)
{
	macropad_report_t report = {
		.keys = 0U,
		.encoder_delta = 0,
		.encoder_pressed = 0U,
	};

	refresh_battery_state(&macropad_input_state);
	report.battery_mv = macropad_input_state.battery_mv;
	report.usb_power_present = macropad_input_state.usb_power_present;

	return radio_esb_send_heartbeat(&report);
}

static int send_encoder_delta_reports(int32_t total_delta)
{
	int rc = 0;
	int8_t step_delta;

	while (total_delta != 0) {
		if (total_delta > INT8_MAX) {
			step_delta = INT8_MAX;
		} else if (total_delta < INT8_MIN) {
			step_delta = INT8_MIN;
		} else {
			step_delta = (int8_t)total_delta;
		}

		macropad_input_state.encoder_delta = step_delta;
		rc = macropad_send_report();
		if (rc != 0) {
			break;
		}

		total_delta -= step_delta;
	}

	macropad_input_state.encoder_delta = 0;
	return rc;
}

static void handle_encoder_delta(int8_t delta)
{
	const struct app_event event = {
		.type = APP_EVENT_ENCODER_DELTA,
	};
	k_spinlock_key_t key;
	bool should_queue = false;
	int rc;

	if (delta == 0) {
		return;
	}

	key = k_spin_lock(&input_state_lock);
	pending_encoder_delta += delta;
	if (!encoder_delta_event_pending) {
		encoder_delta_event_pending = true;
		should_queue = true;
	}
	k_spin_unlock(&input_state_lock, key);

	if (!should_queue) {
		return;
	}

	rc = app_queue_event(&event);
	if (rc != 0) {
		key = k_spin_lock(&input_state_lock);
		encoder_delta_event_pending = false;
		k_spin_unlock(&input_state_lock, key);
	}
}

static void handle_encoder_button(bool pressed)
{
	const struct app_event event = {
		.type = APP_EVENT_ENCODER_BUTTON,
		.pressed = pressed ? 1U : 0U,
	};

	(void)app_queue_event(&event);
}

static void handle_macropad_keys(uint8_t key_mask, bool pressed, uint8_t keys)
{
	const struct app_event event = {
		.type = APP_EVENT_MACROPAD_KEYS,
		.pressed = pressed ? 1U : 0U,
		.keys = keys,
		.key_mask = key_mask,
	};

	if (key_mask == 0U) {
		return;
	}

	(void)app_queue_event(&event);
}

static void handle_radio_delivery(enum radio_esb_tx_kind kind, bool acked)
{
	const struct app_event event = {
		.type = APP_EVENT_TX_RESULT,
		.tx_kind = (uint8_t)kind,
		.acked = acked ? 1U : 0U,
	};

	(void)app_queue_event(&event);
}

static void handle_radio_config(const macropad_config_t *config)
{
	struct app_event event = {
		.type = APP_EVENT_MACROPAD_CONFIG,
		.config = *config,
	};

	(void)app_queue_event(&event);
}

static bool update_time_driven_ui(struct app_ui_state *state, int64_t now_ms)
{
	bool dirty = false;
	bool usb_power_present = status_usb_power_present();

	if (state->usb_power_present != usb_power_present) {
		state->usb_power_present = usb_power_present;
		dirty = true;
	}

	if (state->show_ack && (now_ms >= state->ack_visible_until_ms)) {
		state->show_ack = false;
		dirty = true;
	}

	if (state->connected &&
	    (state->last_delivery_success_ms != INT64_MIN) &&
	    ((now_ms - state->last_delivery_success_ms) >= CONNECTION_TIMEOUT_MS)) {
		state->connected = false;
		dirty = true;
	}

	return dirty;
}

static bool handle_tx_result_event(struct app_ui_state *state, const struct app_event *event, int64_t now_ms)
{
	bool dirty = false;

	if (event->acked != 0U) {
		state->last_delivery_success_ms = now_ms;
		if (!state->connected) {
			state->connected = true;
			dirty = true;
		}

		if (event->tx_kind == RADIO_ESB_TX_INPUT_REPORT) {
			state->show_ack = true;
			state->ack_visible_until_ms = now_ms + ACK_VISIBLE_MS;
			dirty = true;
		}
	} else if (state->connected) {
		state->connected = false;
		dirty = true;
	}

	return dirty;
}

int main(void)
{
	struct dongle_identity identity;
	struct esb_addr_config addr_config;
	struct app_event event;
	struct app_ui_state ui_state = {
		.connected = false,
		.usb_power_present = false,
		.show_ack = false,
		.ack_visible_until_ms = 0,
		.last_delivery_success_ms = INT64_MIN,
		.value = 0,
	};
	int64_t next_heartbeat_ms = 0;
	int64_t last_display_update_ms = INT64_MIN;
	uint8_t rf_channel;
	bool generated;
	bool redraw = true;
	int rc;

	rc = status_led_init();
	if (rc == 0) {
		status_led_blink(1, 120, 120);
	} else {
		LOG_INF("Status LED unavailable (rc=%d)", rc);
	}

	rc = status_buzzer_init();
	if (rc != 0) {
		LOG_INF("Buzzer unavailable (rc=%d)", rc);
	}

	rc = macropad_config_init();
	if (rc != 0) {
		LOG_INF("Macropad LED config unavailable (rc=%d)", rc);
	}

	rc = key_leds_init();
	if (rc != 0) {
		LOG_INF("Macropad LED strip unavailable (rc=%d)", rc);
	}

	rc = battery_init();
	if (rc != 0) {
		LOG_INF("Battery monitor unavailable (rc=%d)", rc);
	}
	ui_state.usb_power_present = status_usb_power_present();

	LOG_INF("boot: entering main()");
	rc = status_display_init();
	if (rc != 0) {
		return 0;
	}

	k_msleep(1000);
	rc = status_display_render(ui_state.connected, ui_state.show_ack,
		ui_state.usb_power_present, ui_state.value);
	if (rc != 0) {
		return 0;
	}

	rc = radio_identity_init();
	if (rc != 0) {
		LOG_ERR("radio_identity_init failed: %d", rc);
		return 0;
	}

	rc = radio_identity_load_or_create(&identity, &generated);
	if (rc != 0) {
		LOG_ERR("radio_identity_load_or_create failed: %d", rc);
		return 0;
	}

	LOG_INF("Macropad identity synchronized");
	rc = radio_identity_derive_esb_config(&identity, &addr_config, &rf_channel);
	if (rc != 0) {
		LOG_ERR("radio_identity_derive_esb_config failed: %d", rc);
		return 0;
	}

	radio_identity_log_esb_config(&addr_config, rf_channel);
	rc = radio_esb_init(&addr_config, rf_channel, handle_radio_delivery, handle_radio_config);
	if (rc != 0) {
		LOG_ERR("radio_esb_init failed: %d", rc);
		return 0;
	}

	rc = encoder_init(handle_encoder_delta, handle_encoder_button);
	if ((rc != 0) && (rc != -ENOTSUP)) {
		LOG_ERR("encoder_init failed: %d", rc);
		return 0;
	}
	if (rc == -ENOTSUP) {
		LOG_INF("Encoder input disabled");
	}

	rc = macropad_keys_init(handle_macropad_keys);
	if ((rc != 0) && (rc != -ENOTSUP)) {
		LOG_ERR("macropad_keys_init failed: %d", rc);
		return 0;
	}
	if (rc == -ENOTSUP) {
		LOG_INF("Macropad keys disabled");
	} else {
		macropad_input_state.keys = macropad_keys_get_pressed_mask();
		rc = key_leds_update(macropad_input_state.keys);
		if (rc != 0) {
			LOG_WRN("Failed to initialize macropad LEDs: %d", rc);
		}
	}

	rc = radio_esb_start();
	if (rc != 0) {
		LOG_ERR("radio_esb_start failed: %d", rc);
		return 0;
	}

	LOG_INF("Macropad sender ready");
	next_heartbeat_ms = k_uptime_get();

	while (1) {
		int64_t now_ms = k_uptime_get();
		int64_t wait_ms;

		if (update_time_driven_ui(&ui_state, now_ms)) {
			redraw = true;
		}

		if (now_ms >= next_heartbeat_ms) {
			rc = macropad_send_heartbeat();
			if (rc != 0) {
				LOG_WRN("macropad_send_heartbeat failed: %d", rc);
			}
			next_heartbeat_ms = now_ms + HEARTBEAT_INTERVAL_MS;
		}

		if (redraw && ((last_display_update_ms == INT64_MIN) ||
			      ((now_ms - last_display_update_ms) >= STATUS_DISPLAY_REFRESH_INTERVAL_MS))) {
			rc = status_display_render(ui_state.connected, ui_state.show_ack,
				ui_state.usb_power_present, ui_state.value);
			if (rc != 0) {
				LOG_ERR("Display update failed: %d", rc);
			}
			last_display_update_ms = now_ms;
			redraw = false;
		}

		now_ms = k_uptime_get();
		wait_ms = next_heartbeat_ms - now_ms;
		if (redraw) {
			int64_t display_wait_ms = 0;

			if (last_display_update_ms != INT64_MIN) {
				display_wait_ms = STATUS_DISPLAY_REFRESH_INTERVAL_MS -
					(now_ms - last_display_update_ms);
				if (display_wait_ms < 0) {
					display_wait_ms = 0;
				}
			}

			if (display_wait_ms < wait_ms) {
				wait_ms = display_wait_ms;
			}
		}

		if (ui_state.show_ack) {
			int64_t ack_wait_ms = ui_state.ack_visible_until_ms - now_ms;
			if (ack_wait_ms < wait_ms) {
				wait_ms = ack_wait_ms;
			}
		}

		if (ui_state.connected && (ui_state.last_delivery_success_ms != INT64_MIN)) {
			int64_t disconnect_wait_ms =
				(ui_state.last_delivery_success_ms + CONNECTION_TIMEOUT_MS) - now_ms;
			if (disconnect_wait_ms < wait_ms) {
				wait_ms = disconnect_wait_ms;
			}
		}

		if (wait_ms < 0) {
			wait_ms = 0;
		}

		rc = k_msgq_get(&app_event_queue, &event,
				K_MSEC(wait_ms));
		if (rc != 0) {
			continue;
		}

		now_ms = k_uptime_get();
		if (event.type == APP_EVENT_ENCODER_DELTA) {
			int32_t encoder_delta;
			k_spinlock_key_t key;

			key = k_spin_lock(&input_state_lock);
			encoder_delta = pending_encoder_delta;
			pending_encoder_delta = 0;
			encoder_delta_event_pending = false;
			k_spin_unlock(&input_state_lock, key);

			if (encoder_delta == 0) {
				continue;
			}

			status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
			ui_state.value += encoder_delta;
			redraw = true;

			rc = send_encoder_delta_reports(encoder_delta);
			if (rc != 0) {
				LOG_ERR("send_encoder_delta_reports failed: %d", rc);
			}
		} else if (event.type == APP_EVENT_ENCODER_BUTTON) {
			macropad_input_state.encoder_pressed = event.pressed;
			status_buzzer_set(event.pressed != 0U);
			rc = macropad_send_report();
			if (rc != 0) {
				LOG_ERR("macropad_send_report failed: %d", rc);
			}

			if (event.pressed == 0U) {
				continue;
			}

			ui_state.value = -ui_state.value;
			redraw = true;
		} else if (event.type == APP_EVENT_MACROPAD_KEYS) {
			macropad_input_state.keys = event.keys;
			rc = key_leds_update(macropad_input_state.keys);
			if (rc != 0) {
				LOG_WRN("Failed to update macropad LEDs: %d", rc);
			}
			rc = macropad_send_report();
			if (rc != 0) {
				LOG_ERR("macropad_send_report failed: %d", rc);
			}

			if (event.pressed == 0U) {
				continue;
			}

			status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
			ui_state.value += 1;
			redraw = true;
		} else if (event.type == APP_EVENT_MACROPAD_CONFIG) {
			key_leds_apply_config(&event.config);
			rc = key_leds_update(macropad_input_state.keys);
			if (rc != 0) {
				LOG_WRN("Failed to apply macropad config: %d", rc);
			}
		} else if (event.type == APP_EVENT_TX_RESULT) {
			if (handle_tx_result_event(&ui_state, &event, now_ms)) {
				redraw = true;
			}
		}
	}

	return 0;
}

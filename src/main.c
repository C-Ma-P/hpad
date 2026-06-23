/*
 * Copyright (c) 2018 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#if defined(CONFIG_MPSL_DYNAMIC_INTERRUPTS)
#include <mpsl.h>
#include <mpsl/mpsl_lib.h>
#endif

#include "battery.h"
#include "ble_hid.h"
#include "bringup.h"
#include "encoder.h"
#include "key_leds.h"
#include "macropad_config.h"
#include "macropad_keys.h"
#include "radio_esb.h"
#include "status_display.h"
#include "status_io.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define HEARTBEAT_INTERVAL_MS 1000
#define CONNECTION_TIMEOUT_MS 2500
#define INPUT_ACTIVITY_LED_PULSE_MS 80
#define APP_EVENT_QUEUE_LEN 8
#define DISPLAY_SLEEP_TIMEOUT_MS 30000
#define DONGLE_ACTIVITY_PULSE_MS 150
#define BATTERY_WARNING_MV 3500U
#define BATTERY_CRITICAL_MV 3400U
#define BATTERY_POWEROFF_MV 3250U
#define BATTERY_POWEROFF_CLEAR_MV 3350U
#define BATTERY_POWEROFF_SUSTAIN_MS 60000
#define BATTERY_CRITICAL_BLINK_MS 500
#define ENCODER_LONG_PRESS_MS 700
#define OPTIONS_MENU_ITEM_COUNT 3U
#define TRANSIENT_MESSAGE_HOLD_MS 700

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
	uint8_t acked;
	uint8_t pressed;
	uint8_t keys;
	uint8_t key_mask;
	int8_t encoder_delta;
	macropad_config_t config;
};

struct app_ui_state {
	enum macropad_operating_mode operating_mode;
	enum ble_hid_state ble_state;
	bool connected;
	bool dongle_activity;
	bool usb_power_present;
	bool battery_warning_visible;
	int64_t last_delivery_success_ms;
};

enum options_menu_action {
	OPTIONS_MENU_ACTION_SWITCH_MODE = 0,
	OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING = 1,
	OPTIONS_MENU_ACTION_CLOSE = 2,
};

struct options_menu_item {
	enum options_menu_action action;
};

struct options_menu_state {
	bool open;
	size_t selected_index;
	size_t first_visible_index;
};

struct battery_policy_state {
	uint16_t filtered_mv;
	int64_t below_poweroff_since_ms;
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
static struct battery_policy_state battery_policy_state = {
	.filtered_mv = 0U,
	.below_poweroff_since_ms = INT64_MIN,
};
static int32_t pending_encoder_delta;
static bool encoder_delta_event_pending;

static const struct options_menu_item options_menu_items[OPTIONS_MENU_ITEM_COUNT] = {
	{ .action = OPTIONS_MENU_ACTION_SWITCH_MODE },
	{ .action = OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING },
	{ .action = OPTIONS_MENU_ACTION_CLOSE },
};

static void handle_radio_delivery(enum radio_esb_tx_kind kind, bool acked);
static void handle_radio_config(const macropad_config_t *config);

static int app_queue_event(const struct app_event *event)
{
	int rc = k_msgq_put(&app_event_queue, event, K_NO_WAIT);

	if (rc != 0) {
		LOG_WRN("Dropping app event type=%u rc=%d", event->type, rc);
	}

	return rc;
}

static void update_battery_filter(uint16_t battery_mv)
{
	if (battery_mv == 0U) {
		return;
	}

	if (battery_policy_state.filtered_mv == 0U) {
		battery_policy_state.filtered_mv = battery_mv;
		return;
	}

	battery_policy_state.filtered_mv = (uint16_t)
		(((uint32_t)battery_policy_state.filtered_mv * 3U + battery_mv + 2U) / 4U);
}

static void update_battery_policy(uint16_t battery_mv, bool usb_power_present, int64_t now_ms)
{
	update_battery_filter(battery_mv);

	if (usb_power_present || (battery_policy_state.filtered_mv == 0U)) {
		battery_policy_state.below_poweroff_since_ms = INT64_MIN;
		return;
	}

	if (battery_policy_state.filtered_mv < BATTERY_POWEROFF_MV) {
		if (battery_policy_state.below_poweroff_since_ms == INT64_MIN) {
			battery_policy_state.below_poweroff_since_ms = now_ms;
		}
	} else if (battery_policy_state.filtered_mv >= BATTERY_POWEROFF_CLEAR_MV) {
		battery_policy_state.below_poweroff_since_ms = INT64_MIN;
	}
}

static void refresh_battery_state(struct macropad_input_state *state)
{
	const bool usb_power_present = status_usb_power_present();
	const int64_t now_ms = k_uptime_get();

	state->battery_mv = battery_sample_mv();
	state->usb_power_present = usb_power_present ? 1U : 0U;
	update_battery_policy(state->battery_mv, usb_power_present, now_ms);
}

static bool battery_warning_active(bool usb_power_present)
{
	return (battery_policy_state.filtered_mv > 0U) &&
		(battery_policy_state.filtered_mv <= BATTERY_WARNING_MV) &&
		!usb_power_present;
}

static bool battery_critical_active(bool usb_power_present)
{
	return (battery_policy_state.filtered_mv > 0U) &&
		(battery_policy_state.filtered_mv <= BATTERY_CRITICAL_MV) &&
		!usb_power_present;
}

static bool battery_warning_visible(bool usb_power_present, int64_t now_ms)
{
	if (!battery_warning_active(usb_power_present)) {
		return false;
	}

	if (!battery_critical_active(usb_power_present)) {
		return true;
	}

	return (((uint64_t)now_ms / BATTERY_CRITICAL_BLINK_MS) & 0x1U) == 0U;
}

static bool battery_poweroff_required(bool usb_power_present, int64_t now_ms)
{
	return !usb_power_present &&
		(battery_policy_state.filtered_mv > 0U) &&
		(battery_policy_state.filtered_mv < BATTERY_POWEROFF_MV) &&
		(battery_policy_state.below_poweroff_since_ms != INT64_MIN) &&
		((now_ms - battery_policy_state.below_poweroff_since_ms) >=
			BATTERY_POWEROFF_SUSTAIN_MS);
}

static uint16_t battery_display_mv(void)
{
	if (battery_policy_state.filtered_mv != 0U) {
		return battery_policy_state.filtered_mv;
	}

	return macropad_input_state.battery_mv;
}

static enum macropad_operating_mode opposite_mode(enum macropad_operating_mode mode)
{
	return (mode == MACROPAD_OPERATING_MODE_BLE) ?
		MACROPAD_OPERATING_MODE_DONGLE : MACROPAD_OPERATING_MODE_BLE;
}

static const char *mode_menu_label(enum macropad_operating_mode current_mode)
{
	return (current_mode == MACROPAD_OPERATING_MODE_BLE) ?
		"Dongle Mode" : "BLE Mode";
}

static void options_menu_labels(enum macropad_operating_mode current_mode,
				const char *labels[OPTIONS_MENU_ITEM_COUNT])
{
	labels[0] = mode_menu_label(current_mode);
	labels[1] = "Forget Pairing";
	labels[2] = "Close";
}

static void options_menu_open(struct options_menu_state *menu)
{
	menu->open = true;
	menu->selected_index = 0U;
	menu->first_visible_index = 0U;
}

static void options_menu_close(struct options_menu_state *menu)
{
	menu->open = false;
	menu->selected_index = 0U;
	menu->first_visible_index = 0U;
}

static void options_menu_move(struct options_menu_state *menu, int32_t delta)
{
	if (!menu->open || (delta == 0)) {
		return;
	}

	while (delta > 0) {
		menu->selected_index = (menu->selected_index + 1U) %
			ARRAY_SIZE(options_menu_items);
		status_display_menu_clamp_viewport(ARRAY_SIZE(options_menu_items),
			menu->selected_index, &menu->first_visible_index);
		delta--;
	}
	while (delta < 0) {
		menu->selected_index = (menu->selected_index == 0U) ?
			(ARRAY_SIZE(options_menu_items) - 1U) : (menu->selected_index - 1U);
		status_display_menu_clamp_viewport(ARRAY_SIZE(options_menu_items),
			menu->selected_index, &menu->first_visible_index);
		delta++;
	}
}

static void show_transient_message(bool display_available, bool *display_sleeping,
				   const char *line1, const char *line2, int32_t hold_ms)
{
	int rc;

	if (!display_available) {
		return;
	}

	if ((display_sleeping != NULL) && *display_sleeping) {
		(void)status_display_unblank();
		*display_sleeping = false;
	}

	rc = status_display_render_message(line1, line2);
	if (rc != 0) {
		LOG_WRN("Transient display message failed: %d", rc);
		return;
	}

	if (hold_ms > 0) {
		k_msleep(hold_ms);
	}
}

static const char *starting_message(enum macropad_operating_mode mode)
{
	return (mode == MACROPAD_OPERATING_MODE_BLE) ?
		"Starting BLE..." : "Starting Dongle...";
}

static const char *failed_message(enum macropad_operating_mode mode)
{
	return (mode == MACROPAD_OPERATING_MODE_BLE) ?
		"BLE failed" : "Dongle failed";
}

static int start_dongle_transport(void)
{
	struct dongle_identity identity;
	struct esb_addr_config addr_config;
	uint8_t rf_channel;
	bool generated;
	int rc;

#if defined(CONFIG_MPSL_DYNAMIC_INTERRUPTS)
	if (mpsl_is_initialized()) {
		int32_t mpsl_rc = mpsl_lib_uninit();

		if (mpsl_rc != 0) {
			LOG_ERR("mpsl_lib_uninit failed before ESB start: %d", (int)mpsl_rc);
			return -EIO;
		}
	}
#endif

	rc = radio_identity_init();
	if (rc != 0) {
		LOG_ERR("radio_identity_init failed: %d", rc);
		return rc;
	}

	rc = radio_identity_load_or_create(&identity, &generated);
	if (rc != 0) {
		LOG_ERR("radio_identity_load_or_create failed: %d", rc);
		return rc;
	}

	LOG_INF("Macropad identity synchronized");
	rc = radio_identity_derive_esb_config(&identity, &addr_config, &rf_channel);
	if (rc != 0) {
		LOG_ERR("radio_identity_derive_esb_config failed: %d", rc);
		return rc;
	}

	radio_identity_log_esb_config(&addr_config, rf_channel);
	rc = radio_esb_init(&addr_config, rf_channel, handle_radio_delivery, handle_radio_config);
	if (rc != 0) {
		LOG_ERR("radio_esb_init failed: %d", rc);
		return rc;
	}

	rc = radio_esb_start();
	if (rc != 0) {
		LOG_ERR("radio_esb_start failed: %d", rc);
		(void)radio_esb_stop();
		return rc;
	}

	return 0;
}

static int start_transport(enum macropad_operating_mode mode)
{
	if (mode == MACROPAD_OPERATING_MODE_BLE) {
		return ble_hid_start();
	}

	return start_dongle_transport();
}

static int stop_transport(enum macropad_operating_mode mode)
{
	if (mode == MACROPAD_OPERATING_MODE_BLE) {
		(void)ble_hid_send_key_state(0U);
		return ble_hid_stop();
	}

	return radio_esb_stop();
}

static int switch_operating_mode(struct app_ui_state *ui_state,
				 enum macropad_operating_mode target_mode,
				 bool display_available, bool *display_sleeping)
{
	enum macropad_operating_mode previous_mode;
	int rc;

	if (ui_state->operating_mode == target_mode) {
		return 0;
	}

	previous_mode = ui_state->operating_mode;
	show_transient_message(display_available, display_sleeping,
		starting_message(target_mode), NULL, 0);

	rc = stop_transport(previous_mode);
	if (rc != 0) {
		LOG_WRN("Failed to stop mode %u cleanly: %d", previous_mode, rc);
	}

	rc = start_transport(target_mode);
	if (rc != 0) {
		LOG_ERR("Failed to start mode %u: %d", target_mode, rc);
		(void)start_transport(previous_mode);
		ui_state->ble_state = ble_hid_get_state();
		show_transient_message(display_available, display_sleeping,
			failed_message(target_mode), "Restored old mode",
			TRANSIENT_MESSAGE_HOLD_MS);
		return rc;
	}

	rc = macropad_config_store_operating_mode(target_mode);
	if (rc != 0) {
		LOG_WRN("Failed to persist mode %u: %d", target_mode, rc);
	}

	ui_state->operating_mode = target_mode;
	ui_state->ble_state = ble_hid_get_state();
	ui_state->connected = false;
	ui_state->dongle_activity = false;
	ui_state->last_delivery_success_ms = INT64_MIN;
	return 0;
}

static int forget_ble_pairing(struct app_ui_state *ui_state,
			      bool display_available, bool *display_sleeping)
{
	int rc;

	if (ui_state->operating_mode != MACROPAD_OPERATING_MODE_BLE) {
		show_transient_message(display_available, display_sleeping,
			"Use BLE mode", "to forget", TRANSIENT_MESSAGE_HOLD_MS);
		return -EAGAIN;
	}

	show_transient_message(display_available, display_sleeping,
		"Forgetting BLE...", NULL, 0);

	rc = ble_hid_forget_pairing();
	ui_state->ble_state = ble_hid_get_state();
	ui_state->connected = ble_hid_is_connected();
	ui_state->dongle_activity = false;
	if (rc != 0) {
		show_transient_message(display_available, display_sleeping,
			"Forget failed", "BLE ERR", TRANSIENT_MESSAGE_HOLD_MS);
		return rc;
	}

	show_transient_message(display_available, display_sleeping,
		"Pairing cleared", "BLE READY", TRANSIENT_MESSAGE_HOLD_MS);
	return 0;
}

static int save_runtime_state(void)
{
	return 0;
}

static void prepare_for_poweroff(bool display_available)
{
	int rc;

	status_buzzer_set(false);
	status_led_set(false);

	rc = key_leds_set_all(0U, 0U, 0U);
	if ((rc != 0) && (rc != -ENOTSUP)) {
		LOG_WRN("Failed to shut down key LEDs: %d", rc);
	}

	if (display_available) {
		rc = status_display_blank();
		if ((rc != 0) && (rc != -ENOSYS)) {
			LOG_WRN("Failed to blank display during poweroff: %d", rc);
		}
	}

	rc = status_power_rails_off();
	if (rc != 0) {
		LOG_WRN("Failed to disable power rails: %d", rc);
	}

	rc = save_runtime_state();
	if (rc != 0) {
		LOG_WRN("Failed to save runtime state: %d", rc);
	}
}

static void enforce_battery_policy(bool usb_power_present,
				   bool display_available,
				   bool *warning_logged)
{
	const bool warning_active = battery_warning_active(usb_power_present);
	const int64_t now_ms = k_uptime_get();

	if (warning_active && !(*warning_logged)) {
		LOG_WRN("Battery low (%u mV filtered)", battery_policy_state.filtered_mv);
	}

	*warning_logged = warning_active;

	if (battery_poweroff_required(usb_power_present, now_ms)) {
		LOG_WRN("Battery critical (%u mV filtered for %u ms), entering system-off",
			battery_policy_state.filtered_mv,
			(unsigned int)BATTERY_POWEROFF_SUSTAIN_MS);
		prepare_for_poweroff(display_available);
		sys_poweroff();
	}
}

static struct status_display_state make_display_state(const struct app_ui_state *ui_state)
{
	return (struct status_display_state){
		.operating_mode = ui_state->operating_mode,
		.ble_state = ui_state->ble_state,
		.connected = ui_state->connected,
		.dongle_activity = ui_state->dongle_activity,
		.usb_power_present = ui_state->usb_power_present,
		.show_battery_warning = ui_state->battery_warning_visible,
		.battery_mv = battery_display_mv(),
		.keys_pressed = macropad_input_state.keys,
	};
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

static int send_encoder_button_click_report(void)
{
	int rc;

	macropad_input_state.encoder_pressed = 1U;
	rc = macropad_send_report();
	if (rc != 0) {
		macropad_input_state.encoder_pressed = 0U;
		return rc;
	}

	macropad_input_state.encoder_pressed = 0U;
	return macropad_send_report();
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
	ARG_UNUSED(kind);

	const struct app_event event = {
		.type = APP_EVENT_TX_RESULT,
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
	bool dongle_activity;
	bool warning_visible;
	bool connected;
	enum ble_hid_state ble_state;

	if (state->usb_power_present != usb_power_present) {
		state->usb_power_present = usb_power_present;
		macropad_input_state.usb_power_present = usb_power_present ? 1U : 0U;
		if (usb_power_present) {
			battery_policy_state.below_poweroff_since_ms = INT64_MIN;
		}
		dirty = true;
	}

	if (state->operating_mode == MACROPAD_OPERATING_MODE_BLE) {
		ble_state = ble_hid_get_state();
		if (state->ble_state != ble_state) {
			state->ble_state = ble_state;
			dirty = true;
		}
		connected = ble_hid_is_connected();
		if (state->connected != connected) {
			state->connected = connected;
			dirty = true;
		}
		if (state->dongle_activity) {
			state->dongle_activity = false;
			dirty = true;
		}
	} else {
		if (state->connected &&
		    (state->last_delivery_success_ms != INT64_MIN) &&
		    ((now_ms - state->last_delivery_success_ms) >= CONNECTION_TIMEOUT_MS)) {
			state->connected = false;
			dirty = true;
		}

		dongle_activity = state->connected &&
			(state->last_delivery_success_ms != INT64_MIN) &&
			((now_ms - state->last_delivery_success_ms) < DONGLE_ACTIVITY_PULSE_MS);
		if (state->dongle_activity != dongle_activity) {
			state->dongle_activity = dongle_activity;
			dirty = true;
		}
	}

	warning_visible = battery_warning_visible(usb_power_present, now_ms);
	if (state->battery_warning_visible != warning_visible) {
		state->battery_warning_visible = warning_visible;
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
		if (!state->dongle_activity) {
			state->dongle_activity = true;
			dirty = true;
		}
	} else if (state->connected) {
		state->connected = false;
		state->dongle_activity = false;
		dirty = true;
	}

	return dirty;
}

/* Raw GPIO smoke test: key pins to GND, LED on P0.15.
 * Key pins: gpio0: 8,6,22,24,2  gpio1: 0
 * Press any key -> LED on; release all -> LED off.
 */
static int __attribute__((unused)) macropad_keys_led_smoke_test(void)
{
	const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	const struct device *g1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	static const uint8_t g0_key_pins[] = {8U, 6U, 22U, 24U, 2U};
	uint32_t log_ctr = 0U;

	if (!device_is_ready(g0) || !device_is_ready(g1)) {
		LOG_ERR("GPIO devices not ready");
		return 0;
	}

	/* LED: P0.15, start off */
	gpio_pin_configure(g0, 15U, GPIO_OUTPUT_INACTIVE);

	/* Blink 3 times to confirm LED is wired and working */
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_raw(g0, 15U, 1);
		k_msleep(300);
		gpio_pin_set_raw(g0, 15U, 0);
		k_msleep(300);
	}

	/* Key inputs: pull-up; button shorts to GND -> raw read = 0 when pressed */
	for (size_t i = 0U; i < ARRAY_SIZE(g0_key_pins); ++i) {
		gpio_pin_configure(g0, g0_key_pins[i], GPIO_INPUT | GPIO_PULL_UP);
	}
	gpio_pin_configure(g1, 0U, GPIO_INPUT | GPIO_PULL_UP);

	LOG_INF("Raw key smoke test running (pins g0:8,6,22,24,2 g1:0)");

	while (1) {
		bool any = false;

		for (size_t i = 0U; i < ARRAY_SIZE(g0_key_pins); ++i) {
			if (gpio_pin_get_raw(g0, g0_key_pins[i]) == 0) {
				any = true;
				break;
			}
		}
		if (!any && (gpio_pin_get_raw(g1, 0U) == 0)) {
			any = true;
		}

		gpio_pin_set_raw(g0, 15U, any ? 1 : 0);

		/* Log raw pin states every second so we can see what hardware reports */
		if (++log_ctr >= 100U) {
			log_ctr = 0U;
			LOG_INF("raw: g0[8]=%d [6]=%d [22]=%d [24]=%d [2]=%d  g1[0]=%d",
				gpio_pin_get_raw(g0, 8U),
				gpio_pin_get_raw(g0, 6U),
				gpio_pin_get_raw(g0, 22U),
				gpio_pin_get_raw(g0, 24U),
				gpio_pin_get_raw(g0, 2U),
				gpio_pin_get_raw(g1, 0U));
		}

		k_msleep(10);
	}
}

int main(void)
{
#if BRINGUP_STAGE > 0
	return bringup_main();
#endif

	//return macropad_keys_led_smoke_test();

	struct app_event event;
	struct app_ui_state ui_state = {
		.operating_mode = MACROPAD_OPERATING_MODE_DONGLE,
		.ble_state = BLE_HID_STATE_INACTIVE,
		.connected = false,
		.dongle_activity = false,
		.usb_power_present = false,
		.battery_warning_visible = false,
		.last_delivery_success_ms = INT64_MIN,
	};
	struct options_menu_state options_menu = {
		.open = false,
		.selected_index = 0U,
		.first_visible_index = 0U,
	};
	int64_t next_heartbeat_ms = 0;
	int64_t last_display_update_ms = INT64_MIN;
	int64_t last_activity_ms = 0;
	int64_t encoder_button_down_ms = INT64_MIN;
	bool display_available = false;
	bool display_sleeping = false;
	bool battery_warning_logged = false;
	bool redraw = true;
	bool encoder_button_down = false;
	bool encoder_long_press_consumed = false;
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
	ui_state.operating_mode = macropad_config_get_operating_mode();

	rc = key_leds_init();
	if (rc != 0) {
		LOG_INF("Macropad LED strip unavailable (rc=%d)", rc);
	}

	rc = battery_init();
	if (rc != 0) {
		LOG_INF("Battery monitor unavailable (rc=%d)", rc);
	}
	ui_state.usb_power_present = status_usb_power_present();
	refresh_battery_state(&macropad_input_state);
	ui_state.battery_warning_visible = battery_warning_visible(ui_state.usb_power_present,
		k_uptime_get());
	enforce_battery_policy(ui_state.usb_power_present, false, &battery_warning_logged);

	LOG_INF("boot: entering main()");
	rc = status_display_init();
	if (rc != 0) {
		LOG_WRN("Display unavailable during boot (rc=%d), continuing without OLED", rc);
		redraw = false;
	} else {
		const struct status_display_state display_state = make_display_state(&ui_state);

		display_available = true;
		k_msleep(1000);
		rc = status_display_render(&display_state);
		if (rc != 0) {
			LOG_WRN("Initial display render failed (rc=%d), continuing without OLED", rc);
			display_available = false;
			redraw = false;
		}
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

	rc = start_transport(ui_state.operating_mode);
	if (rc != 0) {
		LOG_ERR("Failed to start saved mode %u: %d", ui_state.operating_mode, rc);
		return 0;
	}
	ui_state.ble_state = ble_hid_get_state();

	LOG_INF("Macropad sender ready in mode %u", ui_state.operating_mode);
	next_heartbeat_ms = k_uptime_get();
	last_activity_ms = k_uptime_get();

	while (1) {
		int64_t now_ms = k_uptime_get();
		int64_t wait_ms;

		bool prev_usb_power = ui_state.usb_power_present;

		if (update_time_driven_ui(&ui_state, now_ms)) {
			redraw = true;
		}

		if (ui_state.usb_power_present != prev_usb_power) {
			last_activity_ms = now_ms;
			if (display_sleeping && display_available) {
				(void)status_display_unblank();
				display_sleeping = false;
				redraw = true;
			}
		}

		if (encoder_button_down && !encoder_long_press_consumed &&
		    ((now_ms - encoder_button_down_ms) >= ENCODER_LONG_PRESS_MS)) {
			if (options_menu.open) {
				options_menu_close(&options_menu);
			} else {
				options_menu_open(&options_menu);
			}
			encoder_long_press_consumed = true;
			status_buzzer_set(false);
			redraw = true;
		}

		if (now_ms >= next_heartbeat_ms) {
			if (ui_state.operating_mode == MACROPAD_OPERATING_MODE_DONGLE) {
				rc = macropad_send_heartbeat();
				if (rc != 0) {
					LOG_WRN("macropad_send_heartbeat failed: %d", rc);
				}
			}

			enforce_battery_policy(ui_state.usb_power_present, display_available,
				&battery_warning_logged);

			redraw = true;
			next_heartbeat_ms = now_ms + HEARTBEAT_INTERVAL_MS;
		}

		if (display_available && !display_sleeping && redraw && ((last_display_update_ms == INT64_MIN) ||
			      ((now_ms - last_display_update_ms) >= STATUS_DISPLAY_REFRESH_INTERVAL_MS))) {
			if (options_menu.open) {
				const char *menu_labels[OPTIONS_MENU_ITEM_COUNT];

				options_menu_labels(ui_state.operating_mode, menu_labels);
				rc = status_display_render_menu("Options", menu_labels,
					ARRAY_SIZE(menu_labels), options_menu.selected_index,
					options_menu.first_visible_index);
			} else {
				const struct status_display_state display_state =
					make_display_state(&ui_state);

				rc = status_display_render(&display_state);
			}
			if (rc != 0) {
				LOG_WRN("Display update failed (rc=%d), disabling OLED", rc);
				display_available = false;
			}
			last_display_update_ms = now_ms;
			redraw = false;
		}

		if (!display_available) {
			redraw = false;
		}

		if (display_available && !display_sleeping && !ui_state.usb_power_present &&
		    (now_ms - last_activity_ms) >= DISPLAY_SLEEP_TIMEOUT_MS) {
			(void)status_display_blank();
			display_sleeping = true;
		}

		now_ms = k_uptime_get();
		wait_ms = next_heartbeat_ms - now_ms;
		if (display_available && !display_sleeping && redraw) {
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

		if (encoder_button_down && !encoder_long_press_consumed) {
			int64_t long_press_wait_ms =
				(encoder_button_down_ms + ENCODER_LONG_PRESS_MS) - now_ms;

			if (long_press_wait_ms < 0) {
				long_press_wait_ms = 0;
			}
			if (long_press_wait_ms < wait_ms) {
				wait_ms = long_press_wait_ms;
			}
		}

		if (display_available && !display_sleeping && !ui_state.usb_power_present) {
			int64_t sleep_wait_ms = (last_activity_ms + DISPLAY_SLEEP_TIMEOUT_MS) - now_ms;

			if (sleep_wait_ms < 0) {
				sleep_wait_ms = 0;
			}
			if (sleep_wait_ms < wait_ms) {
				wait_ms = sleep_wait_ms;
			}
		}

		if ((ui_state.operating_mode == MACROPAD_OPERATING_MODE_DONGLE) &&
		    ui_state.connected && (ui_state.last_delivery_success_ms != INT64_MIN)) {
			int64_t disconnect_wait_ms =
				(ui_state.last_delivery_success_ms + CONNECTION_TIMEOUT_MS) - now_ms;
			if (disconnect_wait_ms < wait_ms) {
				wait_ms = disconnect_wait_ms;
			}
		}

		if (display_available && !display_sleeping && ui_state.dongle_activity &&
		    (ui_state.last_delivery_success_ms != INT64_MIN)) {
			int64_t pulse_wait_ms =
				(ui_state.last_delivery_success_ms + DONGLE_ACTIVITY_PULSE_MS) - now_ms;
			if (pulse_wait_ms < wait_ms) {
				wait_ms = pulse_wait_ms;
			}
		}

		if (display_available && !display_sleeping &&
		    battery_critical_active(ui_state.usb_power_present)) {
			int64_t blink_wait_ms = BATTERY_CRITICAL_BLINK_MS -
				(now_ms % BATTERY_CRITICAL_BLINK_MS);
			if (blink_wait_ms < wait_ms) {
				wait_ms = blink_wait_ms;
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

		/* Any input event resets the display sleep timer and wakes the display. */
		if (event.type == APP_EVENT_ENCODER_DELTA ||
		    event.type == APP_EVENT_ENCODER_BUTTON ||
		    event.type == APP_EVENT_MACROPAD_KEYS) {
			last_activity_ms = now_ms;
			if (display_sleeping && display_available) {
				(void)status_display_unblank();
				display_sleeping = false;
				redraw = true;
			}
		}

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

			if (options_menu.open) {
				options_menu_move(&options_menu, encoder_delta);
				redraw = true;
				continue;
			}

			status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
			redraw = true;

			if (ui_state.operating_mode == MACROPAD_OPERATING_MODE_DONGLE) {
				rc = send_encoder_delta_reports(encoder_delta);
				if (rc != 0) {
					LOG_ERR("send_encoder_delta_reports failed: %d", rc);
				}
			}
		} else if (event.type == APP_EVENT_ENCODER_BUTTON) {
			if (event.pressed != 0U) {
				encoder_button_down = true;
				encoder_button_down_ms = now_ms;
				encoder_long_press_consumed = false;
				status_buzzer_set(true);
				continue;
			}

			status_buzzer_set(false);
			if (!encoder_button_down) {
				continue;
			}

			encoder_button_down = false;
			encoder_button_down_ms = INT64_MIN;
			if (encoder_long_press_consumed) {
				encoder_long_press_consumed = false;
				continue;
			}

			if (options_menu.open) {
				const struct options_menu_item *selected =
					&options_menu_items[options_menu.selected_index];

				if (selected->action == OPTIONS_MENU_ACTION_SWITCH_MODE) {
					enum macropad_operating_mode target_mode =
						opposite_mode(ui_state.operating_mode);

					options_menu_close(&options_menu);
					rc = switch_operating_mode(&ui_state, target_mode,
						display_available, &display_sleeping);
					if (rc != 0) {
						LOG_ERR("switch_operating_mode failed: %d", rc);
					}
				} else if (selected->action ==
					   OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING) {
					options_menu_close(&options_menu);
					rc = forget_ble_pairing(&ui_state, display_available,
						&display_sleeping);
					if (rc != 0) {
						LOG_ERR("forget_ble_pairing failed: %d", rc);
					}
				} else {
					options_menu_close(&options_menu);
				}
				redraw = true;
				continue;
			}

			if (ui_state.operating_mode == MACROPAD_OPERATING_MODE_DONGLE) {
				rc = send_encoder_button_click_report();
				if (rc != 0) {
					LOG_ERR("send_encoder_button_click_report failed: %d", rc);
				}
			}
			redraw = true;
		} else if (event.type == APP_EVENT_MACROPAD_KEYS) {
			macropad_input_state.keys = event.keys;
			rc = key_leds_update(macropad_input_state.keys);
			if (rc != 0) {
				LOG_WRN("Failed to update macropad LEDs: %d", rc);
			}
			if (ui_state.operating_mode == MACROPAD_OPERATING_MODE_BLE) {
				rc = ble_hid_send_key_state(macropad_input_state.keys);
				if ((rc != 0) && (rc != -ENOTCONN) && (rc != -EAGAIN)) {
					LOG_ERR("ble_hid_send_key_state failed: %d", rc);
				}
			} else {
				rc = macropad_send_report();
				if (rc != 0) {
					LOG_ERR("macropad_send_report failed: %d", rc);
				}
			}

			if (event.pressed == 0U) {
				continue;
			}

			status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
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

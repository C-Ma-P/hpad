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
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>

#if defined(CONFIG_MPSL_DYNAMIC_INTERRUPTS)
#include <mpsl.h>
#include <mpsl/mpsl_lib.h>
#endif

#include "battery.h"
#include "bringup.h"
#include "desktop_ble_transport.h"
#include "encoder.h"
#include "key_leds.h"
#include "kindle_ble_hid.h"
#include "macropad_config.h"
#include "macropad_keys.h"
#include "macropad_mode.h"
#include "radio_esb.h"
#include "status_display.h"
#include "status_io.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#ifndef HPAD_FIRMWARE_VERSION
#define HPAD_FIRMWARE_VERSION "unknown"
#endif

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
#define OPTIONS_MENU_MAX_ITEM_COUNT 9U
#define MODE_MENU_ITEM_COUNT 3U
#define MODE_MENU_LABEL_MAX_LEN 22U
#define POWER_CONFIRM_ITEM_COUNT 2U
#define DFU_CONFIRM_ITEM_COUNT 2U
#define FEEDBACK_ITEM_COUNT 4U
#define DEVICE_INFO_LINE_COUNT 5U
#define DEVICE_INFO_LINE_MAX_LEN 64U
#define TRANSIENT_MESSAGE_HOLD_MS 700
#define FEEDBACK_PREVIEW_MS 80
#define BRIGHTNESS_STEP 16U
#define POWEROFF_RELEASE_WAIT_MS 10
#define WAKE_HOLD_POLL_MS 10
#define ADAFRUIT_DFU_MAGIC_OTA_RESET 0xA8U

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
	enum macropad_ble_link_state ble_state;
	uint8_t ble_retry_attempt;
	uint8_t ble_retry_seconds_remaining;
	bool connected;
	bool dongle_activity;
	bool usb_power_present;
	bool battery_warning_visible;
	bool keys_locked;
	int64_t last_delivery_success_ms;
};

enum options_menu_action {
	OPTIONS_MENU_ACTION_SWITCH_MODE = 0,
	OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING = 1,
	OPTIONS_MENU_ACTION_FEEDBACK = 2,
	OPTIONS_MENU_ACTION_DEVICE_INFO = 3,
	OPTIONS_MENU_ACTION_BRIGHTNESS = 4,
	OPTIONS_MENU_ACTION_WIRELESS_FLASH = 5,
	OPTIONS_MENU_ACTION_POWER_OFF = 6,
	OPTIONS_MENU_ACTION_LOCK_KEYS = 7,
	OPTIONS_MENU_ACTION_CLOSE = 8,
};

struct options_menu_item {
	enum options_menu_action action;
};

struct options_menu_state {
	bool open;
	size_t selected_index;
	size_t first_visible_index;
};

struct device_info_state {
	bool open;
	size_t first_visible_index;
};

struct modal_menu_state {
	bool open;
	size_t selected_index;
	size_t first_visible_index;
};

struct brightness_menu_state {
	bool open;
	uint8_t original_brightness;
	uint8_t edit_brightness;
};

struct mode_menu_labels {
	char storage[MODE_MENU_ITEM_COUNT][MODE_MENU_LABEL_MAX_LEN];
	const char *labels[MODE_MENU_ITEM_COUNT];
};

struct device_info_lines {
	char storage[DEVICE_INFO_LINE_COUNT][DEVICE_INFO_LINE_MAX_LEN];
	const char *lines[DEVICE_INFO_LINE_COUNT];
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

static const struct options_menu_item options_menu_dongle_items[] = {
	{ .action = OPTIONS_MENU_ACTION_SWITCH_MODE },
	{ .action = OPTIONS_MENU_ACTION_DEVICE_INFO },
	{ .action = OPTIONS_MENU_ACTION_BRIGHTNESS },
	{ .action = OPTIONS_MENU_ACTION_WIRELESS_FLASH },
	{ .action = OPTIONS_MENU_ACTION_POWER_OFF },
	{ .action = OPTIONS_MENU_ACTION_LOCK_KEYS },
	{ .action = OPTIONS_MENU_ACTION_CLOSE },
};

static const struct options_menu_item options_menu_ble_items[] = {
	{ .action = OPTIONS_MENU_ACTION_SWITCH_MODE },
	{ .action = OPTIONS_MENU_ACTION_FEEDBACK },
	{ .action = OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING },
	{ .action = OPTIONS_MENU_ACTION_DEVICE_INFO },
	{ .action = OPTIONS_MENU_ACTION_BRIGHTNESS },
	{ .action = OPTIONS_MENU_ACTION_WIRELESS_FLASH },
	{ .action = OPTIONS_MENU_ACTION_POWER_OFF },
	{ .action = OPTIONS_MENU_ACTION_LOCK_KEYS },
	{ .action = OPTIONS_MENU_ACTION_CLOSE },
};

static const struct options_menu_item options_menu_desktop_ble_items[] = {
	{ .action = OPTIONS_MENU_ACTION_SWITCH_MODE },
	{ .action = OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING },
	{ .action = OPTIONS_MENU_ACTION_DEVICE_INFO },
	{ .action = OPTIONS_MENU_ACTION_BRIGHTNESS },
	{ .action = OPTIONS_MENU_ACTION_WIRELESS_FLASH },
	{ .action = OPTIONS_MENU_ACTION_POWER_OFF },
	{ .action = OPTIONS_MENU_ACTION_LOCK_KEYS },
	{ .action = OPTIONS_MENU_ACTION_CLOSE },
};

static const char *const power_confirm_labels[POWER_CONFIRM_ITEM_COUNT] = {
	"Cancel",
	"Confirm",
};

static const char *const dfu_confirm_labels[DFU_CONFIRM_ITEM_COUNT] = {
	"Cancel",
	"Confirm",
};

static const char *const feedback_labels[FEEDBACK_ITEM_COUNT] = {
	"Buzz",
	"Low",
	"Med",
	"High",
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

static void update_battery_filter(uint16_t battery_mv, bool reset_filter)
{
	if (battery_mv == 0U) {
		return;
	}

	if (reset_filter || (battery_policy_state.filtered_mv == 0U)) {
		battery_policy_state.filtered_mv = battery_mv;
		return;
	}

	battery_policy_state.filtered_mv = (uint16_t)
		(((uint32_t)battery_policy_state.filtered_mv * 3U + battery_mv + 2U) / 4U);
}

static void update_battery_policy(uint16_t battery_mv, bool usb_power_present, int64_t now_ms,
				  bool reset_filter)
{
	update_battery_filter(battery_mv, reset_filter);

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

static void update_battery_state(struct macropad_input_state *state, bool usb_power_present,
				 bool reset_filter)
{
	const int64_t now_ms = k_uptime_get();

	state->battery_mv = battery_sample_mv();
	state->usb_power_present = usb_power_present ? 1U : 0U;
	update_battery_policy(state->battery_mv, usb_power_present, now_ms, reset_filter);
}

static void refresh_battery_state(struct macropad_input_state *state, bool reset_filter)
{
	update_battery_state(state, status_usb_power_present(), reset_filter);
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

static uint8_t battery_percent_from_mv(uint16_t mv)
{
	if (mv >= 4200U) {
		return 100U;
	}
	if (mv <= 3000U) {
		return 0U;
	}

	return (uint8_t)((uint32_t)(mv - 3000U) * 100U / 1200U);
}

static enum macropad_ble_link_state kindle_ble_link_state(void)
{
	switch (kindle_ble_hid_get_state()) {
	case KINDLE_BLE_HID_STATE_INACTIVE:
		return MACROPAD_BLE_LINK_STATE_INACTIVE;
	case KINDLE_BLE_HID_STATE_STARTING:
		return MACROPAD_BLE_LINK_STATE_STARTING;
	case KINDLE_BLE_HID_STATE_ADVERTISING:
		return MACROPAD_BLE_LINK_STATE_ADVERTISING;
	case KINDLE_BLE_HID_STATE_CONNECTED:
		return MACROPAD_BLE_LINK_STATE_CONNECTED;
	case KINDLE_BLE_HID_STATE_SECURITY_FAILED:
		return MACROPAD_BLE_LINK_STATE_SECURITY_FAILED;
	case KINDLE_BLE_HID_STATE_STOPPING:
		return MACROPAD_BLE_LINK_STATE_STOPPING;
	case KINDLE_BLE_HID_STATE_ERROR:
	default:
		return MACROPAD_BLE_LINK_STATE_ERROR;
	}
}

static enum macropad_ble_link_state desktop_ble_link_state(void)
{
	switch (desktop_ble_transport_get_state()) {
	case DESKTOP_BLE_TRANSPORT_STATE_INACTIVE:
		return MACROPAD_BLE_LINK_STATE_INACTIVE;
	case DESKTOP_BLE_TRANSPORT_STATE_STARTING:
		return MACROPAD_BLE_LINK_STATE_STARTING;
	case DESKTOP_BLE_TRANSPORT_STATE_ADVERTISING:
		return MACROPAD_BLE_LINK_STATE_ADVERTISING;
	case DESKTOP_BLE_TRANSPORT_STATE_CONNECTED:
		return MACROPAD_BLE_LINK_STATE_CONNECTED;
	case DESKTOP_BLE_TRANSPORT_STATE_STOPPING:
		return MACROPAD_BLE_LINK_STATE_STOPPING;
	case DESKTOP_BLE_TRANSPORT_STATE_ERROR:
	default:
		return MACROPAD_BLE_LINK_STATE_ERROR;
	}
}

static enum macropad_ble_link_state ble_link_state_for_mode(enum macropad_operating_mode mode)
{
	if (mode == MACROPAD_MODE_KINDLE_BLE) {
		return kindle_ble_link_state();
	}
	if (mode == MACROPAD_MODE_DESKTOP_BLE) {
		return desktop_ble_link_state();
	}

	return MACROPAD_BLE_LINK_STATE_INACTIVE;
}

static bool ble_connected_for_mode(enum macropad_operating_mode mode)
{
	if (mode == MACROPAD_MODE_KINDLE_BLE) {
		return kindle_ble_hid_is_connected();
	}
	if (mode == MACROPAD_MODE_DESKTOP_BLE) {
		return desktop_ble_transport_is_connected();
	}

	return false;
}

static bool ble_retry_status_for_mode(enum macropad_operating_mode mode,
				      uint8_t *attempt,
				      uint8_t *seconds_remaining)
{
	*attempt = 0U;
	*seconds_remaining = 0U;

	if (mode == MACROPAD_MODE_KINDLE_BLE) {
		return kindle_ble_hid_get_retry_status(attempt, seconds_remaining);
	}
	if (mode == MACROPAD_MODE_DESKTOP_BLE) {
		return desktop_ble_transport_get_retry_status(attempt, seconds_remaining);
	}

	return false;
}

static void refresh_ble_retry_status(struct app_ui_state *state)
{
	(void)ble_retry_status_for_mode(state->operating_mode,
		&state->ble_retry_attempt, &state->ble_retry_seconds_remaining);
}

static const char *ble_info_link_label(enum macropad_ble_link_state state)
{
	switch (state) {
	case MACROPAD_BLE_LINK_STATE_INACTIVE:
		return "Off";
	case MACROPAD_BLE_LINK_STATE_STARTING:
		return "Starting";
	case MACROPAD_BLE_LINK_STATE_ADVERTISING:
		return "Ready";
	case MACROPAD_BLE_LINK_STATE_CONNECTED:
		return "Connected";
	case MACROPAD_BLE_LINK_STATE_SECURITY_FAILED:
		return "Pair Error";
	case MACROPAD_BLE_LINK_STATE_STOPPING:
		return "Stopping";
	case MACROPAD_BLE_LINK_STATE_ERROR:
	default:
		return "Error";
	}
}

static const char *mode_info_label(enum macropad_operating_mode mode)
{
	return macropad_mode_display_name(mode);
}

static void build_device_info_lines(const struct app_ui_state *ui_state,
				    struct device_info_lines *info)
{
	const uint16_t battery_mv = battery_display_mv();
	const uint8_t battery_pct = battery_percent_from_mv(battery_mv);
	const char *link_label;

	for (size_t index = 0U; index < DEVICE_INFO_LINE_COUNT; ++index) {
		info->lines[index] = info->storage[index];
	}

	if (macropad_mode_is_ble(ui_state->operating_mode)) {
		link_label = ble_info_link_label(ble_link_state_for_mode(ui_state->operating_mode));
	} else {
		link_label = ui_state->connected ? "Connected" : "Waiting";
	}

	(void)snprintf(info->storage[0], sizeof(info->storage[0]), "FW: %s",
		HPAD_FIRMWARE_VERSION);
	(void)snprintf(info->storage[1], sizeof(info->storage[1]), "Mode: %s",
		mode_info_label(ui_state->operating_mode));
	(void)snprintf(info->storage[2], sizeof(info->storage[2]), "Link: %s",
		link_label);
	(void)snprintf(info->storage[3], sizeof(info->storage[3]), "Batt: %u.%02uV %u%%",
		battery_mv / 1000U, (battery_mv % 1000U) / 10U, battery_pct);
	(void)snprintf(info->storage[4], sizeof(info->storage[4]), "USB: %s",
		ui_state->usb_power_present ? "Yes" : "No");
}

static enum macropad_operating_mode mode_menu_target(size_t index)
{
	return macropad_mode_at(index);
}

static const char *mode_menu_label(enum macropad_operating_mode current_mode)
{
	ARG_UNUSED(current_mode);

	return "Mode";
}

static size_t mode_menu_index(enum macropad_operating_mode current_mode)
{
	for (size_t index = 0U; index < macropad_mode_count(); ++index) {
		if (macropad_mode_at(index) == current_mode) {
			return index;
		}
	}

	return 0U;
}

static void build_mode_menu_labels(enum macropad_operating_mode current_mode,
				   struct mode_menu_labels *menu_labels)
{
	for (size_t index = 0U; index < MODE_MENU_ITEM_COUNT; ++index) {
		enum macropad_operating_mode mode = mode_menu_target(index);
		const char *prefix = (mode == current_mode) ? "* " : "  ";

		menu_labels->labels[index] = menu_labels->storage[index];
		(void)snprintf(menu_labels->storage[index], sizeof(menu_labels->storage[index]),
			"%s%s", prefix, macropad_mode_display_name(mode));
	}
}

static const struct options_menu_item *
options_menu_items_for_mode(enum macropad_operating_mode current_mode,
			    size_t *item_count)
{
	if (current_mode == MACROPAD_MODE_KINDLE_BLE) {
		*item_count = ARRAY_SIZE(options_menu_ble_items);
		return options_menu_ble_items;
	}
	if (current_mode == MACROPAD_MODE_DESKTOP_BLE) {
		*item_count = ARRAY_SIZE(options_menu_desktop_ble_items);
		return options_menu_desktop_ble_items;
	}

	*item_count = ARRAY_SIZE(options_menu_dongle_items);
	return options_menu_dongle_items;
}

static const char *options_menu_action_label(enum options_menu_action action,
					     enum macropad_operating_mode current_mode,
					     bool keys_locked)
{
	switch (action) {
	case OPTIONS_MENU_ACTION_SWITCH_MODE:
		return mode_menu_label(current_mode);
	case OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING:
		return "Forget Pairing";
	case OPTIONS_MENU_ACTION_FEEDBACK:
		return "Feedback";
	case OPTIONS_MENU_ACTION_DEVICE_INFO:
		return "Device Info";
	case OPTIONS_MENU_ACTION_BRIGHTNESS:
		return "Brightness";
	case OPTIONS_MENU_ACTION_WIRELESS_FLASH:
		return "Wireless Flash";
	case OPTIONS_MENU_ACTION_POWER_OFF:
		return "Power Off";
	case OPTIONS_MENU_ACTION_LOCK_KEYS:
		return keys_locked ? "Unlock Keys" : "Lock Keys";
	case OPTIONS_MENU_ACTION_CLOSE:
	default:
		return "Close";
	}
}

static size_t options_menu_labels(enum macropad_operating_mode current_mode,
				  bool keys_locked,
				  const char *labels[OPTIONS_MENU_MAX_ITEM_COUNT])
{
	const struct options_menu_item *items;
	size_t item_count;

	items = options_menu_items_for_mode(current_mode, &item_count);
	for (size_t index = 0U; index < item_count; ++index) {
		labels[index] = options_menu_action_label(items[index].action,
			current_mode, keys_locked);
	}

	return item_count;
}

static void options_menu_clamp(struct options_menu_state *menu,
			       enum macropad_operating_mode current_mode)
{
	size_t item_count;

	if (menu == NULL) {
		return;
	}

	(void)options_menu_items_for_mode(current_mode, &item_count);
	if (item_count == 0U) {
		menu->selected_index = 0U;
		menu->first_visible_index = 0U;
		return;
	}

	if (menu->selected_index >= item_count) {
		menu->selected_index = item_count - 1U;
	}
	status_display_menu_clamp_viewport(item_count, menu->selected_index,
		&menu->first_visible_index);
}

static void options_menu_open(struct options_menu_state *menu,
			      enum macropad_operating_mode current_mode)
{
	menu->open = true;
	menu->selected_index = 0U;
	menu->first_visible_index = 0U;
	options_menu_clamp(menu, current_mode);
}

static void options_menu_close(struct options_menu_state *menu)
{
	menu->open = false;
	menu->selected_index = 0U;
	menu->first_visible_index = 0U;
}

static void options_menu_move(struct options_menu_state *menu,
			      enum macropad_operating_mode current_mode,
			      int32_t delta)
{
	size_t item_count;

	if (!menu->open || (delta == 0)) {
		return;
	}

	(void)options_menu_items_for_mode(current_mode, &item_count);
	if (item_count == 0U) {
		return;
	}
	options_menu_clamp(menu, current_mode);

	while (delta > 0) {
		if (menu->selected_index < (item_count - 1U)) {
			menu->selected_index++;
		}
		status_display_menu_clamp_viewport(item_count,
			menu->selected_index, &menu->first_visible_index);
		delta--;
	}
	while (delta < 0) {
		if (menu->selected_index > 0U) {
			menu->selected_index--;
		}
		status_display_menu_clamp_viewport(item_count,
			menu->selected_index, &menu->first_visible_index);
		delta++;
	}
}

static void device_info_open(struct device_info_state *info)
{
	info->open = true;
	info->first_visible_index = 0U;
}

static void device_info_close(struct device_info_state *info)
{
	info->open = false;
	info->first_visible_index = 0U;
}

static void device_info_move(struct device_info_state *info, int32_t delta)
{
	size_t visible_rows;
	size_t max_first;

	if (!info->open || (delta == 0)) {
		return;
	}

	visible_rows = status_display_menu_visible_rows();
	if (visible_rows >= DEVICE_INFO_LINE_COUNT) {
		info->first_visible_index = 0U;
		return;
	}

	max_first = DEVICE_INFO_LINE_COUNT - visible_rows;
	while ((delta > 0) && (info->first_visible_index < max_first)) {
		info->first_visible_index++;
		delta--;
	}
	while ((delta < 0) && (info->first_visible_index > 0U)) {
		info->first_visible_index--;
		delta++;
	}
}

static void brightness_menu_open(struct brightness_menu_state *menu, uint8_t brightness)
{
	menu->open = true;
	menu->original_brightness = brightness;
	menu->edit_brightness = brightness;
}

static void brightness_menu_close(struct brightness_menu_state *menu)
{
	menu->open = false;
	menu->original_brightness = 0U;
	menu->edit_brightness = 0U;
}

static void brightness_menu_move(struct brightness_menu_state *menu, int32_t delta)
{
	int32_t brightness;

	if (!menu->open || (delta == 0)) {
		return;
	}

	brightness = (int32_t)menu->edit_brightness + (delta * (int32_t)BRIGHTNESS_STEP);
	if (brightness < DISPLAY_BRIGHTNESS_MIN) {
		brightness = DISPLAY_BRIGHTNESS_MIN;
	} else if (brightness > DISPLAY_BRIGHTNESS_MAX) {
		brightness = DISPLAY_BRIGHTNESS_MAX;
	}

	menu->edit_brightness = (uint8_t)brightness;
}

static void modal_menu_open(struct modal_menu_state *menu, size_t selected_index)
{
	menu->open = true;
	menu->selected_index = selected_index;
	menu->first_visible_index = 0U;
}

static void modal_menu_close(struct modal_menu_state *menu)
{
	menu->open = false;
	menu->selected_index = 0U;
	menu->first_visible_index = 0U;
}

static void modal_menu_move(struct modal_menu_state *menu, size_t item_count, int32_t delta)
{
	if (!menu->open || (delta == 0) || (item_count == 0U)) {
		return;
	}

	if (menu->selected_index >= item_count) {
		menu->selected_index = item_count - 1U;
	}

	while (delta > 0) {
		if (menu->selected_index < (item_count - 1U)) {
			menu->selected_index++;
		}
		delta--;
	}
	while (delta < 0) {
		if (menu->selected_index > 0U) {
			menu->selected_index--;
		}
		delta++;
	}

	status_display_menu_clamp_viewport(item_count, menu->selected_index,
		&menu->first_visible_index);
}

static enum macropad_ble_feedback feedback_from_index(size_t index)
{
	switch (index) {
	case 0U:
		return MACROPAD_BLE_FEEDBACK_BUZZ;
	case 1U:
		return MACROPAD_BLE_FEEDBACK_LED_LOW;
	case 3U:
		return MACROPAD_BLE_FEEDBACK_LED_HIGH;
	case 2U:
	default:
		return MACROPAD_BLE_FEEDBACK_LED_MED;
	}
}

static size_t feedback_index(enum macropad_ble_feedback feedback)
{
	switch (feedback) {
	case MACROPAD_BLE_FEEDBACK_BUZZ:
		return 0U;
	case MACROPAD_BLE_FEEDBACK_LED_LOW:
		return 1U;
	case MACROPAD_BLE_FEEDBACK_LED_HIGH:
		return 3U;
	case MACROPAD_BLE_FEEDBACK_LED_MED:
	default:
		return 2U;
	}
}

static bool local_overlay_open(const struct options_menu_state *options_menu,
			       const struct modal_menu_state *mode_menu,
			       const struct device_info_state *device_info,
			       const struct brightness_menu_state *brightness_menu,
			       const struct modal_menu_state *feedback_menu,
			       const struct modal_menu_state *power_confirm,
			       const struct modal_menu_state *dfu_confirm)
{
	return options_menu->open || mode_menu->open || device_info->open ||
		brightness_menu->open || feedback_menu->open || power_confirm->open ||
		dfu_confirm->open;
}

static bool ble_feedback_uses_buzzer(enum macropad_ble_feedback feedback)
{
	return feedback == MACROPAD_BLE_FEEDBACK_BUZZ;
}

static int apply_key_feedback(enum macropad_operating_mode mode,
			      enum macropad_ble_feedback ble_feedback,
			      uint8_t keys)
{
	if (macropad_mode_endpoint(mode) == MACROPAD_ENDPOINT_KINDLE) {
		return key_leds_update_ble_feedback(keys, ble_feedback);
	}

	return key_leds_update(keys);
}

static void preview_ble_feedback(enum macropad_ble_feedback feedback)
{
	int rc;

	if (feedback == MACROPAD_BLE_FEEDBACK_BUZZ) {
		rc = key_leds_set_all(0U, 0U, 0U);
		if ((rc != 0) && (rc != -ENOTSUP)) {
			LOG_WRN("Failed to clear LEDs before buzz preview: %d", rc);
		}
		status_buzzer_pulse(FEEDBACK_PREVIEW_MS);
		return;
	}

	status_buzzer_set(false);
	rc = key_leds_preview_ble_feedback(feedback);
	if ((rc != 0) && (rc != -ENOTSUP)) {
		LOG_WRN("Failed to preview BLE feedback LEDs: %d", rc);
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
	return macropad_mode_is_ble(mode) ? "Starting BLE..." : "Starting Dongle...";
}

static const char *failed_message(enum macropad_operating_mode mode)
{
	return macropad_mode_is_ble(mode) ? "BLE failed" : "Dongle failed";
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
	if (macropad_mode_transport(mode) == MACROPAD_TRANSPORT_ESB) {
		return start_dongle_transport();
	}
	if (macropad_mode_endpoint(mode) == MACROPAD_ENDPOINT_KINDLE) {
		return kindle_ble_hid_start();
	}

	desktop_ble_transport_set_config_handler(handle_radio_config);
	return desktop_ble_transport_start();
}

static int stop_transport(enum macropad_operating_mode mode)
{
	if (macropad_mode_transport(mode) == MACROPAD_TRANSPORT_ESB) {
		return radio_esb_stop();
	}
	if (macropad_mode_endpoint(mode) == MACROPAD_ENDPOINT_KINDLE) {
		(void)kindle_ble_hid_send_key_state(0U);
		return kindle_ble_hid_stop();
	}

	return desktop_ble_transport_stop();
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
		ui_state->ble_state = ble_link_state_for_mode(previous_mode);
		refresh_ble_retry_status(ui_state);
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
	ui_state->ble_state = ble_link_state_for_mode(target_mode);
	refresh_ble_retry_status(ui_state);
	ui_state->connected = false;
	ui_state->dongle_activity = false;
	ui_state->last_delivery_success_ms = INT64_MIN;
	return 0;
}

static int forget_ble_pairing(struct app_ui_state *ui_state,
			      bool display_available, bool *display_sleeping)
{
	int rc;

	if (!macropad_mode_has_pairing(ui_state->operating_mode)) {
		show_transient_message(display_available, display_sleeping,
			"No pairing", "for this mode", TRANSIENT_MESSAGE_HOLD_MS);
		return -EAGAIN;
	}

	show_transient_message(display_available, display_sleeping,
		"Forgetting BLE...", NULL, 0);

	if (ui_state->operating_mode == MACROPAD_MODE_KINDLE_BLE) {
		rc = kindle_ble_hid_forget_pairing();
	} else {
		rc = desktop_ble_transport_forget_pairing();
	}
	ui_state->ble_state = ble_link_state_for_mode(ui_state->operating_mode);
	ui_state->connected = ble_connected_for_mode(ui_state->operating_mode);
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

static void log_boot_reset_state(void)
{
	uint32_t reset_reasons = nrf_power_resetreas_get(NRF_POWER);

	LOG_INF("boot reset state: RESETREAS=0x%08x GPREGRET=0x%02x",
		reset_reasons, NRF_POWER->GPREGRET);
}

static bool booted_from_system_off(void)
{
	uint32_t reset_reasons = nrf_power_resetreas_get(NRF_POWER);
	bool from_system_off = (reset_reasons & NRF_POWER_RESETREAS_OFF_MASK) != 0U;

	if (reset_reasons != 0U) {
		nrf_power_resetreas_clear(NRF_POWER, reset_reasons);
	}

	return from_system_off;
}

static void enter_system_off(void)
{
	int rc = encoder_button_configure_system_off_wake();

	if (rc != 0) {
		LOG_ERR("Failed to configure encoder wake source: %d", rc);
	}

	sys_poweroff();
}

static void wait_for_encoder_release_before_poweroff(void)
{
	while (true) {
		bool pressed = false;
		int rc = encoder_button_is_pressed(&pressed);

		if (rc != 0) {
			LOG_WRN("Failed to read encoder button before poweroff: %d", rc);
			return;
		}
		if (!pressed) {
			return;
		}

		k_msleep(POWEROFF_RELEASE_WAIT_MS);
	}
}

static void enforce_system_off_wake_hold(void)
{
	bool pressed = false;
	int rc;
	int64_t hold_start_ms;

	if (!booted_from_system_off()) {
		return;
	}

	rc = encoder_button_is_pressed(&pressed);
	if ((rc != 0) || !pressed) {
		LOG_INF("System OFF wake without held encoder; returning to System OFF");
		enter_system_off();
	}

	hold_start_ms = k_uptime_get();
	while ((k_uptime_get() - hold_start_ms) < ENCODER_LONG_PRESS_MS) {
		rc = encoder_button_is_pressed(&pressed);
		if ((rc != 0) || !pressed) {
			LOG_INF("System OFF wake hold was released early; returning to System OFF");
			enter_system_off();
		}
		k_msleep(WAKE_HOLD_POLL_MS);
	}

	LOG_INF("System OFF wake hold accepted");
	wait_for_encoder_release_before_poweroff();
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

static void perform_user_poweroff(enum macropad_operating_mode mode,
				  bool display_available,
				  bool *display_sleeping)
{
	static const char *const shutdown_lines[] = {
		"Hold encoder",
		"to wake",
	};
	int rc;

	if (display_available) {
		if ((display_sleeping != NULL) && *display_sleeping) {
			(void)status_display_unblank();
			*display_sleeping = false;
		}

		rc = status_display_render_info("Powering Off", shutdown_lines,
			ARRAY_SIZE(shutdown_lines), 0U);
		if (rc != 0) {
			LOG_WRN("Shutdown display message failed: %d", rc);
		}
		k_msleep(250);
	}
	wait_for_encoder_release_before_poweroff();

	rc = stop_transport(mode);
	if (rc != 0) {
		LOG_WRN("Failed to stop active transport before poweroff: %d", rc);
	}

	prepare_for_poweroff(display_available);
	enter_system_off();
}

static void perform_wireless_flash(enum macropad_operating_mode mode,
				   bool display_available,
				   bool *display_sleeping)
{
	int rc;

	show_transient_message(display_available, display_sleeping,
		"Starting DFU", "BLE update mode", 0);

	rc = stop_transport(mode);
	if (rc != 0) {
		LOG_WRN("Failed to stop active transport before DFU handoff: %d", rc);
	}

	status_buzzer_set(false);
	status_led_set(false);

	rc = key_leds_set_all(0U, 0U, 0U);
	if ((rc != 0) && (rc != -ENOTSUP)) {
		LOG_WRN("Failed to clear key LEDs before DFU handoff: %d", rc);
	}

	NRF_POWER->GPREGRET = ADAFRUIT_DFU_MAGIC_OTA_RESET;
	LOG_INF("DFU handoff: GPREGRET=0x%02x, rebooting into Adafruit OTA bootloader",
		NRF_POWER->GPREGRET);
	LOG_PANIC();
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
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
		enter_system_off();
	}
}

static struct status_display_state make_display_state(const struct app_ui_state *ui_state)
{
	return (struct status_display_state){
		.operating_mode = ui_state->operating_mode,
		.ble_state = ui_state->ble_state,
		.ble_retry_attempt = ui_state->ble_retry_attempt,
		.ble_retry_seconds_remaining = ui_state->ble_retry_seconds_remaining,
		.connected = ui_state->connected,
		.dongle_activity = ui_state->dongle_activity,
		.usb_power_present = ui_state->usb_power_present,
		.show_battery_warning = ui_state->battery_warning_visible,
		.keys_locked = ui_state->keys_locked,
		.battery_mv = battery_display_mv(),
		.keys_pressed = macropad_input_state.keys,
	};
}

static int macropad_send_report(enum macropad_operating_mode mode)
{
	macropad_report_t report;

	refresh_battery_state(&macropad_input_state, false);
	report = (macropad_report_t){
		.keys = macropad_input_state.keys,
		.encoder_delta = macropad_input_state.encoder_delta,
		.encoder_pressed = macropad_input_state.encoder_pressed,
		.battery_mv = macropad_input_state.battery_mv,
		.usb_power_present = macropad_input_state.usb_power_present,
	};

	if (mode == MACROPAD_MODE_DESKTOP_BLE) {
		return desktop_ble_transport_send_report(&report);
	}
	if (mode == MACROPAD_MODE_KINDLE_BLE) {
		return kindle_ble_hid_send_key_state(report.keys);
	}
	if (mode != MACROPAD_MODE_DESKTOP_DONGLE) {
		return -EINVAL;
	}

	return radio_esb_send_macropad_report(&report);
}

static int macropad_send_heartbeat(enum macropad_operating_mode mode)
{
	macropad_report_t report = {
		.keys = 0U,
		.encoder_delta = 0,
		.encoder_pressed = 0U,
	};

	refresh_battery_state(&macropad_input_state, false);
	report.battery_mv = macropad_input_state.battery_mv;
	report.usb_power_present = macropad_input_state.usb_power_present;

	if (mode == MACROPAD_MODE_DESKTOP_BLE) {
		return 0;
	}

	return radio_esb_send_heartbeat(&report);
}

static int send_neutral_input_state(enum macropad_operating_mode mode)
{
	uint8_t saved_keys = macropad_input_state.keys;
	uint8_t saved_encoder_pressed = macropad_input_state.encoder_pressed;
	int8_t saved_encoder_delta = macropad_input_state.encoder_delta;
	int rc;

	if (macropad_mode_endpoint(mode) == MACROPAD_ENDPOINT_KINDLE) {
		return kindle_ble_hid_send_key_state(0U);
	}

	macropad_input_state.keys = 0U;
	macropad_input_state.encoder_delta = 0;
	macropad_input_state.encoder_pressed = 0U;
	rc = macropad_send_report(mode);
	macropad_input_state.keys = saved_keys;
	macropad_input_state.encoder_delta = saved_encoder_delta;
	macropad_input_state.encoder_pressed = saved_encoder_pressed;
	return rc;
}

static int send_encoder_delta_reports(enum macropad_operating_mode mode, int32_t total_delta)
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
		rc = macropad_send_report(mode);
		if (rc != 0) {
			break;
		}

		total_delta -= step_delta;
	}

	macropad_input_state.encoder_delta = 0;
	return rc;
}

static int send_encoder_button_click_report(enum macropad_operating_mode mode)
{
	int rc;

	macropad_input_state.encoder_pressed = 1U;
	rc = macropad_send_report(mode);
	if (rc != 0) {
		macropad_input_state.encoder_pressed = 0U;
		return rc;
	}

	macropad_input_state.encoder_pressed = 0U;
	return macropad_send_report(mode);
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
	uint8_t ble_retry_attempt;
	uint8_t ble_retry_seconds_remaining;
	enum macropad_ble_link_state ble_state;

	if (state->usb_power_present != usb_power_present) {
		state->usb_power_present = usb_power_present;
		update_battery_state(&macropad_input_state, usb_power_present, true);
		if (usb_power_present) {
			battery_policy_state.below_poweroff_since_ms = INT64_MIN;
		}
		dirty = true;
	}

	if (macropad_mode_is_ble(state->operating_mode)) {
		ble_state = ble_link_state_for_mode(state->operating_mode);
		if (state->ble_state != ble_state) {
			state->ble_state = ble_state;
			dirty = true;
		}
		connected = ble_connected_for_mode(state->operating_mode);
		if (state->connected != connected) {
			state->connected = connected;
			dirty = true;
		}
		(void)ble_retry_status_for_mode(state->operating_mode,
			&ble_retry_attempt, &ble_retry_seconds_remaining);
		if ((state->ble_retry_attempt != ble_retry_attempt) ||
		    (state->ble_retry_seconds_remaining != ble_retry_seconds_remaining)) {
			state->ble_retry_attempt = ble_retry_attempt;
			state->ble_retry_seconds_remaining = ble_retry_seconds_remaining;
			dirty = true;
		}
		if (state->dongle_activity) {
			state->dongle_activity = false;
			dirty = true;
		}
	} else {
		if ((state->ble_retry_attempt != 0U) ||
		    (state->ble_retry_seconds_remaining != 0U)) {
			state->ble_retry_attempt = 0U;
			state->ble_retry_seconds_remaining = 0U;
			dirty = true;
		}
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

	log_boot_reset_state();
	enforce_system_off_wake_hold();

	struct app_event event;
	struct app_ui_state ui_state = {
		.operating_mode = MACROPAD_MODE_DESKTOP_DONGLE,
		.ble_state = MACROPAD_BLE_LINK_STATE_INACTIVE,
		.ble_retry_attempt = 0U,
		.ble_retry_seconds_remaining = 0U,
		.connected = false,
		.dongle_activity = false,
		.usb_power_present = false,
		.battery_warning_visible = false,
		.keys_locked = false,
		.last_delivery_success_ms = INT64_MIN,
	};
	struct options_menu_state options_menu = {
		.open = false,
		.selected_index = 0U,
		.first_visible_index = 0U,
	};
	struct modal_menu_state mode_menu = {
		.open = false,
		.selected_index = 0U,
		.first_visible_index = 0U,
	};
	struct device_info_state device_info = {
		.open = false,
		.first_visible_index = 0U,
	};
	struct brightness_menu_state brightness_menu = {
		.open = false,
		.original_brightness = 0U,
		.edit_brightness = 0U,
	};
	struct modal_menu_state feedback_menu = {
		.open = false,
		.selected_index = 0U,
		.first_visible_index = 0U,
	};
	struct modal_menu_state power_confirm = {
		.open = false,
		.selected_index = 0U,
		.first_visible_index = 0U,
	};
	struct modal_menu_state dfu_confirm = {
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
	enum macropad_ble_feedback ble_feedback = MACROPAD_BLE_FEEDBACK_LED_MED;
	uint8_t display_brightness = DISPLAY_BRIGHTNESS_DEFAULT;
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
	ui_state.keys_locked = macropad_config_get_keys_locked();
	ble_feedback = macropad_config_get_ble_feedback();
	display_brightness = macropad_config_get_display_brightness();

	rc = key_leds_init();
	if (rc != 0) {
		LOG_INF("Macropad LED strip unavailable (rc=%d)", rc);
	}

	rc = battery_init();
	if (rc != 0) {
		LOG_INF("Battery monitor unavailable (rc=%d)", rc);
	}
	ui_state.usb_power_present = status_usb_power_present();
	refresh_battery_state(&macropad_input_state, false);
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
		rc = status_display_set_brightness(display_brightness);
		if (rc != 0) {
			LOG_WRN("Failed to apply display brightness 0x%02x: %d",
				display_brightness, rc);
		}
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
		rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
			macropad_input_state.keys);
		if (rc != 0) {
			LOG_WRN("Failed to initialize macropad LEDs: %d", rc);
		}
	}

	rc = start_transport(ui_state.operating_mode);
	if (rc != 0) {
		LOG_ERR("Failed to start saved mode %u: %d", ui_state.operating_mode, rc);
		if (ui_state.operating_mode == MACROPAD_MODE_DESKTOP_DONGLE) {
			return 0;
		}

		ui_state.operating_mode = MACROPAD_MODE_DESKTOP_DONGLE;
		rc = start_transport(ui_state.operating_mode);
		if (rc != 0) {
			LOG_ERR("Failed to start fallback dongle mode: %d", rc);
			return 0;
		}
		rc = macropad_config_store_operating_mode(ui_state.operating_mode);
		if (rc != 0) {
			LOG_WRN("Failed to persist fallback dongle mode: %d", rc);
		}
		rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
			macropad_input_state.keys);
		if (rc != 0) {
			LOG_WRN("Failed to refresh fallback key feedback: %d", rc);
		}
		redraw = true;
	}
	ui_state.ble_state = ble_link_state_for_mode(ui_state.operating_mode);
	refresh_ble_retry_status(&ui_state);

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
			if (dfu_confirm.open) {
				modal_menu_close(&dfu_confirm);
				options_menu_close(&options_menu);
			} else if (power_confirm.open) {
				modal_menu_close(&power_confirm);
				options_menu_close(&options_menu);
			} else if (mode_menu.open) {
				modal_menu_close(&mode_menu);
				options_menu_close(&options_menu);
			} else if (brightness_menu.open) {
				uint8_t original_brightness = brightness_menu.original_brightness;

				brightness_menu_close(&brightness_menu);
				options_menu_close(&options_menu);
				if (display_available) {
					(void)status_display_set_brightness(original_brightness);
				}
			} else if (feedback_menu.open) {
				modal_menu_close(&feedback_menu);
				options_menu_close(&options_menu);
				(void)apply_key_feedback(ui_state.operating_mode, ble_feedback,
					macropad_input_state.keys);
			} else if (device_info.open) {
				device_info_close(&device_info);
				options_menu_close(&options_menu);
			} else if (options_menu.open) {
				options_menu_close(&options_menu);
			} else {
				options_menu_open(&options_menu, ui_state.operating_mode);
			}
			encoder_long_press_consumed = true;
			status_buzzer_set(false);
			redraw = true;
		}

		if (now_ms >= next_heartbeat_ms) {
			if (macropad_mode_is_desktop(ui_state.operating_mode)) {
				rc = macropad_send_heartbeat(ui_state.operating_mode);
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
			if (dfu_confirm.open) {
				modal_menu_move(&dfu_confirm, DFU_CONFIRM_ITEM_COUNT, 0);
				rc = status_display_render_menu("Wireless Flash?", dfu_confirm_labels,
					DFU_CONFIRM_ITEM_COUNT, dfu_confirm.selected_index,
					dfu_confirm.first_visible_index);
			} else if (power_confirm.open) {
				modal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT, 0);
				rc = status_display_render_menu("Power Off?", power_confirm_labels,
					POWER_CONFIRM_ITEM_COUNT, power_confirm.selected_index,
					power_confirm.first_visible_index);
			} else if (mode_menu.open) {
				struct mode_menu_labels mode_labels;

				modal_menu_move(&mode_menu, MODE_MENU_ITEM_COUNT, 0);
				build_mode_menu_labels(ui_state.operating_mode, &mode_labels);
				rc = status_display_render_menu("Mode", mode_labels.labels,
					MODE_MENU_ITEM_COUNT, mode_menu.selected_index,
					mode_menu.first_visible_index);
			} else if (feedback_menu.open) {
				modal_menu_move(&feedback_menu, FEEDBACK_ITEM_COUNT, 0);
				rc = status_display_render_menu("Feedback", feedback_labels,
					FEEDBACK_ITEM_COUNT, feedback_menu.selected_index,
					feedback_menu.first_visible_index);
			} else if (brightness_menu.open) {
				rc = status_display_render_brightness(brightness_menu.edit_brightness);
			} else if (device_info.open) {
				struct device_info_lines info_lines;

				build_device_info_lines(&ui_state, &info_lines);
				rc = status_display_render_info("Device Info", info_lines.lines,
					DEVICE_INFO_LINE_COUNT, device_info.first_visible_index);
			} else if (options_menu.open) {
				const char *menu_labels[OPTIONS_MENU_MAX_ITEM_COUNT];
				size_t item_count;

				options_menu_clamp(&options_menu, ui_state.operating_mode);
				item_count = options_menu_labels(ui_state.operating_mode,
					ui_state.keys_locked, menu_labels);
				rc = status_display_render_menu("Options", menu_labels,
					item_count, options_menu.selected_index,
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

		if ((macropad_mode_transport(ui_state.operating_mode) == MACROPAD_TRANSPORT_ESB) &&
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

			if (dfu_confirm.open) {
				modal_menu_move(&dfu_confirm, DFU_CONFIRM_ITEM_COUNT,
					encoder_delta);
				redraw = true;
				continue;
			}

			if (power_confirm.open) {
				modal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT,
					encoder_delta);
				redraw = true;
				continue;
			}

			if (mode_menu.open) {
				modal_menu_move(&mode_menu, MODE_MENU_ITEM_COUNT, encoder_delta);
				redraw = true;
				continue;
			}

			if (feedback_menu.open) {
				size_t previous_index = feedback_menu.selected_index;

				modal_menu_move(&feedback_menu, FEEDBACK_ITEM_COUNT,
					encoder_delta);
				if (feedback_menu.selected_index != previous_index) {
					preview_ble_feedback(
						feedback_from_index(feedback_menu.selected_index));
				}
				redraw = true;
				continue;
			}

			if (brightness_menu.open) {
				brightness_menu_move(&brightness_menu, encoder_delta);
				rc = status_display_set_brightness(brightness_menu.edit_brightness);
				if (rc != 0) {
					LOG_WRN("Failed to preview display brightness 0x%02x: %d",
						brightness_menu.edit_brightness, rc);
				}
				redraw = true;
				continue;
			}

			if (device_info.open) {
				device_info_move(&device_info, encoder_delta);
				redraw = true;
				continue;
			}

			if (options_menu.open) {
				options_menu_move(&options_menu, ui_state.operating_mode,
					encoder_delta);
				redraw = true;
				continue;
			}

			if (ui_state.keys_locked) {
				redraw = true;
				continue;
			}

			status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
			redraw = true;

			if (macropad_mode_is_desktop(ui_state.operating_mode)) {
				rc = send_encoder_delta_reports(ui_state.operating_mode, encoder_delta);
				if (rc != 0) {
					LOG_ERR("send_encoder_delta_reports failed: %d", rc);
				}
			} else {
				while (encoder_delta != 0) {
					uint8_t usage = (encoder_delta > 0) ?
						KINDLE_BLE_HID_USAGE_KEYBOARD_DOWN_ARROW :
						KINDLE_BLE_HID_USAGE_KEYBOARD_UP_ARROW;

					rc = kindle_ble_hid_send_key_tap(usage);
					if ((rc != 0) && (rc != -ENOTCONN) &&
					    (rc != -EAGAIN)) {
						LOG_ERR("kindle_ble_hid_send_key_tap failed: %d", rc);
						break;
					}
					encoder_delta += (encoder_delta > 0) ? -1 : 1;
				}
			}
		} else if (event.type == APP_EVENT_ENCODER_BUTTON) {
			if (event.pressed != 0U) {
				encoder_button_down = true;
				encoder_button_down_ms = now_ms;
				encoder_long_press_consumed = false;
				if (macropad_mode_is_desktop(ui_state.operating_mode) ||
				    ble_feedback_uses_buzzer(ble_feedback)) {
					status_buzzer_set(true);
				}
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

			if (dfu_confirm.open) {
				bool confirmed = (dfu_confirm.selected_index == 1U);

				modal_menu_close(&dfu_confirm);
				if (confirmed) {
					options_menu_close(&options_menu);
					perform_wireless_flash(ui_state.operating_mode,
						display_available, &display_sleeping);
				}
				redraw = true;
				continue;
			}

			if (power_confirm.open) {
				bool confirmed = (power_confirm.selected_index == 1U);

				modal_menu_close(&power_confirm);
				if (confirmed) {
					options_menu_close(&options_menu);
					perform_user_poweroff(ui_state.operating_mode,
						display_available, &display_sleeping);
				}
				redraw = true;
				continue;
			}

			if (mode_menu.open) {
				enum macropad_operating_mode target_mode =
					mode_menu_target(mode_menu.selected_index);

				modal_menu_close(&mode_menu);
				options_menu_close(&options_menu);
				rc = switch_operating_mode(&ui_state, target_mode,
					display_available, &display_sleeping);
				if (rc != 0) {
					LOG_ERR("switch_operating_mode failed: %d", rc);
				}
				rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
					macropad_input_state.keys);
				if ((rc != 0) && (rc != -ENOTSUP)) {
					LOG_WRN("Failed to refresh key feedback: %d", rc);
				}
				redraw = true;
				continue;
			}

			if (feedback_menu.open) {
				enum macropad_ble_feedback selected_feedback =
					feedback_from_index(feedback_menu.selected_index);

				modal_menu_close(&feedback_menu);
				rc = macropad_config_store_ble_feedback(selected_feedback);
				if (rc != 0) {
					LOG_WRN("Failed to persist BLE feedback: %d", rc);
				}
				ble_feedback = selected_feedback;
				rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
					macropad_input_state.keys);
				if ((rc != 0) && (rc != -ENOTSUP)) {
					LOG_WRN("Failed to apply BLE feedback: %d", rc);
				}
				redraw = true;
				continue;
			}

			if (brightness_menu.open) {
				rc = macropad_config_store_display_brightness(
					brightness_menu.edit_brightness);
				if (rc != 0) {
					LOG_WRN("Failed to persist display brightness: %d", rc);
				}
				display_brightness = brightness_menu.edit_brightness;
				brightness_menu_close(&brightness_menu);
				if (!options_menu.open) {
					options_menu_open(&options_menu, ui_state.operating_mode);
				}
				redraw = true;
				continue;
			}

			if (device_info.open) {
				device_info_close(&device_info);
				if (!options_menu.open) {
					options_menu_open(&options_menu, ui_state.operating_mode);
				}
				redraw = true;
				continue;
			}

			if (options_menu.open) {
				const struct options_menu_item *items;
				const struct options_menu_item *selected;
				size_t item_count;

				options_menu_clamp(&options_menu, ui_state.operating_mode);
				items = options_menu_items_for_mode(ui_state.operating_mode,
					&item_count);
				if (item_count == 0U) {
					options_menu_close(&options_menu);
					redraw = true;
					continue;
				}
				selected = &items[options_menu.selected_index];

				if (selected->action == OPTIONS_MENU_ACTION_SWITCH_MODE) {
					modal_menu_open(&mode_menu,
						mode_menu_index(ui_state.operating_mode));
				} else if (selected->action ==
					   OPTIONS_MENU_ACTION_FORGET_BLE_PAIRING) {
					options_menu_close(&options_menu);
					rc = forget_ble_pairing(&ui_state, display_available,
						&display_sleeping);
					if (rc != 0) {
						LOG_ERR("forget_ble_pairing failed: %d", rc);
					}
				} else if (selected->action == OPTIONS_MENU_ACTION_FEEDBACK) {
					modal_menu_open(&feedback_menu, feedback_index(ble_feedback));
					preview_ble_feedback(ble_feedback);
				} else if (selected->action == OPTIONS_MENU_ACTION_DEVICE_INFO) {
					device_info_open(&device_info);
				} else if (selected->action == OPTIONS_MENU_ACTION_BRIGHTNESS) {
					brightness_menu_open(&brightness_menu,
						status_display_get_brightness());
				} else if (selected->action ==
					   OPTIONS_MENU_ACTION_WIRELESS_FLASH) {
					modal_menu_open(&dfu_confirm, 0U);
				} else if (selected->action == OPTIONS_MENU_ACTION_POWER_OFF) {
					modal_menu_open(&power_confirm, 0U);
				} else if (selected->action == OPTIONS_MENU_ACTION_LOCK_KEYS) {
					ui_state.keys_locked = !ui_state.keys_locked;
					options_menu_close(&options_menu);
					rc = macropad_config_store_keys_locked(ui_state.keys_locked);
					if (rc != 0) {
						LOG_WRN("Failed to update key lock state: %d", rc);
					}
					if (ui_state.keys_locked) {
						rc = send_neutral_input_state(ui_state.operating_mode);
						if ((rc != 0) && (rc != -ENOTCONN) &&
						    (rc != -EAGAIN)) {
							LOG_WRN("Failed to send neutral input state: %d",
								rc);
						}
					}
				} else {
					options_menu_close(&options_menu);
				}
				redraw = true;
				continue;
			}

			if (ui_state.keys_locked) {
				redraw = true;
				continue;
			}

			if (macropad_mode_is_desktop(ui_state.operating_mode)) {
				rc = send_encoder_button_click_report(ui_state.operating_mode);
				if (rc != 0) {
					LOG_ERR("send_encoder_button_click_report failed: %d", rc);
				}
			} else {
				rc = kindle_ble_hid_send_key_tap(KINDLE_BLE_HID_USAGE_KEYBOARD_ENTER);
				if ((rc != 0) && (rc != -ENOTCONN) && (rc != -EAGAIN)) {
					LOG_ERR("kindle_ble_hid_send_key_tap enter failed: %d", rc);
				}
			}
			redraw = true;
		} else if (event.type == APP_EVENT_MACROPAD_KEYS) {
			bool overlay_open = local_overlay_open(&options_menu, &mode_menu,
				&device_info, &brightness_menu, &feedback_menu,
				&power_confirm, &dfu_confirm);

			if ((macropad_mode_transport(ui_state.operating_mode) == MACROPAD_TRANSPORT_ESB) &&
			    device_info.open) {
				redraw = true;
				continue;
			}

			macropad_input_state.keys = event.keys;
			rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
				macropad_input_state.keys);
			if ((rc != 0) && (rc != -ENOTSUP)) {
				LOG_WRN("Failed to update macropad LEDs: %d", rc);
			}

			if (ui_state.keys_locked) {
				redraw = true;
				continue;
			}

			if (macropad_mode_endpoint(ui_state.operating_mode) == MACROPAD_ENDPOINT_KINDLE) {
				rc = kindle_ble_hid_send_key_state(overlay_open ? 0U :
					macropad_input_state.keys);
				if ((rc != 0) && (rc != -ENOTCONN) && (rc != -EAGAIN)) {
					LOG_ERR("kindle_ble_hid_send_key_state failed: %d", rc);
				}
			} else {
				rc = macropad_send_report(ui_state.operating_mode);
				if (rc != 0) {
					LOG_ERR("macropad_send_report failed: %d", rc);
				}
			}

			if (event.pressed == 0U) {
				continue;
			}

			if ((macropad_mode_endpoint(ui_state.operating_mode) == MACROPAD_ENDPOINT_KINDLE) &&
			    ble_feedback_uses_buzzer(ble_feedback) && !overlay_open) {
				status_buzzer_pulse(FEEDBACK_PREVIEW_MS);
			} else if (macropad_mode_is_desktop(ui_state.operating_mode)) {
				status_led_pulse(INPUT_ACTIVITY_LED_PULSE_MS);
			}
			redraw = true;
		} else if (event.type == APP_EVENT_MACROPAD_CONFIG) {
			if (macropad_mode_is_desktop(ui_state.operating_mode)) {
				key_leds_apply_config(&event.config);
				rc = apply_key_feedback(ui_state.operating_mode, ble_feedback,
					macropad_input_state.keys);
				if ((rc != 0) && (rc != -ENOTSUP)) {
					LOG_WRN("Failed to apply macropad config: %d", rc);
				}
			}
		} else if (event.type == APP_EVENT_TX_RESULT) {
			if (handle_tx_result_event(&ui_state, &event, now_ms)) {
				redraw = true;
			}
		}
	}

	return 0;
}

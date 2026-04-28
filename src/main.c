/*
 * Copyright (c) 2018 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

#include "encoder.h"
#include "radio_esb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_ICON_X 0U
#define DISPLAY_STATUS_Y 0U
#define DISPLAY_ACK_X 24U
#define DISPLAY_CHARGING_X 88U
#define DISPLAY_VALUE_X 0U
#define DISPLAY_VALUE_Y 16U
#define DISPLAY_CONNECTED_ICON "[+]"
#define DISPLAY_DISCONNECTED_ICON "[x]"
#define DISPLAY_ACK_TEXT "ACK"
#define DISPLAY_CHARGING_TEXT "USB"
#define DISPLAY_REFRESH_INTERVAL_MS 40
#define ACK_VISIBLE_MS 50
#define HEARTBEAT_INTERVAL_MS 1000
#define CONNECTION_TIMEOUT_MS 2500
#define INPUT_ACTIVITY_LED_PULSE_MS 80
#define APP_EVENT_QUEUE_LEN 8
#define BATTERY_ADC_CHANNEL_ID 0U
#define BATTERY_ADC_RESOLUTION 14U
#define BATTERY_ADC_OVERSAMPLING 4U
#define STARTUP_LED_STRIP_BLUE_LEVEL 0x40U

#define LED_STRIP_NODE DT_ALIAS(led_strip)

#if DT_NODE_EXISTS(LED_STRIP_NODE) && DT_NODE_HAS_STATUS(LED_STRIP_NODE, okay)
#define HAVE_STARTUP_LED_STRIP 1
#define STARTUP_LED_STRIP_LENGTH DT_PROP(LED_STRIP_NODE, chain_length)
static const struct device *const startup_led_strip = DEVICE_DT_GET(LED_STRIP_NODE);
static struct led_rgb startup_led_strip_pixels[STARTUP_LED_STRIP_LENGTH];
#else
#define HAVE_STARTUP_LED_STRIP 0
#endif

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static const struct device *const battery_adc = DEVICE_DT_GET(DT_NODELABEL(adc));
static struct k_spinlock input_state_lock;

enum app_event_type {
	APP_EVENT_ENCODER_DELTA = 0,
	APP_EVENT_ENCODER_BUTTON = 1,
	APP_EVENT_TX_RESULT = 2,
};

struct app_event {
	uint8_t type;
	uint8_t tx_kind;
	uint8_t acked;
	uint8_t pressed;
	int8_t encoder_delta;
};

struct app_ui_state {
	bool connected;
	bool charging;
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
	uint8_t charging;
};

K_MSGQ_DEFINE(app_event_queue, sizeof(struct app_event), APP_EVENT_QUEUE_LEN, 4);

static struct macropad_input_state macropad_input_state;
static struct k_work_delayable status_led_off_work;
static int32_t pending_encoder_delta;
static bool encoder_delta_event_pending;
static bool battery_monitor_ready;
static int16_t battery_sample_raw;
static struct adc_channel_cfg battery_channel_cfg = {
	.gain = ADC_GAIN_1_6,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
	.channel_id = BATTERY_ADC_CHANNEL_ID,
	.input_positive = NRF_SAADC_VDDHDIV5,
};
static struct adc_sequence battery_sequence = {
	.channels = BIT(BATTERY_ADC_CHANNEL_ID),
	.buffer = &battery_sample_raw,
	.buffer_size = sizeof(battery_sample_raw),
	.oversampling = BATTERY_ADC_OVERSAMPLING,
	.calibrate = true,
	.resolution = BATTERY_ADC_RESOLUTION,
};

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_STATUS_LED 1
#else
#define HAVE_STATUS_LED 0
#endif

static void set_status_led(bool on)
{
#if HAVE_STATUS_LED
	gpio_pin_set_dt(&status_led, on);
#else
	ARG_UNUSED(on);
#endif
}

static void status_led_off_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	set_status_led(false);
}

static void blink_status_led(uint8_t count, uint32_t pulse_ms, uint32_t gap_ms)
{
	for (uint8_t idx = 0; idx < count; ++idx) {
		set_status_led(true);
		k_msleep(pulse_ms);
		set_status_led(false);
		k_msleep(gap_ms);
	}
}

static void pulse_status_led(uint32_t pulse_ms)
{
#if HAVE_STATUS_LED
	set_status_led(true);
	(void)k_work_reschedule(&status_led_off_work, K_MSEC(pulse_ms));
#else
	ARG_UNUSED(pulse_ms);
#endif
}

static int setup_status_led(void)
{
#if HAVE_STATUS_LED
	if (!gpio_is_ready_dt(&status_led)) {
		return -ENODEV;
	}

	k_work_init_delayable(&status_led_off_work, status_led_off_work_handler);

	return gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
#else
	return -ENOTSUP;
#endif
}

static int setup_startup_led_strip(void)
{
#if HAVE_STARTUP_LED_STRIP
	const struct led_rgb blue = {
		.r = 0U,
		.g = 0U,
		.b = STARTUP_LED_STRIP_BLUE_LEVEL,
	};

	if (!device_is_ready(startup_led_strip)) {
		return -ENODEV;
	}

	for (size_t idx = 0; idx < STARTUP_LED_STRIP_LENGTH; ++idx) {
		startup_led_strip_pixels[idx] = blue;
	}

	return led_strip_update_rgb(startup_led_strip,
					startup_led_strip_pixels,
					STARTUP_LED_STRIP_LENGTH);
#else
	return -ENOTSUP;
#endif
}

static int app_queue_event(const struct app_event *event)
{
	int rc = k_msgq_put(&app_event_queue, event, K_NO_WAIT);

	if (rc != 0) {
		LOG_WRN("Dropping app event type=%u rc=%d", event->type, rc);
	}

	return rc;
}

static int render_status_display(const struct app_ui_state *state)
{
	int rc;
	char value_text[16];
	const char *icon = state->connected ? DISPLAY_CONNECTED_ICON : DISPLAY_DISCONNECTED_ICON;

	(void)snprintf(value_text, sizeof(value_text), "%ld", (long)state->value);

	rc = cfb_framebuffer_clear(display, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	rc = cfb_print(display, icon, DISPLAY_ICON_X, DISPLAY_STATUS_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print icon failed: %d", rc);
		return rc;
	}

	if (state->show_ack) {
		rc = cfb_print(display, DISPLAY_ACK_TEXT, DISPLAY_ACK_X, DISPLAY_STATUS_Y);
		if (rc != 0) {
			LOG_ERR("cfb_print ACK failed: %d", rc);
			return rc;
		}
	}

	if (state->charging) {
		rc = cfb_print(display, DISPLAY_CHARGING_TEXT, DISPLAY_CHARGING_X, DISPLAY_STATUS_Y);
		if (rc != 0) {
			LOG_ERR("cfb_print charging failed: %d", rc);
			return rc;
		}
	}

	rc = cfb_print(display, value_text, DISPLAY_VALUE_X, DISPLAY_VALUE_Y);
	if (rc != 0) {
		LOG_ERR("cfb_print value failed: %d", rc);
		return rc;
	}

	rc = cfb_framebuffer_finalize(display);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
	}

	return rc;
}

static bool battery_is_charging(void)
{
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}

static int battery_monitor_init(void)
{
	int rc;

	if (!device_is_ready(battery_adc)) {
		LOG_WRN("Battery ADC not ready");
		return -ENODEV;
	}

	rc = adc_channel_setup(battery_adc, &battery_channel_cfg);
	if (rc != 0) {
		LOG_ERR("Battery ADC channel setup failed: %d", rc);
		return rc;
	}

	battery_monitor_ready = true;
	return 0;
}

static uint16_t battery_sample_mv(void)
{
	int rc;
	int32_t sample_mv;

	if (!battery_monitor_ready) {
		return 0U;
	}

	rc = adc_read(battery_adc, &battery_sequence);
	battery_sequence.calibrate = false;
	if (rc != 0) {
		LOG_WRN("Battery ADC read failed: %d", rc);
		return 0U;
	}

	sample_mv = battery_sample_raw;
	rc = adc_raw_to_millivolts(adc_ref_internal(battery_adc),
		battery_channel_cfg.gain,
		battery_sequence.resolution,
		&sample_mv);
	if (rc != 0) {
		LOG_WRN("Battery ADC conversion failed: %d", rc);
		return 0U;
	}

	sample_mv *= 5;
	if (sample_mv <= 0) {
		return 0U;
	}
	if (sample_mv > UINT16_MAX) {
		return UINT16_MAX;
	}

	return (uint16_t)sample_mv;
}

static void refresh_battery_state(struct macropad_input_state *state)
{
	state->battery_mv = battery_sample_mv();
	state->charging = battery_is_charging() ? 1U : 0U;
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
		.charging = macropad_input_state.charging,
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
	report.charging = macropad_input_state.charging;

	return radio_esb_send_heartbeat(&report);
}

static int send_encoder_delta_reports(int32_t total_delta)
{
	int rc = 0;

	while (total_delta != 0) {
		int8_t step_delta;

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

static void handle_radio_delivery(enum radio_esb_tx_kind kind, bool acked)
{
	const struct app_event event = {
		.type = APP_EVENT_TX_RESULT,
		.tx_kind = (uint8_t)kind,
		.acked = acked ? 1U : 0U,
	};

	(void)app_queue_event(&event);
}

static bool update_time_driven_ui(struct app_ui_state *state, int64_t now_ms)
{
	bool dirty = false;
	bool charging = battery_is_charging();

	if (state->charging != charging) {
		state->charging = charging;
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
		.charging = false,
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

	rc = setup_status_led();
	if (rc == 0) {
		blink_status_led(1, 120, 120);
	} else {
		LOG_INF("Status LED unavailable (rc=%d)", rc);
	}

	rc = setup_startup_led_strip();
	if (rc != 0) {
		LOG_INF("Startup LED strip unavailable (rc=%d)", rc);
	}

	rc = battery_monitor_init();
	if (rc != 0) {
		LOG_INF("Battery monitor unavailable (rc=%d)", rc);
	}
	ui_state.charging = battery_is_charging();

	LOG_INF("boot: entering main()");
	if (!device_is_ready(display)) {
		LOG_ERR("Display device not ready");
		return 0;
	}

	rc = display_set_pixel_format(display, PIXEL_FORMAT_MONO10);
	if (rc != 0) {
		rc = display_set_pixel_format(display, PIXEL_FORMAT_MONO01);
	}
	LOG_INF("display_set_pixel_format() -> %d", rc);
	if (rc != 0) {
		return 0;
	}

	rc = display_blanking_off(display);
	LOG_INF("display_blanking_off() -> %d", rc);
	if ((rc != 0) && (rc != -ENOSYS)) {
		return 0;
	}

	rc = cfb_framebuffer_init(display);
	LOG_INF("cfb_framebuffer_init() -> %d", rc);
	if (rc != 0) {
		return 0;
	}

	k_msleep(1000);
	rc = render_status_display(&ui_state);
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
	rc = radio_esb_init(&addr_config, rf_channel, handle_radio_delivery);
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
			      ((now_ms - last_display_update_ms) >= DISPLAY_REFRESH_INTERVAL_MS))) {
			rc = render_status_display(&ui_state);
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
				display_wait_ms = DISPLAY_REFRESH_INTERVAL_MS -
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

			pulse_status_led(INPUT_ACTIVITY_LED_PULSE_MS);
			ui_state.value += encoder_delta;
			redraw = true;

			rc = send_encoder_delta_reports(encoder_delta);
			if (rc != 0) {
				LOG_ERR("send_encoder_delta_reports failed: %d", rc);
			}
		} else if (event.type == APP_EVENT_ENCODER_BUTTON) {
			macropad_input_state.encoder_pressed = event.pressed;
			rc = macropad_send_report();
			if (rc != 0) {
				LOG_ERR("macropad_send_report failed: %d", rc);
			}

			if (event.pressed == 0U) {
				continue;
			}

			ui_state.value = -ui_state.value;
			redraw = true;
		} else if (event.type == APP_EVENT_TX_RESULT) {
			if (handle_tx_result_event(&ui_state, &event, now_ms)) {
				redraw = true;
			}
		}
	}

	return 0;
}

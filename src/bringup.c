#include "bringup.h"

#if BRINGUP_STAGE > 0

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "key_leds.h"
#include "macropad_config.h"
#include "status_io.h"

#if BRINGUP_STAGE >= 2
#include "macropad_keys.h"
#endif

#if BRINGUP_STAGE >= 5
#include "status_display.h"
#endif

#if BRINGUP_STAGE >= 6
#include "battery.h"
#include "radio_esb.h"
#include "radio_identity.h"
#endif

LOG_MODULE_REGISTER(bringup, LOG_LEVEL_INF);

#if BRINGUP_STAGE >= 6
static struct {
	uint8_t keys;
	uint8_t encoder_pressed;
} bringup_input_state;
static volatile bool bringup_key_leds_dirty;

static void bringup_render_key_leds(void)
{
	if (bringup_input_state.encoder_pressed != 0U) {
		(void)key_leds_set_all(255U, 255U, 255U);
		return;
	}

	(void)key_leds_update(bringup_input_state.keys);
}

static void bringup_apply_outputs(void)
{
	status_led_set(bringup_input_state.encoder_pressed != 0U);
	status_buzzer_set(bringup_input_state.encoder_pressed != 0U);
	bringup_render_key_leds();
}

static void bringup_send_report(void)
{
	macropad_report_t report = {
		.keys             = bringup_input_state.keys,
		.encoder_delta    = 0,
		.encoder_pressed  = bringup_input_state.encoder_pressed,
		.battery_mv       = battery_sample_mv(),
		.usb_power_present = status_usb_power_present() ? 1U : 0U,
	};
	(void)radio_esb_send_macropad_report(&report);
}
#endif

#if BRINGUP_STAGE >= 2
static void bringup_key_callback(uint8_t key_mask, bool pressed, uint8_t keys)
{
	ARG_UNUSED(key_mask);

	if (pressed) {
		LOG_INF("bringup s2: key PRESS   mask=0x%02x", keys);
	} else {
		LOG_INF("bringup s2: key RELEASE mask=0x%02x", keys);
	}

#if BRINGUP_STAGE >= 6
	bringup_input_state.keys = keys;
	bringup_key_leds_dirty = true;
	bringup_send_report();
#else
	(void)key_leds_update(keys);
#endif
}
#endif

#if BRINGUP_STAGE >= 3

#define BRINGUP_BTN_PIN 6U

static const struct device *bringup_gpio1;

static void bringup_poll_button(void)
{
	int raw = gpio_pin_get_raw(bringup_gpio1, BRINGUP_BTN_PIN);
	uint8_t pressed;

	if (raw < 0) {
		LOG_ERR("bringup: btn read err %d", raw);
		return;
	}

	/* Current board wiring: encoder switch shorts to GND when pressed. */
	pressed = (raw == 0) ? 1U : 0U;

	if (pressed == bringup_input_state.encoder_pressed) {
		return;
	}

	bringup_input_state.encoder_pressed = pressed;
	bringup_apply_outputs();
	bringup_send_report();
}
#endif

int bringup_main(void)
{
	int rc;

	(void)status_led_init();
	status_led_set(false);

	LOG_INF("===== BRINGUP STAGE %d =====", BRINGUP_STAGE);

	LOG_INF("bringup s1: LED strip init");
	rc = macropad_config_init();
	if (rc != 0) {
		LOG_INF("bringup s1: macropad_config_init unavailable (rc=%d)", rc);
	}

	rc = key_leds_init();
	if (rc != 0) {
		LOG_ERR("bringup s1: key_leds_init FAIL (rc=%d)", rc);
	} else {
		rc = key_leds_set_all(0, 0, 0);
		if (rc != 0) {
			LOG_ERR("bringup s1: key_leds_set_all FAIL (rc=%d)", rc);
		} else {
			LOG_INF("bringup s1: LED strip OK");
		}
	}

#if BRINGUP_STAGE >= 2
	LOG_INF("bringup s2: key inputs init");

	rc = macropad_keys_init(bringup_key_callback);
	if (rc == -ENOTSUP) {
		LOG_WRN("bringup s2: key inputs not in devicetree (ENOTSUP) — skipping");
	} else if (rc != 0) {
		LOG_ERR("bringup s2: macropad_keys_init FAIL (rc=%d)", rc);
	} else {
	#if BRINGUP_STAGE >= 6
		bringup_input_state.keys = macropad_keys_get_pressed_mask();
		bringup_key_leds_dirty = true;
	#endif
		LOG_INF("bringup s2: key inputs OK — press keys to test");
	}
#endif

#if BRINGUP_STAGE >= 3
	LOG_INF("bringup s3: raw button + buzzer init");

	bringup_gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	if (!device_is_ready(bringup_gpio1)) {
		LOG_ERR("bringup s3: gpio1 not ready");
	} else {
		rc = gpio_pin_configure(bringup_gpio1, BRINGUP_BTN_PIN, GPIO_INPUT | GPIO_PULL_UP);
		if (rc != 0) {
			LOG_ERR("bringup s3: button pin configure FAIL (rc=%d)", rc);
		} else {
			LOG_INF("bringup s3: raw button OK - press encoder to test outputs");
		}
	}

	rc = status_buzzer_init();
	if (rc == -ENOTSUP) {
		LOG_WRN("bringup s3: buzzer not in devicetree (ENOTSUP) - skipping");
	} else if (rc != 0) {
		LOG_ERR("bringup s3: status_buzzer_init FAIL (rc=%d)", rc);
	} else {
		status_buzzer_set(false);
		LOG_INF("bringup s3: buzzer init OK");
	}
#endif

#if BRINGUP_STAGE >= 5
	LOG_INF("bringup s5: display init");

	/* Mirror what main() does before status_display_init():
	 * a blocking LED blink that gives the OLED time to settle. */
	status_led_blink(1, 120, 120);
	status_led_set(false);

	rc = status_display_init();
	if (rc != 0) {
		LOG_WRN("bringup s5: status_display_init FAIL (rc=%d)", rc);
	} else {
		rc = status_display_render(false, false, false, 0);
		if (rc != 0) {
			LOG_WRN("bringup s5: status_display_render FAIL (rc=%d)", rc);
		} else {
			LOG_INF("bringup s5: display OK");
		}
	}
#endif

#if BRINGUP_STAGE >= 6
	LOG_INF("bringup s6: battery monitor init");

	rc = battery_init();
	if (rc != 0) {
		LOG_WRN("bringup s6: battery_init FAIL (rc=%d) — ADC or voltage divider fault",
			rc);
	} else {
		uint16_t mv = battery_sample_mv();
		bool usb   = status_usb_power_present();

		LOG_INF("bringup s6: battery OK  vbat=%u mV  usb_power=%d", mv, (int)usb);
	}
#endif

#if BRINGUP_STAGE >= 6
	LOG_INF("bringup s6: radio init");

	{
		struct dongle_identity identity;
		struct esb_addr_config addr_config;
		uint8_t rf_channel;
		bool generated;

		rc = radio_identity_init();
		if (rc != 0) {
			LOG_ERR("bringup s6: radio_identity_init FAIL (rc=%d)", rc);
			goto bringup_idle;
		}

		rc = radio_identity_load_or_create(&identity, &generated);
		if (rc != 0) {
			LOG_ERR("bringup s6: radio_identity_load_or_create FAIL (rc=%d)", rc);
			goto bringup_idle;
		}
		LOG_INF("bringup s6: identity %s", generated ? "generated" : "loaded");

		rc = radio_identity_derive_esb_config(&identity, &addr_config, &rf_channel);
		if (rc != 0) {
			LOG_ERR("bringup s6: radio_identity_derive_esb_config FAIL (rc=%d)", rc);
			goto bringup_idle;
		}
		radio_identity_log_esb_config(&addr_config, rf_channel);

		rc = radio_esb_init(&addr_config, rf_channel, NULL, NULL);
		if (rc != 0) {
			LOG_ERR("bringup s6: radio_esb_init FAIL (rc=%d)", rc);
			goto bringup_idle;
		}

		rc = radio_esb_start();
		if (rc != 0) {
			LOG_ERR("bringup s6: radio_esb_start FAIL (rc=%d)", rc);
			goto bringup_idle;
		}
		LOG_INF("bringup s6: radio ESB started");
	}
#endif

bringup_idle:
	__attribute__((unused));
	LOG_INF("bringup: stage %d complete — idling. "
		"Increment BRINGUP_STAGE in bringup_config.h when ready.",
		BRINGUP_STAGE);

	status_buzzer_set(false);
	status_led_set(false);
	#if BRINGUP_STAGE >= 6
	bringup_apply_outputs();
	#else
	(void)key_leds_set_all(0, 0, 0);
	#endif

	while (1) {
#if BRINGUP_STAGE >= 6
		bringup_poll_button();

		if (bringup_key_leds_dirty) {
			bringup_key_leds_dirty = false;
			bringup_render_key_leds();
		}

		{
			static int64_t bringup_next_hb_ms;

			if (k_uptime_get() >= bringup_next_hb_ms) {
				macropad_report_t hb = {
					.battery_mv = battery_sample_mv(),
					.usb_power_present = status_usb_power_present() ? 1U : 0U,
				};
				LOG_INF("bringup idle: vbat=%u mV  usb_power=%d",
					hb.battery_mv, (int)hb.usb_power_present);
				(void)radio_esb_send_heartbeat(&hb);
				bringup_next_hb_ms = k_uptime_get() + 1000;
			}
		}
#elif BRINGUP_STAGE >= 3
		bringup_poll_button();
#endif
		k_msleep(10);
	}

	return 0;
}

#endif

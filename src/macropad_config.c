#include "macropad_config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(macropad_config, LOG_LEVEL_INF);

#define MACROPAD_CONFIG_SETTINGS_ROOT "macropad_cfg"
#define MACROPAD_CONFIG_SETTINGS_KEY MACROPAD_CONFIG_SETTINGS_ROOT "/leds"
#define MACROPAD_MODE_SETTINGS_KEY MACROPAD_CONFIG_SETTINGS_ROOT "/mode"
#define MACROPAD_BLE_FEEDBACK_SETTINGS_KEY MACROPAD_CONFIG_SETTINGS_ROOT "/ble_feedback"

static macropad_config_t stored_config;
static bool stored_config_loaded;
static enum macropad_operating_mode stored_operating_mode;
static bool stored_operating_mode_loaded;
static enum macropad_ble_feedback stored_ble_feedback;
static bool stored_ble_feedback_loaded;

static void macropad_config_default(macropad_config_t *config)
{
	memset(config, 0, sizeof(*config));
	config->kind = HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS;
	for (size_t index = 0; index < HPAD_PROTOCOL_KEY_COUNT; ++index) {
		config->keys[index].brightness = UINT8_MAX;
	}
}

static bool macropad_config_valid(const macropad_config_t *config)
{
	return (config != NULL) && (config->kind == HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS);
}

static bool macropad_operating_mode_valid(uint8_t mode)
{
	return (mode == MACROPAD_OPERATING_MODE_DONGLE) ||
		(mode == MACROPAD_OPERATING_MODE_BLE);
}

static bool macropad_ble_feedback_valid(uint8_t feedback)
{
	return (feedback == MACROPAD_BLE_FEEDBACK_BUZZ) ||
		(feedback == MACROPAD_BLE_FEEDBACK_LED_LOW) ||
		(feedback == MACROPAD_BLE_FEEDBACK_LED_MED) ||
		(feedback == MACROPAD_BLE_FEEDBACK_LED_HIGH);
}

static int macropad_config_settings_set(const char *name, size_t len_rd,
				settings_read_cb read_cb, void *cb_arg)
{
	macropad_config_t loaded_config;
	int rc;

	if (settings_name_steq(name, "mode", NULL)) {
		uint8_t loaded_mode;

		if (len_rd != sizeof(loaded_mode)) {
			LOG_WRN("Stored macropad mode length %zu does not match expected %zu",
				len_rd, sizeof(loaded_mode));
			stored_operating_mode_loaded = false;
			return 0;
		}

		rc = read_cb(cb_arg, &loaded_mode, sizeof(loaded_mode));
		if (rc < 0) {
			return rc;
		}
		if (rc != sizeof(loaded_mode)) {
			LOG_WRN("Settings read returned %d bytes for macropad mode", rc);
			stored_operating_mode_loaded = false;
			return 0;
		}
		if (!macropad_operating_mode_valid(loaded_mode)) {
			LOG_WRN("Ignoring invalid persisted macropad mode %u", loaded_mode);
			stored_operating_mode_loaded = false;
			return 0;
		}

		stored_operating_mode = (enum macropad_operating_mode)loaded_mode;
		stored_operating_mode_loaded = true;
		return 0;
	}

	if (settings_name_steq(name, "ble_feedback", NULL)) {
		uint8_t loaded_feedback;

		if (len_rd != sizeof(loaded_feedback)) {
			LOG_WRN("Stored BLE feedback length %zu does not match expected %zu",
				len_rd, sizeof(loaded_feedback));
			stored_ble_feedback_loaded = false;
			return 0;
		}

		rc = read_cb(cb_arg, &loaded_feedback, sizeof(loaded_feedback));
		if (rc < 0) {
			return rc;
		}
		if (rc != sizeof(loaded_feedback)) {
			LOG_WRN("Settings read returned %d bytes for BLE feedback", rc);
			stored_ble_feedback_loaded = false;
			return 0;
		}
		if (!macropad_ble_feedback_valid(loaded_feedback)) {
			LOG_WRN("Ignoring invalid persisted BLE feedback %u", loaded_feedback);
			stored_ble_feedback_loaded = false;
			return 0;
		}

		stored_ble_feedback = (enum macropad_ble_feedback)loaded_feedback;
		stored_ble_feedback_loaded = true;
		return 0;
	}

	if (!settings_name_steq(name, "leds", NULL)) {
		return -ENOENT;
	}

	if (len_rd != sizeof(loaded_config)) {
		LOG_WRN("Stored macropad config length %zu does not match expected %zu",
			len_rd, sizeof(loaded_config));
		stored_config_loaded = false;
		return 0;
	}

	rc = read_cb(cb_arg, &loaded_config, sizeof(loaded_config));
	if (rc < 0) {
		return rc;
	}
	if (rc != sizeof(loaded_config)) {
		LOG_WRN("Settings read returned %d bytes for macropad config", rc);
		stored_config_loaded = false;
		return 0;
	}
	if (!macropad_config_valid(&loaded_config)) {
		LOG_WRN("Ignoring invalid persisted macropad LED config");
		stored_config_loaded = false;
		return 0;
	}

	stored_config = loaded_config;
	stored_config_loaded = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(macropad_config, MACROPAD_CONFIG_SETTINGS_ROOT, NULL,
			      macropad_config_settings_set, NULL, NULL);

int macropad_config_init(void)
{
	int rc;

	macropad_config_default(&stored_config);
	stored_config_loaded = false;
	stored_operating_mode = MACROPAD_OPERATING_MODE_DONGLE;
	stored_operating_mode_loaded = false;
	stored_ble_feedback = MACROPAD_BLE_FEEDBACK_LED_MED;
	stored_ble_feedback_loaded = false;

	rc = settings_subsys_init();
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_WRN("settings_subsys_init failed: %d, continuing with default config", rc);
		return 0;
	}

	rc = settings_load_subtree(MACROPAD_CONFIG_SETTINGS_ROOT);
	if (rc != 0) {
		LOG_WRN("settings_load_subtree failed: %d, using default config", rc);
		return 0;
	}

	if (stored_config_loaded) {
		LOG_INF("Loaded persisted macropad LED config");
	} else {
		LOG_INF("No persisted macropad LED config found");
	}
	if (stored_operating_mode_loaded) {
		LOG_INF("Loaded persisted macropad mode %u", stored_operating_mode);
	} else {
		LOG_INF("No persisted macropad mode found, defaulting to dongle");
	}
	if (stored_ble_feedback_loaded) {
		LOG_INF("Loaded persisted BLE feedback %u", stored_ble_feedback);
	} else {
		LOG_INF("No persisted BLE feedback found, defaulting to medium LED");
	}

	return 0;
}

const macropad_config_t *macropad_config_get(void)
{
	return &stored_config;
}

int macropad_config_store(const macropad_config_t *config)
{
	macropad_config_t next_config;
	int rc;

	if (config == NULL) {
		return -EINVAL;
	}

	next_config = *config;
	next_config.kind = HPAD_PROTOCOL_CONFIG_KIND_KEY_COLORS;
	if (memcmp(&stored_config, &next_config, sizeof(next_config)) == 0) {
		return 0;
	}

	stored_config = next_config;
	stored_config_loaded = true;
	rc = settings_save_one(MACROPAD_CONFIG_SETTINGS_KEY, &next_config, sizeof(next_config));
	if (rc != 0) {
		LOG_WRN("settings_save_one failed: %d, keeping LED config in memory", rc);
		return rc;
	}

	LOG_INF("Stored macropad LED config");
	return 0;
}

enum macropad_operating_mode macropad_config_get_operating_mode(void)
{
	return stored_operating_mode;
}

int macropad_config_store_operating_mode(enum macropad_operating_mode mode)
{
	uint8_t stored_mode = (uint8_t)mode;
	int rc;

	if (!macropad_operating_mode_valid(stored_mode)) {
		return -EINVAL;
	}

	if (stored_operating_mode_loaded && (stored_operating_mode == mode)) {
		return 0;
	}

	stored_operating_mode = mode;
	stored_operating_mode_loaded = true;
	rc = settings_save_one(MACROPAD_MODE_SETTINGS_KEY, &stored_mode, sizeof(stored_mode));
	if (rc != 0) {
		LOG_WRN("settings_save_one failed: %d, keeping mode in memory", rc);
		return rc;
	}

	LOG_INF("Stored macropad mode %u", stored_mode);
	return 0;
}

enum macropad_ble_feedback macropad_config_get_ble_feedback(void)
{
	return stored_ble_feedback;
}

int macropad_config_store_ble_feedback(enum macropad_ble_feedback feedback)
{
	uint8_t stored_feedback = (uint8_t)feedback;
	int rc;

	if (!macropad_ble_feedback_valid(stored_feedback)) {
		return -EINVAL;
	}

	if (stored_ble_feedback_loaded && (stored_ble_feedback == feedback)) {
		return 0;
	}

	stored_ble_feedback = feedback;
	stored_ble_feedback_loaded = true;
	rc = settings_save_one(MACROPAD_BLE_FEEDBACK_SETTINGS_KEY, &stored_feedback,
			       sizeof(stored_feedback));
	if (rc != 0) {
		LOG_WRN("settings_save_one failed: %d, keeping BLE feedback in memory", rc);
		return rc;
	}

	LOG_INF("Stored BLE feedback %u", stored_feedback);
	return 0;
}

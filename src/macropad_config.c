#include "macropad_config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(macropad_config, LOG_LEVEL_INF);

#define MACROPAD_CONFIG_SETTINGS_ROOT "macropad_cfg"
#define MACROPAD_CONFIG_SETTINGS_KEY MACROPAD_CONFIG_SETTINGS_ROOT "/leds"

static macropad_config_t stored_config;
static bool stored_config_loaded;

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

static int macropad_config_settings_set(const char *name, size_t len_rd,
				settings_read_cb read_cb, void *cb_arg)
{
	macropad_config_t loaded_config;
	int rc;

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

	rc = settings_subsys_init();
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_WRN("settings_subsys_init failed: %d, continuing with default LED config", rc);
		return 0;
	}

	rc = settings_load_subtree(MACROPAD_CONFIG_SETTINGS_ROOT);
	if (rc != 0) {
		LOG_WRN("settings_load_subtree failed: %d, using default LED config", rc);
		return 0;
	}

	if (stored_config_loaded) {
		LOG_INF("Loaded persisted macropad LED config");
	} else {
		LOG_INF("No persisted macropad LED config found");
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
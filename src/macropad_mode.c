#include "macropad_mode.h"

#include <stddef.h>

#include <zephyr/sys/util.h>

static const struct macropad_mode_descriptor mode_descriptors[] = {
	{
		.mode = MACROPAD_MODE_DESKTOP_DONGLE,
		.transport = MACROPAD_TRANSPORT_ESB,
		.endpoint = MACROPAD_ENDPOINT_DESKTOP,
		.display_name = "Desktop Dongle",
		.short_name = "DONGLE",
	},
	{
		.mode = MACROPAD_MODE_DESKTOP_BLE,
		.transport = MACROPAD_TRANSPORT_BLE,
		.endpoint = MACROPAD_ENDPOINT_DESKTOP,
		.display_name = "Desktop BLE",
		.short_name = "DESK BLE",
	},
	{
		.mode = MACROPAD_MODE_KINDLE_BLE,
		.transport = MACROPAD_TRANSPORT_BLE,
		.endpoint = MACROPAD_ENDPOINT_KINDLE,
		.display_name = "Kindle BLE",
		.short_name = "KINDLE",
	},
};

_Static_assert(MACROPAD_MODE_DESKTOP_DONGLE == 0,
	"Desktop Dongle must retain persisted value 0");
_Static_assert(MACROPAD_MODE_KINDLE_BLE == 1,
	"Legacy persisted BLE value 1 must resolve to Kindle BLE");
_Static_assert(MACROPAD_MODE_DESKTOP_BLE == 2,
	"Desktop BLE must use a new persisted value");

const struct macropad_mode_descriptor *
macropad_mode_descriptor(enum macropad_operating_mode mode)
{
	for (size_t index = 0U; index < ARRAY_SIZE(mode_descriptors); ++index) {
		if (mode_descriptors[index].mode == mode) {
			return &mode_descriptors[index];
		}
	}

	return &mode_descriptors[0];
}

bool macropad_mode_valid(enum macropad_operating_mode mode)
{
	for (size_t index = 0U; index < ARRAY_SIZE(mode_descriptors); ++index) {
		if (mode_descriptors[index].mode == mode) {
			return true;
		}
	}

	return false;
}

const char *macropad_mode_display_name(enum macropad_operating_mode mode)
{
	return macropad_mode_descriptor(mode)->display_name;
}

const char *macropad_mode_short_name(enum macropad_operating_mode mode)
{
	return macropad_mode_descriptor(mode)->short_name;
}

enum macropad_transport macropad_mode_transport(enum macropad_operating_mode mode)
{
	return macropad_mode_descriptor(mode)->transport;
}

enum macropad_endpoint macropad_mode_endpoint(enum macropad_operating_mode mode)
{
	return macropad_mode_descriptor(mode)->endpoint;
}

bool macropad_mode_is_ble(enum macropad_operating_mode mode)
{
	return macropad_mode_transport(mode) == MACROPAD_TRANSPORT_BLE;
}

bool macropad_mode_is_desktop(enum macropad_operating_mode mode)
{
	return macropad_mode_endpoint(mode) == MACROPAD_ENDPOINT_DESKTOP;
}

bool macropad_mode_has_pairing(enum macropad_operating_mode mode)
{
	return (mode == MACROPAD_MODE_KINDLE_BLE) ||
		(mode == MACROPAD_MODE_DESKTOP_BLE);
}

size_t macropad_mode_count(void)
{
	return ARRAY_SIZE(mode_descriptors);
}

enum macropad_operating_mode macropad_mode_at(size_t index)
{
	if (index >= ARRAY_SIZE(mode_descriptors)) {
		return mode_descriptors[0].mode;
	}

	return mode_descriptors[index].mode;
}

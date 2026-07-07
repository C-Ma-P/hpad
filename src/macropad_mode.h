#ifndef HPADV2_MACROPAD_MODE_H_
#define HPADV2_MACROPAD_MODE_H_

#include <stdbool.h>
#include <stddef.h>

#include "macropad_config.h"

enum macropad_transport {
	MACROPAD_TRANSPORT_ESB = 0,
	MACROPAD_TRANSPORT_BLE = 1,
};

enum macropad_endpoint {
	MACROPAD_ENDPOINT_DESKTOP = 0,
	MACROPAD_ENDPOINT_KINDLE = 1,
};

enum macropad_ble_link_state {
	MACROPAD_BLE_LINK_STATE_INACTIVE = 0,
	MACROPAD_BLE_LINK_STATE_STARTING,
	MACROPAD_BLE_LINK_STATE_ADVERTISING,
	MACROPAD_BLE_LINK_STATE_CONNECTED,
	MACROPAD_BLE_LINK_STATE_SECURITY_FAILED,
	MACROPAD_BLE_LINK_STATE_STOPPING,
	MACROPAD_BLE_LINK_STATE_ERROR,
};

struct macropad_mode_descriptor {
	enum macropad_operating_mode mode;
	enum macropad_transport transport;
	enum macropad_endpoint endpoint;
	const char *display_name;
	const char *short_name;
};

const struct macropad_mode_descriptor *
macropad_mode_descriptor(enum macropad_operating_mode mode);
bool macropad_mode_valid(enum macropad_operating_mode mode);
const char *macropad_mode_display_name(enum macropad_operating_mode mode);
const char *macropad_mode_short_name(enum macropad_operating_mode mode);
enum macropad_transport macropad_mode_transport(enum macropad_operating_mode mode);
enum macropad_endpoint macropad_mode_endpoint(enum macropad_operating_mode mode);
bool macropad_mode_is_ble(enum macropad_operating_mode mode);
bool macropad_mode_is_desktop(enum macropad_operating_mode mode);
bool macropad_mode_has_pairing(enum macropad_operating_mode mode);
size_t macropad_mode_count(void);
enum macropad_operating_mode macropad_mode_at(size_t index);

#endif

#include "desktop_ble_gatt.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "desktop_ble_protocol.h"

LOG_MODULE_REGISTER(desktop_ble_gatt, LOG_LEVEL_INF);

#define DESKTOP_BLE_DEVICE_NAME "HPAD Desktop"
#define DESKTOP_BLE_DEVICE_NAME_LEN (sizeof(DESKTOP_BLE_DEVICE_NAME) - 1)

static struct bt_uuid_128 desktop_service_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_SERVICE_UUID_VAL);
static struct bt_uuid_128 desktop_protocol_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_PROTOCOL_UUID_VAL);
static struct bt_uuid_128 desktop_input_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_INPUT_UUID_VAL);
static struct bt_uuid_128 desktop_config_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_CONFIG_UUID_VAL);

static struct bt_conn *active_conn;
static atomic_t desktop_state = ATOMIC_INIT(DESKTOP_BLE_GATT_STATE_INACTIVE);
static desktop_ble_gatt_config_handler_t config_handler;
static bool desktop_active;
static bool advertising;
static bool service_registered;
static bool input_notify_enabled;
static uint8_t last_report[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE];

static ssize_t read_protocol(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset);
static ssize_t write_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset,
			    uint8_t flags);
static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

static struct bt_gatt_attr desktop_attrs[] = {
	BT_GATT_PRIMARY_SERVICE(&desktop_service_uuid),
	BT_GATT_CHARACTERISTIC(&desktop_protocol_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_protocol, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&desktop_input_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&desktop_config_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_config, NULL),
};

static struct bt_gatt_service desktop_service = BT_GATT_SERVICE(desktop_attrs);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, HPAD_DESKTOP_BLE_SERVICE_UUID_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DESKTOP_BLE_DEVICE_NAME,
		DESKTOP_BLE_DEVICE_NAME_LEN),
};

static void state_set(enum desktop_ble_gatt_state state)
{
	atomic_set(&desktop_state, (atomic_val_t)state);
}

static ssize_t read_protocol(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	uint8_t payload[HPAD_DESKTOP_BLE_PROTOCOL_SIZE];

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	hpad_desktop_ble_encode_protocol(payload);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, payload, sizeof(payload));
}

static ssize_t write_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset,
			    uint8_t flags)
{
	macropad_config_t config;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (!desktop_active || (offset != 0U)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!hpad_desktop_ble_decode_config(buf, len, &config)) {
		LOG_WRN("Rejected malformed desktop BLE config write len=%u", len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (config_handler != NULL) {
		config_handler(&config);
	}

	return len;
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	input_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static int advertising_start(void)
{
	const struct bt_le_adv_param *adv_param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
			BT_GAP_ADV_FAST_INT_MIN_2,
			BT_GAP_ADV_FAST_INT_MAX_2,
			NULL);
	int rc;

	if (!desktop_active || !bt_is_ready()) {
		state_set(DESKTOP_BLE_GATT_STATE_ERROR);
		return -EAGAIN;
	}

	rc = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc == -EALREADY) {
		advertising = true;
		state_set(DESKTOP_BLE_GATT_STATE_ADVERTISING);
		return 0;
	}
	if (rc != 0) {
		LOG_WRN("Desktop BLE advertising start failed: %d", rc);
		state_set(DESKTOP_BLE_GATT_STATE_ERROR);
		return rc;
	}

	advertising = true;
	state_set(DESKTOP_BLE_GATT_STATE_ADVERTISING);
	LOG_INF("Desktop BLE GATT advertising started");
	return 0;
}

static void advertising_stop(void)
{
	int rc;

	if (!bt_is_ready()) {
		advertising = false;
		return;
	}

	rc = bt_le_adv_stop();
	if ((rc != 0) && (rc != -EALREADY) && (rc != -EINVAL)) {
		LOG_WRN("Desktop BLE advertising stop failed: %d", rc);
	}
	advertising = false;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (!desktop_active) {
		return;
	}
	if (err != 0U) {
		LOG_WRN("Desktop BLE connection failed: 0x%02x", err);
		state_set(DESKTOP_BLE_GATT_STATE_ERROR);
		(void)advertising_start();
		return;
	}
	if (active_conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	active_conn = bt_conn_ref(conn);
	advertising = false;
	memset(last_report, 0, sizeof(last_report));
	state_set(DESKTOP_BLE_GATT_STATE_CONNECTED);
	LOG_INF("Desktop BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (active_conn == conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
		input_notify_enabled = false;
		memset(last_report, 0, sizeof(last_report));
		LOG_INF("Desktop BLE disconnected reason=0x%02x", reason);
	}

	if (desktop_active) {
		(void)advertising_start();
	} else if (active_conn == NULL) {
		state_set(DESKTOP_BLE_GATT_STATE_INACTIVE);
	}
}

BT_CONN_CB_DEFINE(desktop_ble_gatt_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

void desktop_ble_gatt_set_config_handler(desktop_ble_gatt_config_handler_t handler)
{
	config_handler = handler;
}

int desktop_ble_gatt_start(void)
{
	int rc;

	state_set(DESKTOP_BLE_GATT_STATE_STARTING);
	desktop_active = true;
	input_notify_enabled = false;
	memset(last_report, 0, sizeof(last_report));

	if (!bt_is_ready()) {
		rc = bt_enable(NULL);
		if (rc != 0) {
			desktop_active = false;
			state_set(DESKTOP_BLE_GATT_STATE_ERROR);
			LOG_ERR("Bluetooth init failed: %d", rc);
			return rc;
		}

		if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
			rc = settings_load_subtree("bt");
			if (rc != 0) {
				LOG_WRN("Bluetooth settings load failed: %d", rc);
			}
		}
	}

	if (!service_registered) {
		rc = bt_gatt_service_register(&desktop_service);
		if (rc != 0) {
			desktop_active = false;
			state_set(DESKTOP_BLE_GATT_STATE_ERROR);
			LOG_ERR("Desktop BLE service register failed: %d", rc);
			return rc;
		}
		service_registered = true;
	}

	rc = advertising_start();
	if (rc != 0) {
		desktop_active = false;
		if (service_registered) {
			(void)bt_gatt_service_unregister(&desktop_service);
			service_registered = false;
		}
		if (bt_is_ready()) {
			(void)bt_disable();
		}
		return rc;
	}

	LOG_INF("Desktop BLE GATT ready");
	return 0;
}

int desktop_ble_gatt_stop(void)
{
	int rc;

	state_set(DESKTOP_BLE_GATT_STATE_STOPPING);
	desktop_active = false;
	advertising_stop();

	if (active_conn != NULL) {
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	input_notify_enabled = false;
	memset(last_report, 0, sizeof(last_report));

	if (service_registered) {
		rc = bt_gatt_service_unregister(&desktop_service);
		if (rc != 0) {
			LOG_WRN("Desktop BLE service unregister failed: %d", rc);
			state_set(DESKTOP_BLE_GATT_STATE_ERROR);
			return rc;
		}
		service_registered = false;
	}

	if (!bt_is_ready()) {
		state_set(DESKTOP_BLE_GATT_STATE_INACTIVE);
		return 0;
	}

	rc = bt_disable();
	if (rc != 0) {
		LOG_WRN("Bluetooth disable failed: %d", rc);
		state_set(DESKTOP_BLE_GATT_STATE_ERROR);
		return rc;
	}

	state_set(DESKTOP_BLE_GATT_STATE_INACTIVE);
	LOG_INF("Desktop BLE stopped");
	return 0;
}

int desktop_ble_gatt_send_report(const macropad_report_t *report)
{
	uint8_t payload[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE];
	int rc;

	if (!desktop_active) {
		return -EAGAIN;
	}
	if (active_conn == NULL) {
		return -ENOTCONN;
	}
	if (!input_notify_enabled) {
		return -EAGAIN;
	}
	if (report == NULL) {
		return -EINVAL;
	}

	hpad_desktop_ble_encode_input_report(report, payload);
	if (memcmp(last_report, payload, sizeof(payload)) == 0) {
		return 0;
	}

	rc = bt_gatt_notify(active_conn, &desktop_attrs[4], payload, sizeof(payload));
	if (rc != 0) {
		return rc;
	}

	memcpy(last_report, payload, sizeof(last_report));
	return 0;
}

bool desktop_ble_gatt_is_connected(void)
{
	return active_conn != NULL;
}

enum desktop_ble_gatt_state desktop_ble_gatt_get_state(void)
{
	return (enum desktop_ble_gatt_state)atomic_get(&desktop_state);
}

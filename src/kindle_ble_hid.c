#include "kindle_ble_hid.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(kindle_ble_hid, LOG_LEVEL_INF);

#define KINDLE_BLE_HID_DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define KINDLE_BLE_HID_DEVICE_NAME_LEN (sizeof(KINDLE_BLE_HID_DEVICE_NAME) - 1)
#define BASE_USB_HID_SPEC_VERSION 0x0101
#define INPUT_REP_KEYS_IDX 0
#define INPUT_REP_KEYS_REF_ID 0
#define INPUT_REP_KEYS_LEN 8
#define HID_KEY_ARRAY_OFFSET 2U
#define HID_KEY_ARRAY_LEN 6U
#define KEY_1_MASK BIT(0)
#define KEY_2_MASK BIT(1)
#define KEY_3_MASK BIT(2)
#define KEY_4_MASK BIT(3)
#define KEY_5_MASK BIT(4)
#define KEY_6_MASK BIT(5)
#define HID_USAGE_KEYBOARD_ESCAPE 0x29
#define HID_USAGE_KEYBOARD_HOME 0x4A
#define HID_USAGE_KEYBOARD_PAGE_UP 0x4B
#define HID_USAGE_KEYBOARD_PAGE_DOWN 0x4E

BT_HIDS_DEF(hids_obj, INPUT_REP_KEYS_LEN);

struct kindle_ble_hid_key_mapping {
	uint8_t key_mask;
	uint8_t usage;
};

static const struct kindle_ble_hid_key_mapping kindle_key_mappings[] = {
	{ .key_mask = KEY_1_MASK, .usage = HID_USAGE_KEYBOARD_HOME },
	{ .key_mask = KEY_2_MASK, .usage = HID_USAGE_KEYBOARD_ESCAPE },
	{ .key_mask = KEY_3_MASK, .usage = KINDLE_BLE_HID_USAGE_KEYBOARD_UP_ARROW },
	{ .key_mask = KEY_4_MASK, .usage = HID_USAGE_KEYBOARD_PAGE_DOWN },
	{ .key_mask = KEY_5_MASK, .usage = KINDLE_BLE_HID_USAGE_KEYBOARD_DOWN_ARROW },
	{ .key_mask = KEY_6_MASK, .usage = HID_USAGE_KEYBOARD_PAGE_UP },
};

static struct bt_conn *active_conn;
static bool hids_initialized;
static bool advertising;
static bool ble_active;
static bool protocol_boot;
static uint8_t current_matrix_keys;
static uint8_t last_report[INPUT_REP_KEYS_LEN];
static atomic_t ble_state = ATOMIC_INIT(KINDLE_BLE_HID_STATE_INACTIVE);

static void state_set(enum kindle_ble_hid_state state)
{
	atomic_set(&ble_state, (atomic_val_t)state);
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		(CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		(CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, KINDLE_BLE_HID_DEVICE_NAME, KINDLE_BLE_HID_DEVICE_NAME_LEN),
};

static int advertising_start(void)
{
	const struct bt_le_adv_param *adv_param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
			BT_GAP_ADV_FAST_INT_MIN_2,
			BT_GAP_ADV_FAST_INT_MAX_2,
			NULL);
	int rc;

	if (!ble_active || !bt_is_ready()) {
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return -EAGAIN;
	}

	rc = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc == -EALREADY) {
		advertising = true;
		state_set(KINDLE_BLE_HID_STATE_ADVERTISING);
		return 0;
	}
	if (rc != 0) {
		LOG_WRN("BLE advertising start failed: %d", rc);
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return rc;
	}

	advertising = true;
	state_set(KINDLE_BLE_HID_STATE_ADVERTISING);
	LOG_INF("BLE HID advertising started");
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
		LOG_WRN("BLE advertising stop failed: %d", rc);
	}
	advertising = false;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	int rc;

	if (!ble_active) {
		return;
	}

	if (err != 0U) {
		LOG_WRN("BLE connection failed: 0x%02x", err);
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		(void)advertising_start();
		return;
	}

	rc = bt_hids_connected(&hids_obj, conn);
	if (rc != 0) {
		LOG_WRN("Failed to notify HIDS about connection: %d", rc);
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	if (active_conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	active_conn = bt_conn_ref(conn);
	protocol_boot = false;
	advertising = false;
	state_set(KINDLE_BLE_HID_STATE_CONNECTED);
	LOG_INF("BLE HID connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int rc;

	rc = bt_hids_disconnected(&hids_obj, conn);
	if (rc != 0) {
		LOG_WRN("Failed to notify HIDS about disconnection: %d", rc);
	}

	if (active_conn == conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	memset(last_report, 0, sizeof(last_report));
	current_matrix_keys = 0U;
	LOG_INF("BLE HID disconnected reason=0x%02x", reason);

	if (ble_active) {
		(void)advertising_start();
	} else {
		state_set(KINDLE_BLE_HID_STATE_INACTIVE);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if (err != 0) {
		LOG_WRN("BLE security failed level=%u err=%d", level, err);
		if (ble_active && (conn == active_conn)) {
			state_set(KINDLE_BLE_HID_STATE_SECURITY_FAILED);
		}
		return;
	}

	if (ble_active && (conn == active_conn)) {
		state_set(KINDLE_BLE_HID_STATE_CONNECTED);
	}
	LOG_INF("BLE security changed level=%u", level);
}

BT_CONN_CB_DEFINE(kindle_ble_hid_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	if (conn != active_conn) {
		return;
	}

	protocol_boot = (evt == BT_HIDS_PM_EVT_BOOT_MODE_ENTERED);
}

static int hids_init_once(void)
{
	static const uint8_t report_map[] = {
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x06,       /* Usage (Keyboard) */
		0xA1, 0x01,       /* Collection (Application) */
		0x05, 0x07,       /* Usage Page (Key Codes) */
		0x19, 0xE0,       /* Usage Minimum (224) */
		0x29, 0xE7,       /* Usage Maximum (231) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x75, 0x01,       /* Report Size (1) */
		0x95, 0x08,       /* Report Count (8) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x08,       /* Report Size (8) */
		0x81, 0x01,       /* Input (Constant) */
		0x95, 0x06,       /* Report Count (6) */
		0x75, 0x08,       /* Report Size (8) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x65,       /* Logical Maximum (101) */
		0x05, 0x07,       /* Usage Page (Key Codes) */
		0x19, 0x00,       /* Usage Minimum (0) */
		0x29, 0x65,       /* Usage Maximum (101) */
		0x81, 0x00,       /* Input (Data, Array) */
		0xC0,             /* End Collection */
	};
	struct bt_hids_init_param hids_init = { 0 };
	struct bt_hids_inp_rep *input_report;
	int rc;

	if (hids_initialized) {
		return 0;
	}

	hids_init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init.info.b_country_code = 0x00;
	hids_init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;
	hids_init.rep_map.data = report_map;
	hids_init.rep_map.size = sizeof(report_map);
	hids_init.is_kb = true;
	hids_init.pm_evt_handler = hids_pm_evt_handler;

	input_report = &hids_init.inp_rep_group_init.reports[INPUT_REP_KEYS_IDX];
	input_report->id = INPUT_REP_KEYS_REF_ID;
	input_report->size = INPUT_REP_KEYS_LEN;
	hids_init.inp_rep_group_init.cnt++;

	rc = bt_hids_init(&hids_obj, &hids_init);
	if (rc != 0) {
		LOG_ERR("HIDS init failed: %d", rc);
		return rc;
	}

	hids_initialized = true;
	return 0;
}

static bool report_contains_usage(const uint8_t report[INPUT_REP_KEYS_LEN], uint8_t usage)
{
	for (size_t index = 0U; index < HID_KEY_ARRAY_LEN; ++index) {
		if (report[HID_KEY_ARRAY_OFFSET + index] == usage) {
			return true;
		}
	}

	return false;
}

static int report_append_usage(uint8_t report[INPUT_REP_KEYS_LEN], uint8_t usage)
{
	if (usage == 0U) {
		return 0;
	}

	if (report_contains_usage(report, usage)) {
		return 0;
	}

	for (size_t index = 0U; index < HID_KEY_ARRAY_LEN; ++index) {
		size_t report_index = HID_KEY_ARRAY_OFFSET + index;

		if (report[report_index] == 0U) {
			report[report_index] = usage;
			return 0;
		}
	}

	return -ENOMEM;
}

static int build_report_from_matrix_keys(uint8_t keys, uint8_t report[INPUT_REP_KEYS_LEN])
{
	memset(report, 0, INPUT_REP_KEYS_LEN);

	for (size_t index = 0U; index < ARRAY_SIZE(kindle_key_mappings); ++index) {
		if ((keys & kindle_key_mappings[index].key_mask) == 0U) {
			continue;
		}

		int rc = report_append_usage(report, kindle_key_mappings[index].usage);

		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int kindle_ble_hid_send_report(const uint8_t report[INPUT_REP_KEYS_LEN], bool force)
{
	int rc;

	if (!ble_active) {
		return -EAGAIN;
	}
	if (active_conn == NULL) {
		return -ENOTCONN;
	}

	if (!force && (memcmp(last_report, report, sizeof(last_report)) == 0)) {
		return 0;
	}

	if (protocol_boot) {
		rc = bt_hids_boot_kb_inp_rep_send(&hids_obj, active_conn,
			report, INPUT_REP_KEYS_LEN, NULL);
	} else {
		rc = bt_hids_inp_rep_send(&hids_obj, active_conn,
			INPUT_REP_KEYS_IDX, report, INPUT_REP_KEYS_LEN, NULL);
	}
	if (rc != 0) {
		return rc;
	}

	memcpy(last_report, report, sizeof(last_report));
	return 0;
}

int kindle_ble_hid_start(void)
{
	int rc;

	state_set(KINDLE_BLE_HID_STATE_STARTING);

	rc = hids_init_once();
	if (rc != 0) {
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return rc;
	}

	ble_active = true;
	current_matrix_keys = 0U;
	memset(last_report, 0, sizeof(last_report));

	if (!bt_is_ready()) {
		rc = bt_enable(NULL);
		if (rc != 0) {
			ble_active = false;
			state_set(KINDLE_BLE_HID_STATE_ERROR);
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

	rc = advertising_start();
	if (rc != 0) {
		ble_active = false;
		return rc;
	}

	LOG_INF("BLE HID ready");
	return 0;
}

int kindle_ble_hid_stop(void)
{
	int rc;

	state_set(KINDLE_BLE_HID_STATE_STOPPING);
	ble_active = false;
	advertising_stop();

	if (active_conn != NULL) {
		(void)bt_hids_disconnected(&hids_obj, active_conn);
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	memset(last_report, 0, sizeof(last_report));
	current_matrix_keys = 0U;
	protocol_boot = false;

	if (!bt_is_ready()) {
		state_set(KINDLE_BLE_HID_STATE_INACTIVE);
		return 0;
	}

	if (hids_initialized) {
		rc = bt_hids_uninit(&hids_obj);
		if (rc != 0) {
			LOG_WRN("HIDS uninit failed: %d", rc);
			state_set(KINDLE_BLE_HID_STATE_ERROR);
			return rc;
		}
		hids_initialized = false;
	}

	rc = bt_disable();
	if (rc != 0) {
		LOG_WRN("Bluetooth disable failed: %d", rc);
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return rc;
	}

	state_set(KINDLE_BLE_HID_STATE_INACTIVE);
	LOG_INF("BLE HID stopped");
	return 0;
}

int kindle_ble_hid_forget_pairing(void)
{
	bool waiting_for_disconnect = false;
	int rc;

	if (!ble_active || !bt_is_ready()) {
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return -EAGAIN;
	}

	advertising_stop();

	if (active_conn != NULL) {
		int disconnect_rc;

		(void)bt_hids_disconnected(&hids_obj, active_conn);
		disconnect_rc = bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if ((disconnect_rc != 0) && (disconnect_rc != -ENOTCONN)) {
			LOG_WRN("BLE disconnect before unpair failed: %d", disconnect_rc);
		}
		waiting_for_disconnect = (disconnect_rc == 0);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	memset(last_report, 0, sizeof(last_report));
	current_matrix_keys = 0U;
	protocol_boot = false;

	rc = bt_unpair(BT_ID_DEFAULT, NULL);
	if (rc != 0) {
		LOG_WRN("BLE unpair failed: %d", rc);
		state_set(KINDLE_BLE_HID_STATE_ERROR);
		return rc;
	}

	if (waiting_for_disconnect) {
		state_set(KINDLE_BLE_HID_STATE_STARTING);
		LOG_INF("BLE pairing data cleared; waiting for disconnect");
		return 0;
	}

	rc = advertising_start();
	if (rc != 0) {
		return rc;
	}

	LOG_INF("BLE pairing data cleared");
	return 0;
}

int kindle_ble_hid_send_key_state(uint8_t keys)
{
	uint8_t report[INPUT_REP_KEYS_LEN] = { 0 };
	int rc;

	rc = build_report_from_matrix_keys(keys, report);
	if (rc != 0) {
		return rc;
	}

	rc = kindle_ble_hid_send_report(report, false);
	if (rc == 0) {
		current_matrix_keys = keys;
	}

	return rc;
}

int kindle_ble_hid_send_key_tap(uint8_t usage)
{
	uint8_t report[INPUT_REP_KEYS_LEN] = { 0 };
	uint8_t restored_report[INPUT_REP_KEYS_LEN] = { 0 };
	int rc;

	rc = build_report_from_matrix_keys(current_matrix_keys, report);
	if (rc != 0) {
		return rc;
	}

	memcpy(restored_report, report, sizeof(restored_report));
	rc = report_append_usage(report, usage);
	if (rc != 0) {
		return rc;
	}

	rc = kindle_ble_hid_send_report(report, true);
	if (rc != 0) {
		return rc;
	}

	return kindle_ble_hid_send_report(restored_report, true);
}

bool kindle_ble_hid_is_connected(void)
{
	return active_conn != NULL;
}

enum kindle_ble_hid_state kindle_ble_hid_get_state(void)
{
	return (enum kindle_ble_hid_state)atomic_get(&ble_state);
}

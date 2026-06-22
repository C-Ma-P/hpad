#include "ble_hid.h"

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
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ble_hid, LOG_LEVEL_INF);

#define BLE_HID_DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define BLE_HID_DEVICE_NAME_LEN (sizeof(BLE_HID_DEVICE_NAME) - 1)
#define BASE_USB_HID_SPEC_VERSION 0x0101
#define INPUT_REP_KEYS_IDX 0
#define INPUT_REP_KEYS_REF_ID 0
#define INPUT_REP_KEYS_LEN 8
#define KEY_4_MASK BIT(3)
#define KEY_6_MASK BIT(5)
#define HID_USAGE_KEYBOARD_PAGE_UP 0x4B
#define HID_USAGE_KEYBOARD_PAGE_DOWN 0x4E

BT_HIDS_DEF(hids_obj, INPUT_REP_KEYS_LEN);

static struct bt_conn *active_conn;
static bool hids_initialized;
static bool advertising;
static bool ble_active;
static bool protocol_boot;
static uint8_t last_report[INPUT_REP_KEYS_LEN];

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		(CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		(CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, BLE_HID_DEVICE_NAME, BLE_HID_DEVICE_NAME_LEN),
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
		return -EAGAIN;
	}

	rc = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc == -EALREADY) {
		advertising = true;
		return 0;
	}
	if (rc != 0) {
		LOG_WRN("BLE advertising start failed: %d", rc);
		return rc;
	}

	advertising = true;
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
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	if (err != 0U) {
		LOG_WRN("BLE connection failed: 0x%02x", err);
		(void)advertising_start();
		return;
	}

	rc = bt_hids_connected(&hids_obj, conn);
	if (rc != 0) {
		LOG_WRN("Failed to notify HIDS about connection: %d", rc);
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
	LOG_INF("BLE HID disconnected reason=0x%02x", reason);

	if (ble_active) {
		(void)advertising_start();
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if (err != 0) {
		LOG_WRN("BLE security failed level=%u err=%d", level, err);
		return;
	}

	LOG_INF("BLE security changed level=%u", level);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
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

int ble_hid_start(void)
{
	int rc;

	rc = hids_init_once();
	if (rc != 0) {
		return rc;
	}

	ble_active = true;
	memset(last_report, 0, sizeof(last_report));

	if (!bt_is_ready()) {
		rc = bt_enable(NULL);
		if (rc != 0) {
			ble_active = false;
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

int ble_hid_stop(void)
{
	int rc;

	ble_active = false;
	advertising_stop();

	if (active_conn != NULL) {
		(void)bt_hids_disconnected(&hids_obj, active_conn);
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	memset(last_report, 0, sizeof(last_report));
	protocol_boot = false;

	if (!bt_is_ready()) {
		return 0;
	}

	rc = bt_disable();
	if (rc != 0) {
		LOG_WRN("Bluetooth disable failed: %d", rc);
		return rc;
	}

	LOG_INF("BLE HID stopped");
	return 0;
}

int ble_hid_send_key_state(uint8_t keys)
{
	uint8_t report[INPUT_REP_KEYS_LEN] = { 0 };
	int rc;

	if (!ble_active) {
		return -EAGAIN;
	}
	if (active_conn == NULL) {
		return -ENOTCONN;
	}

	if ((keys & KEY_4_MASK) != 0U) {
		report[2] = HID_USAGE_KEYBOARD_PAGE_DOWN;
	}
	if ((keys & KEY_6_MASK) != 0U) {
		report[3] = HID_USAGE_KEYBOARD_PAGE_UP;
	}

	if (memcmp(last_report, report, sizeof(report)) == 0) {
		return 0;
	}

	if (protocol_boot) {
		rc = bt_hids_boot_kb_inp_rep_send(&hids_obj, active_conn,
			report, sizeof(report), NULL);
	} else {
		rc = bt_hids_inp_rep_send(&hids_obj, active_conn,
			INPUT_REP_KEYS_IDX, report, sizeof(report), NULL);
	}
	if (rc != 0) {
		return rc;
	}

	memcpy(last_report, report, sizeof(last_report));
	return 0;
}

bool ble_hid_is_connected(void)
{
	return active_conn != NULL;
}

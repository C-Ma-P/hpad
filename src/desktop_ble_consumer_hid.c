#include "desktop_ble_consumer_hid.h"

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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "consumer_action_engine.h"

LOG_MODULE_REGISTER(desktop_ble_consumer_hid, LOG_LEVEL_INF);

#define DESKTOP_BLE_CONSUMER_HID_DEVICE_NAME "HPAD Desktop"
#define DESKTOP_BLE_CONSUMER_HID_DEVICE_NAME_LEN \
	(sizeof(DESKTOP_BLE_CONSUMER_HID_DEVICE_NAME) - 1)
#define BASE_USB_HID_SPEC_VERSION 0x0101
#define INPUT_REP_CONSUMER_IDX 0
#define INPUT_REP_CONSUMER_REF_ID 1
#define INPUT_REP_CONSUMER_LEN sizeof(uint16_t)
#define HID_USAGE_PAGE_CONSUMER 0x0C
#define HID_USAGE_CONSUMER_CONTROL 0x01
#define HID_USAGE_MUTE 0x00E2U
#define HID_USAGE_VOLUME_INCREMENT 0x00E9U
#define HID_USAGE_VOLUME_DECREMENT 0x00EAU
#define HID_USAGE_PLAY_PAUSE 0x00CDU
#define HID_USAGE_SCAN_NEXT_TRACK 0x00B5U
#define HID_USAGE_SCAN_PREVIOUS_TRACK 0x00B6U
#define HID_USAGE_STOP 0x00B7U
#define ADV_RESTART_DELAY_MS 100

BT_HIDS_DEF(desktop_consumer_hids_obj, INPUT_REP_CONSUMER_LEN);

static struct bt_conn *active_conn;
static struct k_work_delayable advertising_start_work;
static bool hids_initialized;
static bool advertising_start_work_initialized;
static bool advertising;
static bool ble_active;
static bool input_notify_enabled;
static uint16_t last_usage;
static atomic_t ble_state = ATOMIC_INIT(DESKTOP_BLE_CONSUMER_HID_STATE_INACTIVE);

static void advertising_start_work_handler(struct k_work *work);

static void state_set(enum desktop_ble_consumer_hid_state state)
{
	atomic_set(&ble_state, (atomic_val_t)state);
}

static void advertising_start_work_init_once(void)
{
	if (advertising_start_work_initialized) {
		return;
	}

	k_work_init_delayable(&advertising_start_work, advertising_start_work_handler);
	advertising_start_work_initialized = true;
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		(CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		(CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DESKTOP_BLE_CONSUMER_HID_DEVICE_NAME,
		DESKTOP_BLE_CONSUMER_HID_DEVICE_NAME_LEN),
};

static uint16_t usage_for_action(uint16_t action_id)
{
	switch (action_id) {
	case CONSUMER_ACTION_VOLUME_UP:
		return HID_USAGE_VOLUME_INCREMENT;
	case CONSUMER_ACTION_VOLUME_DOWN:
		return HID_USAGE_VOLUME_DECREMENT;
	case CONSUMER_ACTION_MUTE:
		return HID_USAGE_MUTE;
	case CONSUMER_ACTION_NEXT_TRACK:
		return HID_USAGE_SCAN_NEXT_TRACK;
	case CONSUMER_ACTION_PREVIOUS_TRACK:
		return HID_USAGE_SCAN_PREVIOUS_TRACK;
	case CONSUMER_ACTION_STOP:
		return HID_USAGE_STOP;
	case CONSUMER_ACTION_PLAY_PAUSE:
		return HID_USAGE_PLAY_PAUSE;
	default:
		return 0U;
	}
}

static int advertising_start(void)
{
	const struct bt_le_adv_param *adv_param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
			BT_GAP_ADV_FAST_INT_MIN_2,
			BT_GAP_ADV_FAST_INT_MAX_2,
			NULL);
	int rc;

	if (!ble_active || !bt_is_ready()) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return -EAGAIN;
	}

	rc = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc == -EALREADY) {
		advertising = true;
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ADVERTISING);
		return 0;
	}
	if (rc != 0) {
		LOG_WRN("Desktop BLE Consumer HID advertising start failed: %d", rc);
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return rc;
	}

	advertising = true;
	state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ADVERTISING);
	LOG_INF("Desktop BLE Consumer HID advertising started");
	return 0;
}

static void advertising_start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (ble_active) {
		(void)advertising_start();
	}
}

static void advertising_restart_schedule(void)
{
	if (!ble_active) {
		return;
	}

	advertising_start_work_init_once();
	(void)k_work_reschedule(&advertising_start_work, K_MSEC(ADV_RESTART_DELAY_MS));
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
		LOG_WRN("Desktop BLE Consumer HID advertising stop failed: %d", rc);
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
		LOG_WRN("Desktop BLE Consumer HID connection failed: 0x%02x", err);
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		(void)advertising_start();
		return;
	}

	rc = bt_hids_connected(&desktop_consumer_hids_obj, conn);
	if (rc != 0) {
		LOG_WRN("Failed to notify desktop Consumer HIDS about connection: %d", rc);
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	if (active_conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	active_conn = bt_conn_ref(conn);
	advertising = false;
	input_notify_enabled = false;
	last_usage = 0U;
	if (advertising_start_work_initialized) {
		(void)k_work_cancel_delayable(&advertising_start_work);
	}
	state_set(DESKTOP_BLE_CONSUMER_HID_STATE_CONNECTED);
	LOG_INF("Desktop BLE Consumer HID connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int rc;

	if (!ble_active && (active_conn != conn)) {
		return;
	}

	if (active_conn == conn) {
		rc = bt_hids_disconnected(&desktop_consumer_hids_obj, conn);
		if (rc != 0) {
			LOG_WRN("Failed to notify desktop Consumer HIDS about disconnection: %d",
				rc);
		}

		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	last_usage = 0U;
	input_notify_enabled = false;
	LOG_INF("Desktop BLE Consumer HID disconnected reason=0x%02x", reason);

	if (ble_active) {
		advertising_restart_schedule();
	} else {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_INACTIVE);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if (err != 0) {
		LOG_WRN("Desktop BLE Consumer HID security failed level=%u err=%d",
			level, err);
		if (ble_active && (conn == active_conn)) {
			state_set(DESKTOP_BLE_CONSUMER_HID_STATE_SECURITY_FAILED);
		}
		return;
	}

	if (ble_active && (conn == active_conn)) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_CONNECTED);
	}
	LOG_INF("Desktop BLE Consumer HID security changed level=%u", level);
}

BT_CONN_CB_DEFINE(desktop_ble_consumer_hid_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void input_report_notify_handler(enum bt_hids_notify_evt evt)
{
	input_notify_enabled = (evt == BT_HIDS_CCCD_EVT_NOTIFY_ENABLED);
	LOG_INF("Desktop BLE Consumer HID input notifications %s",
		input_notify_enabled ? "enabled" : "disabled");
}

static int hids_init_once(void)
{
	static const uint8_t report_map[] = {
		0x05, HID_USAGE_PAGE_CONSUMER,       /* Usage Page (Consumer) */
		0x09, HID_USAGE_CONSUMER_CONTROL,    /* Usage (Consumer Control) */
		0xA1, 0x01,                          /* Collection (Application) */
		0x85, INPUT_REP_CONSUMER_REF_ID,     /* Report ID */
		0x15, 0x00,                          /* Logical Minimum (0) */
		0x26, 0xFF, 0x03,                    /* Logical Maximum (1023) */
		0x19, 0x00,                          /* Usage Minimum (0) */
		0x2A, 0xFF, 0x03,                    /* Usage Maximum (1023) */
		0x75, 0x10,                          /* Report Size (16) */
		0x95, 0x01,                          /* Report Count (1) */
		0x81, 0x00,                          /* Input (Data, Array, Absolute) */
		0xC0,                                /* End Collection */
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

	input_report = &hids_init.inp_rep_group_init.reports[INPUT_REP_CONSUMER_IDX];
	input_report->id = INPUT_REP_CONSUMER_REF_ID;
	input_report->size = INPUT_REP_CONSUMER_LEN;
	input_report->handler = input_report_notify_handler;
	hids_init.inp_rep_group_init.cnt++;

	rc = bt_hids_init(&desktop_consumer_hids_obj, &hids_init);
	if (rc != 0) {
		LOG_ERR("Desktop Consumer HIDS init failed: %d", rc);
		return rc;
	}

	hids_initialized = true;
	return 0;
}

static int send_usage(uint16_t usage)
{
	uint8_t report[INPUT_REP_CONSUMER_LEN];
	int rc;

	if (!ble_active) {
		return -EAGAIN;
	}
	if (active_conn == NULL) {
		return -ENOTCONN;
	}
	if (!input_notify_enabled) {
		return -EAGAIN;
	}
	if (last_usage == usage) {
		return 0;
	}

	sys_put_le16(usage, report);
	rc = bt_hids_inp_rep_send(&desktop_consumer_hids_obj, active_conn,
		INPUT_REP_CONSUMER_IDX, report, sizeof(report), NULL);
	if (rc != 0) {
		return rc;
	}

	last_usage = usage;
	return 0;
}

int desktop_ble_consumer_hid_start(void)
{
	int rc;

	state_set(DESKTOP_BLE_CONSUMER_HID_STATE_STARTING);
	advertising_start_work_init_once();

	rc = hids_init_once();
	if (rc != 0) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return rc;
	}

	ble_active = true;
	input_notify_enabled = false;
	last_usage = 0U;

	if (!bt_is_ready()) {
		rc = bt_enable(NULL);
		if (rc != 0) {
			ble_active = false;
			state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
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

	LOG_INF("Desktop BLE Consumer HID ready");
	return 0;
}

int desktop_ble_consumer_hid_stop(void)
{
	int rc;

	state_set(DESKTOP_BLE_CONSUMER_HID_STATE_STOPPING);
	ble_active = false;
	if (advertising_start_work_initialized) {
		(void)k_work_cancel_delayable(&advertising_start_work);
	}
	advertising_stop();

	if (active_conn != NULL) {
		(void)bt_hids_disconnected(&desktop_consumer_hids_obj, active_conn);
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	last_usage = 0U;

	if (!bt_is_ready()) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_INACTIVE);
		return 0;
	}

	if (hids_initialized) {
		rc = bt_hids_uninit(&desktop_consumer_hids_obj);
		if (rc != 0) {
			LOG_WRN("Desktop Consumer HIDS uninit failed: %d", rc);
			state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
			return rc;
		}
		hids_initialized = false;
	}

	rc = bt_disable();
	if (rc != 0) {
		LOG_WRN("Bluetooth disable failed: %d", rc);
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return rc;
	}

	state_set(DESKTOP_BLE_CONSUMER_HID_STATE_INACTIVE);
	LOG_INF("Desktop BLE Consumer HID stopped");
	return 0;
}

int desktop_ble_consumer_hid_forget_pairing(void)
{
	bool waiting_for_disconnect = false;
	int rc;

	if (!ble_active || !bt_is_ready()) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return -EAGAIN;
	}

	if (advertising_start_work_initialized) {
		(void)k_work_cancel_delayable(&advertising_start_work);
	}
	advertising_stop();

	if (active_conn != NULL) {
		int disconnect_rc;

		(void)bt_hids_disconnected(&desktop_consumer_hids_obj, active_conn);
		disconnect_rc = bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if ((disconnect_rc != 0) && (disconnect_rc != -ENOTCONN)) {
			LOG_WRN("Desktop BLE disconnect before unpair failed: %d",
				disconnect_rc);
		}
		waiting_for_disconnect = (disconnect_rc == 0);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	last_usage = 0U;

	rc = bt_unpair(BT_ID_DEFAULT, NULL);
	if (rc != 0) {
		LOG_WRN("Desktop BLE unpair failed: %d", rc);
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_ERROR);
		return rc;
	}

	if (waiting_for_disconnect) {
		state_set(DESKTOP_BLE_CONSUMER_HID_STATE_STARTING);
		LOG_INF("Desktop BLE pairing data cleared; waiting for disconnect");
		return 0;
	}

	rc = advertising_start();
	if (rc != 0) {
		return rc;
	}

	LOG_INF("Desktop BLE pairing data cleared");
	return 0;
}

int desktop_ble_consumer_hid_trigger_action(uint16_t action_id)
{
	uint16_t usage = usage_for_action(action_id);
	int rc;

	if (usage == 0U) {
		return -ENOTSUP;
	}

	rc = send_usage(usage);
	if (rc != 0) {
		return rc;
	}

	return send_usage(0U);
}

bool desktop_ble_consumer_hid_is_connected(void)
{
	return active_conn != NULL;
}

enum desktop_ble_consumer_hid_state desktop_ble_consumer_hid_get_state(void)
{
	return (enum desktop_ble_consumer_hid_state)atomic_get(&ble_state);
}

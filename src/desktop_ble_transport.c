#include "desktop_ble_transport.h"

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

LOG_MODULE_REGISTER(desktop_ble_transport, LOG_LEVEL_INF);

#define DESKTOP_BLE_DEVICE_NAME "HPAD Desktop"
#define DESKTOP_BLE_DEVICE_NAME_LEN (sizeof(DESKTOP_BLE_DEVICE_NAME) - 1)
#define DESKTOP_BLE_NOTIFY_RETRY_MS 20
#define DESKTOP_BLE_NOTIFY_NOT_READY_RETRY_MS 100
#define DESKTOP_BLE_NOTIFY_MAX_RETRIES 50
#define DESKTOP_BLE_ADV_RETRY_BASE_MS 1000U
#define DESKTOP_BLE_ADV_RETRY_MAX_ATTEMPTS 5U

static struct bt_uuid_128 desktop_service_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_SERVICE_UUID_VAL);
static struct bt_uuid_128 desktop_protocol_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_PROTOCOL_UUID_VAL);
static struct bt_uuid_128 desktop_input_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_INPUT_UUID_VAL);
static struct bt_uuid_128 desktop_config_uuid =
	BT_UUID_INIT_128(HPAD_DESKTOP_BLE_CONFIG_UUID_VAL);

static struct bt_conn *active_conn;
static atomic_t desktop_state = ATOMIC_INIT(DESKTOP_BLE_TRANSPORT_STATE_INACTIVE);
static desktop_ble_transport_config_handler_t config_handler;
static bool desktop_active;
static bool advertising;
static bool service_registered;
static bool input_notify_enabled;
static bool report_flush_work_initialized;
static bool advertising_retry_work_initialized;
static bool pending_report_valid;
static uint8_t pending_report_retries;
static atomic_t advertising_retry_attempt = ATOMIC_INIT(0);
static atomic_t advertising_retry_deadline_ms = ATOMIC_INIT(0);
static atomic_t advertising_retry_pending = ATOMIC_INIT(0);
static macropad_report_t pending_report;
static uint8_t last_report[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE];
static struct k_work_delayable report_flush_work;
static struct k_work_delayable advertising_retry_work;

static ssize_t read_protocol(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset);
static ssize_t write_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset,
			    uint8_t flags);
static int advertising_start(void);
static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void report_flush_work_handler(struct k_work *work);
static void advertising_retry_work_handler(struct k_work *work);

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

static void state_set(enum desktop_ble_transport_state state)
{
	atomic_set(&desktop_state, (atomic_val_t)state);
}

static bool input_notifications_ready(void)
{
	if (active_conn == NULL) {
		return false;
	}
	if (input_notify_enabled) {
		return true;
	}
	if (bt_gatt_is_subscribed(active_conn, &desktop_attrs[4], BT_GATT_CCC_NOTIFY)) {
		input_notify_enabled = true;
		return true;
	}

	return false;
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
	LOG_INF("Desktop BLE input notifications %s",
		input_notify_enabled ? "enabled" : "disabled");
	if (input_notify_enabled && pending_report_valid) {
		(void)k_work_reschedule(&report_flush_work, K_NO_WAIT);
	}
}

static void report_flush_work_init_once(void)
{
	if (report_flush_work_initialized) {
		return;
	}

	k_work_init_delayable(&report_flush_work, report_flush_work_handler);
	report_flush_work_initialized = true;
}

static void advertising_retry_work_init_once(void)
{
	if (advertising_retry_work_initialized) {
		return;
	}

	k_work_init_delayable(&advertising_retry_work, advertising_retry_work_handler);
	advertising_retry_work_initialized = true;
}

static void reset_advertising_retry(void)
{
	atomic_set(&advertising_retry_attempt, 0);
	atomic_set(&advertising_retry_deadline_ms, 0);
	atomic_set(&advertising_retry_pending, 0);
	if (advertising_retry_work_initialized) {
		(void)k_work_cancel_delayable(&advertising_retry_work);
	}
}

static void schedule_advertising_retry(const char *reason)
{
	uint8_t attempt = (uint8_t)atomic_get(&advertising_retry_attempt);
	uint32_t delay_ms;

	if (!desktop_active) {
		return;
	}

	advertising_retry_work_init_once();
	state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
	if (attempt >= DESKTOP_BLE_ADV_RETRY_MAX_ATTEMPTS) {
		atomic_set(&advertising_retry_pending, 0);
		LOG_ERR("Desktop BLE advertising retry exhausted after %u attempts (%s)",
			(unsigned int)attempt, reason);
		return;
	}

	delay_ms = DESKTOP_BLE_ADV_RETRY_BASE_MS << attempt;
	attempt++;
	atomic_set(&advertising_retry_attempt, attempt);
	atomic_set(&advertising_retry_deadline_ms,
		(atomic_val_t)(k_uptime_get_32() + delay_ms));
	atomic_set(&advertising_retry_pending, 1);
	LOG_WRN("Desktop BLE retry %u/%u in %u ms (%s)",
		(unsigned int)attempt,
		(unsigned int)DESKTOP_BLE_ADV_RETRY_MAX_ATTEMPTS,
		(unsigned int)delay_ms, reason);
	(void)k_work_reschedule(&advertising_retry_work, K_MSEC(delay_ms));
}

static void clear_pending_report(void)
{
	pending_report_valid = false;
	pending_report_retries = 0U;
	memset(&pending_report, 0, sizeof(pending_report));
	if (report_flush_work_initialized) {
		(void)k_work_cancel_delayable(&report_flush_work);
	}
}

static void report_flush_work_handler(struct k_work *work)
{
	uint8_t payload[HPAD_DESKTOP_BLE_INPUT_REPORT_SIZE];
	macropad_report_t report;
	bool notify_ready;
	int rc;

	ARG_UNUSED(work);

	if (!desktop_active || (active_conn == NULL) || !pending_report_valid) {
		return;
	}

	notify_ready = input_notifications_ready();
	report = pending_report;
	hpad_desktop_ble_encode_input_report(&report, payload);
	if (memcmp(last_report, payload, sizeof(payload)) == 0) {
		pending_report_valid = false;
		return;
	}

	rc = bt_gatt_notify(active_conn, &desktop_attrs[4], payload, sizeof(payload));
	if (rc == 0) {
		memcpy(last_report, payload, sizeof(last_report));
		pending_report_valid = false;
		pending_report_retries = 0U;
		return;
	}

	if ((rc == -EAGAIN) || (rc == -ENOMEM) || (rc == -EINVAL)) {
		uint32_t delay_ms = notify_ready ? DESKTOP_BLE_NOTIFY_RETRY_MS :
			DESKTOP_BLE_NOTIFY_NOT_READY_RETRY_MS;

		if (pending_report_retries < DESKTOP_BLE_NOTIFY_MAX_RETRIES) {
			pending_report_retries++;
			(void)k_work_reschedule(&report_flush_work, K_MSEC(delay_ms));
			return;
		}
	}

	LOG_WRN("Desktop BLE input notify failed: %d ready=%u retries=%u",
		rc, notify_ready ? 1U : 0U, pending_report_retries);
	pending_report_valid = false;
	pending_report_retries = 0U;
}

static void advertising_retry_work_handler(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);

	atomic_set(&advertising_retry_pending, 0);
	if (!desktop_active || (active_conn != NULL)) {
		return;
	}

	state_set(DESKTOP_BLE_TRANSPORT_STATE_STARTING);
	rc = advertising_start();
	if (rc != 0) {
		schedule_advertising_retry("advertising start failed");
	}
}

static void restart_advertising_or_retry(const char *reason)
{
	int rc;

	rc = advertising_start();
	if (rc != 0) {
		schedule_advertising_retry(reason);
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

	if (!desktop_active || !bt_is_ready()) {
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
		return -EAGAIN;
	}

	rc = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc == -EALREADY) {
		advertising = true;
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ADVERTISING);
		return 0;
	}
	if (rc != 0) {
		LOG_WRN("Desktop BLE advertising start failed: %d", rc);
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
		return rc;
	}

	advertising = true;
	state_set(DESKTOP_BLE_TRANSPORT_STATE_ADVERTISING);
	LOG_INF("Desktop BLE transport advertising started");
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
		advertising = false;
		schedule_advertising_retry("connection failed");
		return;
	}
	if (active_conn != NULL) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	active_conn = bt_conn_ref(conn);
	advertising = false;
	reset_advertising_retry();
	pending_report_valid = false;
	pending_report_retries = 0U;
	memset(last_report, 0, sizeof(last_report));
	state_set(DESKTOP_BLE_TRANSPORT_STATE_CONNECTED);
	LOG_INF("Desktop BLE transport connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (active_conn == conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
		input_notify_enabled = false;
		clear_pending_report();
		memset(last_report, 0, sizeof(last_report));
		LOG_INF("Desktop BLE transport disconnected reason=0x%02x", reason);
	}

	if (desktop_active) {
		restart_advertising_or_retry("disconnect");
	} else if (active_conn == NULL) {
		state_set(DESKTOP_BLE_TRANSPORT_STATE_INACTIVE);
	}
}

BT_CONN_CB_DEFINE(desktop_ble_transport_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

void desktop_ble_transport_set_config_handler(desktop_ble_transport_config_handler_t handler)
{
	config_handler = handler;
}

int desktop_ble_transport_start(void)
{
	int rc;

	report_flush_work_init_once();
	advertising_retry_work_init_once();
	reset_advertising_retry();
	state_set(DESKTOP_BLE_TRANSPORT_STATE_STARTING);
	desktop_active = true;
	input_notify_enabled = false;
	pending_report_valid = false;
	pending_report_retries = 0U;
	memset(last_report, 0, sizeof(last_report));

	if (!bt_is_ready()) {
		rc = bt_enable(NULL);
		if (rc != 0) {
			desktop_active = false;
			state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
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
			state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
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

	LOG_INF("Desktop BLE transport ready");
	return 0;
}

int desktop_ble_transport_stop(void)
{
	int rc;

	state_set(DESKTOP_BLE_TRANSPORT_STATE_STOPPING);
	desktop_active = false;
	reset_advertising_retry();
	advertising_stop();

	if (active_conn != NULL) {
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	input_notify_enabled = false;
	clear_pending_report();
	pending_report_retries = 0U;
	memset(last_report, 0, sizeof(last_report));

	if (service_registered) {
		rc = bt_gatt_service_unregister(&desktop_service);
		if (rc != 0) {
			LOG_WRN("Desktop BLE service unregister failed: %d", rc);
			state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
			return rc;
		}
		service_registered = false;
	}

	if (!bt_is_ready()) {
		state_set(DESKTOP_BLE_TRANSPORT_STATE_INACTIVE);
		return 0;
	}

	rc = bt_disable();
	if (rc != 0) {
		LOG_WRN("Bluetooth disable failed: %d", rc);
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
		return rc;
	}

	state_set(DESKTOP_BLE_TRANSPORT_STATE_INACTIVE);
	LOG_INF("Desktop BLE transport stopped");
	return 0;
}

int desktop_ble_transport_forget_pairing(void)
{
	bool waiting_for_disconnect = false;
	int rc;

	if (!desktop_active || !bt_is_ready()) {
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
		return -EAGAIN;
	}

	advertising_stop();
	reset_advertising_retry();

	if (active_conn != NULL) {
		int disconnect_rc;

		disconnect_rc = bt_conn_disconnect(active_conn,
			BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if ((disconnect_rc != 0) && (disconnect_rc != -ENOTCONN)) {
			LOG_WRN("Desktop BLE disconnect before unpair failed: %d",
				disconnect_rc);
		}
		waiting_for_disconnect = (disconnect_rc == 0);
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	input_notify_enabled = false;
	clear_pending_report();
	pending_report_retries = 0U;
	memset(last_report, 0, sizeof(last_report));

	rc = bt_unpair(BT_ID_DEFAULT, NULL);
	if (rc != 0) {
		LOG_WRN("Desktop BLE unpair failed: %d", rc);
		state_set(DESKTOP_BLE_TRANSPORT_STATE_ERROR);
		return rc;
	}

	if (waiting_for_disconnect) {
		state_set(DESKTOP_BLE_TRANSPORT_STATE_STARTING);
		LOG_INF("Desktop BLE pairing data cleared; waiting for disconnect");
		return 0;
	}

	rc = advertising_start();
	if (rc != 0) {
		schedule_advertising_retry("pairing reset");
		return rc;
	}

	LOG_INF("Desktop BLE pairing data cleared");
	return 0;
}

int desktop_ble_transport_send_report(const macropad_report_t *report)
{
	if (report == NULL) {
		return -EINVAL;
	}
	if (!desktop_active) {
		return -EAGAIN;
	}

	pending_report = *report;
	pending_report_valid = true;
	pending_report_retries = 0U;
	if (active_conn != NULL) {
		(void)k_work_reschedule(&report_flush_work, K_NO_WAIT);
	}
	return 0;
}

bool desktop_ble_transport_is_connected(void)
{
	return active_conn != NULL;
}

bool desktop_ble_transport_get_retry_status(uint8_t *attempt, uint8_t *seconds_remaining)
{
	int32_t remaining_ms;

	if ((attempt == NULL) || (seconds_remaining == NULL) ||
	    !desktop_active || (atomic_get(&advertising_retry_pending) == 0)) {
		return false;
	}

	*attempt = (uint8_t)atomic_get(&advertising_retry_attempt);
	if (*attempt == 0U) {
		return false;
	}

	remaining_ms = (int32_t)((uint32_t)atomic_get(&advertising_retry_deadline_ms) -
		k_uptime_get_32());
	if (remaining_ms <= 0) {
		*seconds_remaining = 1U;
	} else {
		*seconds_remaining = (uint8_t)((remaining_ms + 999) / 1000);
	}

	return true;
}

enum desktop_ble_transport_state desktop_ble_transport_get_state(void)
{
	return (enum desktop_ble_transport_state)atomic_get(&desktop_state);
}

#include "radio_esb.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if __has_include(<esb.h>)
#include <esb.h>

#define RADIO_ESB_PAYLOAD struct esb_payload
#define RADIO_ESB_EVENT struct esb_evt
#define RADIO_ESB_CONFIG struct esb_config
#define RADIO_ESB_DEFAULT_CONFIG ESB_DEFAULT_CONFIG
#define RADIO_ESB_EVENT_TX_SUCCESS ESB_EVENT_TX_SUCCESS
#define RADIO_ESB_EVENT_TX_FAILED ESB_EVENT_TX_FAILED
#define RADIO_ESB_PROTOCOL_DPL ESB_PROTOCOL_ESB_DPL
#define RADIO_ESB_BITRATE_2MBPS ESB_BITRATE_2MBPS
#define RADIO_ESB_MODE_PTX ESB_MODE_PTX
#define radio_esb_api_flush_rx esb_flush_rx
#define radio_esb_api_flush_tx esb_flush_tx
#define radio_esb_api_init esb_init
#define radio_esb_api_set_base_address_0 esb_set_base_address_0
#define radio_esb_api_set_base_address_1 esb_set_base_address_1
#define radio_esb_api_set_prefixes esb_set_prefixes
#define radio_esb_api_set_rf_channel esb_set_rf_channel
#define radio_esb_api_write_payload esb_write_payload
#elif __has_include(<nrf_esb.h>)
#include <nrf_esb.h>

#define RADIO_ESB_PAYLOAD nrf_esb_payload_t
#define RADIO_ESB_EVENT nrf_esb_evt_t
#define RADIO_ESB_CONFIG nrf_esb_config_t
#define RADIO_ESB_DEFAULT_CONFIG NRF_ESB_DEFAULT_CONFIG
#define RADIO_ESB_EVENT_TX_SUCCESS NRF_ESB_EVENT_TX_SUCCESS
#define RADIO_ESB_EVENT_TX_FAILED NRF_ESB_EVENT_TX_FAILED
#define RADIO_ESB_PROTOCOL_DPL NRF_ESB_PROTOCOL_ESB_DPL
#define RADIO_ESB_BITRATE_2MBPS NRF_ESB_BITRATE_2MBPS
#define RADIO_ESB_MODE_PTX NRF_ESB_MODE_PTX
#define radio_esb_api_flush_rx nrf_esb_flush_rx
#define radio_esb_api_flush_tx nrf_esb_flush_tx
#define radio_esb_api_init nrf_esb_init
#define radio_esb_api_set_base_address_0 nrf_esb_set_base_address_0
#define radio_esb_api_set_base_address_1 nrf_esb_set_base_address_1
#define radio_esb_api_set_prefixes nrf_esb_set_prefixes
#define radio_esb_api_set_rf_channel nrf_esb_set_rf_channel
#define radio_esb_api_write_payload nrf_esb_write_payload
#else
#error "Unable to find an ESB header for this SDK"
#endif

LOG_MODULE_REGISTER(radio_esb, LOG_LEVEL_INF);

#define RADIO_TX_QUEUE_LEN 4
#define RADIO_EVENT_TX_SUCCESS_FLAG BIT(0)
#define RADIO_EVENT_TX_FAILED_FLAG BIT(1)

static struct k_mutex radio_lock;
static struct k_work radio_work;
static atomic_t pending_events;
static radio_esb_delivery_handler_t tx_delivery_handler;
static enum radio_esb_tx_kind tx_queue[RADIO_TX_QUEUE_LEN];
static size_t tx_queue_head;
static size_t tx_queue_tail;
static size_t tx_queue_count;
static bool radio_initialized;

static void radio_esb_schedule_worker(void)
{
	(void)k_work_submit(&radio_work);
}

static void radio_esb_event_handler(const RADIO_ESB_EVENT *event)
{
	switch (event->evt_id) {
	case RADIO_ESB_EVENT_TX_SUCCESS:
		atomic_or(&pending_events, RADIO_EVENT_TX_SUCCESS_FLAG);
		break;
	case RADIO_ESB_EVENT_TX_FAILED:
		atomic_or(&pending_events, RADIO_EVENT_TX_FAILED_FLAG);
		break;
	default:
		break;
	}

	radio_esb_schedule_worker();
}

static bool radio_esb_pop_tx_kind(enum radio_esb_tx_kind *kind)
{
	bool found = false;

	k_mutex_lock(&radio_lock, K_FOREVER);
	if (tx_queue_count != 0U) {
		*kind = tx_queue[tx_queue_tail];
		tx_queue_tail = (tx_queue_tail + 1U) % RADIO_TX_QUEUE_LEN;
		tx_queue_count--;
		found = true;
	}
	k_mutex_unlock(&radio_lock);

	return found;
}

static size_t radio_esb_clear_tracked_tx_queue(void)
{
	size_t cleared;

	k_mutex_lock(&radio_lock, K_FOREVER);
	cleared = tx_queue_count;
	tx_queue_head = 0U;
	tx_queue_tail = 0U;
	tx_queue_count = 0U;
	k_mutex_unlock(&radio_lock);

	return cleared;
}

static int radio_esb_send_report(enum radio_esb_tx_kind kind,
				 const macropad_report_t *report)
{
	RADIO_ESB_PAYLOAD payload;
	int rc;

	if (!radio_initialized) {
		return -EAGAIN;
	}

	if (report == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&radio_lock, K_FOREVER);
	if (tx_queue_count == RADIO_TX_QUEUE_LEN) {
		k_mutex_unlock(&radio_lock);
		LOG_WRN("TX queue full, dropping kind=%u", kind);
		return -ENOMEM;
	}

	memset(&payload, 0, sizeof(payload));
	payload.pipe = 0U;
	payload.length = sizeof(*report);
	memcpy(payload.data, report, sizeof(*report));
	rc = radio_esb_api_write_payload(&payload);
	if (rc == 0) {
		tx_queue[tx_queue_head] = kind;
		tx_queue_head = (tx_queue_head + 1U) % RADIO_TX_QUEUE_LEN;
		tx_queue_count++;
	}
	k_mutex_unlock(&radio_lock);

	if (rc != 0) {
		LOG_ERR("Failed to queue report kind=%u: %d", kind, rc);
		return rc;
	}

	LOG_INF("Queued report kind=%u keys=0x%02x enc_delta=%d enc_pressed=%u",
		kind, report->keys, report->encoder_delta, report->encoder_pressed);
	return 0;
}

static void radio_esb_work_handler(struct k_work *work)
{
	atomic_val_t events;
	enum radio_esb_tx_kind kind;
	bool dequeued;

	ARG_UNUSED(work);

	events = atomic_set(&pending_events, 0);
	if ((events & RADIO_EVENT_TX_SUCCESS_FLAG) != 0) {
		dequeued = radio_esb_pop_tx_kind(&kind);
		if (dequeued) {
			LOG_INF("Report delivery acknowledged for kind=%u", kind);
			if (tx_delivery_handler != NULL) {
				tx_delivery_handler(kind, true);
			}
		} else {
			LOG_WRN("TX success reported with no queued report kind");
		}
	}

	if ((events & RADIO_EVENT_TX_FAILED_FLAG) != 0) {
		radio_esb_api_flush_tx();
		dequeued = radio_esb_pop_tx_kind(&kind);
		if (dequeued) {
			size_t dropped = radio_esb_clear_tracked_tx_queue();

			LOG_WRN("Report delivery failed for kind=%u", kind);
			if (dropped != 0U) {
				LOG_WRN("Dropped %u queued report(s) after TX failure",
					(unsigned int)dropped);
			}
			if (tx_delivery_handler != NULL) {
				tx_delivery_handler(kind, false);
			}
		} else {
			LOG_WRN("TX failure reported with no queued report kind");
		}
	}
}

int radio_esb_init(const struct esb_addr_config *addr_config, uint8_t rf_channel,
		   radio_esb_delivery_handler_t delivery_handler)
{
	RADIO_ESB_CONFIG config = RADIO_ESB_DEFAULT_CONFIG;
	int rc;

	if (addr_config == NULL) {
		return -EINVAL;
	}

	config.protocol = RADIO_ESB_PROTOCOL_DPL;
	config.bitrate = RADIO_ESB_BITRATE_2MBPS;
	config.mode = RADIO_ESB_MODE_PTX;
	config.event_handler = radio_esb_event_handler;
	config.selective_auto_ack = false;
	config.retransmit_delay = 600U;
	config.retransmit_count = 3U;
	config.payload_length = sizeof(macropad_report_t);

	k_mutex_init(&radio_lock);
	k_work_init(&radio_work, radio_esb_work_handler);
	atomic_clear(&pending_events);
	tx_queue_head = 0U;
	tx_queue_tail = 0U;
	tx_queue_count = 0U;
	tx_delivery_handler = delivery_handler;

	rc = radio_esb_api_init(&config);
	if (rc != 0) {
		LOG_ERR("ESB init failed: %d", rc);
		return rc;
	}

	rc = radio_esb_api_set_base_address_0(addr_config->base_addr_0);
	if (rc != 0) {
		LOG_ERR("Failed to set base address 0: %d", rc);
		return rc;
	}

	rc = radio_esb_api_set_base_address_1(addr_config->base_addr_1);
	if (rc != 0) {
		LOG_ERR("Failed to set base address 1: %d", rc);
		return rc;
	}

	rc = radio_esb_api_set_prefixes(addr_config->prefixes, ARRAY_SIZE(addr_config->prefixes));
	if (rc != 0) {
		LOG_ERR("Failed to set prefixes: %d", rc);
		return rc;
	}

	rc = radio_esb_api_set_rf_channel(rf_channel);
	if (rc != 0) {
		LOG_ERR("Failed to set RF channel: %d", rc);
		return rc;
	}

	radio_initialized = true;
	LOG_INF("ESB initialized in PTX mode on channel %u", rf_channel);
	return 0;
}

int radio_esb_start(void)
{
	if (!radio_initialized) {
		return -EAGAIN;
	}

	radio_esb_api_flush_tx();
	radio_esb_api_flush_rx();
	LOG_INF("ESB sender ready");
	return 0;
}

int radio_esb_send_heartbeat(void)
{
	const macropad_report_t report = {
		.keys = 0U,
		.encoder_delta = 0,
		.encoder_pressed = 0U,
	};

	return radio_esb_send_report(RADIO_ESB_TX_HEARTBEAT, &report);
}

int radio_esb_send_macropad_report(const macropad_report_t *report)
{
	return radio_esb_send_report(RADIO_ESB_TX_INPUT_REPORT, report);
}
#ifndef HPADV2_RADIO_ESB_H_
#define HPADV2_RADIO_ESB_H_

#include <stdint.h>

#include "radio_identity.h"
#include "wire_protocol.h"

enum radio_esb_tx_kind {
	RADIO_ESB_TX_HEARTBEAT = 0,
	RADIO_ESB_TX_INPUT_REPORT = 1,
};

typedef void (*radio_esb_delivery_handler_t)(enum radio_esb_tx_kind kind, bool acked);
typedef void (*radio_esb_config_handler_t)(const macropad_config_t *config);

int radio_esb_init(const struct esb_addr_config *addr_config, uint8_t rf_channel,
		   radio_esb_delivery_handler_t delivery_handler,
		   radio_esb_config_handler_t config_handler);
int radio_esb_start(void);
int radio_esb_stop(void);
int radio_esb_send_heartbeat(const macropad_report_t *report);
int radio_esb_send_macropad_report(const macropad_report_t *report);

#endif

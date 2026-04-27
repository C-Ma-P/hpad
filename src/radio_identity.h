#ifndef HPADV2_RADIO_IDENTITY_H_
#define HPADV2_RADIO_IDENTITY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#define DONGLE_IDENTITY_MAGIC 0x8E5CB7A3U
#define DONGLE_IDENTITY_VERSION 1U

struct dongle_identity {
	uint32_t magic;
	uint8_t version;
	uint8_t random[16];
	uint32_t crc;
} __packed;

struct esb_addr_config {
	uint8_t base_addr_0[4];
	uint8_t base_addr_1[4];
	uint8_t prefixes[8];
};

int radio_identity_init(void);
int radio_identity_load_or_create(struct dongle_identity *out_identity, bool *out_generated);
int radio_identity_derive_esb_config(const struct dongle_identity *id,
				     struct esb_addr_config *out_addr,
				     uint8_t *out_rf_channel);
void radio_identity_log_esb_config(const struct esb_addr_config *addr, uint8_t rf_channel);

#endif
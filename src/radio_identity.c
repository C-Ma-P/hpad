#include "radio_identity.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(radio_identity, LOG_LEVEL_INF);

#define DONGLE_SETTINGS_ROOT "dongle"
#define DONGLE_SETTINGS_KEY DONGLE_SETTINGS_ROOT "/id"

static const uint8_t shared_identity_random[16] = {
	0x42U, 0x17U, 0x93U, 0x5EU, 0xC4U, 0x28U, 0x6BU, 0xD1U,
	0x0FU, 0xA6U, 0x31U, 0x7CU, 0xE2U, 0x59U, 0x84U, 0xBDU,
};

static struct dongle_identity stored_identity;
static bool stored_identity_loaded;

static uint32_t radio_identity_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFU;

	for (size_t idx = 0; idx < len; ++idx) {
		crc ^= data[idx];
		for (uint8_t bit = 0; bit < 8U; ++bit) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);

			crc = (crc >> 1U) ^ (0xEDB88320U & mask);
		}
	}

	return ~crc;
}

static uint32_t radio_identity_expected_crc(const struct dongle_identity *identity)
{
	struct __packed {
		uint32_t magic;
		uint8_t version;
		uint8_t random[16];
	} body = {
		.magic = identity->magic,
		.version = identity->version,
	};

	memcpy(body.random, identity->random, sizeof(body.random));
	return radio_identity_crc32((const uint8_t *)&body, sizeof(body));
}

static bool radio_identity_is_valid(const struct dongle_identity *identity)
{
	if ((identity->magic != DONGLE_IDENTITY_MAGIC) ||
	    (identity->version != DONGLE_IDENTITY_VERSION)) {
		return false;
	}

	return identity->crc == radio_identity_expected_crc(identity);
}

static void radio_identity_build_shared(struct dongle_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->magic = DONGLE_IDENTITY_MAGIC;
	identity->version = DONGLE_IDENTITY_VERSION;
	memcpy(identity->random, shared_identity_random, sizeof(identity->random));
	identity->crc = radio_identity_expected_crc(identity);
}

static bool radio_identity_matches(const struct dongle_identity *left,
				   const struct dongle_identity *right)
{
	return memcmp(left, right, sizeof(*left)) == 0;
}

static int radio_identity_settings_set(const char *name, size_t len_rd,
				       settings_read_cb read_cb, void *cb_arg)
{
	int rc;

	if (!settings_name_steq(name, "id", NULL)) {
		return -ENOENT;
	}

	if (len_rd != sizeof(stored_identity)) {
		LOG_WRN("Stored identity length %zu does not match expected %zu",
			len_rd, sizeof(stored_identity));
		stored_identity_loaded = false;
		return 0;
	}

	rc = read_cb(cb_arg, &stored_identity, sizeof(stored_identity));
	if (rc < 0) {
		return rc;
	}

	if (rc != sizeof(stored_identity)) {
		LOG_WRN("Settings read returned %d bytes for identity", rc);
		stored_identity_loaded = false;
		return 0;
	}

	stored_identity_loaded = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dongle_identity, DONGLE_SETTINGS_ROOT, NULL,
			      radio_identity_settings_set, NULL, NULL);

static uint8_t radio_identity_mix_byte(uint8_t a, uint8_t b, uint8_t c)
{
	uint8_t mixed = (uint8_t)(a ^ ((uint8_t)((b << 1U) | (b >> 7U))) ^ c);

	if (mixed == 0x00U) {
		mixed = (uint8_t)(c ^ 0x5AU);
	} else if (mixed == 0xFFU) {
		mixed = (uint8_t)(c ^ 0xA5U);
	}

	return mixed;
}

static void radio_identity_force_non_trivial(uint8_t *bytes, size_t len, uint8_t tweak)
{
	bool all_zero = true;
	bool all_ff = true;

	for (size_t idx = 0; idx < len; ++idx) {
		all_zero = all_zero && (bytes[idx] == 0x00U);
		all_ff = all_ff && (bytes[idx] == 0xFFU);
	}

	if (all_zero || all_ff) {
		bytes[0] ^= tweak;
		bytes[len - 1U] ^= (uint8_t)(tweak ^ 0x3CU);
	}
}

static int radio_identity_fill_random(uint8_t *buffer, size_t len)
{
#if DT_HAS_CHOSEN(zephyr_entropy)
	static const struct device *const entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	int rc;

	if (!device_is_ready(entropy)) {
		return -ENODEV;
	}

	rc = entropy_get_entropy(entropy, buffer, len);
	if (rc != 0) {
		LOG_ERR("entropy_get_entropy failed: %d", rc);
		return rc;
	}

	return 0;
#else
	ARG_UNUSED(buffer);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

static int radio_identity_generate(struct dongle_identity *identity)
{
	int rc;

	memset(identity, 0, sizeof(*identity));
	identity->magic = DONGLE_IDENTITY_MAGIC;
	identity->version = DONGLE_IDENTITY_VERSION;

	rc = radio_identity_fill_random(identity->random, sizeof(identity->random));
	if (rc != 0) {
		return rc;
	}

	identity->crc = radio_identity_expected_crc(identity);
	return 0;
}

int radio_identity_init(void)
{
	int rc;

	stored_identity_loaded = false;
	rc = settings_subsys_init();
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_WRN("settings_subsys_init failed: %d, continuing with shared radio identity", rc);
		return 0;
	}

	LOG_INF("Persistent settings initialized");
	return 0;
}

int radio_identity_load_or_create(struct dongle_identity *out_identity, bool *out_generated)
{
	struct dongle_identity shared_identity;
	int rc;

	if ((out_identity == NULL) || (out_generated == NULL)) {
		return -EINVAL;
	}

	radio_identity_build_shared(&shared_identity);
	*out_identity = shared_identity;
	*out_generated = false;

	stored_identity_loaded = false;
	rc = settings_load_subtree(DONGLE_SETTINGS_ROOT);
	if (rc != 0) {
		LOG_WRN("settings_load_subtree failed: %d, using shared radio identity", rc);
		return 0;
	}

	if (stored_identity_loaded && radio_identity_is_valid(&stored_identity) &&
	    radio_identity_matches(&stored_identity, &shared_identity)) {
		LOG_INF("Loaded shared radio identity from settings");
		return 0;
	}

	if (stored_identity_loaded && radio_identity_is_valid(&stored_identity)) {
		LOG_WRN("Stored radio identity differs from the shared identity, overriding it");
	} else if (stored_identity_loaded) {
		LOG_WRN("Stored radio identity is invalid, replacing it with the shared identity");
	} else {
		LOG_INF("No persisted radio identity found, storing the shared identity");
	}

	rc = settings_save_one(DONGLE_SETTINGS_KEY, &shared_identity, sizeof(shared_identity));
	if (rc != 0) {
		LOG_WRN("settings_save_one failed: %d, continuing with shared radio identity", rc);
		return 0;
	}

	stored_identity = shared_identity;
	stored_identity_loaded = true;
	LOG_INF("Stored shared radio identity");
	return 0;
}

int radio_identity_derive_esb_config(const struct dongle_identity *identity,
				     struct esb_addr_config *out_addr,
				     uint8_t *out_rf_channel)
{
	static const uint8_t base0_salt[4] = { 0x53U, 0xC1U, 0x97U, 0x2DU };
	static const uint8_t base1_salt[4] = { 0xA4U, 0x6EU, 0x39U, 0xD2U };
	static const uint8_t prefix_salt[8] = {
		0x11U, 0x3DU, 0x57U, 0x7BU, 0x91U, 0xB5U, 0xD7U, 0xE9U,
	};

	if ((identity == NULL) || (out_addr == NULL) || (out_rf_channel == NULL)) {
		return -EINVAL;
	}

	if (!radio_identity_is_valid(identity)) {
		return -EINVAL;
	}

	for (size_t idx = 0; idx < ARRAY_SIZE(out_addr->base_addr_0); ++idx) {
		out_addr->base_addr_0[idx] = radio_identity_mix_byte(identity->random[idx],
						   identity->random[idx + 8U],
						   base0_salt[idx]);
		out_addr->base_addr_1[idx] = radio_identity_mix_byte(identity->random[idx + 4U],
						   identity->random[idx + 12U],
						   base1_salt[idx]);
	}

	for (size_t idx = 0; idx < ARRAY_SIZE(out_addr->prefixes); ++idx) {
		out_addr->prefixes[idx] = radio_identity_mix_byte(identity->random[idx],
						  identity->random[(idx + 5U) % ARRAY_SIZE(identity->random)],
						  prefix_salt[idx]);
	}

	radio_identity_force_non_trivial(out_addr->base_addr_0, ARRAY_SIZE(out_addr->base_addr_0), 0x5EU);
	radio_identity_force_non_trivial(out_addr->base_addr_1, ARRAY_SIZE(out_addr->base_addr_1), 0xA1U);
	radio_identity_force_non_trivial(out_addr->prefixes, ARRAY_SIZE(out_addr->prefixes), 0x3CU);

	*out_rf_channel = (uint8_t)(10U + ((identity->random[1] ^ identity->random[14] ^ 0x2BU) % 71U));
	return 0;
}

void radio_identity_log_esb_config(const struct esb_addr_config *addr, uint8_t rf_channel)
{
	LOG_INF("Derived ESB base0=%02x:%02x:%02x:%02x",
		addr->base_addr_0[0], addr->base_addr_0[1], addr->base_addr_0[2], addr->base_addr_0[3]);
	LOG_INF("Derived ESB base1=%02x:%02x:%02x:%02x",
		addr->base_addr_1[0], addr->base_addr_1[1], addr->base_addr_1[2], addr->base_addr_1[3]);
	LOG_INF("Derived ESB prefixes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
		addr->prefixes[0], addr->prefixes[1], addr->prefixes[2], addr->prefixes[3],
		addr->prefixes[4], addr->prefixes[5], addr->prefixes[6], addr->prefixes[7]);
	LOG_INF("Derived ESB RF channel=%u", rf_channel);
	LOG_INF("Random ESB addressing prevents accidental cross-talk, but it is not encryption");
}
#include "battery.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define BATTERY_ADC_CHANNEL_ID 0U
#define BATTERY_ADC_RESOLUTION 14U
#define BATTERY_ADC_OVERSAMPLING 4U

static const struct device *const battery_adc = DEVICE_DT_GET(DT_NODELABEL(adc));
static bool battery_ready;
static int16_t battery_sample_raw;
static struct adc_channel_cfg battery_channel_cfg = {
	.gain = ADC_GAIN_1_6,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
	.channel_id = BATTERY_ADC_CHANNEL_ID,
	.input_positive = NRF_SAADC_VDDHDIV5,
};
static struct adc_sequence battery_sequence = {
	.channels = BIT(BATTERY_ADC_CHANNEL_ID),
	.buffer = &battery_sample_raw,
	.buffer_size = sizeof(battery_sample_raw),
	.oversampling = BATTERY_ADC_OVERSAMPLING,
	.calibrate = true,
	.resolution = BATTERY_ADC_RESOLUTION,
};

int battery_init(void)
{
	int rc;

	if (!device_is_ready(battery_adc)) {
		LOG_WRN("Battery ADC not ready");
		return -ENODEV;
	}

	rc = adc_channel_setup(battery_adc, &battery_channel_cfg);
	if (rc != 0) {
		LOG_ERR("Battery ADC channel setup failed: %d", rc);
		return rc;
	}

	battery_ready = true;
	return 0;
}

uint16_t battery_sample_mv(void)
{
	int rc;
	int32_t sample_mv;

	if (!battery_ready) {
		return 0U;
	}

	rc = adc_read(battery_adc, &battery_sequence);
	battery_sequence.calibrate = false;
	if (rc != 0) {
		LOG_WRN("Battery ADC read failed: %d", rc);
		return 0U;
	}

	sample_mv = battery_sample_raw;
	rc = adc_raw_to_millivolts(adc_ref_internal(battery_adc),
		battery_channel_cfg.gain,
		battery_sequence.resolution,
		&sample_mv);
	if (rc != 0) {
		LOG_WRN("Battery ADC conversion failed: %d", rc);
		return 0U;
	}

	sample_mv *= 5;
	if (sample_mv <= 0) {
		return 0U;
	}
	if (sample_mv > UINT16_MAX) {
		return UINT16_MAX;
	}

	return (uint16_t)sample_mv;
}
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define OLED_NODE DT_NODELABEL(ssd1306)

#define SSD1306_SET_CONTRAST 0x81U
#define SSD1306_DISPLAY_OFF 0xAEU
#define SSD1306_DISPLAY_ON 0xAFU
#define SSD1306_SET_MEM_MODE 0x20U
#define SSD1306_SET_PAGE_START 0xB0U
#define SSD1306_SET_NORMAL_DISPLAY 0xA6U
#define SSD1306_SET_REVERSE_DISPLAY 0xA7U
#define SSD1306_SDA_PIN 17U
#define SSD1306_SCL_PIN 20U

#define SSD1306_WIDTH DT_PROP(OLED_NODE, width)
#define SSD1306_HEIGHT DT_PROP(OLED_NODE, height)
#define SSD1306_PAGE_COUNT (SSD1306_HEIGHT / 8U)

struct ssd1306_local_config {
	struct i2c_dt_spec i2c;
	uint8_t width;
	uint8_t height;
	uint8_t segment_offset;
	uint8_t display_offset;
	uint8_t multiplex_ratio;
	uint8_t prechargep;
	uint32_t ready_time_ms;
	bool segment_remap;
	bool com_invdir;
	bool com_sequential;
};

struct ssd1306_local_data {
	enum display_pixel_format pixel_format;
	uint8_t page_payload[SSD1306_WIDTH + 1U];
	bool initialized;
	bool blanking;
};

static const struct device *const ssd1306_gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static int ssd1306_local_write_command(const struct ssd1306_local_config *config, uint8_t command);
static int ssd1306_local_write_command_list(const struct ssd1306_local_config *config,
						   const uint8_t *commands,
						   size_t length);

static void ssd1306_local_log_bus_state(const struct ssd1306_local_config *config, const char *context)
{
	int sda;
	int scl;
	int recover_rc;

	if (!device_is_ready(ssd1306_gpio0)) {
		printk("ssd1306_local bus state after %s: gpio0 not ready\n", context);
		return;
	}

	sda = gpio_pin_get_raw(ssd1306_gpio0, SSD1306_SDA_PIN);
	scl = gpio_pin_get_raw(ssd1306_gpio0, SSD1306_SCL_PIN);
	recover_rc = i2c_recover_bus(config->i2c.bus);
	printk("ssd1306_local bus state after %s: SDA=%d SCL=%d recover=%d\n",
	       context, sda, scl, recover_rc);
}

static int ssd1306_local_hw_init(const struct device *dev)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	const uint8_t init_sequence[] = {
		0xAEU,
		0xD5U, 0x80U,
		0xA8U, config->multiplex_ratio,
		0xD3U, config->display_offset,
		0x40U,
		0x8DU, 0x14U,
		SSD1306_SET_MEM_MODE, 0x00U,
		(config->segment_remap ? 0xA1U : 0xA0U),
		(config->com_invdir ? 0xC8U : 0xC0U),
		0xDAU, (config->com_sequential ? 0x02U : 0x12U),
		0x81U, 0x8FU,
		0xD9U, config->prechargep,
		0xDBU, 0x40U,
		0xA4U,
		0xA6U,
		0x2EU,
		0xAFU,
	};
	int rc;

	printk("ssd1306_local init start bus=%s addr=0x%02x\n",
	       config->i2c.bus->name,
	       config->i2c.addr);
	k_msleep(config->ready_time_ms);

	rc = ssd1306_local_write_command(config, 0xE3U);
	if (rc != 0) {
		printk("ssd1306_local probe -> %d\n", rc);
		return rc;
	}
	printk("ssd1306_local probe -> 0\n");

	rc = ssd1306_local_write_command_list(config, init_sequence, ARRAY_SIZE(init_sequence));
	if (rc != 0) {
		printk("ssd1306_local init sequence -> %d\n", rc);
		return rc;
	}
	printk("ssd1306_local init sequence -> 0\n");

	data->pixel_format = PIXEL_FORMAT_MONO10;
	data->page_payload[0] = 0x40U;
	data->initialized = true;
	data->blanking = false;
	printk("ssd1306_local init done\n");
	return 0;
}

static int ssd1306_local_ensure_initialized(const struct device *dev)
{
	struct ssd1306_local_data *data = dev->data;

	if (data->initialized) {
		return 0;
	}

	return ssd1306_local_hw_init(dev);
}

static int ssd1306_local_write_command(const struct ssd1306_local_config *config, uint8_t command)
{
	uint8_t payload[2] = { 0x00U, command };

	return i2c_write_dt(&config->i2c, payload, sizeof(payload));
}

static int ssd1306_local_write_command_list(const struct ssd1306_local_config *config,
						   const uint8_t *commands,
						   size_t length)
{
	for (size_t index = 0; index < length; ++index) {
		int rc = ssd1306_local_write_command(config, commands[index]);

		if (rc != 0) {
			printk("ssd1306_local command[%u]=0x%02x -> %d\n",
			       (unsigned int)index,
			       commands[index],
			       rc);
			ssd1306_local_log_bus_state(config, "command list");
			return rc;
		}
	}

	return 0;
}

static int ssd1306_local_blanking_on(const struct device *dev)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	int rc = ssd1306_local_ensure_initialized(dev);

	if (rc != 0) {
		return rc;
	}

	if (data->blanking) {
		return 0;
	}

	rc = ssd1306_local_write_command(config, SSD1306_DISPLAY_OFF);
	if (rc == 0) {
		data->blanking = true;
	}

	return rc;
}

static int ssd1306_local_blanking_off(const struct device *dev)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	int rc = ssd1306_local_ensure_initialized(dev);

	if (rc != 0) {
		return rc;
	}

	if (!data->blanking) {
		return 0;
	}

	rc = ssd1306_local_write_command(config, SSD1306_DISPLAY_ON);
	if (rc == 0) {
		data->blanking = false;
	}

	return rc;
}

static int ssd1306_local_write(const struct device *dev, const uint16_t x, const uint16_t y,
				      const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	const uint8_t *source = buf;
	uint8_t page_start;
	uint8_t page_count;
	int rc;

	if ((x != 0U) || (desc->pitch != desc->width) || ((y & 0x7U) != 0U)) {
		return -ENOTSUP;
	}

	if ((desc->width != config->width) || (desc->height > config->height) ||
	    ((desc->height & 0x7U) != 0U)) {
		return -EINVAL;
	}

	rc = ssd1306_local_ensure_initialized(dev);
	if (rc != 0) {
		return rc;
	}

	page_start = (uint8_t)(y / 8U);
	page_count = (uint8_t)(desc->height / 8U);

	for (uint8_t page = 0U; page < page_count; ++page) {
		rc = ssd1306_local_write_command(config, (uint8_t)(SSD1306_SET_PAGE_START + page_start + page));
		if (rc != 0) {
			printk("ssd1306_local set page %u -> %d\n", (unsigned int)(page_start + page), rc);
			return rc;
		}

		rc = ssd1306_local_write_command(config, config->segment_offset & 0x0FU);
		if (rc != 0) {
			printk("ssd1306_local set column low -> %d\n", rc);
			return rc;
		}

		rc = ssd1306_local_write_command(config,
				       (uint8_t)(0x10U | ((config->segment_offset >> 4U) & 0x0FU)));
		if (rc != 0) {
			printk("ssd1306_local set column high -> %d\n", rc);
			return rc;
		}

		memcpy(&data->page_payload[1], &source[page * desc->width], desc->width);
		rc = i2c_write_dt(&config->i2c, data->page_payload, desc->width + 1U);
		if (rc != 0) {
			printk("ssd1306_local write page %u -> %d\n", (unsigned int)(page_start + page), rc);
			ssd1306_local_log_bus_state(config, "page write");
			return rc;
		}
	}

	return 0;
}

static void ssd1306_local_get_capabilities(const struct device *dev,
					   struct display_capabilities *caps)
{
	struct ssd1306_local_data *data = dev->data;

	caps->x_resolution = SSD1306_WIDTH;
	caps->y_resolution = SSD1306_HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10 | PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = data->pixel_format;
	caps->screen_info = SCREEN_INFO_MONO_VTILED;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int ssd1306_local_set_pixel_format(const struct device *dev,
					  const enum display_pixel_format pixel_format)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	uint8_t command;
	int rc;

	rc = ssd1306_local_ensure_initialized(dev);
	if (rc != 0) {
		return rc;
	}

	if (pixel_format == data->pixel_format) {
		return 0;
	}

	if (pixel_format == PIXEL_FORMAT_MONO10) {
		command = SSD1306_SET_REVERSE_DISPLAY;
	} else if (pixel_format == PIXEL_FORMAT_MONO01) {
		command = SSD1306_SET_NORMAL_DISPLAY;
	} else {
		return -ENOTSUP;
	}

	data->pixel_format = pixel_format;
	return ssd1306_local_write_command(config, command);
}

static int ssd1306_local_set_contrast(const struct device *dev, const uint8_t contrast)
{
	const struct ssd1306_local_config *config = dev->config;
	const uint8_t commands[] = { SSD1306_SET_CONTRAST, contrast };
	int rc = ssd1306_local_ensure_initialized(dev);

	if (rc != 0) {
		return rc;
	}

	return ssd1306_local_write_command_list(config, commands, ARRAY_SIZE(commands));
}

static int ssd1306_local_init(const struct device *dev)
{
	const struct ssd1306_local_config *config = dev->config;
	struct ssd1306_local_data *data = dev->data;
	if (!i2c_is_ready_dt(&config->i2c)) {
		printk("ssd1306_local i2c not ready\n");
		return -ENODEV;
	}

	data->pixel_format = PIXEL_FORMAT_MONO10;
	data->page_payload[0] = 0x40U;
	data->initialized = false;
	return 0;
}

static const struct display_driver_api ssd1306_local_api = {
	.blanking_on = ssd1306_local_blanking_on,
	.blanking_off = ssd1306_local_blanking_off,
	.write = ssd1306_local_write,
	.set_contrast = ssd1306_local_set_contrast,
	.get_capabilities = ssd1306_local_get_capabilities,
	.set_pixel_format = ssd1306_local_set_pixel_format,
};

static struct ssd1306_local_data ssd1306_local_data_0;

static const struct ssd1306_local_config ssd1306_local_config_0 = {
	.i2c = I2C_DT_SPEC_GET(OLED_NODE),
	.width = DT_PROP(OLED_NODE, width),
	.height = DT_PROP(OLED_NODE, height),
	.segment_offset = DT_PROP(OLED_NODE, segment_offset),
	.display_offset = DT_PROP(OLED_NODE, display_offset),
	.multiplex_ratio = DT_PROP(OLED_NODE, multiplex_ratio),
	.prechargep = DT_PROP(OLED_NODE, prechargep),
	.ready_time_ms = DT_PROP(OLED_NODE, ready_time_ms),
	.segment_remap = DT_NODE_HAS_PROP(OLED_NODE, segment_remap),
	.com_invdir = DT_NODE_HAS_PROP(OLED_NODE, com_invdir),
	.com_sequential = DT_NODE_HAS_PROP(OLED_NODE, com_sequential),
};

DEVICE_DT_DEFINE(OLED_NODE,
			 ssd1306_local_init,
			 NULL,
			 &ssd1306_local_data_0,
			 &ssd1306_local_config_0,
			 POST_KERNEL,
			 CONFIG_DISPLAY_INIT_PRIORITY,
			 &ssd1306_local_api);
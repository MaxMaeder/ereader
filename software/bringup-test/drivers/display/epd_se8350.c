/*
 * SE8350 e-Paper display driver (SSD1683-compatible command set).
 *
 * Copyright (c) 2025 Max Maeder
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(epd_se8350, CONFIG_DISPLAY_LOG_LEVEL);

#define SE8350_PIXELS_PER_BYTE 8U

/* SSD1683-family commands */
#define CMD_SWRESET  0x12
#define CMD_TSENSOR  0x18
#define CMD_BTST     0x0C  /* Booster Soft Start */
#define CMD_DOC      0x01  /* Driver Output Control */
#define CMD_DEM      0x11  /* Data Entry Mode */
#define CMD_RAM_XSE  0x44  /* Set RAM X Start/End */
#define CMD_RAM_YSE  0x45  /* Set RAM Y Start/End */
#define CMD_RAM_XC   0x4E  /* Set RAM X Counter */
#define CMD_RAM_YC   0x4F  /* Set RAM Y Counter */
#define CMD_BWF      0x3C  /* Border Waveform */
#define CMD_WRITE_BW 0x24  /* Write B/W RAM */
#define CMD_WRITE_R  0x26  /* Write RED/secondary RAM */
#define CMD_DUC2     0x22  /* Display Update Control 2 */
#define CMD_MA       0x20  /* Master Activation */
#define CMD_SLEEP    0x10  /* Deep Sleep */

/* Display Update Control 2 sequences */
#define DUC2_FULL    0xF7
#define DUC2_PARTIAL 0xFF

/* Timing */
#define RESET_DELAY_MS    20U
#define BUSY_POLL_MS      10U
#define BUSY_TIMEOUT_MS   10000U

struct se8350_config {
	const struct device *mipi_dev;
	struct mipi_dbi_config dbi_config;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
	const uint8_t *softstart;
	uint8_t softstart_len;
};

struct se8350_data {
	bool blanking_on;
	bool refresh_pending;
};

static int se8350_busy_wait(const struct device *dev)
{
	const struct se8350_config *cfg = dev->config;
	uint32_t elapsed = 0;

	while (gpio_pin_get_dt(&cfg->busy_gpio) > 0) {
		if (elapsed >= BUSY_TIMEOUT_MS) {
			LOG_ERR("Busy timeout after %u ms", BUSY_TIMEOUT_MS);
			return -ETIMEDOUT;
		}
		k_sleep(K_MSEC(BUSY_POLL_MS));
		elapsed += BUSY_POLL_MS;
	}
	return 0;
}

static int se8350_write_cmd(const struct device *dev, uint8_t cmd,
			    const uint8_t *data, size_t len)
{
	const struct se8350_config *cfg = dev->config;
	int err;

	err = mipi_dbi_command_write(cfg->mipi_dev, &cfg->dbi_config,
				     cmd, data, len);
	mipi_dbi_release(cfg->mipi_dev, &cfg->dbi_config);
	return err;
}

static int se8350_write_cmd_u8(const struct device *dev, uint8_t cmd,
			       uint8_t val)
{
	return se8350_write_cmd(dev, cmd, &val, 1);
}

static int se8350_turn_on_display(const struct device *dev)
{
	int err;

	err = se8350_write_cmd_u8(dev, CMD_DUC2, DUC2_FULL);
	if (err) {
		return err;
	}

	return se8350_write_cmd(dev, CMD_MA, NULL, 0);
}

static int se8350_wait_idle(const struct device *dev)
{
	struct se8350_data *data = dev->data;

	if (data->refresh_pending) {
		int err = se8350_busy_wait(dev);

		data->refresh_pending = false;
		if (err) {
			return err;
		}
	}
	return 0;
}

static int se8350_blanking_off(const struct device *dev)
{
	struct se8350_data *data = dev->data;
	int err;

	if (data->blanking_on) {
		err = se8350_turn_on_display(dev);
		if (err) {
			return err;
		}
		data->refresh_pending = true;
	}
	data->blanking_on = false;
	return 0;
}

static int se8350_blanking_on(const struct device *dev)
{
	struct se8350_data *data = dev->data;

	data->blanking_on = true;
	return 0;
}

static int se8350_send_buffer(const struct device *dev, uint8_t cmd,
			      const uint8_t *src, size_t len)
{
	const struct se8350_config *cfg = dev->config;
	int err;

	while (len > 0) {
		size_t chunk = MIN(len, 4096);

		err = mipi_dbi_command_write(cfg->mipi_dev, &cfg->dbi_config,
					     cmd, src, chunk);
		mipi_dbi_release(cfg->mipi_dev, &cfg->dbi_config);
		if (err) {
			return err;
		}
		src += chunk;
		len -= chunk;
	}
	return 0;
}

static int se8350_write(const struct device *dev, const uint16_t x,
			const uint16_t y,
			const struct display_buffer_descriptor *desc,
			const void *buf)
{
	const struct se8350_config *cfg = dev->config;
	struct se8350_data *data = dev->data;
	const uint8_t *src = buf;
	size_t buf_len;
	uint8_t zero[2] = {0x00, 0x00};
	int err;

	err = se8350_wait_idle(dev);
	if (err) {
		return err;
	}

	buf_len = MIN(desc->buf_size,
		      desc->height * desc->width / SE8350_PIXELS_PER_BYTE);

	if ((x + desc->width) > cfg->width || (y + desc->height) > cfg->height) {
		LOG_ERR("Position out of bounds");
		return -EINVAL;
	}

	err = se8350_write_cmd(dev, CMD_RAM_XC, zero, 2);
	if (err) {
		return err;
	}
	err = se8350_write_cmd(dev, CMD_RAM_YC, zero, 2);
	if (err) {
		return err;
	}

	err = se8350_send_buffer(dev, CMD_WRITE_BW, src, buf_len);
	if (err) {
		return err;
	}

	err = se8350_write_cmd(dev, CMD_RAM_XC, zero, 2);
	if (err) {
		return err;
	}
	err = se8350_write_cmd(dev, CMD_RAM_YC, zero, 2);
	if (err) {
		return err;
	}

	err = se8350_send_buffer(dev, CMD_WRITE_R, src, buf_len);
	if (err) {
		return err;
	}

	if (!data->blanking_on) {
		return se8350_turn_on_display(dev);
	}

	return 0;
}

static void se8350_get_capabilities(const struct device *dev,
				    struct display_capabilities *caps)
{
	const struct se8350_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = cfg->width;
	caps->y_resolution = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = PIXEL_FORMAT_MONO10;
	caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST | SCREEN_INFO_EPD;
}

static int se8350_set_pixel_format(const struct device *dev,
				   const enum display_pixel_format pf)
{
	if (pf == PIXEL_FORMAT_MONO10) {
		return 0;
	}
	return -ENOTSUP;
}

static int se8350_init(const struct device *dev)
{
	const struct se8350_config *cfg = dev->config;
	struct se8350_data *data = dev->data;
	uint16_t h = cfg->height;
	uint16_t w = cfg->width;
	uint8_t buf[5];
	int err;

	if (!device_is_ready(cfg->mipi_dev)) {
		LOG_ERR("MIPI DBI device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->busy_gpio)) {
		LOG_ERR("Busy GPIO not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&cfg->busy_gpio, GPIO_INPUT);
	data->blanking_on = true;

	/* Hardware reset */
	if (mipi_dbi_reset(cfg->mipi_dev, RESET_DELAY_MS) < 0) {
		return -EIO;
	}
	k_sleep(K_MSEC(RESET_DELAY_MS));

	err = se8350_busy_wait(dev);
	if (err) {
		LOG_ERR("Display not responding after reset");
		return err;
	}

	/* Software reset */
	err = se8350_write_cmd(dev, CMD_SWRESET, NULL, 0);
	if (err) {
		return err;
	}
	err = se8350_busy_wait(dev);
	if (err) {
		return err;
	}

	/* Temperature sensor: internal */
	err = se8350_write_cmd_u8(dev, CMD_TSENSOR, 0x80);
	if (err) {
		return err;
	}

	/* Booster Soft Start */
	if (cfg->softstart_len) {
		err = se8350_write_cmd(dev, CMD_BTST, cfg->softstart,
				       cfg->softstart_len);
	} else {
		buf[0] = 0xAE;
		buf[1] = 0xC7;
		buf[2] = 0xC3;
		buf[3] = 0xC0;
		buf[4] = 0x80;
		err = se8350_write_cmd(dev, CMD_BTST, buf, 5);
	}
	if (err) {
		return err;
	}

	/* Driver Output Control */
	buf[0] = (h - 1) & 0xFF;
	buf[1] = ((h - 1) >> 8) & 0xFF;
	buf[2] = 0x02;
	err = se8350_write_cmd(dev, CMD_DOC, buf, 3);
	if (err) {
		return err;
	}

	/* Data Entry Mode: Y decrement, X increment */
	err = se8350_write_cmd_u8(dev, CMD_DEM, 0x01);
	if (err) {
		return err;
	}

	/* RAM X address range */
	buf[0] = 0x00;
	buf[1] = 0x00;
	buf[2] = (w - 1) & 0xFF;
	buf[3] = ((w - 1) >> 8) & 0xFF;
	err = se8350_write_cmd(dev, CMD_RAM_XSE, buf, 4);
	if (err) {
		return err;
	}

	/* RAM Y address range: Y start=(H-1), Y end=0 */
	buf[0] = (h - 1) & 0xFF;
	buf[1] = ((h - 1) >> 8) & 0xFF;
	buf[2] = 0x00;
	buf[3] = 0x00;
	err = se8350_write_cmd(dev, CMD_RAM_YSE, buf, 4);
	if (err) {
		return err;
	}

	/* RAM X counter = 0 */
	buf[0] = 0x00;
	buf[1] = 0x00;
	err = se8350_write_cmd(dev, CMD_RAM_XC, buf, 2);
	if (err) {
		return err;
	}

	/* RAM Y counter = 0 */
	buf[0] = 0x00;
	buf[1] = 0x00;
	err = se8350_write_cmd(dev, CMD_RAM_YC, buf, 2);
	if (err) {
		return err;
	}

	err = se8350_busy_wait(dev);
	if (err) {
		return err;
	}

	/* Border Waveform */
	err = se8350_write_cmd_u8(dev, CMD_BWF, 0x01);
	if (err) {
		return err;
	}

	LOG_INF("SE8350 initialized (%ux%u)", w, h);
	return 0;
}

static DEVICE_API(display, se8350_api) = {
	.blanking_on = se8350_blanking_on,
	.blanking_off = se8350_blanking_off,
	.write = se8350_write,
	.get_capabilities = se8350_get_capabilities,
	.set_pixel_format = se8350_set_pixel_format,
};

#define SE8350_SOFTSTART(n) \
	static const uint8_t se8350_softstart_##n[] = \
		DT_PROP_OR(n, softstart, {});

#define SE8350_DEFINE(n)						\
	SE8350_SOFTSTART(n)						\
									\
	static const struct se8350_config se8350_cfg_##n = {		\
		.mipi_dev = DEVICE_DT_GET(DT_PARENT(n)),		\
		.dbi_config = {						\
			.mode = MIPI_DBI_MODE_SPI_4WIRE,		\
			.config = MIPI_DBI_SPI_CONFIG_DT(n,		\
				SPI_OP_MODE_MASTER |			\
				SPI_LOCK_ON | SPI_WORD_SET(8), 0),	\
		},							\
		.busy_gpio = GPIO_DT_SPEC_GET(n, busy_gpios),		\
		.width = DT_PROP(n, width),				\
		.height = DT_PROP(n, height),				\
		.softstart = se8350_softstart_##n,			\
		.softstart_len = sizeof(se8350_softstart_##n),		\
	};								\
									\
	static struct se8350_data se8350_data_##n;			\
									\
	DEVICE_DT_DEFINE(n, se8350_init, NULL,				\
			 &se8350_data_##n, &se8350_cfg_##n,		\
			 POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,	\
			 &se8350_api);

DT_FOREACH_STATUS_OKAY(waveshare_5in_epaper, SE8350_DEFINE)

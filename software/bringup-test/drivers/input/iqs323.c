#define DT_DRV_COMPAT azoteq_iqs323

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(iqs323, CONFIG_INPUT_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * Register addresses (8-bit addressing, 16-bit little-endian data at each)
 * -------------------------------------------------------------------------*/

/* Read-only */
#define REG_PRODUCT_NUM        0x00
#define REG_SYSTEM_STATUS      0x10

/* Per-sensor setup (base + 0x10 * sensor_index) */
#define REG_SENSOR_SETUP(s)    (0x30 + 0x10 * (s))
#define REG_CONV_FREQ(s)       (0x31 + 0x10 * (s))
#define REG_PROX_CTRL(s)       (0x32 + 0x10 * (s))
#define REG_PROX_INPUT(s)      (0x33 + 0x10 * (s))
#define REG_PATTERN_DEF(s)     (0x34 + 0x10 * (s))
#define REG_PATTERN_SEL(s)     (0x35 + 0x10 * (s))
#define REG_ATI_SETUP(s)       (0x36 + 0x10 * (s))
#define REG_ATI_BASE(s)        (0x37 + 0x10 * (s))

/* Per-channel UI (base + 0x10 * channel_index) */
#define REG_CH_SETUP(ch)       (0x60 + 0x10 * (ch))
#define REG_PROX_SETTINGS(ch)  (0x61 + 0x10 * (ch))
#define REG_TOUCH_SETTINGS(ch) (0x62 + 0x10 * (ch))

/* Filter betas */
#define REG_COUNTS_FILTER      0xB0
#define REG_LTA_FILTER         0xB1
#define REG_LTA_FAST_FILTER    0xB2
#define REG_FAST_FILTER_BAND   0xB4

/* System control */
#define REG_SYSTEM_CTRL        0xC0
#define REG_NP_REPORT_RATE     0xC1
#define REG_LP_REPORT_RATE     0xC2
#define REG_ULP_REPORT_RATE    0xC3
#define REG_HALT_REPORT_RATE   0xC4
#define REG_PM_TIMEOUT         0xC5

/* General */
#define REG_EVENTS_ENABLE      0xD3

/* ---------------------------------------------------------------------------
 * Bit-field helpers for System Status (0x10)
 * -------------------------------------------------------------------------*/
#define STATUS_RESET_EVENT     BIT(7)
#define STATUS_ATI_ERROR       BIT(6)
#define STATUS_ATI_ACTIVE      BIT(5)

#define STATUS_CH0_TOUCH       BIT(9)
#define STATUS_CH1_TOUCH       BIT(11)
#define STATUS_CH2_TOUCH       BIT(13)

/* ---------------------------------------------------------------------------
 * System Control (0xC0) helpers
 * -------------------------------------------------------------------------*/
#define SYSCTL_EVENT_MODE      BIT(7)
#define SYSCTL_PM_AUTO         (0x04 << 4)   /* bits [6:4] = 100 */
#define SYSCTL_RE_ATI          BIT(2)
#define SYSCTL_ACK_RESET       BIT(0)

/* Events Enable (0xD3) */
#define EVENT_TOUCH            BIT(1)
#define EVENT_PROX             BIT(0)

/* ---------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------*/
#define IQS323_NUM_CHANNELS    3
#define IQS323_PRODUCT_NUM     1106   /* IQS323-001 */

#define RDY_TIMEOUT_MS         200
#define RESET_PULSE_US         1000   /* 1 ms  (minimum 250 ns) */
#define STARTUP_DELAY_MS       50     /* time after MCLR release for first RDY */

/* ---------------------------------------------------------------------------
 * Per-register initialisation table
 *
 * Written in order during first-time setup while the Reset Event flag
 * keeps communication windows continuously open.  The final entry
 * (REG_SYSTEM_CTRL) acknowledges the reset, enables event mode and
 * triggers an ATI.
 * -------------------------------------------------------------------------*/
struct reg_val {
	uint8_t  reg;
	uint16_t val;
};

/*
 * Sensor Setup encoding (per sensor):
 *   [15]    reserved
 *   [14]    CalCap Rx        = 0
 *   [13]    CalCap Tx        = 0
 *   [11]    TxA              = 0
 *   [10:8]  CTx2/1/0         = one-hot for the matching channel
 *   [6]     Release UI       = 0
 *   [5]     FOSC Tx Freq     = 0
 *   [3]     Invert           = 0  (self-cap: counts decrease on touch)
 *   [0]     Enable           = 1
 *
 * Prox Input encoding (per sensor):
 *   [13]    Internal Ref     = 0
 *   [12]    Bias Current     = 0
 *   [11]    CalCap Sel       = 0
 *   [10:8]  CRx2/1/0         = one-hot for the matching channel
 *   [7]     reserved         = 1
 *   [6]     Dead Time En     = 1
 *   [3:2]   Auto Prox Cycle  = 11 (32)
 *   [1:0]   reserved         = 11
 */

static const struct reg_val init_regs[] = {
	/* --- Sensor 0: CRx0 / CTx0 (defaults are already correct) --- */
	{ REG_SENSOR_SETUP(0), 0x0101 },
	{ REG_CONV_FREQ(0),    0x057F },  /* Period 5 → 1 MHz, Fraction 127 */
	{ REG_PROX_CTRL(0),    0x1290 },  /* Cs 80pF, S/H 10 µA, Self-Cap */
	{ REG_PROX_INPUT(0),   0x01CF },  /* CRx0, dead-time on */
	{ REG_PATTERN_DEF(0),  0x030A },  /* WavPat0 0x03, Inactive Rx VSS */
	{ REG_PATTERN_SEL(0),  0x0000 },
	{ REG_ATI_SETUP(0),    0x040C },  /* Full ATI, large band, res=64 */
	{ REG_ATI_BASE(0),     0x0064 },  /* ATI base 100 */

	/* --- Sensor 1: CRx1 / CTx1 --- */
	{ REG_SENSOR_SETUP(1), 0x0201 },  /* CTx1, Enable */
	{ REG_CONV_FREQ(1),    0x057F },
	{ REG_PROX_CTRL(1),    0x1290 },
	{ REG_PROX_INPUT(1),   0x02CF },  /* CRx1 */
	{ REG_PATTERN_DEF(1),  0x030A },
	{ REG_PATTERN_SEL(1),  0x0000 },
	{ REG_ATI_SETUP(1),    0x040C },
	{ REG_ATI_BASE(1),     0x0064 },

	/* --- Sensor 2: CRx2 / CTx2 --- */
	{ REG_SENSOR_SETUP(2), 0x0401 },  /* CTx2, Enable */
	{ REG_CONV_FREQ(2),    0x057F },
	{ REG_PROX_CTRL(2),    0x1290 },
	{ REG_PROX_INPUT(2),   0x04CF },  /* CRx2 */
	{ REG_PATTERN_DEF(2),  0x030A },
	{ REG_PATTERN_SEL(2),  0x0000 },
	{ REG_ATI_SETUP(2),    0x040C },
	{ REG_ATI_BASE(2),     0x0064 },

	/* --- Channel 0/1/2 UI --- */
	{ REG_CH_SETUP(0),       0x0000 },  /* Independent */
	{ REG_PROX_SETTINGS(0),  0x330A },  /* Debounce 3/3, Prox threshold 10 */
	{ REG_TOUCH_SETTINGS(0), 0x2014 },  /* Hysteresis 32, Touch threshold 20 */

	{ REG_CH_SETUP(1),       0x0000 },
	{ REG_PROX_SETTINGS(1),  0x330A },
	{ REG_TOUCH_SETTINGS(1), 0x2014 },

	{ REG_CH_SETUP(2),       0x0000 },
	{ REG_PROX_SETTINGS(2),  0x330A },
	{ REG_TOUCH_SETTINGS(2), 0x2014 },

	/* --- Filter betas --- */
	{ REG_COUNTS_FILTER,    0x4080 },  /* LP β=64, NP β=128 */
	{ REG_LTA_FILTER,       0x0707 },
	{ REG_LTA_FAST_FILTER,  0xA0A0 },
	{ REG_FAST_FILTER_BAND, 0x0014 },

	/* --- Report rates (ms) --- */
	{ REG_NP_REPORT_RATE,   0x0010 },  /* 16 ms  */
	{ REG_LP_REPORT_RATE,   0x0064 },  /* 100 ms */
	{ REG_ULP_REPORT_RATE,  0x00C8 },  /* 200 ms */
	{ REG_HALT_REPORT_RATE, 0x0BB8 },  /* 3000 ms */
	{ REG_PM_TIMEOUT,       0x2710 },  /* 10 000 ms */

	/* --- Events --- */
	{ REG_EVENTS_ENABLE, EVENT_TOUCH | EVENT_PROX },

	/*
	 * System Control – MUST be written last.
	 * ACK Reset clears the continuous-window behaviour, event mode
	 * activates, and Re-ATI calibrates all channels.
	 */
	{ REG_SYSTEM_CTRL, SYSCTL_EVENT_MODE | SYSCTL_PM_AUTO |
			   SYSCTL_RE_ATI | SYSCTL_ACK_RESET },
};

/* Touch-state bits for each channel within the System Status word */
static const uint16_t ch_touch_mask[IQS323_NUM_CHANNELS] = {
	STATUS_CH0_TOUCH,
	STATUS_CH1_TOUCH,
	STATUS_CH2_TOUCH,
};

/* ---------------------------------------------------------------------------
 * Driver data structures
 * -------------------------------------------------------------------------*/
struct iqs323_config {
	struct i2c_dt_spec  i2c;
	struct gpio_dt_spec rdy_gpio;
	const uint16_t     *input_codes;
};

struct iqs323_data {
	const struct device  *dev;
	struct k_work         work;
	struct gpio_callback  rdy_cb;
	uint16_t              prev_touch;
};

/* ---------------------------------------------------------------------------
 * Low-level I2C helpers
 * -------------------------------------------------------------------------*/

static int iqs323_wait_rdy(const struct iqs323_config *cfg,
			   k_timeout_t timeout)
{
	int64_t deadline = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	while (k_uptime_get() < deadline) {
		if (gpio_pin_get_dt(&cfg->rdy_gpio)) {
			return 0;
		}
		k_usleep(100);
	}
	return -ETIMEDOUT;
}

static int iqs323_reg_write(const struct iqs323_config *cfg,
			    uint8_t reg, uint16_t val)
{
	uint8_t buf[3];

	buf[0] = reg;
	sys_put_le16(val, &buf[1]);
	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static int iqs323_reg_read(const struct iqs323_config *cfg,
			   uint8_t reg, uint16_t *val)
{
	uint8_t data[2];
	int ret;

	ret = i2c_write_read_dt(&cfg->i2c, &reg, 1, data, sizeof(data));
	if (ret == 0) {
		*val = sys_get_le16(data);
	}
	return ret;
}

/**
 * Send the "force communication" command (write 0xFF) which causes the
 * IQS323 to open a communication window regardless of event state.
 */
static int iqs323_force_comms(const struct iqs323_config *cfg)
{
	uint8_t cmd = 0xFF;

	return i2c_write_dt(&cfg->i2c, &cmd, 1);
}

/* ---------------------------------------------------------------------------
 * Hardware reset via MCLR
 *
 * Briefly drives the RDY/MCLR pin low (>250 ns) then reconfigures it as
 * an input so the IQS323's internal pull-up can release it.
 * -------------------------------------------------------------------------*/
static int iqs323_hw_reset(const struct iqs323_config *cfg)
{
	int ret;

	ret = gpio_pin_configure_dt(&cfg->rdy_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return ret;
	}
	k_busy_wait(RESET_PULSE_US);

	ret = gpio_pin_configure_dt(&cfg->rdy_gpio, GPIO_INPUT);
	if (ret) {
		return ret;
	}
	k_sleep(K_MSEC(STARTUP_DELAY_MS));
	return 0;
}

/* ---------------------------------------------------------------------------
 * Device configuration (called once after POR / reset)
 *
 * While Reset Event is set the IQS323 continuously opens communication
 * windows, so we can write one register per window, waiting for RDY
 * between each.
 * -------------------------------------------------------------------------*/
static int iqs323_configure(const struct device *dev)
{
	const struct iqs323_config *cfg = dev->config;
	uint16_t status;
	int ret;

	/* Wait for the first RDY window */
	ret = iqs323_wait_rdy(cfg, K_MSEC(RDY_TIMEOUT_MS));
	if (ret) {
		LOG_ERR("Timeout waiting for initial RDY");
		return ret;
	}

	/* Read system status – expect Reset Event to be set */
	ret = iqs323_reg_read(cfg, REG_SYSTEM_STATUS, &status);
	if (ret) {
		LOG_ERR("Failed to read system status (%d)", ret);
		return ret;
	}

	if (!(status & STATUS_RESET_EVENT)) {
		LOG_WRN("Reset Event not set (status 0x%04x); forcing reset",
			status);
		ret = iqs323_hw_reset(cfg);
		if (ret) {
			return ret;
		}
		ret = iqs323_wait_rdy(cfg, K_MSEC(RDY_TIMEOUT_MS));
		if (ret) {
			return ret;
		}
		ret = iqs323_reg_read(cfg, REG_SYSTEM_STATUS, &status);
		if (ret) {
			return ret;
		}
	}

	LOG_DBG("System status after reset: 0x%04x", status);

	/* Write all configuration registers */
	for (size_t i = 0; i < ARRAY_SIZE(init_regs); i++) {
		ret = iqs323_wait_rdy(cfg, K_MSEC(RDY_TIMEOUT_MS));
		if (ret) {
			LOG_ERR("Timeout waiting for RDY at reg 0x%02x",
				init_regs[i].reg);
			return ret;
		}
		ret = iqs323_reg_write(cfg, init_regs[i].reg,
				       init_regs[i].val);
		if (ret) {
			LOG_ERR("Failed to write reg 0x%02x (%d)",
				init_regs[i].reg, ret);
			return ret;
		}
	}

	LOG_INF("Configuration written, ATI running");
	return 0;
}

/* ---------------------------------------------------------------------------
 * Runtime event processing
 * -------------------------------------------------------------------------*/
static void iqs323_process(const struct device *dev)
{
	const struct iqs323_config *cfg = dev->config;
	struct iqs323_data *data = dev->data;
	uint16_t status;
	int ret;

	ret = iqs323_reg_read(cfg, REG_SYSTEM_STATUS, &status);
	if (ret) {
		LOG_ERR("Failed to read status (%d)", ret);
		return;
	}

	/* Re-initialise after an unexpected reset */
	if (status & STATUS_RESET_EVENT) {
		LOG_WRN("Device reset detected, reconfiguring");
		iqs323_configure(dev);
		return;
	}

	/* If ATI errored, retrigger it */
	if (status & STATUS_ATI_ERROR) {
		LOG_WRN("ATI error, retriggering");
		iqs323_force_comms(cfg);
		iqs323_wait_rdy(cfg, K_MSEC(RDY_TIMEOUT_MS));
		iqs323_reg_write(cfg, REG_SYSTEM_CTRL,
				 SYSCTL_EVENT_MODE | SYSCTL_PM_AUTO |
				 SYSCTL_RE_ATI);
		return;
	}

	/* Report touch state changes */
	for (int i = 0; i < IQS323_NUM_CHANNELS; i++) {
		bool now   = (status & ch_touch_mask[i]) != 0;
		bool prev  = (data->prev_touch & ch_touch_mask[i]) != 0;

		if (now != prev) {
			input_report_key(dev, cfg->input_codes[i],
					 (int)now, true, K_FOREVER);
		}
	}
	data->prev_touch = status & (STATUS_CH0_TOUCH | STATUS_CH1_TOUCH |
				     STATUS_CH2_TOUCH);

	LOG_DBG("status 0x%04x", status);
}

static void iqs323_work_handler(struct k_work *w)
{
	struct iqs323_data *data = CONTAINER_OF(w, struct iqs323_data, work);

	iqs323_process(data->dev);
}

static void iqs323_rdy_isr(const struct device *port,
			    struct gpio_callback *cb, uint32_t pins)
{
	struct iqs323_data *data = CONTAINER_OF(cb, struct iqs323_data, rdy_cb);

	k_work_submit(&data->work);
}

/* ---------------------------------------------------------------------------
 * Device initialisation
 * -------------------------------------------------------------------------*/
static int iqs323_init(const struct device *dev)
{
	const struct iqs323_config *cfg = dev->config;
	struct iqs323_data *data = dev->data;
	uint16_t product;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->rdy_gpio)) {
		LOG_ERR("RDY GPIO not ready");
		return -ENODEV;
	}

	data->dev = dev;
	k_work_init(&data->work, iqs323_work_handler);

	/* Hardware reset to guarantee a known starting state */
	ret = iqs323_hw_reset(cfg);
	if (ret) {
		LOG_ERR("Hardware reset failed (%d)", ret);
		return ret;
	}

	/* Verify product number */
	ret = iqs323_wait_rdy(cfg, K_MSEC(RDY_TIMEOUT_MS));
	if (ret) {
		LOG_ERR("Timeout waiting for RDY after reset");
		return ret;
	}

	ret = iqs323_reg_read(cfg, REG_PRODUCT_NUM, &product);
	if (ret) {
		LOG_ERR("Failed to read product number (%d)", ret);
		return ret;
	}

	if (product != IQS323_PRODUCT_NUM) {
		LOG_ERR("Unexpected product number: %u (expected %u)",
			product, IQS323_PRODUCT_NUM);
		return -ENODEV;
	}

	LOG_INF("IQS323 detected (product %u)", product);

	/* Write full configuration */
	ret = iqs323_configure(dev);
	if (ret) {
		return ret;
	}

	/* Set up RDY GPIO interrupt for runtime events */
	ret = gpio_pin_configure_dt(&cfg->rdy_gpio, GPIO_INPUT);
	if (ret) {
		LOG_ERR("Failed to configure RDY pin (%d)", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->rdy_gpio,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		LOG_ERR("Failed to configure RDY interrupt (%d)", ret);
		return ret;
	}

	gpio_init_callback(&data->rdy_cb, iqs323_rdy_isr,
			   BIT(cfg->rdy_gpio.pin));

	ret = gpio_add_callback(cfg->rdy_gpio.port, &data->rdy_cb);
	if (ret) {
		LOG_ERR("Failed to add RDY callback (%d)", ret);
		return ret;
	}

	LOG_INF("IQS323 initialised");
	return 0;
}

/* ---------------------------------------------------------------------------
 * Devicetree instantiation
 * -------------------------------------------------------------------------*/
#define IQS323_INIT(inst)                                                     \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, input_codes) ==                   \
		     IQS323_NUM_CHANNELS,                                      \
		     "input-codes must have exactly 3 entries");                \
									       \
	static const uint16_t iqs323_codes_##inst[] =                          \
		DT_INST_PROP(inst, input_codes);                               \
									       \
	static const struct iqs323_config iqs323_cfg_##inst = {                \
		.i2c        = I2C_DT_SPEC_INST_GET(inst),                      \
		.rdy_gpio   = GPIO_DT_SPEC_INST_GET(inst, rdy_gpios),         \
		.input_codes = iqs323_codes_##inst,                            \
	};                                                                     \
									       \
	static struct iqs323_data iqs323_data_##inst;                          \
									       \
	DEVICE_DT_INST_DEFINE(inst, iqs323_init, NULL,                         \
			      &iqs323_data_##inst, &iqs323_cfg_##inst,         \
			      POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,         \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(IQS323_INIT)

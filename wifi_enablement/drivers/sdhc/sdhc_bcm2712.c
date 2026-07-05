/*
 * Copyright 2026 matsuoka
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT brcm_bcm2712_sdhci

#include <zephyr/kernel.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(sdhc_bcm2712, CONFIG_SDHC_LOG_LEVEL);

/* Standard SDHCI Register Map */
#define SDHCI_DMA_ADDRESS	0x00
#define SDHCI_BLOCK_SIZE	0x04
#define SDHCI_BLOCK_COUNT	0x06
#define SDHCI_ARGUMENT		0x08
#define SDHCI_TRANSFER_MODE	0x0C
#define SDHCI_COMMAND		0x0E
#define SDHCI_RESPONSE		0x10
#define SDHCI_BUFFER_DATA_PORT	0x20
#define SDHCI_PRESENT_STATE	0x24
#define SDHCI_HOST_CONTROL	0x28
#define SDHCI_POWER_CONTROL	0x29
#define SDHCI_CLOCK_CONTROL	0x2C
#define SDHCI_TIMEOUT_CONTROL	0x2E
#define SDHCI_SOFTWARE_RESET	0x2F
#define SDHCI_INT_STATUS	0x30
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_SIGNAL_ENABLE	0x38
#define SDHCI_CAPABILITIES	0x40

/* Software Reset Bits */
#define SDHCI_RESET_ALL		0x01
#define SDHCI_RESET_CMD		0x02
#define SDHCI_RESET_DATA	0x04

struct sdhc_bcm2712_config {
	DEVICE_MMIO_ROM;
	uint32_t power_delay_ms;
};

struct sdhc_bcm2712_data {
	DEVICE_MMIO_RAM;
	struct sdhc_io host_io;
};

/* Register Read/Write Helpers */
static inline uint32_t sdhc_bcm2712_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void sdhc_bcm2712_write(const struct device *dev, uint32_t val, uint32_t reg)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + reg);
}

static int sdhc_bcm2712_reset_internal(const struct device *dev)
{
	int timeout = 1000;

	/* Issue Software Reset for all */
	sys_write8(SDHCI_RESET_ALL, DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET);

	/* Wait for reset completion */
	while (sys_read8(DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET) & SDHCI_RESET_ALL) {
		k_busy_wait(10);
		if (--timeout == 0) {
			LOG_ERR("SDHCI reset timeout");
			return -ETIMEDOUT;
		}
	}

	/* Enable Interrupt Status and Signal */
	sys_write32(0xFFFFFFFF, DEVICE_MMIO_GET(dev) + SDHCI_INT_ENABLE);
	sys_write32(0xFFFFFFFF, DEVICE_MMIO_GET(dev) + SDHCI_SIGNAL_ENABLE);

	LOG_DBG("SDHCI reset complete and interrupts enabled");
	return 0;
}

#define SDHCI_DIV_CLOCK_EN	BIT(0)
#define SDHCI_CLOCK_INT_STABLE	BIT(1)
#define SDHCI_CLOCK_CARD_EN	BIT(2)
#define SDHCI_CTRL_4BITBUS	0x02

#define SDIO_CFG_SD_PIN_SEL			0x444
#define  SDIO_CFG_SD_PIN_SEL_MASK		0x3
#define  SDIO_CFG_SD_PIN_SEL_SD			BIT(1)

static int sdhc_bcm2712_set_clk(const struct device *dev, uint32_t hz);

static void sdhc_bcm2712_recover_error(const struct device *dev, uint32_t int_status)
{
	int timeout = 1000;
	uint8_t reset_bit = 0;
	uint32_t pin_sel;
	struct sdhc_bcm2712_data *data = dev->data;

	/* Determine which reset is needed */
	if (int_status & (0xF << 16)) { /* Command errors: bits 19-16 */
		reset_bit |= 0x02; /* Reset CMD line (SDHCI_RESET_CMD) */
	}
	if (int_status & (0xF0 << 16)) { /* Data errors: bits 23-20 */
		reset_bit |= 0x04; /* Reset DAT line (SDHCI_RESET_DATA) */
	}

	if (reset_bit) {
		LOG_WRN("SDHCI error recovery reset: 0x%x", reset_bit);
		sys_write8(reset_bit, DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET);
		while (sys_read8(DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET) & reset_bit) {
			k_busy_wait(10);
			if (--timeout == 0) {
				LOG_ERR("SDHCI error recovery reset timeout");
				break;
			}
		}

		/* Re-apply BCM2712 specific config registers after soft reset */
		pin_sel = sys_read32(DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);
		pin_sel &= ~SDIO_CFG_SD_PIN_SEL_MASK;
		pin_sel |= SDIO_CFG_SD_PIN_SEL_SD;
		sys_write32(pin_sel, DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);

		/* Force fully re-configure the clock after soft reset to guarantee PLL lock */
		sdhc_bcm2712_set_clk(dev, data->host_io.clock);
	}
}

static int sdhc_bcm2712_request(const struct device *dev,
				struct sdhc_command *cmd,
				struct sdhc_data *data)
{
	int timeout = 10000;
	uint16_t cmd_reg = 0;
	uint32_t int_status;
	uint32_t resp_type;

	if (cmd == NULL) {
		LOG_ERR("Invalid command pointer");
		return -EINVAL;
	}

	resp_type = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;

	LOG_DBG("Sending CMD opcode: %d, arg: 0x%x, resp_type: %d", 
		cmd->opcode, cmd->arg, resp_type);

	/* 1. Wait for Command Inhibit (CMD) to clear */
	while (sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE) & BIT(0)) {
		k_busy_wait(10);
		if (--timeout == 0) {
			LOG_ERR("Command inhibit timeout");
			return -EBUSY;
		}
	}

	/* 2. Write argument */
	sdhc_bcm2712_write(dev, cmd->arg, SDHCI_ARGUMENT);

	/* 3. Setup command register bits */
	cmd_reg |= (cmd->opcode & 0x3f) << 8; /* Command index */

	/* Set response type bits based on native response type */
	switch (resp_type) {
	case SD_RSP_TYPE_NONE:
		cmd_reg |= 0x00; /* No response */
		break;
	case SD_RSP_TYPE_R2:
		cmd_reg |= 0x01; /* Long response (136 bits) */
		cmd_reg |= BIT(3); /* Enable CRC check */
		break;
	case SD_RSP_TYPE_R1b:
	case SD_RSP_TYPE_R5b:
		cmd_reg |= 0x03; /* Short response with busy */
		cmd_reg |= BIT(3); /* Enable CRC check */
		cmd_reg |= BIT(4); /* Enable Index check */
		break;
	case SD_RSP_TYPE_R1:
	case SD_RSP_TYPE_R5:
	case SD_RSP_TYPE_R6:
	case SD_RSP_TYPE_R7:
		cmd_reg |= 0x02; /* Short response */
		cmd_reg |= BIT(3); /* Enable CRC check */
		cmd_reg |= BIT(4); /* Enable Index check */
		break;
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:
		cmd_reg |= 0x02; /* Short response */
		/* CRC and Index checks are disabled for R3/R4 (OCR responses) */
		break;
	default:
		LOG_ERR("Unsupported response type: %d (orig: %d)", resp_type, cmd->response_type);
		return -EINVAL;
	}

	if (data != NULL) {
		cmd_reg |= BIT(5); /* Data present */
		/* TODO: Setup transfer mode and DMA if data transfer is required */
	}

	/* 4. Clear interrupt status for command complete and errors */
	sdhc_bcm2712_write(dev, 0xFFFF, SDHCI_INT_STATUS);

	/* 5. Issue the command */
	sys_write16(cmd_reg, DEVICE_MMIO_GET(dev) + SDHCI_COMMAND);

	/* 6. Wait for Command Complete (or error) */
	timeout = 20000;
	while (1) {
		int_status = sdhc_bcm2712_read(dev, SDHCI_INT_STATUS);
		if (int_status & 0xFFFF0000) {
			uint32_t err_status = int_status >> 16;
			/* Error occurred - check this FIRST */
			LOG_ERR("Command error: 0x%x (CMD%d)", err_status, cmd->opcode);
			/* Clear interrupt */
			sdhc_bcm2712_write(dev, int_status, SDHCI_INT_STATUS);
			/* Perform error recovery reset to unblock the command engine */
			sdhc_bcm2712_recover_error(dev, int_status);
			if (err_status & BIT(0)) {
				return -ETIMEDOUT;
			}
			return -EIO;
		}
		if (int_status & BIT(0)) {
			/* Command complete */
			break;
		}
		k_busy_wait(10);
		if (--timeout == 0) {
			uint32_t state = sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE);
			LOG_ERR("Command complete timeout. INT_STATUS: 0x%x, PRESENT_STATE: 0x%x", 
				int_status, state);
			return -ETIMEDOUT;
		}
	}

	/* 7. Clear command complete interrupt */
	sdhc_bcm2712_write(dev, BIT(0), SDHCI_INT_STATUS);

	/* 8. Get Response */
	if (cmd->response_type != SD_RSP_TYPE_NONE) {
		if (cmd->response_type == SD_RSP_TYPE_R2) {
			/* Long response: read RESP0-RESP3 (128 bits total) */
			cmd->response[0] = sdhc_bcm2712_read(dev, SDHCI_RESPONSE + 0x0C);
			cmd->response[1] = sdhc_bcm2712_read(dev, SDHCI_RESPONSE + 0x08);
			cmd->response[2] = sdhc_bcm2712_read(dev, SDHCI_RESPONSE + 0x04);
			cmd->response[3] = sdhc_bcm2712_read(dev, SDHCI_RESPONSE + 0x00);
		} else {
			/* Short response: read RESP0 */
			cmd->response[0] = sdhc_bcm2712_read(dev, SDHCI_RESPONSE);
		}
	}

	/* Wait for the card to stabilize its internal state machine after CMD0 */
	if (cmd->opcode == 0) {
		k_msleep(5);
	}

	return 0;
}

/* Macros moved to helper functions section */

static int sdhc_bcm2712_set_clk(const struct device *dev, uint32_t hz)
{
	uint16_t clk_reg;
	uint32_t div = 0;
	uint32_t caps;
	uint32_t base_clk;
	uint32_t pin_sel;
	int timeout = 1000;

	if (hz == 0) {
		/* Stop clock */
		sys_write16(0, DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL);
		return 0;
	}

	/* Select SD mode on BCM2712 SD Pin select register (Config Reg offset 0x44) */
	pin_sel = sys_read32(DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);
	pin_sel &= ~SDIO_CFG_SD_PIN_SEL_MASK;
	pin_sel |= SDIO_CFG_SD_PIN_SEL_SD;
	sys_write32(pin_sel, DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);

	/* Get base clock from Capabilities register */
	caps = sys_read32(DEVICE_MMIO_GET(dev) + SDHCI_CAPABILITIES);
	base_clk = ((caps >> 8) & 0xFF) * 1000000;
	if (base_clk == 0) {
		base_clk = 100000000; /* Fallback to 100MHz */
	}

	/* Calculate divisor */
	if (base_clk <= hz) {
		div = 0;
	} else {
		for (div = 2; div < 2046; div += 2) {
			if ((base_clk / div) <= hz) {
				break;
			}
		}
		div >>= 1;
	}

	/* For initialization clock (<= 400kHz), force lower speed (multiply divisor by 4)
	 * to prevent high actual speed if base clock is higher than reported.
	 */
	if (hz <= 400000 && hz > 0) {
		div *= 4;
		if (div > 2046) {
			div = 2046;
		}
	}

	/* Stop SD clock output before changing speed */
	clk_reg = sys_read16(DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL);
	clk_reg &= ~SDHCI_CLOCK_CARD_EN;
	sys_write16(clk_reg, DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL);

	/* Set divisor and enable internal clock */
	clk_reg = ((div & 0xFF) << 8) | (((div & 0x300) >> 8) << 6);
	clk_reg |= SDHCI_DIV_CLOCK_EN;
	sys_write16(clk_reg, DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL);

	/* Wait for internal clock stable */
	while (!(sys_read16(DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE)) {
		k_busy_wait(10);
		if (--timeout == 0) {
			LOG_ERR("SDHCI clock stabilization timeout");
			return -ETIMEDOUT;
		}
	}

	/* Enable SD card clock output */
	clk_reg |= SDHCI_CLOCK_CARD_EN;
	sys_write16(clk_reg, DEVICE_MMIO_GET(dev) + SDHCI_CLOCK_CONTROL);

	/* Wait for SD clock to stabilize (minimum 74 cycles, ~185us at 400kHz) */
	k_msleep(2);

	LOG_DBG("Base clock: %d Hz, set clock to %d Hz (div: %d), reg: 0x%x",
		base_clk, hz, div, clk_reg);
	return 0;
}

static int sdhc_bcm2712_set_bus_width(const struct device *dev, enum sdhc_bus_width width)
{
	uint8_t ctrl;

	ctrl = sys_read8(DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL);
	if (width == SDHC_BUS_WIDTH4BIT) {
		ctrl |= SDHCI_CTRL_4BITBUS;
	} else if (width == SDHC_BUS_WIDTH1BIT) {
		ctrl &= ~SDHCI_CTRL_4BITBUS;
	} else {
		LOG_ERR("Unsupported bus width: %d", width);
		return -ENOTSUP;
	}
	sys_write8(ctrl, DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL);
	LOG_DBG("Bus width set to %d bit, reg: 0x%x", width == SDHC_BUS_WIDTH4BIT ? 4 : 1, ctrl);
	return 0;
}

#define SDHCI_POWER_ON_BIT	0x01
#define SDHCI_POWER_180		0x0A
#define SDHCI_POWER_300		0x0C
#define SDHCI_POWER_330		0x0E
#define SDHCI_HOST_CONTROL2	0x3E
#define  SDHCI_CTRL_18V_SIG_EN	BIT(3)

static int sdhc_bcm2712_set_power(const struct device *dev, struct sdhc_io *ios)
{
	uint8_t pwr = 0;
	uint16_t ctrl2;

	if (ios->power_mode == SDHC_POWER_OFF) {
		sys_write8(0, DEVICE_MMIO_GET(dev) + SDHCI_POWER_CONTROL);
		/* Clear 1.8V signal enable */
		ctrl2 = sys_read16(DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL2);
		ctrl2 &= ~SDHCI_CTRL_18V_SIG_EN;
		sys_write16(ctrl2, DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL2);
		LOG_DBG("Power set to OFF");
		return 0;
	}

	/* Select voltage based on host I/O request */
	switch (ios->signal_voltage) {
	case SD_VOL_1_8_V:
		pwr = SDHCI_POWER_180;
		break;
	case SD_VOL_3_0_V:
		pwr = SDHCI_POWER_300;
		break;
	case SD_VOL_3_3_V:
	default:
		pwr = SDHCI_POWER_330;
		break;
	}

	/* Set voltage and enable bus power */
	sys_write8(pwr, DEVICE_MMIO_GET(dev) + SDHCI_POWER_CONTROL);
	sys_write8(pwr | SDHCI_POWER_ON_BIT, DEVICE_MMIO_GET(dev) + SDHCI_POWER_CONTROL);

	/* Configure 1.8V signaling bit in Host Control 2 accordingly */
	ctrl2 = sys_read16(DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL2);
	if (ios->signal_voltage == SD_VOL_1_8_V) {
		ctrl2 |= SDHCI_CTRL_18V_SIG_EN;
	} else {
		ctrl2 &= ~SDHCI_CTRL_18V_SIG_EN;
	}
	sys_write16(ctrl2, DEVICE_MMIO_GET(dev) + SDHCI_HOST_CONTROL2);

	/* Wait for power stabilization (at least 20ms) */
	k_msleep(20);

	LOG_DBG("Power set to ON, reg: 0x%x, ctrl2: 0x%x (voltage req: %d)", 
		pwr | SDHCI_POWER_ON_BIT, ctrl2, ios->signal_voltage);
	return 0;
}

static int sdhc_bcm2712_set_io(const struct device *dev, struct sdhc_io *ios)
{
	struct sdhc_bcm2712_data *data = dev->data;
	int ret;

	LOG_DBG("Set IO: bus width %d, clock %d Hz", ios->bus_width, ios->clock);

	/* 1. Configure power */
	ret = sdhc_bcm2712_set_power(dev, ios);
	if (ret) {
		return ret;
	}

	/* 2. Configure clock */
	ret = sdhc_bcm2712_set_clk(dev, ios->clock);
	if (ret) {
		return ret;
	}

	/* 3. Configure bus width */
	ret = sdhc_bcm2712_set_bus_width(dev, ios->bus_width);
	if (ret) {
		return ret;
	}

	data->host_io.bus_width = ios->bus_width;
	data->host_io.clock = ios->clock;

	return 0;
}

static int sdhc_bcm2712_get_host_props(const struct device *dev,
				       struct sdhc_host_props *props)
{
	const struct sdhc_bcm2712_config *config = dev->config;

	memset(props, 0, sizeof(struct sdhc_host_props));
	props->f_min = SDMMC_CLOCK_400KHZ;
	props->f_max = SD_CLOCK_25MHZ;
	props->power_delay = config->power_delay_ms;
	props->host_caps.vol_330_support = true;
	props->is_spi = false;

	return 0;
}

static int sdhc_bcm2712_get_card_present(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* For SDIO on RPi5, WiFi chip is always connected */
	return 1;
}

static int sdhc_bcm2712_reset(const struct device *dev)
{
	LOG_DBG("Resetting BCM2712 SDHost");
	return sdhc_bcm2712_reset_internal(dev);
}

static int sdhc_bcm2712_card_busy(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* TODO: Check DAT line status */
	return 0;
}

static int sdhc_bcm2712_init(const struct device *dev)
{
	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	printk("BCM2712 SDHost driver init at 0x%lx\n",
		(unsigned long)DEVICE_MMIO_GET(dev));

	return sdhc_bcm2712_reset_internal(dev);
}

static DEVICE_API(sdhc, sdhc_bcm2712_api) = {
	.request = sdhc_bcm2712_request,
	.set_io = sdhc_bcm2712_set_io,
	.get_host_props = sdhc_bcm2712_get_host_props,
	.get_card_present = sdhc_bcm2712_get_card_present,
	.reset = sdhc_bcm2712_reset,
	.card_busy = sdhc_bcm2712_card_busy,
};

#define SDHC_BCM2712_INIT(inst)							\
	static const struct sdhc_bcm2712_config sdhc_bcm2712_config_##inst = {	\
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),			\
		.power_delay_ms = DT_INST_PROP_OR(inst, power_delay_ms, 10),	\
	};									\
										\
	static struct sdhc_bcm2712_data sdhc_bcm2712_data_##inst;		\
										\
	DEVICE_DT_INST_DEFINE(inst,						\
			      sdhc_bcm2712_init,				\
			      NULL,						\
			      &sdhc_bcm2712_data_##inst,			\
			      &sdhc_bcm2712_config_##inst,			\
			      POST_KERNEL,					\
			      CONFIG_SDHC_INIT_PRIORITY,			\
			      &sdhc_bcm2712_api);

DT_INST_FOREACH_STATUS_OKAY(SDHC_BCM2712_INIT)

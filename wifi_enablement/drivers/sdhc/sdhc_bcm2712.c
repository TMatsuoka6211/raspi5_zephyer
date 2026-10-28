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
#include <zephyr/sys/device_mmio.h>

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

	/* Set Data Timeout Control to maximum (0x0E = 2^27 TMCLK cycles) */
	sys_write8(0x0E, DEVICE_MMIO_GET(dev) + SDHCI_TIMEOUT_CONTROL);

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

	/* Determine which reset is needed */
	if (int_status & (0xF << 16)) { /* Command errors: bits 19-16 */
		reset_bit |= 0x02; /* Reset CMD line (SDHCI_RESET_CMD) */
	}
	if (int_status & (0xF0 << 16)) { /* Data errors: bits 23-20 */
		reset_bit |= 0x04; /* Reset DAT line (SDHCI_RESET_DATA) */
	}


	/* If no specific error bit matched (e.g. timeout fallback), reset both CMD and DAT lines */
	if (reset_bit == 0) {
		reset_bit = 0x06;
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
		/* Do not set SDIO_CFG_SD_PIN_SEL_SD for non-removable SDIO */
		sys_write32(pin_sel, DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);

		/* Force fully re-configure the clock after soft reset to guarantee PLL lock */
		/* sdhc_bcm2712_set_clk(dev, data->host_io.clock); */
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

	/* 1. Wait for Command Inhibit (CMD and DAT) and DAT Line Active to clear */
	while (sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE) & (BIT(0) | BIT(1) | BIT(2))) {
		k_busy_wait(10);
		if (--timeout == 0) {
			/* If DAT line is stuck busy, issue software reset for CMD & DAT to unblock */
			sys_write8(0x06, DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET);
			int r_timeout = 1000;
			while ((sys_read8(DEVICE_MMIO_GET(dev) + SDHCI_SOFTWARE_RESET) & 0x06) && --r_timeout) {
				k_busy_wait(10);
			}
			uint32_t state = sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE);
			if (state & (BIT(0) | BIT(1) | BIT(2))) {
				printf("SDHC: Command inhibit timeout (CMD%d arg 0x%x PRESENT_STATE: 0x%x)\n",
					cmd->opcode, cmd->arg, state);
				fflush(stdout);
				LOG_ERR("Command inhibit timeout");
				return -EBUSY;
			}
			break;
		}
	}

	/* 2. Setup command register bits */
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

	bool is_read = false;
	uint16_t transfer_mode = 0;

	if (data != NULL) {
		cmd_reg |= BIT(5); /* Data present */

		/* Determine data transfer direction:
		 * For CMD53 (SDIO), bit 31 of argument is R/W flag (1: write, 0: read).
		 * For standard SD read commands: CMD17, CMD18.
		 */
		if (cmd->opcode == 53) {
			is_read = ((cmd->arg & BIT(31)) == 0);
		} else if (cmd->opcode == 17 || cmd->opcode == 18) {
			is_read = true;
		}

		/* 1. Setup Block Size and Block Count */
		sys_write16(data->block_size, DEVICE_MMIO_GET(dev) + SDHCI_BLOCK_SIZE);
		sys_write16(data->blocks, DEVICE_MMIO_GET(dev) + SDHCI_BLOCK_COUNT);

		/* 2. Setup Transfer Mode */
		if (is_read) {
			transfer_mode |= BIT(4); /* Data Transfer Direction: Read (1) */
		}
		/* Enable Block Count for all data transfers (Single-block, multi-block, and SDIO byte mode) */
		transfer_mode |= BIT(1); /* Block Count Enable */
		if (data->blocks > 1) {
			transfer_mode |= BIT(5); /* Multi-block */
		}
		/* Set Data Timeout Control to maximum (0x0E) */
		sys_write8(0x0E, DEVICE_MMIO_GET(dev) + SDHCI_TIMEOUT_CONTROL);
	}

	/* 3. Clear all normal and error interrupt status */
	sdhc_bcm2712_write(dev, 0xFFFFFFFF, SDHCI_INT_STATUS);

	/* 4. Write argument */
	sdhc_bcm2712_write(dev, cmd->arg, SDHCI_ARGUMENT);

	/* 5. Issue Command & Transfer Mode as combined 32-bit register */
	uint32_t cmd_and_mode = (cmd_reg << 16) | transfer_mode;
	sdhc_bcm2712_write(dev, cmd_and_mode, SDHCI_TRANSFER_MODE);

	/* 6. Wait for Command Complete (or error) */
	timeout = 20000;
	while (1) {
		int_status = sdhc_bcm2712_read(dev, SDHCI_INT_STATUS);
		uint32_t err_mask = 0x000F0000; /* Only check Command Errors (bits 16-19: CMD Timeout/CRC/EndBit/Index) */
		if (int_status & err_mask) {
			uint32_t err_status = (int_status & err_mask) >> 16;
			/* Error occurred - check this FIRST */
			printf("SDHC: Command error: 0x%x (CMD%d arg 0x%x, INT_STATUS=0x%x, PRESENT_STATE=0x%x)\n",
			       err_status, cmd->opcode, cmd->arg, int_status, sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE));
			fflush(stdout);
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
			printf("SDHC: Command complete timeout. INT_STATUS: 0x%x, PRESENT_STATE: 0x%x\n", 
				int_status, state);
			fflush(stdout);
			LOG_ERR("Command complete timeout. INT_STATUS: 0x%x, PRESENT_STATE: 0x%x", 
				int_status, state);
			/* Perform error recovery reset to unblock the command engine */
			sdhc_bcm2712_recover_error(dev, 0x00020000);
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

	/* 9. Handle Data Transfer (PIO via Buffer Data Port 0x20) */
	if (data != NULL) {
		uint8_t *buf = (uint8_t *)data->data;
		uint32_t bytes_done = 0;

		for (uint32_t b = 0; b < data->blocks; b++) {
			uint32_t block_bytes = data->block_size;
			uint32_t words = (block_bytes + 3) / 4;

			if (is_read) {
				/* Wait for Buffer Read Ready (BIT 5 in INT_STATUS) or Transfer Complete (BIT 1) */
				timeout = 20000;
				while (1) {
					uint32_t status = sdhc_bcm2712_read(dev, SDHCI_INT_STATUS);
					if (status & (BIT(5) | BIT(1))) {
						sdhc_bcm2712_write(dev, BIT(5) | BIT(1), SDHCI_INT_STATUS);
						break;
					}
					if (b == 0 && (sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE) & BIT(11))) {
						sdhc_bcm2712_write(dev, BIT(5), SDHCI_INT_STATUS);
						break;
					}
					if (status & 0xFFFF0000) {
						printf("SDHC: Data read error: 0x%x (CMD%d arg 0x%x, INT:0x%x STATE:0x%x)\n",
						       status >> 16, cmd->opcode, cmd->arg, status, sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE));
						fflush(stdout);
						LOG_ERR("Data read error: 0x%x", status >> 16);
						sdhc_bcm2712_write(dev, status, SDHCI_INT_STATUS);
						sdhc_bcm2712_recover_error(dev, status);
						return -EIO;
					}
					k_busy_wait(10);
					if (--timeout == 0) {
						uint32_t state = sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE);
						printf("SDHC: Buffer Read Ready timeout (CMD%d arg 0x%x INT:0x%x STATE:0x%x)\n",
							cmd->opcode, cmd->arg, status, state);
						fflush(stdout);
						LOG_ERR("Buffer Read Ready timeout (CMD%d arg 0x%x INT:0x%x STATE:0x%x)",
							cmd->opcode, cmd->arg, status, state);
						sdhc_bcm2712_recover_error(dev, status ? status : 0x00200000);
						return -ETIMEDOUT;
					}
				}

				/* Read words from SDHCI_BUFFER_DATA_PORT */
				for (uint32_t w = 0; w < words; w++) {
					uint32_t val = sys_read32(DEVICE_MMIO_GET(dev) + SDHCI_BUFFER_DATA_PORT);
					uint32_t rem = block_bytes - (w * 4);
					uint32_t chunk = (rem < 4) ? rem : 4;
					memcpy(buf + bytes_done, &val, chunk);
					bytes_done += chunk;
				}
			} else {
				/* Wait for Buffer Write Ready (BIT 4 in INT_STATUS) or Buffer Write Enable (BIT 10 in PRESENT_STATE for block 0) */
				timeout = 20000;
				while (1) {
					uint32_t status = sdhc_bcm2712_read(dev, SDHCI_INT_STATUS);
					if (status & BIT(4)) {
						sdhc_bcm2712_write(dev, BIT(4), SDHCI_INT_STATUS);
						break;
					}
					if (b == 0 && (sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE) & BIT(10))) {
						sdhc_bcm2712_write(dev, BIT(4), SDHCI_INT_STATUS);
						break;
					}
					if (status & 0xFFFF0000) {
						printf("SDHC: Data write error: 0x%x (CMD%d arg 0x%x, INT:0x%x STATE:0x%x)\n",
						       status >> 16, cmd->opcode, cmd->arg, status, sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE));
						fflush(stdout);
						LOG_ERR("Data write error: 0x%x", status >> 16);
						sdhc_bcm2712_write(dev, status, SDHCI_INT_STATUS);
						sdhc_bcm2712_recover_error(dev, status);
						return -EIO;
					}
					k_busy_wait(10);
					if (--timeout == 0) {
						uint32_t state = sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE);
						printf("SDHC: Buffer Write Ready timeout (CMD%d arg 0x%x blk:%d/%d INT:0x%x STATE:0x%x)\n",
							cmd->opcode, cmd->arg, b, data->blocks, status, state);
						fflush(stdout);
						LOG_ERR("Buffer Write Ready timeout (CMD%d arg 0x%x blk:%d/%d INT:0x%x STATE:0x%x)",
							cmd->opcode, cmd->arg, b, data->blocks, status, state);
						return -ETIMEDOUT;
					}
				}

				/* Write words to SDHCI_BUFFER_DATA_PORT */
				for (uint32_t w = 0; w < words; w++) {
					uint32_t val = 0;
					uint32_t rem = block_bytes - (w * 4);
					uint32_t chunk = (rem < 4) ? rem : 4;
					memcpy(&val, buf + bytes_done, chunk);
					sys_write32(val, DEVICE_MMIO_GET(dev) + SDHCI_BUFFER_DATA_PORT);
					bytes_done += chunk;
				}
			}
		}

		/* Wait for Transfer Complete (BIT 1 in INT_STATUS) or all DAT active/inhibit bits to clear */
		timeout = 20000;
		while (1) {
			uint32_t status = sdhc_bcm2712_read(dev, SDHCI_INT_STATUS);
			if (status & BIT(1)) {
				/* Clear Transfer Complete */
				sdhc_bcm2712_write(dev, BIT(1) | BIT(4) | BIT(5), SDHCI_INT_STATUS);
				break;
			}
			if (status & 0xFFFF0000) {
				printf("SDHC: Data transfer error: 0x%x (CMD%d arg 0x%x, INT:0x%x STATE:0x%x)\n",
					status >> 16, cmd->opcode, cmd->arg, status, sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE));
				fflush(stdout);
				sdhc_bcm2712_write(dev, status, SDHCI_INT_STATUS);
				sdhc_bcm2712_recover_error(dev, status);
				return -EIO;
			}
			uint32_t state = sdhc_bcm2712_read(dev, SDHCI_PRESENT_STATE);
			/* Check Command Inhibit DAT (BIT 1), DAT Line Active (BIT 2), Write Active (BIT 8), Read Active (BIT 9) are all 0, and DAT0 (BIT 20) is HIGH */
			if ((state & (BIT(1) | BIT(2) | BIT(8) | BIT(9))) == 0 && (state & BIT(20)) != 0) {
				sdhc_bcm2712_write(dev, BIT(1) | BIT(4) | BIT(5), SDHCI_INT_STATUS);
				break;
			}
			k_busy_wait(5);
			if (--timeout == 0) {
				printf("SDHC: Transfer Complete timeout (CMD%d arg 0x%x read:%d blks:%d sz:%d done:%d INT:0x%x STATE:0x%x)\n",
					cmd->opcode, cmd->arg, is_read, data->blocks, data->block_size,
					bytes_done, status, state);
				fflush(stdout);
				/* Perform error recovery reset to unblock the data engine (0x00100000 = Data Timeout) */
				sdhc_bcm2712_recover_error(dev, 0x00100000);
				return -ETIMEDOUT;
			}
		}
		data->bytes_xfered = bytes_done;
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
	/* Do not set SDIO_CFG_SD_PIN_SEL_SD for non-removable SDIO */
	sys_write32(pin_sel, DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);

	/* Get base clock (BCM2712 SDIO base clock is 200MHz) */
	caps = sys_read32(DEVICE_MMIO_GET(dev) + SDHCI_CAPABILITIES);
	base_clk = ((caps >> 8) & 0xFF) * 1000000;
	if (base_clk == 0 || base_clk < 200000000) {
		base_clk = 200000000; /* BCM2712 SDIO base clock is 200MHz */
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

	uint8_t pwr_read = sys_read8(DEVICE_MMIO_GET(dev) + SDHCI_POWER_CONTROL);
	LOG_DBG("Power set to ON, reg: 0x%x (read: 0x%x), ctrl2: 0x%x (voltage req: %d)", 
		pwr | SDHCI_POWER_ON_BIT, pwr_read, ctrl2, ios->signal_voltage);
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
	props->f_max = 12500000; /* Test with 12.5MHz for robust SDIO signal timing */
	props->power_delay = config->power_delay_ms;
	props->host_caps.vol_180_support = true;
	props->host_caps.vol_330_support = false;
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

static void bcm2712_configure_pinctrl(void)
{
	mm_reg_t pinctrl_base;
	uintptr_t phys_addr = 0x107d504100;
	size_t size = 0x20;

	device_map(&pinctrl_base, phys_addr, size, K_MEM_CACHE_NONE);

	printk("Mapped BCM2712 pinctrl at 0x%lx\n", (unsigned long)pinctrl_base);

	/* 1. Configure Function Select (Mux) registers for BCM2712 D0:
	 * Offset 0x08 controls Pin 28, 29, 30, 31:
	 *   Pin 28: bits 19-16 -> 0 (GPIO)
	 *   Pin 29: bits 23-20 -> 0 (GPIO)
	 *   Pin 30: bits 27-24 -> 1 (sd2)
	 *   Pin 31: bits 31-28 -> 1 (sd2)
	 */
	uint32_t val = sys_read32(pinctrl_base + 0x08);
	val &= 0x0000FFFF;
	val |= 0x11000000;
	sys_write32(val, pinctrl_base + 0x08);

	/* Offset 0x0C controls Pin 32, 33, 34, 35:
	 *   Pin 32-35: bits 15-0 -> 1 (sd2)
	 */
	val = sys_read32(pinctrl_base + 0x0C);
	val &= 0xFFFF0000;
	val |= 0x1111;
	sys_write32(val, pinctrl_base + 0x0C);

	/* 2. Configure Pull (Bias) registers for BCM2712 D0:
	 * Offset 0x14 controls Pin 28-32 pull-up/pull-down:
	 *   Pin 31, 32 -> 2 (Pull-Up), Pin 30 (CLK) -> 0 (None)
	 */
	val = sys_read32(pinctrl_base + 0x14);
	val &= 0x000FFFFF;
	val |= 0x28000000;
	sys_write32(val, pinctrl_base + 0x14);

	/* Offset 0x18 controls Pin 33-35 pull-up/pull-down:
	 *   Pin 33-35 -> 2 (Pull-Up)
	 */
	val = sys_read32(pinctrl_base + 0x18);
	val &= 0xFFFFFFC0;
	val |= 0x2A;
	sys_write32(val, pinctrl_base + 0x18);

	printk("BCM2712 SDIO pinctrl configured successfully in SDHC driver init!\n");
}

static int sdhc_bcm2712_init(const struct device *dev)
{
	uint32_t pin_sel;

	bcm2712_configure_pinctrl();

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	printk("BCM2712 SDHost driver init at 0x%lx\n",
		(unsigned long)DEVICE_MMIO_GET(dev));

	/* Log the initial value of SDIO_CFG_SD_PIN_SEL register */
	pin_sel = sys_read32(DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);
	printk("Initial SDIO_CFG_SD_PIN_SEL: 0x%x\n", pin_sel);

	/* Ensure SDIO_CFG_SD_PIN_SEL is set to 1 for slot 1 */
	sys_write32(0x1, DEVICE_MMIO_GET(dev) + SDIO_CFG_SD_PIN_SEL);

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

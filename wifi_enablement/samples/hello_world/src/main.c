#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>

struct sd_card g_sd_card;

void configure_bcm2712_pinctrl(void)
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
	 *   Value to write: mask out 16-31 and write (1 << 24) | (1 << 28) = 0x11000000
	 */
	uint32_t val = sys_read32(pinctrl_base + 0x08);
	val &= 0x0000FFFF; /* Clear bits 16-31 */
	val |= 0x11000000; /* Write sd2 to pins 30, 31, GPIO to 28, 29 */
	sys_write32(val, pinctrl_base + 0x08);

	/* Offset 0x0C controls Pin 32, 33, 34, 35:
	 *   Pin 32: bits 3-0 -> 1 (sd2)
	 *   Pin 33: bits 7-4 -> 1 (sd2)
	 *   Pin 34: bits 11-8 -> 1 (sd2)
	 *   Pin 35: bits 15-12 -> 1 (sd2)
	 *   Value to write: mask out 0-15 and write 1 | (1 << 4) | (1 << 8) | (1 << 12) = 0x1111
	 */
	val = sys_read32(pinctrl_base + 0x0C);
	val &= 0xFFFF0000; /* Clear bits 0-15 */
	val |= 0x1111;     /* Write sd2 to pins 32, 33, 34, 35 */
	sys_write32(val, pinctrl_base + 0x0C);

	/* 2. Configure Pull (Bias) registers for BCM2712 D0:
	 * Offset 0x14 controls Pin 28-32 pull-up/pull-down:
	 *   Pin 28: bits 21-20 -> 0 (None)
	 *   Pin 29: bits 23-22 -> 0 (None)
	 *   Pin 30: bits 25-24 -> 0 (None)
	 *   Pin 31: bits 27-26 -> 2 (Pull-Up) -> 2 << 26 = 0x08000000
	 *   Pin 32: bits 29-28 -> 2 (Pull-Up) -> 2 << 28 = 0x20000000
	 *   Value to write: mask out bits 20-31 and write 0x28000000
	 */
	val = sys_read32(pinctrl_base + 0x14);
	val &= ~0xFFF00000; /* Clear bits 20-31 */
	val |= 0x28000000;  /* Set Pin 31, 32 to Pull-Up (2), Pin 30 to None (0) */
	sys_write32(val, pinctrl_base + 0x14);

	/* Offset 0x18 controls Pin 33-35 pull-up/pull-down:
	 *   Pin 33: bits 1-0 -> 2 (Pull-Up) -> 2
	 *   Pin 34: bits 3-2 -> 2 (Pull-Up) -> 2 << 2 = 8
	 *   Pin 35: bits 5-4 -> 2 (Pull-Up) -> 2 << 4 = 0x20
	 *   Value to write: mask out bits 0-5 and write 2 | 8 | 0x20 = 0x2A
	 */
	val = sys_read32(pinctrl_base + 0x18);
	val &= ~0x0000003F; /* Clear bits 0-5 */
	val |= 0x0000002A;  /* Set Pin 33-35 to Pull-Up (2) */
	sys_write32(val, pinctrl_base + 0x18);

	printk("BCM2712 SDIO pinctrl registers written successfully!\n");

	device_unmap(pinctrl_base, size);
}

/* --- Zephyr Shell command handlers --- */

static int cmd_sdio_pinctrl(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Configuring BCM2712 SDIO Pin Muxing...");
	configure_bcm2712_pinctrl();
	shell_print(sh, "Pin Muxing configuration completed successfully.");
	return 0;
}

static int cmd_sdio_power(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gio));

	if (!device_is_ready(gpio_dev)) {
		shell_error(sh, "GPIO device not ready!");
		return -ENODEV;
	}

	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_configure(gpio_dev, 28, GPIO_OUTPUT_ACTIVE);
		shell_print(sh, "WL_ON (GPIO 28) set to HIGH (WiFi Power ON).");
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_configure(gpio_dev, 28, GPIO_OUTPUT_INACTIVE);
		shell_print(sh, "WL_ON (GPIO 28) set to LOW (WiFi Power OFF).");
	} else {
		shell_error(sh, "Usage: sdio power <on|off>");
		return -EINVAL;
	}
	return 0;
}

static int cmd_sdio_init(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *const sdhc_dev = DEVICE_DT_GET(DT_NODELABEL(sdhci1));
	const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gio));

	if (!device_is_ready(sdhc_dev)) {
		shell_error(sh, "SDHC device not ready!");
		return -ENODEV;
	}

	if (device_is_ready(gpio_dev)) {
		shell_print(sh, "Power cycling WiFi chip (WL_ON / GPIO 28)...");
		/* Power OFF */
		gpio_pin_configure(gpio_dev, 28, GPIO_OUTPUT_INACTIVE);
		k_msleep(100);
		/* Re-configure BCM2712 pinctrl */
		configure_bcm2712_pinctrl();
		/* Power ON */
		gpio_pin_configure(gpio_dev, 28, GPIO_OUTPUT_ACTIVE);
		k_msleep(150);
	}

	shell_print(sh, "Initializing SD/SDIO Card...");
	int ret = sd_init(sdhc_dev, &g_sd_card);
	if (ret < 0) {
		shell_error(sh, "sd_init failed: %d", ret);
		return ret;
	}

	shell_print(sh, "sd_init succeeded! Card Status: %d, Card Type: %d (1: SDIO, 0: SDMMC)",
				g_sd_card.status, g_sd_card.type);
	return 0;
}

static int cmd_sdio_read(const struct shell *sh, size_t argc, char **argv)
{
	if (g_sd_card.status != CARD_INITIALIZED) {
		shell_error(sh, "Card is not initialized. Run 'sdio init' first.");
		return -EAGAIN;
	}

	uint8_t func_num = strtoul(argv[1], NULL, 0);
	uint32_t addr = strtoul(argv[2], NULL, 0);
	uint8_t val = 0;

	struct sdio_func func = {
		.num = func_num,
		.card = &g_sd_card
	};

	int ret = sdio_read_byte(&func, addr, &val);
	if (ret < 0) {
		shell_error(sh, "sdio_read_byte failed: %d", ret);
		return ret;
	}

	shell_print(sh, "Read Success: Func %d, Addr 0x%04X = 0x%02X", func_num, addr, val);
	return 0;
}

static int cmd_sdio_write(const struct shell *sh, size_t argc, char **argv)
{
	if (g_sd_card.status != CARD_INITIALIZED) {
		shell_error(sh, "Card is not initialized. Run 'sdio init' first.");
		return -EAGAIN;
	}

	uint8_t func_num = strtoul(argv[1], NULL, 0);
	uint32_t addr = strtoul(argv[2], NULL, 0);
	uint8_t val = strtoul(argv[3], NULL, 0);
	uint8_t resp = 0;

	struct sdio_func func = {
		.num = func_num,
		.card = &g_sd_card
	};

	int ret = sdio_rw_byte(&func, addr, val, &resp);
	if (ret < 0) {
		shell_error(sh, "sdio_rw_byte failed: %d", ret);
		return ret;
	}

	shell_print(sh, "Write Success: Func %d, Addr 0x%04X <- 0x%02X (Response: 0x%02X)",
				func_num, addr, val, resp);
	return 0;
}

/* Register subcommands and root command 'sdio' */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sdio,
	SHELL_CMD(pinctrl, NULL, "Configure BCM2712 SDIO Pin Muxing", cmd_sdio_pinctrl),
	SHELL_CMD_ARG(power, NULL, "Set WL_ON (GPIO 28) <on/off>", cmd_sdio_power, 2, 0),
	SHELL_CMD(init, NULL, "Initialize SDIO WiFi Card", cmd_sdio_init),
	SHELL_CMD_ARG(read, NULL, "Read SDIO register (CMD52) <func_num> <addr>", cmd_sdio_read, 3, 0),
	SHELL_CMD_ARG(write, NULL, "Write SDIO register (CMD52) <func_num> <addr> <val>", cmd_sdio_write, 4, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sdio, &sub_sdio, "BCM2712 SDIO Control Commands", NULL);

int main(void)
{
	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);
	return 0;
}


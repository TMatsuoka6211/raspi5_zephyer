/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd.h>

#include <zephyr/drivers/gpio.h>

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

int main(void)
{
	configure_bcm2712_pinctrl();
	const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gio));

	if (!device_is_ready(gpio_dev)) {
		printf("Error: GPIO controller (gio) is not ready!\n");
	} else {
		/* Configure GPIO 28 (WL_ON) as output and set it HIGH to power up WiFi */
		int ret = gpio_pin_configure(gpio_dev, 28, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			printf("Error: failed to configure WL_ON (GPIO 28): %d\n", ret);
		} else {
			printf("Successfully configured WL_ON (GPIO 28) to HIGH. Waiting 150ms...\n");
			k_msleep(150);
		}
	}

	/* Get SDHC device */
	const struct device *const sdhc_dev = DEVICE_DT_GET(DT_NODELABEL(sdhci1));

	if (!device_is_ready(sdhc_dev)) {
		printf("Error: SDHC device is not ready!\n");
	} else {
		printf("SDHC device is ready. Initializing SD/SDIO Card...\n");
		int ret = sd_init(sdhc_dev, &g_sd_card);
		if (ret < 0) {
			printf("sd_init failed: %d\n", ret);
		} else {
			printf("sd_init succeeded! Card status: %d, Card type: %d\n",
				g_sd_card.status, g_sd_card.type);
		}
	}

	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);

	return 0;
}

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

struct sd_card g_sd_card;

int main(void)
{
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



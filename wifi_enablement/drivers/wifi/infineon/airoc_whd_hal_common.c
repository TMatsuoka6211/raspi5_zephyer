/*
 * Copyright (c) 2023 Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "airoc_whd_hal_common.h"
#include "airoc_wifi.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>

LOG_MODULE_DECLARE(infineon_airoc_wifi, CONFIG_WIFI_LOG_LEVEL);

static void airoc_configure_bcm2712_pinctrl(void)
{
	mm_reg_t pinctrl_base;
	uintptr_t phys_addr = 0x107d504100ULL;
	size_t size = 0x20;

	device_map(&pinctrl_base, phys_addr, size, K_MEM_CACHE_NONE);

	uint32_t val = sys_read32(pinctrl_base + 0x08);
	val &= 0x0000FFFF;
	val |= 0x11000000; /* Pins 30, 31 = sd2; Pins 28, 29 = GPIO */
	sys_write32(val, pinctrl_base + 0x08);

	val = sys_read32(pinctrl_base + 0x0C);
	val &= 0xFFFF0000;
	val |= 0x00001111; /* Pins 32-35 = sd2 */
	sys_write32(val, pinctrl_base + 0x0C);

	val = sys_read32(pinctrl_base + 0x14);
	val &= ~0xFFF00000;
	val |= 0x28000000; /* Pin 31, 32 = Pull-Up, Pin 30 = None */
	sys_write32(val, pinctrl_base + 0x14);

	val = sys_read32(pinctrl_base + 0x18);
	val &= ~0x0000003F;
	val |= 0x0000002A; /* Pins 33-35 = Pull-Up */
	sys_write32(val, pinctrl_base + 0x18);

	device_unmap(pinctrl_base, size);
}

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************
 *                 Function
 ******************************************************/

int airoc_wifi_power_on(const struct device *dev)
{
#if DT_INST_NODE_HAS_PROP(0, wifi_reg_on_gpios)
	int ret;
	const struct airoc_wifi_config *config = dev->config;

	printk("AIROC: Power on checking wifi_reg_on port: %s pin %d\n",
		config->wifi_reg_on_gpio.port ? config->wifi_reg_on_gpio.port->name : "NULL",
		config->wifi_reg_on_gpio.pin);

	airoc_configure_bcm2712_pinctrl();

	/* Check WIFI REG_ON gpio instance */
	if (!device_is_ready(config->wifi_reg_on_gpio.port)) {
		printk("AIROC Error: gpio port %s is not ready!\n",
			config->wifi_reg_on_gpio.port ? config->wifi_reg_on_gpio.port->name : "NULL");
		LOG_ERR("Error: failed to configure wifi_reg_on %s pin %d",
			config->wifi_reg_on_gpio.port->name, config->wifi_reg_on_gpio.pin);
		return -EIO;
	}

	/* Configure wifi_reg_on as output  */
	ret = gpio_pin_configure_dt(&config->wifi_reg_on_gpio, GPIO_OUTPUT);
	if (ret) {
		printk("AIROC Error: gpio_pin_configure_dt failed: %d\n", ret);
		LOG_ERR("Error %d: failed to configure wifi_reg_on %s pin %d", ret,
			config->wifi_reg_on_gpio.port->name, config->wifi_reg_on_gpio.pin);
		return ret;
	}
	ret = gpio_pin_set_dt(&config->wifi_reg_on_gpio, 0);
	if (ret) {
		printk("AIROC Error: gpio_pin_set_dt(0) failed: %d\n", ret);
		return ret;
	}

	printk("AIROC: Power cycling WL_ON (LOW for %d ms)...\n", WLAN_CBUCK_DISCHARGE_MS);
	/* Allow CBUCK regulator to discharge */
	k_msleep(WLAN_CBUCK_DISCHARGE_MS);

	/* WIFI power on */
	ret = gpio_pin_set_dt(&config->wifi_reg_on_gpio, 1);
	if (ret) {
		printk("AIROC Error: gpio_pin_set_dt(1) failed: %d\n", ret);
		return ret;
	}

	/* Also enable BT_ON (GPIO 29) to power full chip subsystem */
	gpio_pin_configure(config->wifi_reg_on_gpio.port, 29, GPIO_OUTPUT_HIGH);
	gpio_pin_set(config->wifi_reg_on_gpio.port, 29, 1);
	printk("AIROC: WL_ON (GPIO 28) and BT_ON (GPIO 29) set HIGH (waiting %d ms)...\n", WLAN_POWER_UP_DELAY_MS);
	k_msleep(WLAN_POWER_UP_DELAY_MS);
#else
	printk("AIROC: No wifi_reg_on_gpios property defined!\n");
#endif /* DT_INST_NODE_HAS_PROP(0, reg_on_gpios) */

	return 0;
}

/*
 * Implement WHD memory wrappers
 */

inline void *whd_mem_malloc(size_t size)
{
	return k_malloc(size);
}

inline void *whd_mem_calloc(size_t nitems, size_t size)
{
	return k_calloc(nitems, size);
}

inline void whd_mem_free(void *ptr)
{
	k_free(ptr);
}

inline void whd_mem_memcpy(void *dest, const void *src, size_t len)
{
	memcpy(dest, src, len);
}

inline void whd_mem_memset(void *buf, int val, size_t len)
{
	memset(buf, val, len);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

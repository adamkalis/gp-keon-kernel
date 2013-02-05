/* Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

//Cellon add and modify start, Eagle.Yin, 2012/12/25 for porting AR6005G code from QRD8625r4 ICS version
//we use machine_is_msm7x27a_ffa() in C8666GP project.
 
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <asm/mach-types.h>
#include <mach/rpc_pmapp.h>
#include "board-msm7627a.h"
#include "devices-msm7x2xa.h"
#include "timer.h"

#define GPIO_WLAN_3V3_EN 39
static const char *id = "WLPW";

enum {
	WLAN_VREG_S3 = 0,
	WLAN_VREG_L17,
	WLAN_VREG_L19
};

struct wlan_vreg_info {
	const char *vreg_id;
	unsigned int level_min;
	unsigned int level_max;
	unsigned int pmapp_id;
	unsigned int is_vreg_pin_controlled;
	struct regulator *reg;
};

static struct wlan_vreg_info vreg_info[] = {
	{"msme1",     1800000, 1800000, 2,  0, NULL},
	{"wlan3v3",        3300000, 3300000, 21, 1, NULL},
	{"wlan1v8",     1800000, 1800000, 0, 0, NULL}
};

int gpio_wlan_sys_rest_en = 16; //WLAN reset pin

static unsigned int qrf6285_init_regs(void)
{
	struct regulator_bulk_data regs[ARRAY_SIZE(vreg_info)];
	int i = 0, rc = 0;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		regs[i].supply = vreg_info[i].vreg_id;
		regs[i].min_uV = vreg_info[i].level_min;
		regs[i].max_uV = vreg_info[i].level_max;
	}

	rc = regulator_bulk_get(NULL, ARRAY_SIZE(regs), regs);
	if (rc) {
		pr_err("%s: could not get regulators: %d\n", __func__, rc);
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		vreg_info[i].reg = regs[i].consumer;

out:
	return rc;
}

static unsigned int setup_wlan_gpio(bool on)
{
	int rc = 0;

	if (on) {
		rc = gpio_direction_output(gpio_wlan_sys_rest_en, 1);
		msleep(100);
	} else {
		gpio_set_value_cansleep(gpio_wlan_sys_rest_en, 0);
		rc = gpio_direction_input(gpio_wlan_sys_rest_en);
		msleep(100);
	}

	if (rc)
		pr_err("%s: WLAN sys_reset_en GPIO: Error", __func__);

	return rc;
}

static unsigned int setup_wlan_clock(bool on)
{
	int rc = 0;

	if (on) {
		/* Vote for A0 clock */
		rc = pmapp_clock_vote(id, PMAPP_CLOCK_ID_A0,
					PMAPP_CLOCK_VOTE_ON);
	} else {
		/* Vote against A0 clock */
		rc = pmapp_clock_vote(id, PMAPP_CLOCK_ID_A0,
					 PMAPP_CLOCK_VOTE_OFF);
	}

	if (rc)
		pr_err("%s: Configuring A0 clock for WLAN: Error", __func__);

	return rc;
}

static unsigned int wlan_switch_regulators(int on)
{
	int rc = 0, index = 0;

	if (machine_is_msm7627a_qrd1())
		index = 2;

	for ( ; index < ARRAY_SIZE(vreg_info); index++) {
		if (on) {
			rc = regulator_set_voltage(vreg_info[index].reg,
						vreg_info[index].level_min,
						vreg_info[index].level_max);
			if (rc) {
				pr_err("%s:%s set voltage failed %d\n",
					__func__, vreg_info[index].vreg_id, rc);
				goto reg_disable;
			}

			rc = regulator_enable(vreg_info[index].reg);
			if (rc) {
				pr_err("%s:%s vreg enable failed %d\n",
					__func__, vreg_info[index].vreg_id, rc);
				goto reg_disable;
			}

			if (vreg_info[index].is_vreg_pin_controlled) {
				rc = pmapp_vreg_lpm_pincntrl_vote(id,
						vreg_info[index].pmapp_id,
						PMAPP_CLOCK_ID_A0, 1);
				if (rc) {
					pr_err("%s:%s pincntrl failed %d\n",
						__func__,
						vreg_info[index].vreg_id, rc);
					goto pin_cnt_fail;
				}
			}
		} else {
			if (vreg_info[index].is_vreg_pin_controlled) {
				rc = pmapp_vreg_lpm_pincntrl_vote(id,
						vreg_info[index].pmapp_id,
						PMAPP_CLOCK_ID_A0, 0);
				if (rc) {
					pr_err("%s:%s pincntrl failed %d\n",
						__func__,
						vreg_info[index].vreg_id, rc);
					goto pin_cnt_fail;
				}
			}

			rc = regulator_disable(vreg_info[index].reg);
			if (rc) {
				pr_err("%s:%s vreg disable failed %d\n",
					__func__,
					vreg_info[index].vreg_id, rc);
				goto reg_disable;
			}
		}
	}
	return 0;
pin_cnt_fail:
	if (on)
		regulator_disable(vreg_info[index].reg);
reg_disable:
	if (!machine_is_msm7627a_qrd1()) {
		while (index) {
			if (on) {
				index--;
				regulator_disable(vreg_info[index].reg);
				regulator_put(vreg_info[index].reg);
			}
		}
	}
	return rc;
}

static unsigned int msm_AR600X_setup_power(bool on)
{
	int rc = 0;
	static bool init_done;

	if (unlikely(!init_done)) {
		rc = qrf6285_init_regs();
		if (rc) {
			pr_err("%s: qrf6285 init failed = %d\n", __func__, rc);
			return rc;
		} else {
			init_done = true;
		}
	}

	rc = wlan_switch_regulators(on);
	if (rc) {
		pr_err("%s: wlan_switch_regulators error = %d\n", __func__, rc);
		goto out;
	}

	/* GPIO_WLAN_3V3_EN is only required for the QRD7627a */
	if (machine_is_msm7627a_qrd1()) {
		rc = gpio_tlmm_config(GPIO_CFG(GPIO_WLAN_3V3_EN, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config 119 failed,error = %d\n",
				__func__, rc);
			goto reg_disable;
		}
		gpio_set_value(GPIO_WLAN_3V3_EN, 1);
	}

	/*
	 * gpio_wlan_sys_rest_en is not from the GPIO expander for QRD7627a,
	 * EVB1.0 and QRD8625,so the below step is required for those devices.
	 */
	/*if (machine_is_msm7627a_qrd1() || machine_is_msm7627a_evb()
					|| machine_is_msm8625_evb()
					|| machine_is_msm8625_qrd5() 
					|| machine_is_msm7x27a_qrd5a()  
					|| machine_is_msm8625_skua()
					|| machine_is_msm8625_evt()
					|| machine_is_msm7627a_qrd3()
					|| machine_is_msm8625_qrd7()) {
				*/
	if (1) {
		rc = gpio_tlmm_config(GPIO_CFG(gpio_wlan_sys_rest_en, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config 119 failed,error = %d\n",
				__func__, rc);
			goto qrd_gpio_fail;
		}
		gpio_set_value(gpio_wlan_sys_rest_en, 1);
	} else {
		rc = gpio_request(gpio_wlan_sys_rest_en, "WLAN_DEEP_SLEEP_N");
		if (rc) {
			pr_err("%s: WLAN sys_rest_en GPIO %d request failed %d\n",
				__func__,
				gpio_wlan_sys_rest_en, rc);
			goto qrd_gpio_fail;
		}
		rc = setup_wlan_gpio(on);
		if (rc) {
			pr_err("%s: wlan_set_gpio = %d\n", __func__, rc);
			goto gpio_fail;
		}
		gpio_free(gpio_wlan_sys_rest_en);
	}

	/* Enable the A0 clock */
	rc = setup_wlan_clock(on);
	if (rc) {
		pr_err("%s: setup_wlan_clock = %d\n", __func__, rc);
		goto set_gpio_fail;
	}

	/* Configure A0 clock to be slave to WLAN_CLK_PWR_REQ */
	rc = pmapp_clock_vote(id, PMAPP_CLOCK_ID_A0,
				 PMAPP_CLOCK_VOTE_PIN_CTRL);
	if (rc) {
		pr_err("%s: Configuring A0 to Pin controllable failed %d\n",
				__func__, rc);
		goto set_clock_fail;
	}

	pr_info("WLAN power-up success\n");
	return 0;
set_clock_fail:
	setup_wlan_clock(0);
set_gpio_fail:
	setup_wlan_gpio(0);
gpio_fail:
	gpio_free(gpio_wlan_sys_rest_en);
qrd_gpio_fail:
	/* GPIO_WLAN_3V3_EN is only required for the QRD7627a */
	if (machine_is_msm7627a_qrd1())
		gpio_free(GPIO_WLAN_3V3_EN);
reg_disable:
	wlan_switch_regulators(0);
out:
	pr_info("WLAN power-up failed\n");
	return rc;
}

static unsigned int msm_AR600X_shutdown_power(bool on)
{
	int rc = 0;

	/* Disable the A0 clock */
	rc = setup_wlan_clock(on);
	if (rc) {
		pr_err("%s: setup_wlan_clock = %d\n", __func__, rc);
		goto set_clock_fail;
	}

	/*
	 * gpio_wlan_sys_rest_en is not from the GPIO expander for QRD7627a,
	 * EVB1.0 and QRD8625,so the below step is required for those devices.
	 */
/*	if (machine_is_msm7627a_qrd1() || machine_is_msm7627a_evb()
					|| machine_is_msm8625_evb()
					|| machine_is_msm8625_qrd5() 
					|| machine_is_msm7x27a_qrd5a()
					|| machine_is_msm8625_skua()
					|| machine_is_msm8625_evt()
					|| machine_is_msm7627a_qrd3()
					|| machine_is_msm8625_qrd7()) {
					*/
	if (1) {
		rc = gpio_tlmm_config(GPIO_CFG(gpio_wlan_sys_rest_en, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config 119 failed,error = %d\n",
				__func__, rc);
			goto gpio_fail;
		}
		gpio_set_value(gpio_wlan_sys_rest_en, 0);
	} else {
		rc = gpio_request(gpio_wlan_sys_rest_en, "WLAN_DEEP_SLEEP_N");
		if (!rc) {
			rc = setup_wlan_gpio(on);
			if (rc) {
				pr_err("%s: setup_wlan_gpio = %d\n",
					__func__, rc);
				goto set_gpio_fail;
			}
			gpio_free(gpio_wlan_sys_rest_en);
		}
	}

	/* GPIO_WLAN_3V3_EN is only required for the QRD7627a */
	if (machine_is_msm7627a_qrd1()) {
		rc = gpio_tlmm_config(GPIO_CFG(GPIO_WLAN_3V3_EN, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config 119 failed,error = %d\n",
				__func__, rc);
			goto qrd_gpio_fail;
		}
		gpio_set_value(GPIO_WLAN_3V3_EN, 0);
	}

	rc = wlan_switch_regulators(on);
	if (rc) {
		pr_err("%s: wlan_switch_regulators error = %d\n",
			__func__, rc);
		goto reg_disable;
	}

	pr_info("WLAN power-down success\n");
	return 0;
set_clock_fail:
	setup_wlan_clock(0);
set_gpio_fail:
	setup_wlan_gpio(0);
gpio_fail:
	gpio_free(gpio_wlan_sys_rest_en);
qrd_gpio_fail:
	/* GPIO_WLAN_3V3_EN is only required for the QRD7627a */
	if (machine_is_msm7627a_qrd1())
		gpio_free(GPIO_WLAN_3V3_EN);
reg_disable:
	wlan_switch_regulators(0);
	pr_info("WLAN power-down failed\n");
	return rc;
}

int  ar600x_wlan_power(bool on)
{
	if (on)
		msm_AR600X_setup_power(on);
	else
		msm_AR600X_shutdown_power(on);

	return 0;
}
EXPORT_SYMBOL(ar600x_wlan_power);

#define NV_ITEM_WLAN_MAC_ADDR   4678
extern int msm_read_nv(unsigned int nv_item, void *buf);
unsigned char wlan_mac_addr[6];

/*ATH6KL_USES_PREALLOCATE_MEM*/
#define CONFIG_ATH6KL_USES_PREALLOCATE_MEM
#ifdef CONFIG_ATH6KL_USES_PREALLOCATE_MEM
void *wlan_mem_ptr = NULL;
#define HIF_DMA_BUFFER_SIZE       (32 * 1024)
static void __init wlan_init_memory(void)
{
	wlan_mem_ptr = kmalloc(HIF_DMA_BUFFER_SIZE, GFP_KERNEL);
	if (!wlan_mem_ptr)
		panic("Failed to allocate memory for wlan in boot stage!!\n");
	else
		pr_info("ath6kl: Allocate wlan ram at addr %p successfully\n", wlan_mem_ptr);
}

void *ath6kl_fetch_memory(void)
{
	return wlan_mem_ptr;
}
EXPORT_SYMBOL(ath6kl_fetch_memory);
#endif
/*ATH6KL_USES_PREALLOCATE_MEM*/

static int __init qrd_wlan_init(void)
{
	int rc;

	pr_info("WLAN power init\n");
	/* if (machine_is_msm7627a_qrd1() || machine_is_msm7627a_evb()
					|| machine_is_msm8625_evb()
					|| machine_is_msm8625_qrd5() 
					|| machine_is_msm7x27a_qrd5a()
					|| machine_is_msm8625_skua()
					|| machine_is_msm8625_evt()
					|| machine_is_msm7627a_qrd3()
					|| machine_is_msm8625_qrd7()) {
					*/
	if (1) {
		rc = gpio_tlmm_config(GPIO_CFG(gpio_wlan_sys_rest_en, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config %d failed,error = %d\n",
				__func__, gpio_wlan_sys_rest_en, rc);
			goto exit;
		}
		gpio_set_value(gpio_wlan_sys_rest_en, 0);
	} else {
		gpio_request(gpio_wlan_sys_rest_en, "WLAN_DEEP_SLEEP_N");
		rc = setup_wlan_gpio(false);
		gpio_free(gpio_wlan_sys_rest_en);
		if (rc) {
			pr_err("%s: wlan_set_gpio = %d\n", __func__, rc);
			goto exit;
		}
	}

	/* GPIO_WLAN_3V3_EN is only required for the QRD7627a */
	if (machine_is_msm7627a_qrd1()) {
		rc = gpio_tlmm_config(GPIO_CFG(GPIO_WLAN_3V3_EN, 0,
					GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
					GPIO_CFG_2MA), GPIO_CFG_ENABLE);
		if (rc) {
			pr_err("%s gpio_tlmm_config %d failed,error = %d\n",
				__func__, GPIO_WLAN_3V3_EN, rc);
			goto exit;
		}
		gpio_set_value(GPIO_WLAN_3V3_EN, 0);
	}

    /*ATH6KL_USES_PREALLOCATE_MEM*/
#ifdef CONFIG_ATH6KL_USES_PREALLOCATE_MEM
	wlan_init_memory();
#endif
	/* NV mac init */
	msm_read_nv(NV_ITEM_WLAN_MAC_ADDR,wlan_mac_addr);
       printk("MAC from NV %02X:%02X:%02X:%02X:%02X:%02X\n",
                     wlan_mac_addr[0], wlan_mac_addr[1], wlan_mac_addr[2],
                     wlan_mac_addr[3], wlan_mac_addr[4], wlan_mac_addr[5]);
exit:
	return rc;
}
device_initcall(qrd_wlan_init);

//Cellon add and modify end, Eagle.Yin, 2012/12/25 for porting AR6005G code from QRD8625r4 ICS version

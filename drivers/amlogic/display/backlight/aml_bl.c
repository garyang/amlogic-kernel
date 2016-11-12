/*
 * drivers/amlogic/display/backlight/aml_bl.c
 *
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
*/


#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/backlight.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/notifier.h>
#include <linux/amlogic/cpu_version.h>
#include <linux/amlogic/aml_gpio_consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/amlogic/vout/aml_bl.h>
#ifdef CONFIG_AML_LCD
#include <linux/amlogic/vout/lcd_notify.h>
#include <linux/amlogic/vout/lcd_unifykey.h>
#endif
#ifdef CONFIG_AML_BL_EXTERN
#include <linux/amlogic/vout/aml_bl_extern.h>
#endif
#ifdef CONFIG_AML_LOCAL_DIMMING
#include <linux/amlogic/vout/aml_ldim.h>
#endif

#include "aml_bl_reg.h"

/* #define AML_BACKLIGHT_DEBUG */
static unsigned int bl_debug_print_flag;
module_param(bl_debug_print_flag, uint, 0664);
MODULE_PARM_DESC(bl_debug_print_flag, "bl_debug_print_flag");

static enum bl_chip_type_e bl_chip_type = BL_CHIP_MAX;
static struct aml_bl_drv_s *bl_drv;

static unsigned int bl_key_valid;
static unsigned char bl_config_load;

/* bl_off_policy support */
static int aml_bl_off_policy_cnt;

static unsigned int bl_off_policy;
module_param(bl_off_policy, uint, 0664);
MODULE_PARM_DESC(bl_off_policy, "bl_off_policy");

static unsigned int brightness_bypass;
module_param(brightness_bypass, uint, 0664);
MODULE_PARM_DESC(brightness_bypass, "bl_brightness_bypass");
static unsigned int pwm_bypass;
module_param(pwm_bypass, uint, 0664);
MODULE_PARM_DESC(pwm_bypass, "bl_pwm_bypass");

static unsigned int bl_pwm_duty_free;
module_param(bl_pwm_duty_free, uint, 0664);
MODULE_PARM_DESC(bl_pwm_duty_free, "bl_pwm_duty_free");

static DEFINE_MUTEX(bl_power_mutex);
static DEFINE_MUTEX(bl_level_mutex);

static void bl_set_pwm_gpio_check(struct bl_pwm_config_s *bl_pwm);

static struct bl_config_s bl_config = {
	.level_default = 128,
	.level_mid = 128,
	.level_mid_mapping = 128,
	.level_min = 10,
	.level_max = 255,
	.power_on_delay = 100,
	.power_off_delay = 30,
	.method = BL_CTRL_MAX,

	.bl_pwm = NULL,
	.bl_pwm_combo0 = NULL,
	.bl_pwm_combo1 = NULL,
	.pwm_on_delay = 0,
	.pwm_off_delay = 0,

	.bl_gpio = {
		{.flag = 0,},
		{.flag = 0,},
		{.flag = 0,},
		{.flag = 0,},
		{.flag = 0,},
	},
};

const char *bl_chip_table[] = {
	"M8",
	"M8b",
	"M8M2",
	"G9TV",
	"G9BB",
	"GXTVBB",
	"invalid",
};

struct bl_method_match_s {
	char *name;
	enum bl_ctrl_method_e type;
};

static struct bl_method_match_s bl_method_match_table[] = {
	{"gpio",         BL_CTRL_GPIO},
	{"pwm",          BL_CTRL_PWM},
	{"pwm_combo",    BL_CTRL_PWM_COMBO},
	{"local_diming", BL_CTRL_LOCAL_DIMING},
	{"extern",       BL_CTRL_EXTERN},
	{"invalid",      BL_CTRL_MAX},
};

static char *bl_method_type_to_str(int type)
{
	int i;
	char *str = bl_method_match_table[BL_CTRL_MAX].name;

	for (i = 0; i < BL_CTRL_MAX; i++) {
		if (type == bl_method_match_table[i].type) {
			str = bl_method_match_table[i].name;
			break;
		}
	}
	return str;
}

static unsigned int pwm_misc[6][5] = {
	/* pwm_reg,         div bit, clk_sel bit, clk_en bit, pwm_en bit*/
	{PWM_MISC_REG_AB,   8,       4,           15,         0,},
	{PWM_MISC_REG_AB,   16,      6,           23,         0,},
	{PWM_MISC_REG_CD,   8,       4,           15,         0,},
	{PWM_MISC_REG_CD,   16,      6,           23,         0,},
	{PWM_MISC_REG_EF,   8,       4,           15,         0,},
	{PWM_MISC_REG_EF,   16,      6,           23,         0,},
};

static unsigned int pwm_reg[6] = {
	PWM_PWM_A,
	PWM_PWM_B,
	PWM_PWM_C,
	PWM_PWM_D,
	PWM_PWM_E,
	PWM_PWM_F,
};

static enum bl_chip_type_e aml_bl_check_chip(void)
{
	unsigned int cpu_type;
	enum bl_chip_type_e bl_chip = BL_CHIP_MAX;

	cpu_type = get_cpu_type();
	switch (cpu_type) {
	case MESON_CPU_MAJOR_ID_M8:
		bl_chip = BL_CHIP_M8;
		break;
	case MESON_CPU_MAJOR_ID_M8B:
		bl_chip = BL_CHIP_M8B;
		break;
	case MESON_CPU_MAJOR_ID_M8M2:
		bl_chip = BL_CHIP_M8M2;
		break;
	case MESON_CPU_MAJOR_ID_MG9TV:
		bl_chip = BL_CHIP_G9TV;
		break;
	case MESON_CPU_MAJOR_ID_GXTVBB:
		bl_chip = BL_CHIP_GXTVBB;
		break;
	default:
		bl_chip = BL_CHIP_MAX;
	}

	if (bl_debug_print_flag)
		BLPR("BL driver check chip : %s\n", bl_chip_table[bl_chip]);
	return bl_chip;
}

static int aml_bl_check_driver(void)
{
	int ret = 0;

	if (bl_drv == NULL) {
		BLERR("no bl driver\n");
		return -1;
	}
	switch (bl_drv->bconf->method) {
	case BL_CTRL_PWM:
		if (bl_drv->bconf->bl_pwm == NULL) {
			ret = -1;
			BLERR("no bl_pwm struct\n");
		}
		break;
	case BL_CTRL_PWM_COMBO:
		if (bl_drv->bconf->bl_pwm_combo0 == NULL) {
			ret = -1;
			BLERR("no bl_pwm_combo_0 struct\n");
		}
		if (bl_drv->bconf->bl_pwm_combo1 == NULL) {
			ret = -1;
			BLERR("no bl_pwm_combo_1 struct\n");
		}
		break;
	default:
		break;
	}

	return ret;
}

struct aml_bl_drv_s *aml_bl_get_driver(void)
{
	if (bl_drv == NULL)
		BLERR("no bl driver");

	return bl_drv;
}

/* **********************************
 * bl gpio & pinmux
 * ********************************** */
static void bl_gpio_release(int index)
{
	struct bl_gpio_s *bl_gpio;

	if (index >= BL_GPIO_NUM_MAX) {
		BLERR("gpio index %d, exit\n", index);
		return;
	}
	bl_gpio = &bl_drv->bconf->bl_gpio[index];
	if (bl_gpio->flag == 0) {
		if (bl_debug_print_flag) {
			BLPR("gpio %s[%d] is not registered\n",
				bl_gpio->name, index);
		}
		return;
	}
	if (IS_ERR(bl_gpio->gpio)) {
		BLERR("gpio %s[%d]: %p, err: %ld\n",
			bl_gpio->name, index, bl_gpio->gpio,
			PTR_ERR(bl_gpio->gpio));
		return;
	}

	/* release gpio */
	devm_gpiod_put(bl_drv->dev, bl_gpio->gpio);
	bl_gpio->flag = 0;
	if (bl_debug_print_flag)
		BLPR("release gpio %s[%d]\n", bl_gpio->name, index);
}

static void bl_gpio_register(int index)
{
	struct bl_gpio_s *bl_gpio;
	const char *str;
	int ret;

	if (index >= BL_GPIO_NUM_MAX) {
		BLERR("gpio index %d, exit\n", index);
		return;
	}
	bl_gpio = &bl_drv->bconf->bl_gpio[index];
	if (bl_gpio->flag) {
		if (bl_debug_print_flag) {
			BLPR("gpio %s[%d] is already registered\n",
				bl_gpio->name, index);
		}
		return;
	}

	/* get gpio name */
	ret = of_property_read_string_index(bl_drv->dev->of_node,
		"bl_gpio_names", index, &str);
	if (ret) {
		BLERR("failed to get bl_gpio_names: %d\n", index);
		str = "unknown";
	}
	strcpy(bl_gpio->name, str);

	/* request gpio */
	bl_gpio->gpio = devm_gpiod_get_index(bl_drv->dev, "bl", index);
	if (IS_ERR(bl_gpio->gpio)) {
		BLERR("register gpio %s[%d]: %p, err: %ld\n",
			bl_gpio->name, index, bl_gpio->gpio,
			IS_ERR(bl_gpio->gpio));
	} else {
		bl_gpio->flag = 1;
		if (bl_debug_print_flag) {
			BLPR("register gpio %s[%d]: %p\n",
				bl_gpio->name, index, bl_gpio->gpio);
		}
	}
}

static void bl_gpio_set(int index, int value)
{
	struct bl_gpio_s *bl_gpio;

	if (index >= BL_GPIO_NUM_MAX) {
		BLERR("gpio index %d, exit\n", index);
		return;
	}
	bl_gpio = &bl_drv->bconf->bl_gpio[index];
	if (bl_gpio->flag == 0) {
		BLERR("gpio [%d] is not registered\n", index);
		return;
	}
	if (IS_ERR(bl_gpio->gpio)) {
		BLERR("gpio %s[%d]: %p, err: %ld\n",
			bl_gpio->name, index, bl_gpio->gpio,
			PTR_ERR(bl_gpio->gpio));
		return;
	}

	switch (value) {
	case BL_GPIO_OUTPUT_LOW:
	case BL_GPIO_OUTPUT_HIGH:
		gpiod_direction_output(bl_gpio->gpio, value);
		break;
	case BL_GPIO_INPUT:
	default:
		gpiod_direction_input(bl_gpio->gpio);
		break;
	}
	if (bl_debug_print_flag) {
		BLPR("set gpio %s[%d] value: %d\n",
			bl_gpio->name, index, value);
	}
}

static void bl_gpio_multiplex_register(int index)
{
	struct bl_gpio_s *bl_gpio;
	const char *str;
	int ret;

	if (index >= BL_GPIO_NUM_MAX) {
		BLERR("gpio index %d, exit\n", index);
		return;
	}
	bl_gpio = &bl_drv->bconf->bl_gpio[index];

	/* get gpio name */
	ret = of_property_read_string_index(bl_drv->dev->of_node,
		"bl_gpio_names", index, &str);
	if (ret) {
		BLERR("failed to get bl_gpio_names: %d\n", index);
		str = "unknown";
	}
	strcpy(bl_gpio->name, str);
	if (bl_debug_print_flag)
		BLPR("register multiplex_gpio %s[%d]\n", bl_gpio->name, index);
}

static void bl_gpio_multiplex_set(int index, int value)
{
	struct bl_gpio_s *bl_gpio;

	if (index >= BL_GPIO_NUM_MAX) {
		BLERR("gpio index %d, exit\n", index);
		return;
	}
	bl_gpio = &bl_drv->bconf->bl_gpio[index];

	if (bl_gpio->flag == 0) {
		/* request gpio */
		bl_gpio->gpio = devm_gpiod_get_index(bl_drv->dev, "bl", index);
		if (IS_ERR(bl_gpio->gpio)) {
			BLERR("register gpio %s[%d]: %p, err: %ld\n",
				bl_gpio->name, index, bl_gpio->gpio,
				IS_ERR(bl_gpio->gpio));
			return;
		} else {
			bl_gpio->flag = 1;
			if (bl_debug_print_flag) {
				BLPR("register gpio %s[%d]: %p\n",
					bl_gpio->name, index, bl_gpio->gpio);
			}
		}
	}

	switch (value) {
	case BL_GPIO_OUTPUT_LOW:
	case BL_GPIO_OUTPUT_HIGH:
		gpiod_direction_output(bl_gpio->gpio, value);
		break;
	case BL_GPIO_INPUT:
	default:
		gpiod_direction_input(bl_gpio->gpio);
		break;
	}
	if (bl_debug_print_flag) {
		BLPR("set gpio %s[%d] value: %d\n",
			bl_gpio->name, index, value);
	}
}
/* ****************************************************** */
static char *bl_pinmux_str[] = {
	"pwm_on",           /* 0 */
	"pwm_vs_on",        /* 1 */
	"pwm_combo_on",     /* 2 */
	"pwm_combo_0_on",   /* 3 */
	"pwm_combo_1_on",   /* 4 */
};

static void bl_pwm_pinmux_gpio_set(int pwm_index, int gpio_level)
{
	struct bl_config_s *bconf = bl_drv->bconf;
	struct bl_pwm_config_s *bl_pwm = NULL;
	int index = 0xff;

	switch (bconf->method) {
	case BL_CTRL_PWM:
		bl_pwm = bconf->bl_pwm;
		break;
	case BL_CTRL_PWM_COMBO:
		if (pwm_index == 0) {
			bl_pwm = bconf->bl_pwm_combo0;
			if (bconf->bl_pwm_combo1->pinmux_flag > 0)
				index = 4;
			else
				index = 0xff;
		} else {
			bl_pwm = bconf->bl_pwm_combo1;
			if (bconf->bl_pwm_combo0->pinmux_flag > 0)
				index = 3;
			else
				index = 0xff;
		}
		break;
	default:
		BLERR("%s: wrong ctrl_mothod=%d\n", __func__, bconf->method);
		break;
	}

	if (bl_pwm == NULL)
		return;

	if (bl_debug_print_flag) {
		BLPR("%s: pwm_port=%d, pinmux_flag=%d(%d)\n",
			__func__, bl_pwm->pwm_port,
			bl_pwm->pinmux_flag, bconf->pinmux_flag);
	}
	if (bl_pwm->pinmux_flag > 0) {
		/* release pwm pinmux */
		if (bl_debug_print_flag)
			BLPR("release pinmux: %p\n", bconf->pin);
		devm_pinctrl_put(bconf->pin); /* release pinmux */
		bl_pwm->pinmux_flag = 0;

		/* request combo pinmux */
		if (index != 0xff) {
			bconf->pin = devm_pinctrl_get_select(bl_drv->dev,
					bl_pinmux_str[index]);
			if (IS_ERR(bconf->pin)) {
				BLERR("set %s pinmux error\n",
					bl_pinmux_str[index]);
			} else {
				if (bl_debug_print_flag) {
					BLPR("request %s pinmux: %p\n",
						bl_pinmux_str[index],
						bconf->pin);
				}
			}
			bconf->pinmux_flag = 1;
		} else {
			bconf->pinmux_flag = 0;
		}
	}

	/* set gpio */
	if (bl_pwm->pwm_gpio < BL_GPIO_NUM_MAX)
		bl_gpio_multiplex_set(bl_pwm->pwm_gpio, gpio_level);
}

static void bl_pwm_pinmux_gpio_clr(unsigned int pwm_index)
{
	struct bl_config_s *bconf = bl_drv->bconf;
	struct bl_pwm_config_s *bl_pwm = NULL;
	int index = 0xff, release_flag = 0;

	switch (bconf->method) {
	case BL_CTRL_PWM:
		bl_pwm = bconf->bl_pwm;
		if (bconf->bl_pwm->pwm_port == BL_PWM_VS)
			index = 1;
		else
			index = 0;
		break;
		release_flag = 0;
	case BL_CTRL_PWM_COMBO:
		if (pwm_index == 0) {
			bl_pwm = bconf->bl_pwm_combo0;
			if (bconf->bl_pwm_combo1->pinmux_flag > 0) {
				index = 2;
				release_flag = 1;
			} else {
				index = 3;
				release_flag = 0;
			}
		} else {
			bl_pwm = bconf->bl_pwm_combo1;
			if (bconf->bl_pwm_combo0->pinmux_flag > 0) {
				index = 2;
				release_flag = 1;
			} else {
				index = 4;
				release_flag = 0;
			}
		}
		break;
	default:
		BLERR("%s: wrong ctrl_mothod=%d\n", __func__, bconf->method);
		break;
	}

	if (bl_pwm == NULL)
		return;

	if (bl_debug_print_flag) {
		BLPR("%s: pwm_port=%d, pinmux_flag=%d(%d)\n",
			__func__, bl_pwm->pwm_port,
			bl_pwm->pinmux_flag, bconf->pinmux_flag);
	}
	if (bl_pwm->pinmux_flag > 0)
		return;

	/* release gpio */
	if (bl_pwm->pwm_gpio < BL_GPIO_NUM_MAX)
		bl_gpio_release(bl_pwm->pwm_gpio);

	if (release_flag > 0) {
		/* release pwm pinmux */
		if (bl_debug_print_flag)
			BLPR("release pinmux: %p\n", bconf->pin);
		devm_pinctrl_put(bconf->pin); /* release pinmux */
	}

	/* request pwm pinmux */
	bconf->pin = devm_pinctrl_get_select(bl_drv->dev,
			bl_pinmux_str[index]);
	if (IS_ERR(bconf->pin)) {
		BLERR("set %s pinmux error\n", bl_pinmux_str[index]);
	} else {
		if (bl_debug_print_flag) {
			BLPR("request %s pinmux: %p\n",
				bl_pinmux_str[index], bconf->pin);
		}
	}
	bconf->pinmux_flag = 1;
	bl_pwm->pinmux_flag = 1;
}

static void bl_pwm_pinmux_ctrl(struct bl_config_s *bconf, int status)
{
	if (status) {
		/* release gpio */
		switch (bconf->method) {
		case BL_CTRL_PWM:
			bl_set_pwm_gpio_check(bconf->bl_pwm);
			break;
		case BL_CTRL_PWM_COMBO:
			bl_set_pwm_gpio_check(bconf->bl_pwm_combo0);
			bl_set_pwm_gpio_check(bconf->bl_pwm_combo1);
			break;
		default:
			break;
		}
	} else {
		/* release pwm pinmux */
		if (bconf->pinmux_flag > 0) {
			if (bl_debug_print_flag)
				BLPR("release pinmux: %p\n", bconf->pin);
			devm_pinctrl_put(bconf->pin);
			bconf->pinmux_flag = 0;
		}
		switch (bconf->method) {
		case BL_CTRL_PWM:
			bconf->bl_pwm->pinmux_flag = 0;
			/* set gpio */
			if (bconf->bl_pwm->pwm_gpio < BL_GPIO_NUM_MAX) {
				bl_gpio_multiplex_set(bconf->bl_pwm->pwm_gpio,
					bconf->bl_pwm->pwm_gpio_off);
			}
			break;
		case BL_CTRL_PWM_COMBO:
			bconf->bl_pwm_combo0->pinmux_flag = 0;
			bconf->bl_pwm_combo1->pinmux_flag = 0;
			/* set gpio */
			if (bconf->bl_pwm_combo0->pwm_gpio < BL_GPIO_NUM_MAX) {
				bl_gpio_multiplex_set(
					bconf->bl_pwm_combo0->pwm_gpio,
					bconf->bl_pwm_combo0->pwm_gpio_off);
			}
			if (bconf->bl_pwm_combo1->pwm_gpio < BL_GPIO_NUM_MAX) {
				bl_gpio_multiplex_set(
					bconf->bl_pwm_combo1->pwm_gpio,
					bconf->bl_pwm_combo1->pwm_gpio_off);
			}
			break;
		default:
			break;
		}
	}
}

void bl_pwm_ctrl(struct bl_pwm_config_s *bl_pwm, int status)
{
	int port, pre_div;

	port = bl_pwm->pwm_port;
	pre_div = bl_pwm->pwm_pre_div;
	if (status) {
		/* enable pwm */
		switch (port) {
		case BL_PWM_A:
		case BL_PWM_B:
		case BL_PWM_C:
		case BL_PWM_D:
		case BL_PWM_E:
		case BL_PWM_F:
			/* pwm clk_div */
			bl_cbus_setb(pwm_misc[port][0], pre_div,
				pwm_misc[port][1], 7);
			/* pwm clk_sel */
			bl_cbus_setb(pwm_misc[port][0], 0,
				pwm_misc[port][2], 2);
			/* pwm clk_en */
			bl_cbus_setb(pwm_misc[port][0], 1,
				pwm_misc[port][3], 1);
			/* pwm enable */
			bl_cbus_setb(pwm_misc[port][0], 0x3,
				pwm_misc[port][4], 2);
			break;
		default:
			break;
		}
	} else {
		/* disable pwm */
		switch (port) {
		case BL_PWM_A:
		case BL_PWM_B:
		case BL_PWM_C:
		case BL_PWM_D:
		case BL_PWM_E:
		case BL_PWM_F:
			/* pwm clk_disable */
			bl_cbus_setb(pwm_misc[port][0], 0,
				pwm_misc[port][3], 1);
			break;
		default:
			break;
		}
	}
}

static void bl_power_en_ctrl(struct bl_config_s *bconf, int status)
{
	if (status) {
		if (bconf->power_on_delay > 0)
			mdelay(bconf->power_on_delay);
		if (bconf->en_gpio < BL_GPIO_NUM_MAX)
			bl_gpio_set(bconf->en_gpio, bconf->en_gpio_on);
	} else {
		if (bconf->en_gpio < BL_GPIO_NUM_MAX)
			bl_gpio_set(bconf->en_gpio, bconf->en_gpio_off);
		if (bconf->power_off_delay > 0)
			mdelay(bconf->power_off_delay);
	}
}

static void bl_power_on(void)
{
	struct bl_config_s *bconf = bl_drv->bconf;
#ifdef CONFIG_AML_BL_EXTERN
	struct aml_bl_extern_driver_s *bl_ext;
#endif
#ifdef CONFIG_AML_LOCAL_DIMMING
	struct aml_ldim_driver_s *ldim_drv;
#endif
	int ret;

	if (aml_bl_check_driver())
		return;

	/* bl_off_policy */
	if (bl_off_policy != BL_OFF_POLICY_NONE) {
		BLPR("bl_off_policy=%d for bl_off\n", bl_off_policy);
		return;
	}

	mutex_lock(&bl_power_mutex);

	if (brightness_bypass == 0) {
		if ((bl_drv->level == 0) ||
			(bl_drv->state & BL_STATE_BL_ON)) {
			goto exit_power_on_bl;
		}
	}

	ret = 0;
	switch (bconf->method) {
	case BL_CTRL_GPIO:
		bl_power_en_ctrl(bconf, 1);
		break;
	case BL_CTRL_PWM:
		/* step 1: power on pwm */
		if (bconf->pwm_on_delay > 0)
			mdelay(bconf->pwm_on_delay);
		bl_pwm_ctrl(bconf->bl_pwm, 1);
		bl_pwm_pinmux_ctrl(bconf, 1);
		/* step 2: power on enable */
		bl_power_en_ctrl(bconf, 1);
		break;
	case BL_CTRL_PWM_COMBO:
		/* step 1: power on pwm_combo */
		if (bconf->pwm_on_delay > 0)
			mdelay(bconf->pwm_on_delay);
		bl_pwm_ctrl(bconf->bl_pwm_combo0, 1);
		bl_pwm_ctrl(bconf->bl_pwm_combo1, 1);
		bl_pwm_pinmux_ctrl(bconf, 1);
		/* step 2: power on enable */
		bl_power_en_ctrl(bconf, 1);
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		/* step 1: power on enable */
		bl_power_en_ctrl(bconf, 1);
		/* step 2: power on ldim */
		ldim_drv = aml_ldim_get_driver();
		if (ldim_drv == NULL) {
			BLERR("no ldim driver\n");
		} else {
			if (ldim_drv->power_on) {
				ret = ldim_drv->power_on();
				if (ret) {
					BLERR("ldim: power on error\n");
					goto exit_power_on_bl;
				}
			} else {
				BLPR("ldim: power on is null\n");
			}
		}
		break;
#endif
#ifdef CONFIG_AML_BL_EXTERN
	case BL_CTRL_EXTERN:
		/* step 1: power on enable */
		bl_power_en_ctrl(bconf, 1);
		/* step 2: power on bl_extern */
		bl_ext = aml_bl_extern_get_driver();
		if (bl_ext == NULL) {
			BLERR("no bl_extern driver\n");
		} else {
			if (bl_ext->power_on) {
				ret = bl_ext->power_on();
				if (ret) {
					BLERR("bl_extern: power on error\n");
					goto exit_power_on_bl;
				}
			} else {
				BLERR("bl_extern: power on is null\n");
			}
		}
		break;
#endif
	default:
		BLPR("invalid backlight control method\n");
		goto exit_power_on_bl;
		break;
	}
	bl_drv->state |= BL_STATE_BL_ON;
	BLPR("backlight power on\n");

exit_power_on_bl:
	mutex_unlock(&bl_power_mutex);
}

static void bl_power_off(void)
{
	struct bl_config_s *bconf = bl_drv->bconf;
#ifdef CONFIG_AML_BL_EXTERN
	struct aml_bl_extern_driver_s *bl_ext;
#endif
#ifdef CONFIG_AML_LOCAL_DIMMING
	struct aml_ldim_driver_s *ldim_drv;
#endif
	int ret;

	if (aml_bl_check_driver())
		return;
	mutex_lock(&bl_power_mutex);

	if ((bl_drv->state & BL_STATE_BL_ON) == 0) {
		mutex_unlock(&bl_power_mutex);
		return;
	}

	ret = 0;
	switch (bconf->method) {
	case BL_CTRL_GPIO:
		bl_power_en_ctrl(bconf, 0);
		break;
	case BL_CTRL_PWM:
		/* step 1: power off enable */
		bl_power_en_ctrl(bconf, 0);
		/* step 2: power off pwm */
		bl_pwm_ctrl(bconf->bl_pwm, 0);
		bl_pwm_pinmux_ctrl(bconf, 0);
		if (bconf->pwm_off_delay > 0)
			mdelay(bconf->pwm_off_delay);
		break;
	case BL_CTRL_PWM_COMBO:
		/* step 1: power off enable */
		bl_power_en_ctrl(bconf, 0);
		/* step 2: power off pwm_combo */
		bl_pwm_ctrl(bconf->bl_pwm_combo0, 0);
		bl_pwm_ctrl(bconf->bl_pwm_combo1, 0);
		bl_pwm_pinmux_ctrl(bconf, 0);
		if (bconf->pwm_off_delay > 0)
			mdelay(bconf->pwm_off_delay);
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		/* step 1: power off ldim */
		ldim_drv = aml_ldim_get_driver();
		if (ldim_drv == NULL) {
			BLERR("no ldim driver\n");
		} else {
			if (ldim_drv->power_off) {
				ret = ldim_drv->power_off();
				if (ret)
					BLERR("ldim: power off error\n");
			} else {
				BLERR("ldim: power off is null\n");
			}
		}
		/* step 2: power off enable */
		bl_power_en_ctrl(bconf, 0);
		break;
#endif
#ifdef CONFIG_AML_BL_EXTERN
	case BL_CTRL_EXTERN:
		/* step 1: power off bl_extern */
		bl_ext = aml_bl_extern_get_driver();
		if (bl_ext == NULL) {
			BLERR("no bl_extern driver\n");
		} else {
			if (bl_ext->power_off) {
				ret = bl_ext->power_off();
				if (ret)
					BLERR("bl_extern: power off error\n");
			} else {
				BLERR("bl_extern: power off is null\n");
			}
		}
		/* step 2: power off enable */
		bl_power_en_ctrl(bconf, 0);
		break;
#endif
	default:
		BLPR("invalid backlight control method\n");
		break;
	}
	bl_drv->state &= ~BL_STATE_BL_ON;
	BLPR("backlight power off\n");
	mutex_unlock(&bl_power_mutex);
}

static unsigned int bl_level_mapping(unsigned int level)
{
	unsigned int mid = bl_drv->bconf->level_mid;
	unsigned int mid_map = bl_drv->bconf->level_mid_mapping;
	unsigned int max = bl_drv->bconf->level_max;
	unsigned int min = bl_drv->bconf->level_min;

	if (mid == mid_map)
		return level;

	level = level > max ? max : level;
	if ((level >= mid) && (level <= max)) {
		level = (((level - mid) * (max - mid_map)) / (max - mid)) +
			mid_map;
	} else if ((level >= min) && (level < mid)) {
		level = (((level - min) * (mid_map - min)) / (mid - min)) + min;
	} else {
		level = 0;
	}
	return level;
}

static void bl_set_pwm_gpio_check(struct bl_pwm_config_s *bl_pwm)
{
	unsigned int pwm_index, gpio_level;

	pwm_index = bl_pwm->index;

	/* pwm duty 100% or 0% special control */
	if ((bl_pwm->pwm_duty == 0) || (bl_pwm->pwm_duty == 100)) {
		switch (bl_pwm->pwm_method) {
		case BL_PWM_POSITIVE:
			if (bl_pwm->pwm_duty == 0)
				gpio_level = 0;
			else
				gpio_level = 1;
			break;
		case BL_PWM_NEGATIVE:
			if (bl_pwm->pwm_duty == 0)
				gpio_level = 1;
			else
				gpio_level = 0;
			break;
		default:
			BLERR("port %d: invalid pwm_method %d\n",
				bl_pwm->pwm_port, bl_pwm->pwm_method);
			gpio_level = 0;
			break;
		}
		if (bl_debug_print_flag) {
			BLPR("pwm port %d: duty %d%%, switch to gpio %d\n",
			bl_pwm->pwm_port, bl_pwm->pwm_duty, gpio_level);
		}
		bl_pwm_pinmux_gpio_set(pwm_index, gpio_level);
	} else {
		bl_pwm_pinmux_gpio_clr(pwm_index);
	}
}

static void bl_set_pwm(struct bl_pwm_config_s *bl_pwm)
{
	unsigned int pwm_hi = 0, pwm_lo = 0;
	unsigned int port = bl_pwm->pwm_port;
	unsigned int vs[4], ve[4], sw, n, i;

	if (bl_drv->state & BL_STATE_BL_ON)
		bl_set_pwm_gpio_check(bl_pwm);

	switch (bl_pwm->pwm_method) {
	case BL_PWM_POSITIVE:
		pwm_hi = bl_pwm->pwm_level;
		pwm_lo = bl_pwm->pwm_cnt - bl_pwm->pwm_level;
		break;
	case BL_PWM_NEGATIVE:
		pwm_lo = bl_pwm->pwm_level;
		pwm_hi = bl_pwm->pwm_cnt - bl_pwm->pwm_level;
		break;
	default:
		BLERR("port %d: invalid pwm_method %d\n",
			port, bl_pwm->pwm_method);
		break;
	}
	if (bl_debug_print_flag) {
		BLPR("port %d: pwm_cnt=%d, pwm_hi=%d, pwm_lo=%d\n",
			port, bl_pwm->pwm_cnt, pwm_hi, pwm_lo);
	}

	switch (port) {
	case BL_PWM_A:
	case BL_PWM_B:
	case BL_PWM_C:
	case BL_PWM_D:
	case BL_PWM_E:
	case BL_PWM_F:
		bl_cbus_write(pwm_reg[port], (pwm_hi << 16) | pwm_lo);
		break;
	case BL_PWM_VS:
		memset(vs, 0xffff, sizeof(unsigned int) * 4);
		memset(ve, 0xffff, sizeof(unsigned int) * 4);
		n = bl_pwm->pwm_freq;
		sw = (bl_pwm->pwm_cnt * 10 / n + 5) / 10;
		pwm_hi = (pwm_hi * 10 / n + 5) / 10;
		pwm_hi = (pwm_hi > 1) ? pwm_hi : 1;
		if (bl_debug_print_flag)
			BLPR("n=%d, sw=%d, pwm_high=%d\n", n, sw, pwm_hi);
		for (i = 0; i < n; i++) {
			vs[i] = 1 + (sw * i);
			ve[i] = vs[i] + pwm_hi - 1;
			if (bl_debug_print_flag) {
				BLPR("vs[%d]=%d, ve[%d]=%d\n",
					i, vs[i], i, ve[i]);
			}
		}
		bl_vcbus_write(VPU_VPU_PWM_V0, (ve[0] << 16) | (vs[0]));
		bl_vcbus_write(VPU_VPU_PWM_V1, (ve[1] << 16) | (vs[1]));
		bl_vcbus_write(VPU_VPU_PWM_V2, (ve[2] << 16) | (vs[2]));
		bl_vcbus_write(VPU_VPU_PWM_V3, (ve[3] << 16) | (vs[3]));
		break;
	default:
		break;
	}
}

static void bl_set_duty_pwm(struct bl_pwm_config_s *bl_pwm)
{
	if (pwm_bypass)
		return;

	if (bl_pwm_duty_free) {
		if (bl_pwm->pwm_duty > 100) {
			BLERR("pwm_duty %d%% is bigger 100%%\n",
				bl_pwm->pwm_duty);
			bl_pwm->pwm_duty = 100;
			BLPR("reset to 100%%\n");
		}
	} else {
		if (bl_pwm->pwm_duty > bl_pwm->pwm_duty_max) {
			BLERR("pwm_duty %d%% is bigger than duty_max %d%%\n",
				bl_pwm->pwm_duty, bl_pwm->pwm_duty_max);
			bl_pwm->pwm_duty = bl_pwm->pwm_duty_max;
			BLPR("reset to duty_max\n");
		} else if (bl_pwm->pwm_duty < bl_pwm->pwm_duty_min) {
			BLERR("pwm_duty %d%% is smaller than duty_min %d%%\n",
				bl_pwm->pwm_duty, bl_pwm->pwm_duty_min);
			bl_pwm->pwm_duty = bl_pwm->pwm_duty_min;
			BLPR("reset to duty_min\n");
		}
	}

	bl_pwm->pwm_level = bl_pwm->pwm_cnt * bl_pwm->pwm_duty / 100;
	if (bl_debug_print_flag) {
		BLPR("pwm port %d: duty=%d%%, duty_max=%d, duty_min=%d\n",
			bl_pwm->pwm_port, bl_pwm->pwm_duty,
			bl_pwm->pwm_duty_max, bl_pwm->pwm_duty_min);
	}
	bl_set_pwm(bl_pwm);
}

static void bl_set_level_pwm(struct bl_pwm_config_s *bl_pwm, unsigned int level)
{
	unsigned int min = bl_pwm->level_min;
	unsigned int max = bl_pwm->level_max;
	unsigned int pwm_max = bl_pwm->pwm_max;
	unsigned int pwm_min = bl_pwm->pwm_min;

	if (pwm_bypass)
		return;

	level = bl_level_mapping(level);
	max = bl_level_mapping(max);
	min = bl_level_mapping(min);
	if ((max <= min) || (level < min)) {
		bl_pwm->pwm_level = pwm_min;
	} else {
		bl_pwm->pwm_level = ((pwm_max - pwm_min) * (level - min) /
			(max - min)) + pwm_min;
	}
	if (bl_debug_print_flag) {
		BLPR("port %d mapping: level=%d, level_max=%d, level_min=%d\n",
			bl_pwm->pwm_port, level, max, min);
		BLPR("port %d: pwm_max=%d, pwm_min=%d, pwm_level=%d\n",
			bl_pwm->pwm_port, pwm_max, pwm_min, bl_pwm->pwm_level);
	}

	bl_pwm->pwm_duty = bl_pwm->pwm_level * 100 / bl_pwm->pwm_cnt;

	bl_set_pwm(bl_pwm);
}

#ifdef CONFIG_AML_BL_EXTERN
static void bl_set_level_extern(unsigned int level)
{
	struct aml_bl_extern_driver_s *bl_ext;
	int ret;

	bl_ext = aml_bl_extern_get_driver();
	if (bl_ext == NULL) {
		BLERR("no bl_extern driver\n");
	} else {
		if (bl_ext->set_level) {
			ret = bl_ext->set_level(level);
			if (ret)
				BLERR("bl_extern: set_level error\n");
		} else {
			BLERR("bl_extern: set_level is null\n");
		}
	}
}
#endif

#ifdef CONFIG_AML_LOCAL_DIMMING
static void bl_set_level_ldim(unsigned int level)
{
	struct aml_ldim_driver_s *ldim_drv;
	int ret = 0;

	ldim_drv = aml_ldim_get_driver();
	if (ldim_drv == NULL) {
		BLERR("no ldim driver\n");
	} else {
		if (ldim_drv->set_level) {
			ret = ldim_drv->set_level(level);
			if (ret)
				BLERR("ldim: set_level error\n");
		} else {
			BLERR("ldim: set_level is null\n");
		}
	}
}
#endif

static void aml_bl_set_level(unsigned int level)
{
	unsigned int temp;
	struct bl_pwm_config_s *pwm0, *pwm1;

	if (aml_bl_check_driver())
		return;

	if (bl_debug_print_flag) {
		BLPR("aml_bl_set_level=%u, last_level=%u, state=0x%x\n",
			level, bl_drv->level, bl_drv->state);
	}

	/* level range check */
	if (level > bl_drv->bconf->level_max)
		level = bl_drv->bconf->level_max;
	if (level < bl_drv->bconf->level_min) {
		if (level < BL_LEVEL_OFF)
			level = 0;
		else
			level = bl_drv->bconf->level_min;
	}
	bl_drv->level = level;

	if (level == 0)
		return;

	switch (bl_drv->bconf->method) {
	case BL_CTRL_GPIO:
		break;
	case BL_CTRL_PWM:
		bl_set_level_pwm(bl_drv->bconf->bl_pwm, level);
		break;
	case BL_CTRL_PWM_COMBO:
		pwm0 = bl_drv->bconf->bl_pwm_combo0;
		pwm1 = bl_drv->bconf->bl_pwm_combo1;
		if ((level >= pwm0->level_min) &&
			(level <= pwm0->level_max)) {
			temp = (pwm0->level_min > pwm1->level_min) ?
				pwm1->level_max : pwm1->level_min;
			if (bl_debug_print_flag) {
				BLPR("pwm0 region, level=%u, pwm1_level=%u\n",
					level, temp);
			}
			bl_set_level_pwm(pwm0, level);
			bl_set_level_pwm(pwm1, temp);
		} else if ((level >= pwm1->level_min) &&
			(level <= pwm1->level_max)) {
			temp = (pwm1->level_min > pwm0->level_min) ?
				pwm0->level_max : pwm0->level_min;
			if (bl_debug_print_flag) {
				BLPR("pwm1 region, level=%u, pwm0_level=%u\n",
					level, temp);
			}
			bl_set_level_pwm(pwm0, temp);
			bl_set_level_pwm(pwm1, level);
		}
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		bl_set_level_ldim(level);
		break;
#endif
#ifdef CONFIG_AML_BL_TABLET_EXTERN
	case BL_CTRL_EXTERN:
		bl_set_level_extern(level);
		break;
#endif
	default:
		if (bl_debug_print_flag)
			BLPR("invalid backlight control method\n");
		break;
	}
}

static unsigned int aml_bl_get_level(void)
{
	if (aml_bl_check_driver())
		return 0;

	BLPR("aml bl state: 0x%x\n", bl_drv->state);
	return bl_drv->level;
}

static int aml_bl_update_status(struct backlight_device *bd)
{
	int brightness = bd->props.brightness;

	if (brightness_bypass)
		return 0;

	mutex_lock(&bl_level_mutex);
	if (brightness < 0)
		brightness = 0;
	else if (brightness > 255)
		brightness = 255;

	if (((bl_drv->state & BL_STATE_LCD_ON) == 0) ||
		((bl_drv->state & BL_STATE_BL_POWER_ON) == 0))
		brightness = 0;

	if (bl_debug_print_flag) {
		BLPR("%s: %u, real brightness: %u, state: 0x%x\n",
			__func__, bd->props.brightness,
			brightness, bl_drv->state);
	}
	if (brightness == 0) {
		if (bl_drv->state & BL_STATE_BL_ON)
			bl_power_off();
	} else {
		aml_bl_set_level(brightness);
		if ((bl_drv->state & BL_STATE_BL_ON) == 0)
			bl_power_on();
	}
	mutex_unlock(&bl_level_mutex);
	return 0;
}

static int aml_bl_get_brightness(struct backlight_device *bd)
{
	return aml_bl_get_level();
}

static const struct backlight_ops aml_bl_ops = {
	.get_brightness = aml_bl_get_brightness,
	.update_status  = aml_bl_update_status,
};

#ifdef CONFIG_OF
static char *bl_pwm_name[] = {
	"PWM_A",
	"PWM_B",
	"PWM_C",
	"PWM_D",
	"PWM_E",
	"PWM_F",
	"PWM_VS",
};

enum bl_pwm_port_e bl_pwm_str_to_pwm(const char *str)
{
	enum bl_pwm_port_e pwm_port = BL_PWM_MAX;
	int i;

	for (i = 0; i < ARRAY_SIZE(bl_pwm_name); i++) {
		if (strcmp(str, bl_pwm_name[i]) == 0) {
			pwm_port = i;
			break;
		}
	}

	return pwm_port;
}

void bl_pwm_config_init(struct bl_pwm_config_s *bl_pwm)
{
	unsigned int freq, cnt, pre_div;
	int i;

	if (bl_debug_print_flag) {
		BLPR("%s pwm_port %d: freq = %u\n",
			__func__, bl_pwm->pwm_port, bl_pwm->pwm_freq);
	}
	freq = bl_pwm->pwm_freq;
	switch (bl_pwm->pwm_port) {
	case BL_PWM_VS:
		cnt = bl_vcbus_read(ENCL_VIDEO_MAX_LNCNT) + 1;
		bl_pwm->pwm_cnt = cnt;
		bl_pwm->pwm_pre_div = 0;
		if (bl_debug_print_flag)
			BLPR("pwm_cnt = %u\n", bl_pwm->pwm_cnt);
		break;
	default:
		for (i = 0; i < 0x7f; i++) {
			pre_div = i;
			cnt = XTAL_FREQ_HZ / (freq * (pre_div + 1)) - 2;
			if (cnt <= 0xffff)
				break;
		}
		bl_pwm->pwm_cnt = cnt;
		bl_pwm->pwm_pre_div = pre_div;
		if (bl_debug_print_flag)
			BLPR("pwm_cnt = %u, pwm_pre_div = %u\n", cnt, pre_div);
		break;
	}
	bl_pwm->pwm_max = (bl_pwm->pwm_cnt * bl_pwm->pwm_duty_max / 100);
	bl_pwm->pwm_min = (bl_pwm->pwm_cnt * bl_pwm->pwm_duty_min / 100);

	if (bl_debug_print_flag) {
		BLPR("pwm_max = %u, pwm_min = %u\n",
			bl_pwm->pwm_max, bl_pwm->pwm_min);
	}
}

static void aml_bl_config_print(struct bl_config_s *bconf)
{
	struct bl_pwm_config_s *bl_pwm;

	BLPR("name              = %s\n", bconf->name);
	BLPR("method            = %s(%d)\n",
		bl_method_type_to_str(bconf->method), bconf->method);

	if (bl_debug_print_flag == 0)
		return;

	BLPR("level_default     = %d\n", bconf->level_default);
	BLPR("level_min         = %d\n", bconf->level_min);
	BLPR("level_max         = %d\n", bconf->level_max);
	BLPR("level_mid         = %d\n", bconf->level_mid);
	BLPR("level_mid_mapping = %d\n", bconf->level_mid_mapping);

	BLPR("en_gpio           = %d\n", bconf->en_gpio);
	BLPR("en_gpio_on        = %d\n", bconf->en_gpio_on);
	BLPR("en_gpio_off       = %d\n", bconf->en_gpio_off);
	BLPR("power_on_delay    = %dms\n", bconf->power_on_delay);
	BLPR("power_off_delay   = %dms\n\n", bconf->power_off_delay);

	switch (bconf->method) {
	case BL_CTRL_PWM:
		BLPR("pwm_on_delay        = %dms\n", bconf->pwm_on_delay);
		BLPR("pwm_off_delay       = %dms\n", bconf->pwm_off_delay);
		if (bconf->bl_pwm) {
			bl_pwm = bconf->bl_pwm;
			BLPR("pwm_index     = %d\n", bl_pwm->index);
			BLPR("pwm_method    = %d\n", bl_pwm->pwm_method);
			BLPR("pwm_port      = %d\n", bl_pwm->pwm_port);
			if (bl_pwm->pwm_port == BL_PWM_VS) {
				BLPR("pwm_freq      = %d x vfreq\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_cnt       = %u\n", bl_pwm->pwm_cnt);
			} else {
				BLPR("pwm_freq      = %uHz\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_cnt       = %u\n", bl_pwm->pwm_cnt);
				BLPR("pwm_pre_div   = %u\n",
					bl_pwm->pwm_pre_div);
			}
			BLPR("pwm_level_max = %u\n", bl_pwm->level_max);
			BLPR("pwm_level_min = %u\n", bl_pwm->level_min);
			BLPR("pwm_duty_max  = %d%%\n", bl_pwm->pwm_duty_max);
			BLPR("pwm_duty_min  = %d%%\n", bl_pwm->pwm_duty_min);
			BLPR("pwm_max       = %u\n", bl_pwm->pwm_max);
			BLPR("pwm_min       = %u\n", bl_pwm->pwm_min);
			BLPR("pwm_gpio      = %d\n", bl_pwm->pwm_gpio);
			BLPR("pwm_gpio_off  = %d\n", bl_pwm->pwm_gpio_off);
		}
		break;
	case BL_CTRL_PWM_COMBO:
		BLPR("pwm_on_delay        = %dms\n", bconf->pwm_on_delay);
		BLPR("pwm_off_delay       = %dms\n", bconf->pwm_off_delay);
		/* pwm_combo_0 */
		if (bconf->bl_pwm_combo0) {
			bl_pwm = bconf->bl_pwm_combo0;
			BLPR("pwm_combo0_index     = %d\n", bl_pwm->index);
			BLPR("pwm_combo0_method    = %d\n", bl_pwm->pwm_method);
			BLPR("pwm_combo0_port      = %d\n", bl_pwm->pwm_port);
			if (bl_pwm->pwm_port == BL_PWM_VS) {
				BLPR("pwm_combo0_freq      = %d x vfreq\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_combo0_cnt       = %u\n",
					bl_pwm->pwm_cnt);
			} else {
				BLPR("pwm_combo0_freq      = %uHz\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_combo0_cnt       = %u\n",
					bl_pwm->pwm_cnt);
				BLPR("pwm_combo0_pre_div   = %u\n",
					bl_pwm->pwm_pre_div);
			}
			BLPR("pwm_combo0_level_max = %u\n", bl_pwm->level_max);
			BLPR("pwm_combo0_level_min = %u\n", bl_pwm->level_min);
			BLPR("pwm_combo0_duty_max  = %d\n",
				bl_pwm->pwm_duty_max);
			BLPR("pwm_combo0_duty_min  = %d\n",
				bl_pwm->pwm_duty_min);
			BLPR("pwm_combo0_max       = %u\n", bl_pwm->pwm_max);
			BLPR("pwm_combo0_min       = %u\n", bl_pwm->pwm_min);
			BLPR("pwm_combo0_gpio      = %d\n", bl_pwm->pwm_gpio);
			BLPR("pwm_combo0_gpio_off  = %d\n",
				bl_pwm->pwm_gpio_off);
		}
		/* pwm_combo_1 */
		if (bconf->bl_pwm_combo1) {
			bl_pwm = bconf->bl_pwm_combo1;
			BLPR("pwm_combo1_index     = %d\n", bl_pwm->index);
			BLPR("pwm_combo1_method    = %d\n", bl_pwm->pwm_method);
			BLPR("pwm_combo1_port      = %d\n", bl_pwm->pwm_port);
			if (bl_pwm->pwm_port == BL_PWM_VS) {
				BLPR("pwm_combo1_freq      = %d x vfreq\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_combo1_cnt       = %u\n",
					bl_pwm->pwm_cnt);
			} else {
				BLPR("pwm_combo1_freq      = %uHz\n",
					bl_pwm->pwm_freq);
				BLPR("pwm_combo1_cnt       = %u\n",
					bl_pwm->pwm_cnt);
				BLPR("pwm_combo1_pre_div   = %u\n",
					bl_pwm->pwm_pre_div);
			}
			BLPR("pwm_combo1_level_max = %u\n", bl_pwm->level_max);
			BLPR("pwm_combo1_level_min = %u\n", bl_pwm->level_min);
			BLPR("pwm_combo1_duty_max  = %d\n",
				bl_pwm->pwm_duty_max);
			BLPR("pwm_combo1_duty_min  = %d\n",
				bl_pwm->pwm_duty_min);
			BLPR("pwm_combo1_max       = %u\n", bl_pwm->pwm_max);
			BLPR("pwm_combo1_min       = %u\n", bl_pwm->pwm_min);
			BLPR("pwm_combo1_gpio      = %d\n", bl_pwm->pwm_gpio);
			BLPR("pwm_combo1_gpio_off  = %d\n",
				bl_pwm->pwm_gpio_off);
		}
		break;
	default:
		break;
	}
}

static int aml_bl_pinmux_load(struct bl_config_s *bconf)
{
	switch (bconf->method) {
	case BL_CTRL_PWM:
	case BL_CTRL_PWM_COMBO:
		/* get pinmux ctrl */
		bl_pwm_pinmux_ctrl(bconf, 1);
		break;
	case BL_CTRL_LOCAL_DIMING:
		break;
	case BL_CTRL_EXTERN:
		break;
	default:
		break;
	}

	return 0;
}

static int aml_bl_config_load_from_dts(struct bl_config_s *bconf,
		struct platform_device *pdev)
{
	int ret = 0;
	int val;
	const char *str;
	unsigned int bl_para[10];
	char bl_propname[20];
	int index = BL_INDEX_DEFAULT;
	struct device_node *child;
	struct bl_pwm_config_s *bl_pwm;
	struct bl_pwm_config_s *pwm_combo0, *pwm_combo1;

	/* select backlight by index */
#ifdef CONFIG_AML_LCD
	aml_lcd_notifier_call_chain(LCD_EVENT_BACKLIGHT_SEL, &index);
#endif
	bl_drv->index = index;
	sprintf(bl_propname, "backlight_%d", index);
	BLPR("load: %s\n", bl_propname);
	child = of_get_child_by_name(pdev->dev.of_node, bl_propname);
	if (child == NULL) {
		BLERR("failed to get %s\n", bl_propname);
		return -1;
	}

	ret = of_property_read_string(child, "bl_name", &str);
	if (ret) {
		BLERR("failed to get bl_name\n");
		str = "backlight";
	}
	strcpy(bconf->name, str);

	ret = of_property_read_u32_array(child, "bl_level_default_uboot_kernel",
		&bl_para[0], 2);
	if (ret) {
		BLERR("failed to get bl_level_default_uboot_kernel\n");
		bconf->level_default = BL_LEVEL_DEFAULT;
	} else {
		bconf->level_default = bl_para[1];
	}
	ret = of_property_read_u32_array(child, "bl_level_attr",
		&bl_para[0], 4);
	if (ret) {
		BLERR("failed to get bl_level_attr\n");
		bconf->level_min = BL_LEVEL_MIN;
		bconf->level_max = BL_LEVEL_MAX;
		bconf->level_mid = BL_LEVEL_MID;
		bconf->level_mid_mapping = BL_LEVEL_MID_MAPPED;
	} else {
		bconf->level_max = bl_para[0];
		bconf->level_min = bl_para[1];
		bconf->level_mid = bl_para[2];
		bconf->level_mid_mapping = bl_para[3];
	}
	/* adjust brightness_bypass by level_default */
	if (bconf->level_default > bconf->level_max) {
		brightness_bypass = 1;
		BLPR("level_default > level_max, enable brightness_bypass\n");
	}

	ret = of_property_read_u32(child, "bl_ctrl_method", &val);
	if (ret) {
		BLERR("failed to get bl_ctrl_method\n");
		bconf->method = BL_CTRL_MAX;
	} else {
		bconf->method = (val >= BL_CTRL_MAX) ? BL_CTRL_MAX : val;
	}
	ret = of_property_read_u32_array(child, "bl_power_attr",
		&bl_para[0], 5);
	if (ret) {
		BLERR("failed to get bl_power_attr\n");
		bconf->en_gpio = BL_GPIO_MAX;
		bconf->en_gpio_on = BL_GPIO_OUTPUT_HIGH;
		bconf->en_gpio_off = BL_GPIO_OUTPUT_LOW;
		bconf->power_on_delay = 100;
		bconf->power_off_delay = 30;
	} else {
		if (bl_para[0] >= BL_GPIO_NUM_MAX) {
			bconf->en_gpio = BL_GPIO_MAX;
		} else {
			bconf->en_gpio = bl_para[0];
			bl_gpio_register(bconf->en_gpio);
		}
		bconf->en_gpio_on = bl_para[1];
		bconf->en_gpio_off = bl_para[2];
		bconf->power_on_delay = bl_para[3];
		bconf->power_off_delay = bl_para[4];
	}

	switch (bconf->method) {
	case BL_CTRL_PWM:
		bconf->bl_pwm = kzalloc(sizeof(struct bl_pwm_config_s),
				GFP_KERNEL);
		if (bconf->bl_pwm == NULL) {
			BLERR("bl_pwm struct malloc error\n");
			return -1;
		}
		bl_pwm = bconf->bl_pwm;
		bl_pwm->index = 0;

		bl_pwm->level_max = bconf->level_max;
		bl_pwm->level_min = bconf->level_min;

		ret = of_property_read_string(child, "bl_pwm_port", &str);
		if (ret) {
			BLERR("failed to get bl_pwm_port\n");
			bl_pwm->pwm_port = BL_PWM_MAX;
		} else {
			bl_pwm->pwm_port = bl_pwm_str_to_pwm(str);
			BLPR("bl pwm_port: %s(%u)\n", str, bl_pwm->pwm_port);
		}
		ret = of_property_read_u32_array(child, "bl_pwm_attr",
			&bl_para[0], 4);
		if (ret) {
			BLERR("failed to get bl_pwm_attr\n");
			bl_pwm->pwm_method = BL_PWM_POSITIVE;
			if (bl_pwm->pwm_port == BL_PWM_VS)
				bl_pwm->pwm_freq = BL_FREQ_VS_DEFAULT;
			else
				bl_pwm->pwm_freq = BL_FREQ_DEFAULT;
			bl_pwm->pwm_duty_max = 80;
			bl_pwm->pwm_duty_min = 20;
		} else {
			bl_pwm->pwm_method = bl_para[0];
			bl_pwm->pwm_freq = bl_para[1];
			bl_pwm->pwm_duty_max = bl_para[2];
			bl_pwm->pwm_duty_min = bl_para[3];
		}
		if (bl_pwm->pwm_port == BL_PWM_VS) {
			if (bl_pwm->pwm_freq > 4) {
				BLERR("bl_pwm_vs wrong freq %d\n",
					bl_pwm->pwm_freq);
				bl_pwm->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (bl_pwm->pwm_freq > XTAL_HALF_FREQ_HZ)
				bl_pwm->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		ret = of_property_read_u32_array(child, "bl_pwm_power",
			&bl_para[0], 4);
		if (ret) {
			BLERR("failed to get bl_pwm_power\n");
			bl_pwm->pwm_gpio = BL_GPIO_MAX;
			bl_pwm->pwm_gpio_off = BL_GPIO_INPUT;
			bconf->pwm_on_delay = 0;
			bconf->pwm_off_delay = 0;
		} else {
			if (bl_para[0] >= BL_GPIO_NUM_MAX) {
				bl_pwm->pwm_gpio = BL_GPIO_MAX;
			} else {
				bl_pwm->pwm_gpio = bl_para[0];
				bl_gpio_multiplex_register(bl_pwm->pwm_gpio);
			}
			bl_pwm->pwm_gpio_off = bl_para[1];
			bconf->pwm_on_delay = bl_para[2];
			bconf->pwm_off_delay = bl_para[3];
		}

		bl_pwm->pwm_duty = bl_pwm->pwm_duty_min;
		/* init pwm config */
		bl_pwm_config_init(bl_pwm);
		bl_pwm->pinmux_flag = 0;
		break;
	case BL_CTRL_PWM_COMBO:
		bconf->bl_pwm_combo0 = kzalloc(sizeof(struct bl_pwm_config_s),
				GFP_KERNEL);
		if (bconf->bl_pwm_combo0 == NULL) {
			BLERR("bl_pwm_combo0 struct malloc error\n");
			return -1;
		}
		bconf->bl_pwm_combo1 = kzalloc(sizeof(struct bl_pwm_config_s),
				GFP_KERNEL);
		if (bconf->bl_pwm_combo1 == NULL) {
			BLERR("bl_pwm_combo1 struct malloc error\n");
			return -1;
		}
		pwm_combo0 = bconf->bl_pwm_combo0;
		pwm_combo1 = bconf->bl_pwm_combo1;

		pwm_combo0->index = 0;
		pwm_combo1->index = 1;

		ret = of_property_read_string_index(child, "bl_pwm_combo_port",
			0, &str);
		if (ret) {
			BLERR("failed to get bl_pwm_combo_port\n");
			pwm_combo0->pwm_port = BL_PWM_MAX;
		} else {
			pwm_combo0->pwm_port = bl_pwm_str_to_pwm(str);
		}
		ret = of_property_read_string_index(child, "bl_pwm_combo_port",
			1, &str);
		if (ret) {
			BLERR("failed to get bl_pwm_combo_port\n");
			pwm_combo1->pwm_port = BL_PWM_MAX;
		} else {
			pwm_combo1->pwm_port = bl_pwm_str_to_pwm(str);
		}
		BLPR("pwm_combo_port: %s(%u), %s(%u)\n",
			bl_pwm_name[pwm_combo0->pwm_port], pwm_combo0->pwm_port,
			bl_pwm_name[pwm_combo1->pwm_port],
			pwm_combo1->pwm_port);
		ret = of_property_read_u32_array(child,
			"bl_pwm_combo_level_mapping", &bl_para[0], 4);
		if (ret) {
			BLERR("failed to get bl_pwm_combo_level_mapping\n");
			pwm_combo0->level_max = BL_LEVEL_MAX;
			pwm_combo0->level_min = BL_LEVEL_MID;
			pwm_combo1->level_max = BL_LEVEL_MID;
			pwm_combo1->level_min = BL_LEVEL_MIN;
		} else {
			pwm_combo0->level_max = bl_para[0];
			pwm_combo0->level_min = bl_para[1];
			pwm_combo1->level_max = bl_para[2];
			pwm_combo1->level_min = bl_para[3];
		}
		ret = of_property_read_u32_array(child, "bl_pwm_combo_attr",
			&bl_para[0], 8);
		if (ret) {
			BLERR("failed to get bl_pwm_combo_attr\n");
			pwm_combo0->pwm_method = BL_PWM_POSITIVE;
			if (pwm_combo0->pwm_port == BL_PWM_VS)
				pwm_combo0->pwm_freq = BL_FREQ_VS_DEFAULT;
			else
				pwm_combo0->pwm_freq = BL_FREQ_DEFAULT;
			pwm_combo0->pwm_duty_max = 80;
			pwm_combo0->pwm_duty_min = 20;
			pwm_combo1->pwm_method = BL_PWM_NEGATIVE;
			if (pwm_combo1->pwm_port == BL_PWM_VS)
				pwm_combo1->pwm_freq = BL_FREQ_VS_DEFAULT;
			else
				pwm_combo1->pwm_freq = BL_FREQ_DEFAULT;
			pwm_combo1->pwm_duty_max = 80;
			pwm_combo1->pwm_duty_min = 20;
		} else {
			pwm_combo0->pwm_method = bl_para[0];
			pwm_combo0->pwm_freq = bl_para[1];
			pwm_combo0->pwm_duty_max = bl_para[2];
			pwm_combo0->pwm_duty_min = bl_para[3];
			pwm_combo1->pwm_method = bl_para[4];
			pwm_combo1->pwm_freq = bl_para[5];
			pwm_combo1->pwm_duty_max = bl_para[6];
			pwm_combo1->pwm_duty_min = bl_para[7];
		}
		if (pwm_combo0->pwm_port == BL_PWM_VS) {
			if (pwm_combo0->pwm_freq > 4) {
				BLERR("bl_pwm_0_vs wrong freq %d\n",
					pwm_combo0->pwm_freq);
				pwm_combo0->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (pwm_combo0->pwm_freq > XTAL_HALF_FREQ_HZ)
				pwm_combo0->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		if (pwm_combo1->pwm_port == BL_PWM_VS) {
			if (pwm_combo1->pwm_freq > 4) {
				BLERR("bl_pwm_1_vs wrong freq %d\n",
					pwm_combo1->pwm_freq);
				pwm_combo1->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (pwm_combo1->pwm_freq > XTAL_HALF_FREQ_HZ)
				pwm_combo1->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		ret = of_property_read_u32_array(child, "bl_pwm_combo_power",
			&bl_para[0], 6);
		if (ret) {
			BLERR("failed to get bl_pwm_combo_power\n");
			pwm_combo0->pwm_gpio = BL_GPIO_MAX;
			pwm_combo0->pwm_gpio_off = BL_GPIO_INPUT;
			pwm_combo1->pwm_gpio = BL_GPIO_MAX;
			pwm_combo1->pwm_gpio_off = BL_GPIO_INPUT;
			bconf->pwm_on_delay = 0;
			bconf->pwm_off_delay = 0;
		} else {
			if (bl_para[0] >= BL_GPIO_NUM_MAX) {
				pwm_combo0->pwm_gpio = BL_GPIO_MAX;
			} else {
				pwm_combo0->pwm_gpio = bl_para[0];
				bl_gpio_multiplex_register(
					pwm_combo0->pwm_gpio);
			}
			pwm_combo0->pwm_gpio_off = bl_para[1];
			if (bl_para[2] >= BL_GPIO_NUM_MAX) {
				pwm_combo1->pwm_gpio = BL_GPIO_MAX;
			} else {
				pwm_combo1->pwm_gpio = bl_para[2];
				bl_gpio_multiplex_register(
					pwm_combo1->pwm_gpio);
			}
			pwm_combo1->pwm_gpio_off = bl_para[3];
			bconf->pwm_on_delay = bl_para[4];
			bconf->pwm_off_delay = bl_para[5];
		}

		pwm_combo0->pwm_duty = pwm_combo0->pwm_duty_min;
		pwm_combo1->pwm_duty = pwm_combo1->pwm_duty_min;
		/* init pwm config */
		bl_pwm_config_init(pwm_combo0);
		bl_pwm_config_init(pwm_combo1);
		pwm_combo0->pinmux_flag = 0;
		pwm_combo1->pinmux_flag = 0;
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		break;
#endif
	case BL_CTRL_EXTERN:
		break;
	default:
		break;
	}

	return ret;
}
#endif

static int aml_bl_config_load_from_unifykey(struct bl_config_s *bconf)
{
	unsigned char *para;
	int key_len, len;
	unsigned char *p;
	const char *str;
	unsigned char temp;
	struct aml_lcd_unifykey_header_s bl_header;
	struct bl_pwm_config_s *bl_pwm;
	struct bl_pwm_config_s *pwm_combo0, *pwm_combo1;
	int ret;

	para = kmalloc((sizeof(unsigned char) * LCD_UKEY_BL_SIZE), GFP_KERNEL);
	if (!para) {
		BLERR("%s: Not enough memory\n", __func__);
		return -1;
	}

	key_len = LCD_UKEY_BL_SIZE;
	memset(para, 0, (sizeof(unsigned char) * key_len));
	ret = lcd_unifykey_get("backlight", para, &key_len);
	if (ret < 0) {
		kfree(para);
		return -1;
	}

	/* check backlight unifykey length */
	len = 10 + 30 + 12 + 8 + 32;
	ret = lcd_unifykey_len_check(key_len, len);
	if (ret < 0) {
		BLERR("unifykey length is not correct\n");
		kfree(para);
		return -1;
	}

	/* header: 10byte */
	lcd_unifykey_header_check(para, &bl_header);
	if (bl_debug_print_flag) {
		BLPR("unifykey header:\n");
		BLPR("crc32             = 0x%08x\n", bl_header.crc32);
		BLPR("data_len          = %d\n", bl_header.data_len);
		BLPR("version           = 0x%04x\n", bl_header.version);
		BLPR("reserved          = 0x%04x\n", bl_header.reserved);
	}

	/* basic: 30byte */
	p = para + LCD_UKEY_HEAD_SIZE;
	*(p + LCD_UKEY_BL_NAME - 1) = '\0'; /* ensure string ending */
	str = (const char *)p;
	strcpy(bconf->name, str);
	p += LCD_UKEY_BL_NAME;

	/* level: 6byte */
	p += LCD_UKEY_BL_LEVEL_UBOOT;
	bconf->level_default = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_LEVEL_KERNEL;  /* dummy pointer */
	bconf->level_max = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_LEVEL_MAX;
	bconf->level_min = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_LEVEL_MIN;
	bconf->level_mid = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_LEVEL_MID;
	bconf->level_mid_mapping = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_LEVEL_MID_MAP;

	/* adjust brightness_bypass by level_default */
	if (bconf->level_default > bconf->level_max) {
		brightness_bypass = 1;
		BLPR("level_default > level_max, enable brightness_bypass\n");
	}

	/* method: 8byte */
	temp = *p;
	bconf->method = (temp >= BL_CTRL_MAX) ? BL_CTRL_MAX : temp;
	p += LCD_UKEY_BL_METHOD;

	temp = *p;
	if (temp >= BL_GPIO_NUM_MAX) {
		bconf->en_gpio = BL_GPIO_MAX;
	} else {
		bconf->en_gpio = temp;
		bl_gpio_register(bconf->en_gpio);
	}
	p += LCD_UKEY_BL_EN_GPIO;
	bconf->en_gpio_on = *p;
	p += LCD_UKEY_BL_EN_GPIO_ON;
	bconf->en_gpio_off = *p;
	p += LCD_UKEY_BL_EN_GPIO_OFF;
	bconf->power_on_delay = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_ON_DELAY;
	bconf->power_off_delay = (*p | ((*(p + 1)) << 8));
	p += LCD_UKEY_BL_OFF_DELAY;

	/* pwm: 24byte */
	switch (bconf->method) {
	case BL_CTRL_PWM:
		bconf->bl_pwm = kzalloc(sizeof(struct bl_pwm_config_s),
				GFP_KERNEL);
		if (bconf->bl_pwm == NULL) {
			BLERR("bl_pwm struct malloc error\n");
			kfree(para);
			return -1;
		}
		bl_pwm = bconf->bl_pwm;
		bl_pwm->index = 0;

		bl_pwm->level_max = bconf->level_max;
		bl_pwm->level_min = bconf->level_min;

		bconf->pwm_on_delay = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_ON_DELAY;
		bconf->pwm_off_delay = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_OFF_DELAY;
		bl_pwm->pwm_method =  *p;
		p += LCD_UKEY_BL_PWM_METHOD;
		bl_pwm->pwm_port = *p;
		p += LCD_UKEY_BL_PWM_PORT;
		bl_pwm->pwm_freq = (*p | ((*(p + 1)) << 8) |
				((*(p + 2)) << 8) | ((*(p + 3)) << 8));
		if (bl_pwm->pwm_port == BL_PWM_VS) {
			if (bl_pwm->pwm_freq > 4) {
				BLERR("bl_pwm_vs wrong freq %d\n",
					bl_pwm->pwm_freq);
				bl_pwm->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (bl_pwm->pwm_freq > XTAL_HALF_FREQ_HZ)
				bl_pwm->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		p += LCD_UKEY_BL_PWM_FREQ;
		bl_pwm->pwm_duty_max = *p;
		p += LCD_UKEY_BL_PWM_DUTY_MAX;
		bl_pwm->pwm_duty_min = *p;
		p += LCD_UKEY_BL_PWM_DUTY_MIN;
		temp = *p;
		if (temp >= BL_GPIO_NUM_MAX) {
			bl_pwm->pwm_gpio = BL_GPIO_MAX;
		} else {
			bl_pwm->pwm_gpio = temp;
			bl_gpio_multiplex_register(bl_pwm->pwm_gpio);
		}
		p += LCD_UKEY_BL_PWM_GPIO;
		bl_pwm->pwm_gpio_off = *p;
		p += LCD_UKEY_BL_PWM_GPIO_OFF;

		/* dummy pointer (no need)
		p += LCD_UKEY_BL_PWM2_METHOD;
		p += LCD_UKEY_BL_PWM2_PORT;
		p += LCD_UKEY_BL_PWM2_FREQ;
		p += LCD_UKEY_BL_PWM2_DUTY_MAX;
		p += LCD_UKEY_BL_PWM2_DUTY_MIN;
		p += LCD_UKEY_BL_PWM2_GPIO;
		p += LCD_UKEY_BL_PWM2_GPIO_OFF;

		p += LCD_UKEY_BL_PWM_LEVEL_MAX;
		p += LCD_UKEY_BL_PWM_LEVEL_MIN;
		p += LCD_UKEY_BL_PWM2_LEVEL_MAX;
		p += LCD_UKEY_BL_PWM2_LEVEL_MIN; */

		bl_pwm->pwm_duty = bl_pwm->pwm_duty_min;
		bl_pwm_config_init(bl_pwm);
		bl_pwm->pinmux_flag = 0;
		break;
	case BL_CTRL_PWM_COMBO:
		bconf->bl_pwm_combo0 = kzalloc(sizeof(struct bl_pwm_config_s),
					GFP_KERNEL);
		if (bconf->bl_pwm_combo0 == NULL) {
			BLERR("bl_pwm_combo0 struct malloc error\n");
			kfree(para);
			return -1;
		}
		bconf->bl_pwm_combo1 = kzalloc(sizeof(struct bl_pwm_config_s),
					GFP_KERNEL);
		if (bconf->bl_pwm_combo1 == NULL) {
			BLERR("bl_pwm_combo1 struct malloc error\n");
			kfree(para);
			return -1;
		}
		pwm_combo0 = bconf->bl_pwm_combo0;
		pwm_combo1 = bconf->bl_pwm_combo1;
		pwm_combo0->index = 0;
		pwm_combo1->index = 1;

		bconf->pwm_on_delay = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_ON_DELAY;
		bconf->pwm_off_delay = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_OFF_DELAY;

		pwm_combo0->pwm_method = *p;
		p += LCD_UKEY_BL_PWM_METHOD;
		pwm_combo0->pwm_port = *p;
		p += LCD_UKEY_BL_PWM_PORT;
		pwm_combo0->pwm_freq = (*p | ((*(p + 1)) << 8) |
				((*(p + 2)) << 8) | ((*(p + 3)) << 8));
		if (pwm_combo0->pwm_port == BL_PWM_VS) {
			if (pwm_combo0->pwm_freq > 4) {
				BLERR("bl_pwm_0_vs wrong freq %d\n",
					pwm_combo0->pwm_freq);
				pwm_combo0->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (pwm_combo0->pwm_freq > XTAL_HALF_FREQ_HZ)
				pwm_combo0->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		p += LCD_UKEY_BL_PWM_FREQ;
		pwm_combo0->pwm_duty_max = *p;
		p += LCD_UKEY_BL_PWM_DUTY_MAX;
		pwm_combo0->pwm_duty_min = *p;
		p += LCD_UKEY_BL_PWM_DUTY_MIN;
		temp = *p;
		if (temp >= BL_GPIO_NUM_MAX) {
			pwm_combo0->pwm_gpio = BL_GPIO_MAX;
		} else {
			pwm_combo0->pwm_gpio = temp;
			bl_gpio_multiplex_register(pwm_combo0->pwm_gpio);
		}
		p += LCD_UKEY_BL_PWM_GPIO;
		pwm_combo0->pwm_gpio_off = *p;
		p += LCD_UKEY_BL_PWM_GPIO_OFF;
		pwm_combo1->pwm_method = *p;
		p += LCD_UKEY_BL_PWM2_METHOD;
		pwm_combo1->pwm_port =  *p;
		p += LCD_UKEY_BL_PWM2_PORT;
		pwm_combo1->pwm_freq = (*p | ((*(p + 1)) << 8) |
				((*(p + 2)) << 8) | ((*(p + 3)) << 8));
		if (pwm_combo1->pwm_port == BL_PWM_VS) {
			if (pwm_combo1->pwm_freq > 4) {
				BLERR("bl_pwm_1_vs wrong freq %d\n",
					pwm_combo1->pwm_freq);
				pwm_combo1->pwm_freq = BL_FREQ_VS_DEFAULT;
			}
		} else {
			if (pwm_combo1->pwm_freq > XTAL_HALF_FREQ_HZ)
				pwm_combo1->pwm_freq = XTAL_HALF_FREQ_HZ;
		}
		p += LCD_UKEY_BL_PWM2_FREQ;
		pwm_combo1->pwm_duty_max = *p;
		p += LCD_UKEY_BL_PWM2_DUTY_MAX;
		pwm_combo1->pwm_duty_min = *p;
		p += LCD_UKEY_BL_PWM2_DUTY_MIN;
		temp = *p;
		if (temp >= BL_GPIO_NUM_MAX) {
			pwm_combo1->pwm_gpio = BL_GPIO_MAX;
		} else {
			pwm_combo1->pwm_gpio = temp;
			bl_gpio_multiplex_register(pwm_combo1->pwm_gpio);
		}
		p += LCD_UKEY_BL_PWM2_GPIO;
		pwm_combo1->pwm_gpio_off = *p;
		p += LCD_UKEY_BL_PWM2_GPIO_OFF;

		pwm_combo0->level_max = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_LEVEL_MAX;
		pwm_combo0->level_min = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM_LEVEL_MIN;
		pwm_combo1->level_max = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM2_LEVEL_MAX;
		pwm_combo1->level_min = (*p | ((*(p + 1)) << 8));
		p += LCD_UKEY_BL_PWM2_LEVEL_MIN;

		pwm_combo0->pwm_duty = pwm_combo0->pwm_duty_min;
		pwm_combo1->pwm_duty = pwm_combo1->pwm_duty_min;
		bl_pwm_config_init(pwm_combo0);
		bl_pwm_config_init(pwm_combo1);
		pwm_combo0->pinmux_flag = 0;
		pwm_combo1->pinmux_flag = 0;
		break;
	default:
		break;
	}

	kfree(para);
	return 0;
}

static void aml_bl_config_load(struct bl_config_s *bconf,
		struct platform_device *pdev)
{
	int load_id = 0;
	int ret = 0;

	if (pdev->dev.of_node == NULL) {
		BLERR("no backlight of_node exist\n");
		return;
	}
	ret = of_property_read_u32(pdev->dev.of_node,
			"key_valid", &bl_key_valid);
	if (ret) {
		if (bl_debug_print_flag)
			BLPR("failed to get key_valid\n");
		bl_key_valid = 0;
	}
	BLPR("key_valid: %d\n", bl_key_valid);

	if (bl_key_valid) {
		ret = lcd_unifykey_check("backlight");
		if (ret < 0)
			load_id = 0;
		else
			load_id = 1;
	}
	if (load_id) {
		BLPR("%s from unifykey\n", __func__);
		bl_config_load = 1;
		aml_bl_config_load_from_unifykey(bconf);
	} else {
#ifdef CONFIG_OF
		BLPR("%s from dts\n", __func__);
		bl_config_load = 0;
		aml_bl_config_load_from_dts(bconf, pdev);
#endif
	}
	aml_bl_pinmux_load(bconf);
	aml_bl_config_print(bconf);

	switch (bconf->method) {
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		aml_ldim_probe(pdev);
		break;
#endif
	default:
		break;
	}
}

/* ****************************************
 * lcd notify
 * **************************************** */
static void aml_bl_delayd_on(struct work_struct *work)
{
	if (aml_bl_check_driver())
		return;

	/* lcd power on backlight flag */
	bl_drv->state |= (BL_STATE_LCD_ON | BL_STATE_BL_POWER_ON);
	BLPR("%s: bl_level=%u, state=0x%x\n",
		__func__, bl_drv->level, bl_drv->state);
	if (brightness_bypass) {
		if ((bl_drv->state & BL_STATE_BL_ON) == 0)
			bl_power_on();
	} else {
		aml_bl_update_status(bl_drv->bldev);
	}
}

static int aml_bl_on_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct bl_config_s *bconf;

	if ((event & LCD_EVENT_BL_ON) == 0)
		return NOTIFY_DONE;
	if (bl_debug_print_flag)
		BLPR("%s: 0x%lx\n", __func__, event);

	if (aml_bl_check_driver())
		return NOTIFY_DONE;

	bconf = bl_drv->bconf;
	/* lcd power on sequence control */
	if (bconf->method < BL_CTRL_MAX) {
		if (bl_drv->workqueue) {
			queue_delayed_work(bl_drv->workqueue,
				&bl_drv->bl_delayed_work,
				msecs_to_jiffies(bconf->power_on_delay));
		} else {
			BLPR("Warning: no bl workqueue\n");
			msleep(bconf->power_on_delay);
			/* lcd power on backlight flag */
			bl_drv->state |=
				(BL_STATE_LCD_ON | BL_STATE_BL_POWER_ON);
			if (brightness_bypass) {
				if ((bl_drv->state & BL_STATE_BL_ON) == 0)
					bl_power_on();
			} else {
				aml_bl_update_status(bl_drv->bldev);
			}
		}
	} else {
		BLERR("wrong backlight control method\n");
	}

	return NOTIFY_OK;
}

static int aml_bl_off_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	if ((event & LCD_EVENT_BL_OFF) == 0)
		return NOTIFY_DONE;
	if (bl_debug_print_flag)
		BLPR("%s: 0x%lx\n", __func__, event);

	if (aml_bl_check_driver())
		return NOTIFY_DONE;

	bl_drv->state &= ~(BL_STATE_LCD_ON | BL_STATE_BL_POWER_ON);
	if (brightness_bypass) {
		if (bl_drv->state & BL_STATE_BL_ON)
			bl_power_off();
	} else {
		aml_bl_update_status(bl_drv->bldev);
	}

	return NOTIFY_OK;
}

static struct notifier_block aml_bl_on_nb = {
	.notifier_call = aml_bl_on_notifier,
	.priority = LCD_PRIORITY_POWER_BL_ON,
};

static struct notifier_block aml_bl_off_nb = {
	.notifier_call = aml_bl_off_notifier,
	.priority = LCD_PRIORITY_POWER_BL_OFF,
};

static int aml_bl_lcd_update_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct bl_pwm_config_s *bl_pwm = NULL;

	/* If we aren't interested in this event, skip it immediately */
	if (event != LCD_EVENT_BACKLIGHT_UPDATE)
		return NOTIFY_DONE;

	if (aml_bl_check_driver())
		return NOTIFY_DONE;
	if (bl_debug_print_flag)
		BLPR("bl_lcd_update_notifier for pwm_vs\n");
	switch (bl_drv->bconf->method) {
	case BL_CTRL_PWM:
		if (bl_drv->bconf->bl_pwm->pwm_port == BL_PWM_VS)
			bl_pwm = bl_drv->bconf->bl_pwm;
		break;
	case BL_CTRL_PWM_COMBO:
		if (bl_drv->bconf->bl_pwm_combo0->pwm_port == BL_PWM_VS)
			bl_pwm = bl_drv->bconf->bl_pwm_combo0;
		else if (bl_drv->bconf->bl_pwm_combo1->pwm_port == BL_PWM_VS)
			bl_pwm = bl_drv->bconf->bl_pwm_combo1;
		break;
	default:
		break;
	}

	if (bl_pwm) {
		bl_pwm_config_init(bl_pwm);
		if (brightness_bypass)
			bl_set_duty_pwm(bl_pwm);
		else
			aml_bl_update_status(bl_drv->bldev);
	}

	return NOTIFY_OK;
}

static struct notifier_block aml_bl_lcd_update_nb = {
	.notifier_call = aml_bl_lcd_update_notifier,
};

static int aml_bl_lcd_test_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	int *flag;
#ifdef CONFIG_AML_LOCAL_DIMMING
	struct aml_ldim_driver_s *ldim_drv = aml_ldim_get_driver();
#endif

	/* If we aren't interested in this event, skip it immediately */
	if (event != LCD_EVENT_TEST_PATTERN)
		return NOTIFY_DONE;

	if (aml_bl_check_driver())
		return NOTIFY_DONE;
	if (bl_debug_print_flag)
		BLPR("bl_lcd_test_notifier for lcd test_pattern\n");

	flag = (int *)data;
	switch (bl_drv->bconf->method) {
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		if (ldim_drv->test_ctrl)
			ldim_drv->test_ctrl(*flag);
		break;
#endif
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block aml_bl_lcd_test_nb = {
	.notifier_call = aml_bl_lcd_test_notifier,
};
/* **************************************** */

/* ****************************************
 * bl debug calss
 * **************************************** */
struct class *bl_debug_class;

static const char *bl_debug_usage_str = {
"Usage:\n"
"    cat status ; dump backlight config\n"
"\n"
"    echo freq <index> <pwm_freq> > pwm ; set pwm frequency(unit in Hz for pwm, vfreq multiple for pwm_vs)\n"
"    echo duty <index> <pwm_duty> > pwm ; set pwm duty cycle(unit: %)\n"
"    echo pol <index> <pwm_pol> > pwm ; set pwm polarity(unit: %)\n"
"    cat pwm ; dump pwm state\n"
"\n"
"    echo <0|1> > power ; backlight power ctrl\n"
"    cat power ; print backlight power state\n"
"\n"
"    echo <0|1> > print ; 0=disable debug print; 1=enable debug print\n"
"    cat print ; read current debug print flag\n"
};

static ssize_t bl_debug_help(struct class *class,
		struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", bl_debug_usage_str);
}

static ssize_t bl_status_read(struct class *class,
		struct class_attribute *attr, char *buf)
{
	struct bl_config_s *bconf = bl_drv->bconf;
	struct bl_pwm_config_s *bl_pwm;
	struct bl_pwm_config_s *pwm_combo0, *pwm_combo1;
#ifdef CONFIG_AML_LOCAL_DIMMING
	struct aml_ldim_driver_s *ldim_drv = aml_ldim_get_driver();
#endif
	ssize_t len = 0;

	len = sprintf(buf, "read backlight status:\n"
		"key_valid:          %d\n"
		"config_load:        %d\n"
		"index:              %d\n"
		"name:               %s\n"
		"state:              0x%x\n"
		"level:              %d\n"
		"brightness_bypass:  %d\n\n"
		"level_max:          %d\n"
		"level_min:          %d\n"
		"level_mid:          %d\n"
		"level_mid_mapping:  %d\n\n"
		"method:             %s\n"
		"en_gpio:            %s(%d)\n"
		"en_gpio_on:         %d\n"
		"en_gpio_off:        %d\n"
		"power_on_delay:     %d\n"
		"power_off_delay:    %d\n\n",
		bl_key_valid, bl_config_load,
		bl_drv->index, bconf->name, bl_drv->state,
		bl_drv->level, brightness_bypass,
		bconf->level_max, bconf->level_min,
		bconf->level_mid, bconf->level_mid_mapping,
		bl_method_type_to_str(bconf->method),
		bconf->bl_gpio[bconf->en_gpio].name,
		bconf->en_gpio, bconf->en_gpio_on, bconf->en_gpio_off,
		bconf->power_on_delay, bconf->power_off_delay);
	switch (bconf->method) {
	case BL_CTRL_GPIO:
		len += sprintf(buf+len, "to do\n");
		break;
	case BL_CTRL_PWM:
		bl_pwm = bconf->bl_pwm;
		len += sprintf(buf+len,
			"pwm_method:         %d\n"
			"pwm_port:           %d\n"
			"pwm_freq:           %d\n"
			"pwm_duty_max:       %d\n"
			"pwm_duty_min:       %d\n"
			"pwm_gpio:           %s(%d)\n"
			"pwm_gpio_off:       %d\n"
			"pwm_on_delay:       %d\n"
			"pwm_off_delay:      %d\n\n",
			bl_pwm->pwm_method, bl_pwm->pwm_port, bl_pwm->pwm_freq,
			bl_pwm->pwm_duty_max, bl_pwm->pwm_duty_min,
			bconf->bl_gpio[bl_pwm->pwm_gpio].name,
			bl_pwm->pwm_gpio, bl_pwm->pwm_gpio_off,
			bconf->pwm_on_delay, bconf->pwm_off_delay);
		break;
	case BL_CTRL_PWM_COMBO:
		pwm_combo0 = bconf->bl_pwm_combo0;
		pwm_combo1 = bconf->bl_pwm_combo1;
		len += sprintf(buf+len,
			"pwm_0_level_max:    %d\n"
			"pwm_0_level_min:    %d\n"
			"pwm_0_method:       %d\n"
			"pwm_0_port:         %d\n"
			"pwm_0_freq:         %d\n"
			"pwm_0_duty_max:     %d\n"
			"pwm_0_duty_min:     %d\n"
			"pwm_0_gpio:         %s(%d)\n"
			"pwm_0_gpio_off:     %d\n"
			"pwm_1_level_max:    %d\n"
			"pwm_1_level_min:    %d\n"
			"pwm_1_method:       %d\n"
			"pwm_1_port:         %d\n"
			"pwm_1_freq:         %d\n"
			"pwm_1_duty_max:     %d\n"
			"pwm_1_duty_min:     %d\n"
			"pwm_1_gpio:         %s(%d)\n"
			"pwm_1_gpio_off:     %d\n"
			"pwm_on_delay:       %d\n"
			"pwm_off_delay:      %d\n\n",
			pwm_combo0->level_max, pwm_combo0->level_min,
			pwm_combo0->pwm_method, pwm_combo0->pwm_port,
			pwm_combo0->pwm_freq,
			pwm_combo0->pwm_duty_max, pwm_combo0->pwm_duty_min,
			bconf->bl_gpio[pwm_combo0->pwm_gpio].name,
			pwm_combo0->pwm_gpio, pwm_combo0->pwm_gpio_off,
			pwm_combo1->level_max, pwm_combo1->level_min,
			pwm_combo1->pwm_method, pwm_combo1->pwm_port,
			pwm_combo1->pwm_freq,
			pwm_combo1->pwm_duty_max, pwm_combo1->pwm_duty_min,
			bconf->bl_gpio[pwm_combo1->pwm_gpio].name,
			pwm_combo1->pwm_gpio, pwm_combo1->pwm_gpio_off,
			bconf->pwm_on_delay, bconf->pwm_off_delay);
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		if (ldim_drv->config_print)
			ldim_drv->config_print();
		break;
#endif
	case BL_CTRL_EXTERN:
		break;
	default:
		len += sprintf(buf+len, "wrong backlight control method\n");
		break;
	}
	return len;
}

static ssize_t bl_debug_pwm_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	struct bl_config_s *bconf = bl_drv->bconf;
	struct bl_pwm_config_s *bl_pwm;
	unsigned int value;
	ssize_t len = 0;

	len = sprintf(buf, "read backlight pwm state:\n");
	switch (bconf->method) {
	case BL_CTRL_PWM:
		if (bconf->bl_pwm) {
			bl_pwm = bconf->bl_pwm;
			len += sprintf(buf+len,
				"pwm_index:      %d\n"
				"pwm_method:     %d\n"
				"pwm_port:       %d\n"
				"pwm_freq:       %d\n"
				"pwm_duty_max:   %d\n"
				"pwm_duty_min:   %d\n"
				"pwm_cnt:        %d\n"
				"pwm_duty:       %d%%\n",
				bl_pwm->index, bl_pwm->pwm_method,
				bl_pwm->pwm_port, bl_pwm->pwm_freq,
				bl_pwm->pwm_duty_max, bl_pwm->pwm_duty_min,
				bl_pwm->pwm_cnt, bl_pwm->pwm_duty);
			switch (bl_pwm->pwm_port) {
			case BL_PWM_A:
			case BL_PWM_B:
			case BL_PWM_C:
			case BL_PWM_D:
			case BL_PWM_E:
			case BL_PWM_F:
				value = bl_cbus_read(pwm_reg[bl_pwm->pwm_port]);
				len += sprintf(buf+len,
					"pwm_reg:        0x%08x\n",
					value);
				break;
			case BL_PWM_VS:
				len += sprintf(buf+len,
					"pwm_reg0:        0x%08x\n"
					"pwm_reg1:        0x%08x\n"
					"pwm_reg2:        0x%08x\n"
					"pwm_reg3:        0x%08x\n",
					bl_vcbus_read(VPU_VPU_PWM_V0),
					bl_vcbus_read(VPU_VPU_PWM_V1),
					bl_vcbus_read(VPU_VPU_PWM_V2),
					bl_vcbus_read(VPU_VPU_PWM_V3));
				break;
			default:
				break;
			}
		}
		break;
	case BL_CTRL_PWM_COMBO:
		if (bconf->bl_pwm_combo0) {
			bl_pwm = bconf->bl_pwm_combo0;
			len += sprintf(buf+len,
				"pwm_0_index:        %d\n"
				"pwm_0_method:       %d\n"
				"pwm_0_port:         %d\n"
				"pwm_0_freq:         %d\n"
				"pwm_0_duty_max:     %d\n"
				"pwm_0_duty_min:     %d\n"
				"pwm_0_cnt:          %d\n"
				"pwm_0_duty:         %d%%\n",
				bl_pwm->index, bl_pwm->pwm_method,
				bl_pwm->pwm_port, bl_pwm->pwm_freq,
				bl_pwm->pwm_duty_max, bl_pwm->pwm_duty_min,
				bl_pwm->pwm_cnt, bl_pwm->pwm_duty);
			switch (bl_pwm->pwm_port) {
			case BL_PWM_A:
			case BL_PWM_B:
			case BL_PWM_C:
			case BL_PWM_D:
			case BL_PWM_E:
			case BL_PWM_F:
				value = bl_cbus_read(pwm_reg[bl_pwm->pwm_port]);
				len += sprintf(buf+len,
					"pwm_0_reg:          0x%08x\n",
					value);
				break;
			case BL_PWM_VS:
				len += sprintf(buf+len,
					"pwm_0_reg0:         0x%08x\n"
					"pwm_0_reg1:         0x%08x\n"
					"pwm_0_reg2:         0x%08x\n"
					"pwm_0_reg3:         0x%08x\n",
					bl_vcbus_read(VPU_VPU_PWM_V0),
					bl_vcbus_read(VPU_VPU_PWM_V1),
					bl_vcbus_read(VPU_VPU_PWM_V2),
					bl_vcbus_read(VPU_VPU_PWM_V3));
				break;
			default:
				break;
			}
		}
		if (bconf->bl_pwm_combo1) {
			bl_pwm = bconf->bl_pwm_combo1;
			len += sprintf(buf+len,
				"pwm_1_index:        %d\n"
				"pwm_1_method:       %d\n"
				"pwm_1_port:         %d\n"
				"pwm_1_freq:         %d\n"
				"pwm_1_duty_max:     %d\n"
				"pwm_1_duty_min:     %d\n"
				"pwm_1_cnt:          %d\n"
				"pwm_1_duty:         %d%%\n",
				bl_pwm->index, bl_pwm->pwm_method,
				bl_pwm->pwm_port, bl_pwm->pwm_freq,
				bl_pwm->pwm_duty_max, bl_pwm->pwm_duty_min,
				bl_pwm->pwm_cnt, bl_pwm->pwm_duty);
			switch (bl_pwm->pwm_port) {
			case BL_PWM_A:
			case BL_PWM_B:
			case BL_PWM_C:
			case BL_PWM_D:
			case BL_PWM_E:
			case BL_PWM_F:
				value = bl_cbus_read(pwm_reg[bl_pwm->pwm_port]);
				len += sprintf(buf+len,
					"pwm_1_reg:          0x%08x\n",
					value);
				break;
			case BL_PWM_VS:
				len += sprintf(buf+len,
					"pwm_1_reg0:         0x%08x\n"
					"pwm_1_reg1:         0x%08x\n"
					"pwm_1_reg2:         0x%08x\n"
					"pwm_1_reg3:         0x%08x\n",
					bl_vcbus_read(VPU_VPU_PWM_V0),
					bl_vcbus_read(VPU_VPU_PWM_V1),
					bl_vcbus_read(VPU_VPU_PWM_V2),
					bl_vcbus_read(VPU_VPU_PWM_V3));
				break;
			default:
				break;
			}
		}
		break;
	default:
		len += sprintf(buf+len, "not pwm control method\n");
		break;
	}
	return len;
}

#define BL_DEBUG_PWM_FREQ    0
#define BL_DEBUG_PWM_DUTY    1
#define BL_DEBUG_PWM_POL     2
static void bl_debug_pwm_set(unsigned int index, unsigned int value, int state)
{
	struct bl_config_s *bconf = bl_drv->bconf;
	struct bl_pwm_config_s *bl_pwm = NULL;

	if (aml_bl_check_driver())
		return;

	switch (bconf->method) {
	case BL_CTRL_PWM:
		bl_pwm = bconf->bl_pwm;
		break;
	case BL_CTRL_PWM_COMBO:
		if (index == 0)
			bl_pwm = bconf->bl_pwm_combo0;
		else
			bl_pwm = bconf->bl_pwm_combo1;
		break;
	default:
		BLERR("not pwm control method\n");
		break;
	}
	if (bl_pwm) {
		switch (state) {
		case BL_DEBUG_PWM_FREQ:
			bl_pwm->pwm_freq = value;
			bl_pwm_config_init(bl_pwm);
			bl_pwm_ctrl(bl_pwm, 1);
			bl_set_duty_pwm(bl_pwm);
			if (bl_debug_print_flag) {
				BLPR("set index(%d) pwm_port(%d) freq: %dHz\n",
				index, bl_pwm->pwm_port, bl_pwm->pwm_freq);
			}
			break;
		case BL_DEBUG_PWM_DUTY:
			bl_pwm->pwm_duty = value;
			bl_set_duty_pwm(bl_pwm);
			if (bl_debug_print_flag) {
				BLPR("set index(%d) pwm_port(%d) duty: %d%%\n",
				index, bl_pwm->pwm_port, bl_pwm->pwm_duty);
			}
			break;
		case BL_DEBUG_PWM_POL:
			bl_pwm->pwm_method = value;
			bl_pwm_config_init(bl_pwm);
			bl_set_duty_pwm(bl_pwm);
			if (bl_debug_print_flag) {
				BLPR("set index(%d) pwm_port(%d) method: %d\n",
				index, bl_pwm->pwm_port, bl_pwm->pwm_method);
			}
			break;
		default:
			break;
		}
	}
}

static ssize_t bl_debug_pwm_store(struct class *class,
		struct class_attribute *attr, const char *buf, size_t count)
{
	unsigned int ret;
	unsigned int index = 0, val = 0;

	switch (buf[0]) {
	case 'f': /* frequency */
		ret = sscanf(buf, "freq %d %d", &index, &val);
		bl_debug_pwm_set(index, val, BL_DEBUG_PWM_FREQ);
		break;
	case 'd': /* duty */
		ret = sscanf(buf, "duty %d %d", &index, &val);
		bl_debug_pwm_set(index, val, BL_DEBUG_PWM_DUTY);
		break;
	case 'p': /* polarity */
		ret = sscanf(buf, "pol %d %d", &index, &val);
		bl_debug_pwm_set(index, val, BL_DEBUG_PWM_POL);
		break;
	default:
		BLERR("wrong command\n");
		break;
	}

	if (ret != 1 || ret != 2)
		return -EINVAL;

	return count;
}

static ssize_t bl_debug_power_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	int state;

	if ((bl_drv->state & BL_STATE_LCD_ON) == 0) {
		state = 0;
	} else {
		if (bl_drv->state & BL_STATE_BL_POWER_ON)
			state = 1;
		else
			state = 0;
	}
	return sprintf(buf, "backlight power state: %d\n", state);
}

static ssize_t bl_debug_power_store(struct class *class,
		struct class_attribute *attr, const char *buf, size_t count)
{
	unsigned int ret;
	unsigned int temp = 0;

	ret = sscanf(buf, "%d", &temp);
	BLPR("power control: %u\n", temp);
	if ((bl_drv->state & BL_STATE_LCD_ON) == 0) {
		temp = 0;
		BLPR("backlight force off for lcd is off\n");
	}
	if (temp == 0) {
		bl_drv->state &= ~BL_STATE_BL_POWER_ON;
		if (bl_drv->state & BL_STATE_BL_ON)
			bl_power_off();
	} else {
		bl_drv->state |= BL_STATE_BL_POWER_ON;
		if ((bl_drv->state & BL_STATE_BL_ON) == 0)
			bl_power_on();
	}

	if (ret != 1 || ret != 2)
		return -EINVAL;

	return count;
}

static ssize_t bl_debug_key_valid_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", bl_key_valid);
}

static ssize_t bl_debug_config_load_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", bl_config_load);
}

static struct class_attribute bl_debug_class_attrs[] = {
	__ATTR(help, S_IRUGO | S_IWUSR, bl_debug_help, NULL),
	__ATTR(status, S_IRUGO | S_IWUSR, bl_status_read, NULL),
	__ATTR(pwm, S_IRUGO | S_IWUSR, bl_debug_pwm_show,
			bl_debug_pwm_store),
	__ATTR(power, S_IRUGO | S_IWUSR, bl_debug_power_show,
			bl_debug_power_store),
	__ATTR(key_valid,   S_IRUGO | S_IWUSR, bl_debug_key_valid_show, NULL),
	__ATTR(config_load, S_IRUGO | S_IWUSR,
		bl_debug_config_load_show, NULL),
};

static int aml_bl_creat_class(void)
{
	int i;

	bl_debug_class = class_create(THIS_MODULE, "aml_bl");
	if (IS_ERR(bl_debug_class)) {
		BLERR("create aml_bl debug class fail\n");
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(bl_debug_class_attrs); i++) {
		if (class_create_file(bl_debug_class,
				&bl_debug_class_attrs[i])) {
			BLERR("create aml_bl debug attribute %s fail\n",
				bl_debug_class_attrs[i].attr.name);
		}
	}
	return 0;
}

static int aml_bl_remove_class(void)
{
	int i;
	if (bl_debug_class == NULL)
		return -1;

	for (i = 0; i < ARRAY_SIZE(bl_debug_class_attrs); i++)
		class_remove_file(bl_debug_class, &bl_debug_class_attrs[i]);
	class_destroy(bl_debug_class);
	bl_debug_class = NULL;
	return 0;
}
/* **************************************** */

#ifdef CONFIG_PM
static int aml_bl_suspend(struct platform_device *pdev, pm_message_t state)
{
	switch (bl_off_policy) {
	case BL_OFF_POLICY_ONCE:
		aml_bl_off_policy_cnt += 1;
		break;
	default:
		break;
	}

	if (aml_bl_off_policy_cnt == 2) {
		aml_bl_off_policy_cnt = 0;
		bl_off_policy = BL_OFF_POLICY_NONE;
		BLPR("change bl_off_policy: %d\n", bl_off_policy);
	}

	BLPR("aml_bl_suspend: bl_off_policy=%d, aml_bl_off_policy_cnt=%d\n",
		bl_off_policy, aml_bl_off_policy_cnt);
	return 0;
}

static int aml_bl_resume(struct platform_device *pdev)
{
	return 0;
}
#endif

static int aml_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bldev;
	struct bl_config_s *bconf;
	int ret;

#ifdef AML_BACKLIGHT_DEBUG
	bl_debug_print_flag = 1;
#else
	bl_debug_print_flag = 0;
#endif

	aml_bl_off_policy_cnt = 0;

	/* init backlight parameters */
	brightness_bypass = 0;
	pwm_bypass = 0;
	bl_pwm_duty_free = 0;

	bl_chip_type = aml_bl_check_chip();
	bl_drv = kzalloc(sizeof(struct aml_bl_drv_s), GFP_KERNEL);
	if (!bl_drv) {
		BLERR("driver malloc error\n");
		return -ENOMEM;
	}

	bconf = &bl_config;
	bl_drv->dev = &pdev->dev;
	bl_drv->bconf = bconf;
	aml_bl_config_load(bconf, pdev);

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.power = FB_BLANK_UNBLANK; /* full on */
	props.max_brightness = (bconf->level_max > 0 ?
			bconf->level_max : BL_LEVEL_MAX);
	props.brightness = (bconf->level_default > 0 ?
			bconf->level_default : BL_LEVEL_DEFAULT);

	bldev = backlight_device_register(AML_BL_NAME, &pdev->dev,
					bl_drv, &aml_bl_ops, &props);
	if (IS_ERR(bldev)) {
		BLERR("failed to register backlight\n");
		ret = PTR_ERR(bldev);
		goto err;
	}
	bl_drv->bldev = bldev;
	/* platform_set_drvdata(pdev, bl_drv); */

	/* init workqueue */
	INIT_DELAYED_WORK(&bl_drv->bl_delayed_work, aml_bl_delayd_on);
	bl_drv->workqueue = create_workqueue("bl_power_on_queue");
	if (bl_drv->workqueue == NULL)
		BLERR("can't create bl work queue\n");

#ifdef CONFIG_AML_LCD
	ret = aml_lcd_notifier_register(&aml_bl_on_nb);
	if (ret)
		BLERR("register aml_bl_on_notifier failed\n");
	ret = aml_lcd_notifier_register(&aml_bl_off_nb);
	if (ret)
		BLERR("register aml_bl_off_notifier failed\n");
	ret = aml_lcd_notifier_register(&aml_bl_lcd_update_nb);
	if (ret)
		BLERR("register aml_bl_lcd_update_nb failed\n");
	ret = aml_lcd_notifier_register(&aml_bl_lcd_test_nb);
	if (ret)
		BLERR("register aml_bl_lcd_test_nb failed\n");
#endif
	aml_bl_creat_class();

	/* update bl status */
	bl_drv->state = (BL_STATE_LCD_ON |
			BL_STATE_BL_POWER_ON | BL_STATE_BL_ON);
	aml_bl_update_status(bl_drv->bldev);

	BLPR("probe OK\n");
	return 0;
err:
	kfree(bl_drv);
	bl_drv = NULL;
	return ret;
}

static int __exit aml_bl_remove(struct platform_device *pdev)
{
	int ret;
	/*struct aml_bl *bl_drv = platform_get_drvdata(pdev);*/

	aml_bl_remove_class();

	ret = cancel_delayed_work(&bl_drv->bl_delayed_work);
	if (bl_drv->workqueue)
		destroy_workqueue(bl_drv->workqueue);

	backlight_device_unregister(bl_drv->bldev);
#ifdef CONFIG_AML_LCD
	aml_lcd_notifier_unregister(&aml_bl_lcd_update_nb);
	aml_lcd_notifier_unregister(&aml_bl_on_nb);
	aml_lcd_notifier_unregister(&aml_bl_off_nb);
#endif
	/* platform_set_drvdata(pdev, NULL); */
	switch (bl_drv->bconf->method) {
	case BL_CTRL_PWM:
		kfree(bl_drv->bconf->bl_pwm);
		break;
	case BL_CTRL_PWM_COMBO:
		kfree(bl_drv->bconf->bl_pwm_combo0);
		kfree(bl_drv->bconf->bl_pwm_combo1);
		break;
#ifdef CONFIG_AML_LOCAL_DIMMING
	case BL_CTRL_LOCAL_DIMING:
		aml_ldim_remove();
		break;
#endif
	default:
		break;
	}
	kfree(bl_drv);
	bl_drv = NULL;
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id aml_bl_dt_match[] = {
	{
		.compatible = "amlogic, backlight",
	},
	{},
};
#endif

static struct platform_driver aml_bl_driver = {
	.driver = {
		.name  = AML_BL_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = aml_bl_dt_match,
#endif
	},
	.probe   = aml_bl_probe,
	.remove  = __exit_p(aml_bl_remove),
#ifdef CONFIG_PM
	.suspend = aml_bl_suspend,
	.resume  = aml_bl_resume,
#endif
};

static int __init aml_bl_init(void)
{
	if (platform_driver_register(&aml_bl_driver)) {
		BLPR("failed to register bl driver module\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit aml_bl_exit(void)
{
	platform_driver_unregister(&aml_bl_driver);
}

late_initcall(aml_bl_init);
module_exit(aml_bl_exit);

static int __init aml_bl_boot_para_setup(char *s)
{
	char bl_off_policy_str[10] = "none";

	bl_off_policy = BL_OFF_POLICY_NONE;
	aml_bl_off_policy_cnt = 0;
	if (NULL != s)
		sprintf(bl_off_policy_str, "%s", s);

	if (strncmp(bl_off_policy_str, "none", 2) == 0)
		bl_off_policy = BL_OFF_POLICY_NONE;
	else if (strncmp(bl_off_policy_str, "always", 2) == 0)
		bl_off_policy = BL_OFF_POLICY_ALWAYS;
	else if (strncmp(bl_off_policy_str, "once", 2) == 0)
		bl_off_policy = BL_OFF_POLICY_ONCE;
	BLPR("bl_off_policy: %s\n", bl_off_policy_str);

	return 0;
}
__setup("bl_off=", aml_bl_boot_para_setup);

MODULE_DESCRIPTION("AML Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amlogic, Inc.");

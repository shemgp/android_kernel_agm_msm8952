/* Copyright (c) 2009-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "%s:%d " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/of_gpio.h>
#include "msm_flash.h"
#include "msm_camera_dt_util.h"
#include "msm_cci.h"

#undef CDBG
#define CDBG(fmt, args...) pr_debug(fmt, ##args)

#define FATE_TORCH_CUR     180
#define FATE_FLASH_CUR     800


/* Just for hisense picking dual chroma  leds */
/*
#define TUNING_DUAL_LED_DEBUG 
*/

DEFINE_MSM_MUTEX(msm_flash_mutex);

static struct v4l2_file_operations msm_flash_v4l2_subdev_fops;
static struct led_trigger *torch_trigger;

static const struct of_device_id msm_flash_i2c_dt_match[] = {
	{.compatible = "qcom,camera-flash"},
	{}
};

static const struct i2c_device_id msm_flash_i2c_id[] = {
	{"qcom,camera-flash", (kernel_ulong_t)NULL},
	{ }
};

static const struct of_device_id msm_flash_dt_match[] = {
	{.compatible = "qcom,camera-flash", .data = NULL},
	{}
};

static struct msm_flash_table msm_i2c_flash_table;
static struct msm_flash_table msm_gpio_flash_table;
static struct msm_flash_table msm_pmic_flash_table;

static struct msm_flash_table *flash_table[] = {
	&msm_pmic_flash_table,
	&msm_i2c_flash_table,
	&msm_gpio_flash_table,
};

static struct msm_camera_i2c_fn_t msm_flash_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_qup_i2c_write_table_w_microdelay,
};

static struct msm_camera_i2c_fn_t msm_flash_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_poll =  msm_camera_cci_i2c_poll,
};
#ifdef CONFIG_HISENSE_LOW_BATTERY_FLASH_CTL
#include <linux/power_supply.h>
#define CAP_LIMIT_FOR_FLASH		15
bool check_battery_capacity_enough(void)
{
	union power_supply_propval prop = {0,};
	struct power_supply *batt_psy = power_supply_get_by_name("battery");
	int rc , batt_soc;

	rc = batt_psy->get_property(batt_psy, POWER_SUPPLY_PROP_CAPACITY, &prop);

	if (rc < 0)
		pr_err("could not read battery  capacity, rc=%d\n", rc);
	else
		batt_soc = prop.intval;

	pr_err("check_battery_capacity_enough  capacity=%d\n", batt_soc);

	if(batt_soc >= CAP_LIMIT_FOR_FLASH)
		return true;
	else
		return false;

}
#endif

void msm_torch_brightness_set(struct led_classdev *led_cdev,
				enum led_brightness value)
{
	if (!torch_trigger) {
		pr_err("No torch trigger found, can't set brightness\n");
		return;
	}

	led_trigger_event(torch_trigger, value);
};

static struct led_classdev msm_torch_led[MAX_LED_TRIGGERS] = {
	{
		.name		= "torch-light0",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
	{
		.name		= "torch-light1",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
	{
		.name		= "torch-light2",
		.brightness_set	= msm_torch_brightness_set,
		.brightness	= LED_OFF,
	},
};

static int32_t msm_torch_create_classdev(struct platform_device *pdev,
				void *data)
{
	int32_t rc = 0;
	int32_t i = 0;
	struct msm_flash_ctrl_t *fctrl =
		(struct msm_flash_ctrl_t *)data;

	if (!fctrl) {
		pr_err("Invalid fctrl\n");
		return -EINVAL;
	}

	for (i = 0; i < fctrl->torch_num_sources; i++) {
		if (fctrl->torch_trigger[i]) {
			torch_trigger = fctrl->torch_trigger[i];
			CDBG("%s:%d msm_torch_brightness_set for torch %d",
				__func__, __LINE__, i);
			msm_torch_brightness_set(&msm_torch_led[i],
				LED_OFF);

			rc = led_classdev_register(&pdev->dev,
				&msm_torch_led[i]);
			if (rc) {
				pr_err("Failed to register %d led dev. rc = %d\n",
						i, rc);
				return rc;
			}
		} else {
			pr_err("Invalid fctrl->torch_trigger[%d]\n", i);
			return -EINVAL;
		}
	}

	return 0;
};

static int32_t msm_flash_get_subdev_id(
	struct msm_flash_ctrl_t *flash_ctrl, void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("Enter\n");
	if (!subdev_id) {
		pr_err("failed\n");
		return -EINVAL;
	}
	if (flash_ctrl->flash_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		*subdev_id = flash_ctrl->pdev->id;
	else
		*subdev_id = flash_ctrl->subdev_id;

	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("Exit\n");
	return 0;
}

static int32_t msm_flash_i2c_write_table(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_camera_i2c_reg_setting_array *settings)
{
	struct msm_camera_i2c_reg_setting conf_array;

	conf_array.addr_type = settings->addr_type;
	conf_array.data_type = settings->data_type;
	conf_array.delay = settings->delay;
	conf_array.reg_setting = settings->reg_setting_a;
	conf_array.size = settings->size;
	flash_ctrl->flash_i2c_client.addr_type = conf_array.addr_type;

	return flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
		&flash_ctrl->flash_i2c_client, &conf_array);
}

#ifdef CONFIG_COMPAT
static void msm_flash_copy_power_settings_compat(
	struct msm_sensor_power_setting *ps,
	struct msm_sensor_power_setting32 *ps32, uint32_t size)
{
	uint16_t i = 0;

	for (i = 0; i < size; i++) {
		ps[i].config_val = ps32[i].config_val;
		ps[i].delay = ps32[i].delay;
		ps[i].seq_type = ps32[i].seq_type;
		ps[i].seq_val = ps32[i].seq_val;
	}
}
#endif

static int32_t msm_flash_i2c_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t rc = 0;
	struct msm_flash_init_info_t *flash_init_info =
		flash_data->cfg.flash_init_info;
	struct msm_camera_i2c_reg_setting_array *settings = NULL;
	struct msm_camera_cci_client *cci_client = NULL;
#ifdef CONFIG_COMPAT
	struct msm_sensor_power_setting_array32 *power_setting_array32 = NULL;
#endif
	if (!flash_init_info || !flash_init_info->power_setting_array) {
		pr_err("%s:%d failed: Null pointer\n", __func__, __LINE__);
		return -EFAULT;
	}

#ifdef CONFIG_COMPAT
	if (is_compat_task()) {
		power_setting_array32 = kzalloc(
			sizeof(struct msm_sensor_power_setting_array32),
			GFP_KERNEL);
		if (!power_setting_array32) {
			pr_err("%s mem allocation failed %d\n",
				__func__, __LINE__);
			return -ENOMEM;
		}

		if (copy_from_user(power_setting_array32,
			(void *)flash_init_info->power_setting_array,
			sizeof(struct msm_sensor_power_setting_array32))) {
			pr_err("%s copy_from_user failed %d\n",
				__func__, __LINE__);
			kfree(power_setting_array32);
			return -EFAULT;
		}

		flash_ctrl->power_setting_array.size =
			power_setting_array32->size;
		flash_ctrl->power_setting_array.size_down =
			power_setting_array32->size_down;
		flash_ctrl->power_setting_array.power_down_setting =
			compat_ptr(power_setting_array32->power_down_setting);
		flash_ctrl->power_setting_array.power_setting =
			compat_ptr(power_setting_array32->power_setting);

		/* Validate power_up array size and power_down array size */
		if ((!flash_ctrl->power_setting_array.size) ||
			(flash_ctrl->power_setting_array.size >
			MAX_POWER_CONFIG) ||
			(!flash_ctrl->power_setting_array.size_down) ||
			(flash_ctrl->power_setting_array.size_down >
			MAX_POWER_CONFIG)) {

			pr_err("failed: invalid size %d, size_down %d",
				flash_ctrl->power_setting_array.size,
				flash_ctrl->power_setting_array.size_down);
			kfree(power_setting_array32);
			power_setting_array32 = NULL;
			return -EINVAL;
		}
		/* Copy the settings from compat struct to regular struct */
		msm_flash_copy_power_settings_compat(
			flash_ctrl->power_setting_array.power_setting_a,
			power_setting_array32->power_setting_a,
			flash_ctrl->power_setting_array.size);

		msm_flash_copy_power_settings_compat(
			flash_ctrl->power_setting_array.power_down_setting_a,
			power_setting_array32->power_down_setting_a,
			flash_ctrl->power_setting_array.size_down);
	} else
#endif
	if (copy_from_user(&flash_ctrl->power_setting_array,
		(void *)flash_init_info->power_setting_array,
		sizeof(struct msm_sensor_power_setting_array))) {
		pr_err("%s copy_from_user failed %d\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (flash_ctrl->flash_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		cci_client = flash_ctrl->flash_i2c_client.cci_client;
		cci_client->sid = flash_init_info->slave_addr >> 1;
		cci_client->retries = 3;
		cci_client->id_map = 0;
		cci_client->i2c_freq_mode = flash_init_info->i2c_freq_mode;
	} else {
		flash_ctrl->flash_i2c_client.client->addr =
			flash_init_info->slave_addr;
	}

	flash_ctrl->power_info.power_setting =
		flash_ctrl->power_setting_array.power_setting_a;
	flash_ctrl->power_info.power_down_setting =
		flash_ctrl->power_setting_array.power_down_setting_a;
	flash_ctrl->power_info.power_setting_size =
		flash_ctrl->power_setting_array.size;
	flash_ctrl->power_info.power_down_setting_size =
		flash_ctrl->power_setting_array.size_down;

	rc = msm_camera_power_up(&flash_ctrl->power_info,
		flash_ctrl->flash_device_type,
		&flash_ctrl->flash_i2c_client);
	if (rc < 0) {
		pr_err("%s msm_camera_power_up failed %d\n",
			__func__, __LINE__);
		goto msm_flash_i2c_init_fail;
	}

	if (flash_data->cfg.flash_init_info->settings) {
		settings = kzalloc(sizeof(
			struct msm_camera_i2c_reg_setting_array), GFP_KERNEL);
		if (!settings) {
			pr_err("%s mem allocation failed %d\n",
				__func__, __LINE__);
			return -ENOMEM;
		}

		if (copy_from_user(settings, (void *)flash_init_info->settings,
			sizeof(struct msm_camera_i2c_reg_setting_array))) {
			kfree(settings);
			pr_err("%s copy_from_user failed %d\n",
				__func__, __LINE__);
			return -EFAULT;
		}

		rc = msm_flash_i2c_write_table(flash_ctrl, settings);
		kfree(settings);

		if (rc < 0) {
			pr_err("%s:%d msm_flash_i2c_write_table rc %d failed\n",
				__func__, __LINE__, rc);
		}
	}

	return 0;

msm_flash_i2c_init_fail:
	return rc;
}

static int32_t msm_flash_gpio_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t i = 0;
	int32_t rc = 0;

	CDBG("Enter");
	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		flash_ctrl->flash_op_current[i] = LED_FULL;

	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		flash_ctrl->torch_op_current[i] = LED_HALF;

	for (i = 0; i < flash_ctrl->torch_num_sources; i++) {
		if (!flash_ctrl->torch_trigger[i]) {
			if (i < flash_ctrl->flash_num_sources)
				flash_ctrl->torch_trigger[i] =
					flash_ctrl->flash_trigger[i];
			else
				flash_ctrl->torch_trigger[i] =
					flash_ctrl->flash_trigger[
					flash_ctrl->flash_num_sources - 1];
		}
	}

	rc = flash_ctrl->func_tbl->camera_flash_off(flash_ctrl, flash_data);

	CDBG("Exit");
	return rc;
}

static int32_t msm_flash_i2c_release(
	struct msm_flash_ctrl_t *flash_ctrl)
{
	int32_t rc = 0;

	if (!(&flash_ctrl->power_info) || !(&flash_ctrl->flash_i2c_client)) {
		pr_err("%s:%d failed: %p %p\n",
			__func__, __LINE__, &flash_ctrl->power_info,
			&flash_ctrl->flash_i2c_client);
		return -EINVAL;
	}

	rc = msm_camera_power_down(&flash_ctrl->power_info,
		flash_ctrl->flash_device_type,
		&flash_ctrl->flash_i2c_client);
	if (rc < 0) {
		pr_err("%s msm_camera_power_down failed %d\n",
			__func__, __LINE__);
		return -EINVAL;
	}
	return 0;
}

static int32_t msm_flash_off(struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t i = 0;

	CDBG("Enter\n");

	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		if (flash_ctrl->flash_trigger[i])
			led_trigger_event(flash_ctrl->flash_trigger[i], 0);

	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		if (flash_ctrl->torch_trigger[i])
			led_trigger_event(flash_ctrl->torch_trigger[i], 0);
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 0);

	CDBG("Exit\n");
	return 0;
}

static int32_t msm_flash_i2c_write_setting_array(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t rc = 0;
	struct msm_camera_i2c_reg_setting_array *settings = NULL;

	if (!flash_data->cfg.settings) {
		pr_err("%s:%d failed: Null pointer\n", __func__, __LINE__);
		return -EFAULT;
	}

	settings = kzalloc(sizeof(struct msm_camera_i2c_reg_setting_array),
		GFP_KERNEL);
	if (!settings) {
		pr_err("%s mem allocation failed %d\n", __func__, __LINE__);
		return -ENOMEM;
	}

	if (copy_from_user(settings, (void *)flash_data->cfg.settings,
		sizeof(struct msm_camera_i2c_reg_setting_array))) {
		kfree(settings);
		pr_err("%s copy_from_user failed %d\n", __func__, __LINE__);
		return -EFAULT;
	}

	rc = msm_flash_i2c_write_table(flash_ctrl, settings);
	kfree(settings);

	if (rc < 0) {
		pr_err("%s:%d msm_flash_i2c_write_table rc = %d failed\n",
			__func__, __LINE__, rc);
	}
	return rc;
}

static int32_t msm_flash_init(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
//	uint32_t i = 0;
	int32_t rc = -EFAULT;
	enum msm_flash_driver_type flash_driver_type = FLASH_DRIVER_DEFAULT;

	CDBG("%s: Enter flash_driver_type=%d \n", __func__, flash_ctrl->flash_driver_type);

	if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		pr_err("%s:%d Invalid flash state = %d\n",
			__func__, __LINE__, flash_ctrl->flash_state);
		return 0;
	}

	if(!flash_data){
		pr_err("%s:%d Invalid flash data\n", __func__, __LINE__);
		return 0;
	}

	flash_driver_type = flash_ctrl->flash_driver_type;
	
#if 0	
	if (flash_data->cfg.flash_init_info->flash_driver_type ==
		FLASH_DRIVER_DEFAULT) {
		flash_driver_type = flash_ctrl->flash_driver_type;
		for (i = 0; i < MAX_LED_TRIGGERS; i++) {
			flash_data->flash_current[i] =
				flash_ctrl->flash_max_current[i];
			flash_data->flash_duration[i] =
				flash_ctrl->flash_max_duration[i];

			CDBG("ctrl->flash_max_current[%d]=%d\n", i, flash_ctrl->flash_max_current[i]);
			CDBG("ctrl->flash_max_duration[%d]=%d\n", i, flash_ctrl->flash_max_duration[i]);
		}
	} else if (flash_data->cfg.flash_init_info->flash_driver_type ==
		flash_ctrl->flash_driver_type) {
		flash_driver_type = flash_ctrl->flash_driver_type;
		for (i = 0; i < MAX_LED_TRIGGERS; i++) {
			CDBG("data->flash_current[%d]=%d\n", i, flash_data->flash_current[i]);
			CDBG("data->flash_duration[%d]=%d\n", i, flash_data->flash_duration[i]);
	
			flash_ctrl->flash_max_current[i] =
				flash_data->flash_current[i];
			flash_data->flash_duration[i] =
				flash_ctrl->flash_max_duration[i];
		}
	}
#endif	

	if (flash_driver_type == FLASH_DRIVER_DEFAULT) {
		pr_err("%s:%d invalid flash_driver_type\n", __func__, __LINE__);
		return -EINVAL;
	}

#if 0
	for (i = 0; i < ARRAY_SIZE(flash_table); i++) {
		if (flash_driver_type == flash_table[i]->flash_driver_type) {
			flash_ctrl->func_tbl = &flash_table[i]->func_tbl;
			rc = 0;
		}
	}

	if (rc < 0) {
		pr_err("%s:%d failed invalid flash_driver_type %d\n",
			__func__, __LINE__,
			flash_data->cfg.flash_init_info->flash_driver_type);
	}
#endif

	rc = flash_ctrl->func_tbl->camera_flash_init(
			flash_ctrl, flash_data);
	if (rc < 0) {
		pr_err("%s:%d camera_flash_init failed rc = %d",
			__func__, __LINE__, rc);
		return rc;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_INIT;
	flash_ctrl->led_cl_dev.brightness = LED_DISABLE;//disable touch  added by hisense 16.02.17
	CDBG("%s Exit\n", __func__);
	return 0;
}

static int32_t msm_flash_low(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	uint32_t curr = 0, max_current = 0;
	int32_t i = 0;

	CDBG("Enter\n");
	/* Turn off flash triggers */
	for (i = 0; i < flash_ctrl->flash_num_sources; i++)
		if (flash_ctrl->flash_trigger[i])
			led_trigger_event(flash_ctrl->flash_trigger[i], 0);

	/* Turn on flash triggers */
	for (i = 0; i < flash_ctrl->torch_num_sources; i++) {
		if (flash_ctrl->torch_trigger[i]) {
			max_current = flash_ctrl->torch_max_current[i];
			if (flash_data->flash_current[i] >= 0 &&
				flash_data->flash_current[i] <
				max_current) {
				curr = flash_data->flash_current[i];
			} else {
				curr = flash_ctrl->torch_op_current[i];
				pr_debug("LED current clamped to %d\n",
					curr);
			}
			CDBG("low_flash_current[%d] = %d", i, curr);
			led_trigger_event(flash_ctrl->torch_trigger[i],
				curr);
		}
	}
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 1);
	CDBG("Exit\n");
	return 0;
}

static int32_t msm_flash_high(
	struct msm_flash_ctrl_t *flash_ctrl,
	struct msm_flash_cfg_data_t *flash_data)
{
	int32_t curr = 0;
	int32_t max_current = 0;
	int32_t i = 0;

	/* Turn off torch triggers */
	for (i = 0; i < flash_ctrl->torch_num_sources; i++)
		if (flash_ctrl->torch_trigger[i])
			led_trigger_event(flash_ctrl->torch_trigger[i], 0);

	/* Turn on flash triggers */
	for (i = 0; i < flash_ctrl->flash_num_sources; i++) {
		if (flash_ctrl->flash_trigger[i]) {
			max_current = flash_ctrl->flash_max_current[i];
			if (flash_data->flash_current[i] >= 0 &&
				flash_data->flash_current[i] <
				max_current) {
				curr = flash_data->flash_current[i];
			} else {
				curr = flash_ctrl->flash_op_current[i];
				pr_debug("LED flash_current[%d] clamped %d\n",
					i, curr);
			}
			CDBG("high_flash_current[%d] = %d", i, curr);
			led_trigger_event(flash_ctrl->flash_trigger[i],
				curr);
		}
	}
	if (flash_ctrl->switch_trigger)
		led_trigger_event(flash_ctrl->switch_trigger, 1);
	return 0;
}

static int32_t msm_flash_release(
	struct msm_flash_ctrl_t *flash_ctrl)
{
	int32_t rc = 0;
	if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_RELEASE) {
		pr_err("%s:%d Invalid flash state = %d",
			__func__, __LINE__, flash_ctrl->flash_state);
		return 0;
	}

	rc = flash_ctrl->func_tbl->camera_flash_off(flash_ctrl, NULL);
	if (rc < 0) {
		pr_err("%s:%d camera_flash_init failed rc = %d",
			__func__, __LINE__, rc);
		return rc;
	}
	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	flash_ctrl->led_cl_dev.brightness = LED_OFF;//disable touch  added by hisense 16.02.17
	return 0;
}

static int32_t msm_flash_config(struct msm_flash_ctrl_t *flash_ctrl,
	void __user *argp)
{
	int32_t rc = -EINVAL;
	struct msm_flash_cfg_data_t *flash_data =
		(struct msm_flash_cfg_data_t *) argp;

	mutex_lock(flash_ctrl->flash_mutex);

	CDBG("Enter %s type %d\n", __func__, flash_data->cfg_type);

	switch (flash_data->cfg_type) {
	case CFG_FLASH_INIT:
		rc = msm_flash_init(flash_ctrl, flash_data);
		break;
	case CFG_FLASH_RELEASE:
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT)
			rc = flash_ctrl->func_tbl->camera_flash_release(
				flash_ctrl);
		break;
	case CFG_FLASH_OFF:
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT)
			rc = flash_ctrl->func_tbl->camera_flash_off(
				flash_ctrl, flash_data);
		break;
	case CFG_FLASH_LOW:
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		#ifdef CONFIG_HISENSE_LOW_BATTERY_FLASH_CTL
		if (check_battery_capacity_enough()) {
			rc = flash_ctrl->func_tbl->camera_flash_low(
				flash_ctrl, flash_data);
		} else {
			pr_err("MSM_CAMERA_LED_LOW, don't flash\n");
			kobject_uevent(&flash_ctrl->led_cl_dev.dev->kobj, KOBJ_CHANGE);
		}
		#else
		rc = flash_ctrl->func_tbl->camera_flash_low(
				flash_ctrl, flash_data);
		#endif
		}
		break;
	case CFG_FLASH_HIGH:
		if (flash_ctrl->flash_state == MSM_CAMERA_FLASH_INIT) {
		#ifdef CONFIG_HISENSE_LOW_BATTERY_FLASH_CTL
		if (check_battery_capacity_enough()) {
			rc = flash_ctrl->func_tbl->camera_flash_high(
				flash_ctrl, flash_data);
		} else {
			pr_err("MSM_CAMERA_LED_LOW, don't flash\n");
			kobject_uevent(&flash_ctrl->led_cl_dev.dev->kobj, KOBJ_CHANGE);
		}
		#else
		rc = flash_ctrl->func_tbl->camera_flash_high(
				flash_ctrl, flash_data);
		#endif
		}
		break;
	default:
		rc = -EFAULT;
		break;
	}

	mutex_unlock(flash_ctrl->flash_mutex);

	CDBG("Exit %s type %d\n", __func__, flash_data->cfg_type);

	return rc;
}

static long msm_flash_subdev_ioctl(struct v4l2_subdev *sd,
	unsigned int cmd, void *arg)
{
	struct msm_flash_ctrl_t *fctrl = NULL;
	void __user *argp = (void __user *)arg;

	CDBG("Enter\n");

	if (!sd) {
		pr_err("sd NULL\n");
		return -EINVAL;
	}
	fctrl = v4l2_get_subdevdata(sd);
	if (!fctrl) {
		pr_err("fctrl NULL\n");
		return -EINVAL;
	}
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_flash_get_subdev_id(fctrl, argp);
	case VIDIOC_MSM_FLASH_CFG:
		return msm_flash_config(fctrl, argp);
	case MSM_SD_NOTIFY_FREEZE:
		return 0;
	case MSM_SD_SHUTDOWN:
		if (!fctrl->func_tbl) {
			pr_err("fctrl->func_tbl NULL\n");
			return -EINVAL;
		} else {
			return fctrl->func_tbl->camera_flash_release(fctrl);
		}
	default:
		pr_err_ratelimited("invalid cmd %d\n", cmd);
		return -ENOIOCTLCMD;
	}
	CDBG("Exit\n");
}

static struct v4l2_subdev_core_ops msm_flash_subdev_core_ops = {
	.ioctl = msm_flash_subdev_ioctl,
};

static struct v4l2_subdev_ops msm_flash_subdev_ops = {
	.core = &msm_flash_subdev_core_ops,
};

static const struct v4l2_subdev_internal_ops msm_flash_internal_ops;

static int32_t msm_flash_get_gpio_dt_data(struct device_node *of_node,
		struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0, i = 0;
	uint16_t *gpio_array = NULL;
	int16_t gpio_array_size = 0;
	struct msm_camera_gpio_conf *gconf = NULL;

	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size > 0) {
		fctrl->power_info.gpio_conf =
			 kzalloc(sizeof(struct msm_camera_gpio_conf),
				 GFP_KERNEL);
		if (!fctrl->power_info.gpio_conf) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			return rc;
		}
		gconf = fctrl->power_info.gpio_conf;

		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			goto free_gpio_conf;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			if (((int16_t)gpio_array[i]) < 0) {
				pr_err("%s failed %d\n", __func__, __LINE__);
				rc = -EINVAL;
				goto free_gpio_array;
			}
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_camera_get_dt_gpio_req_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_gpio_array;
		}

		rc = msm_camera_get_dt_gpio_set_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_cam_gpio_req_tbl;
		}

		rc = msm_camera_init_gpio_pin_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto free_cam_gpio_set_tbl;
		}
#if 0		
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_GPIO;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
#endif		
	}

	return 0;

free_cam_gpio_set_tbl:
	kfree(gconf->cam_gpio_set_tbl);
free_cam_gpio_req_tbl:
	kfree(gconf->cam_gpio_req_tbl);
free_gpio_array:
	kfree(gpio_array);
free_gpio_conf:
	kfree(fctrl->power_info.gpio_conf);
	return rc;
}

static int32_t msm_flash_get_setting(struct device_node *of_node,
		struct msm_flash_ctrl_t *fctrl, char *name, void* data)
{

	uint32_t count = 0;
	int i; 
	int rc;
	
	CDBG("%s:%d  E.\n", __func__, __LINE__);

	if (!of_node || !fctrl) {
		pr_err("%s:%d of_node or fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}	
	
	if (!name || !data) {
		pr_err("%s:%d name or data NULL\n", __func__, __LINE__);
		return -EINVAL;
	}
	
	if (of_get_property(of_node, name, &count)) {
		count /= sizeof(uint32_t);

		CDBG("%s:%d %s's num=%d\n", __func__, __LINE__, name, count);
		
		if (count > MAX_LED_TRIGGERS) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			rc = -EINVAL;
			goto Error;
		}
		
		rc = of_property_read_u32_array(of_node, name, data, count);
		if (rc < 0) {
			pr_err("%s:%d failed\n", __func__, __LINE__);
			goto Error;
		}

		for(i = 0; i < count; i++){
			CDBG("%s[%d] = %d\n", name, i, *((uint32_t*)data + i));	
		}
		
	}
	
	CDBG("%s:%d  X.\n", __func__, __LINE__);
	return 0;
Error:
	
	pr_err("%s:%d return fail\n", __func__, __LINE__);
	return  -EINVAL;

}



static int32_t msm_flash_get_caps(struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	int rc;
	uint32_t count = 0;


	CDBG("msm_flash_get_caps E.\n");
	
	rc = of_property_read_u32(of_node, "qcom,leds-num", &count);
	if (rc < 0) {
		pr_err("get qcom,leds-num failed\n");
		fctrl->flash_num_sources = 1;
		fctrl->torch_num_sources = 1;
	}else{
		fctrl->flash_num_sources = count;
		fctrl->torch_num_sources = count;		
	}

	msm_flash_get_setting(of_node, fctrl, "qcom,flash-max-current", fctrl->flash_max_current);
	msm_flash_get_setting(of_node, fctrl, "qcom,flash-op-current", fctrl->flash_op_current);
	msm_flash_get_setting(of_node, fctrl, "qcom,flash-max-timeout", fctrl->flash_max_timeout);
	msm_flash_get_setting(of_node, fctrl, "qcom,flash-op-timeout", fctrl->flash_op_timeout);
	msm_flash_get_setting(of_node, fctrl, "qcom,torch-max-current", fctrl->torch_max_current);
	msm_flash_get_setting(of_node, fctrl, "qcom,torch-op-current", fctrl->torch_op_current);
	msm_flash_get_setting(of_node, fctrl, "qcom,max-duration", fctrl->flash_max_duration);
	
	CDBG("msm_flash_get_caps X.\n");

	return 0;
}


static int32_t msm_flash_get_pmic_source_info(
	struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0;
	uint32_t count = 0, i = 0;
	struct device_node *flash_src_node = NULL;
	struct device_node *torch_src_node = NULL;
	struct device_node *switch_src_node = NULL;

	switch_src_node = of_parse_phandle(of_node, "qcom,switch-source", 0);
	if (!switch_src_node) {
		CDBG("%s:%d switch_src_node NULL\n", __func__, __LINE__);
	} else {
		rc = of_property_read_string(switch_src_node,
			"qcom,default-led-trigger",
			&fctrl->switch_trigger_name);
		if (rc < 0) {
			rc = of_property_read_string(switch_src_node,
				"linux,default-trigger",
				&fctrl->switch_trigger_name);
			if (rc < 0)
				pr_err("default-trigger read failed\n");
		}
		of_node_put(switch_src_node);
		switch_src_node = NULL;
		if (!rc) {
			CDBG("switch trigger %s\n",
				fctrl->switch_trigger_name);
			led_trigger_register_simple(
				fctrl->switch_trigger_name,
				&fctrl->switch_trigger);
		}
	}

	if (of_get_property(of_node, "qcom,flash-source", &count)) {
		count /= sizeof(uint32_t);
		CDBG("count %d\n", count);
		if (count > MAX_LED_TRIGGERS) {
			pr_err("invalid count\n");
			return -EINVAL;
		}
		fctrl->flash_num_sources = count;
		CDBG("%s:%d flash_num_sources = %d",
			__func__, __LINE__, fctrl->flash_num_sources);
		for (i = 0; i < count; i++) {
			flash_src_node = of_parse_phandle(of_node,
				"qcom,flash-source", i);
			if (!flash_src_node) {
				pr_err("flash_src_node NULL\n");
				continue;
			}

			rc = of_property_read_string(flash_src_node,
				"qcom,default-led-trigger",
				&fctrl->flash_trigger_name[i]);
			if (rc < 0) {
				rc = of_property_read_string(flash_src_node,
					"linux,default-trigger",
					&fctrl->flash_trigger_name[i]);
				if (rc < 0) {
					pr_err("default-trigger read failed\n");
					of_node_put(flash_src_node);
					continue;
				}
			}

			CDBG("default trigger %s\n",
				fctrl->flash_trigger_name[i]);

			/* Read operational-current */
			rc = of_property_read_u32(flash_src_node,
				"qcom,current",
				&fctrl->flash_op_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(flash_src_node);
				continue;
			}

			/* Read max-current */
			rc = of_property_read_u32(flash_src_node,
				"qcom,max-current",
				&fctrl->flash_max_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(flash_src_node);
				/* Non-fatal; this property is optional */
			}

			of_node_put(flash_src_node);

			CDBG("max_current[%d] %d current[%d] %d\n",
				i, fctrl->flash_max_current[i], i, fctrl->flash_op_current[i]);

			led_trigger_register_simple(
				fctrl->flash_trigger_name[i],
				&fctrl->flash_trigger[i]);
		}

#if 0		
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_PMIC;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
#endif

	}

	if (of_get_property(of_node, "qcom,torch-source", &count)) {
		count /= sizeof(uint32_t);
		CDBG("count %d\n", count);
		if (count > MAX_LED_TRIGGERS) {
			pr_err("invalid count\n");
			return -EINVAL;
		}
		fctrl->torch_num_sources = count;
		CDBG("%s:%d torch_num_sources = %d",
			__func__, __LINE__, fctrl->torch_num_sources);
		for (i = 0; i < count; i++) {
			torch_src_node = of_parse_phandle(of_node,
				"qcom,torch-source", i);
			if (!torch_src_node) {
				pr_err("torch_src_node NULL\n");
				continue;
			}

			rc = of_property_read_string(torch_src_node,
				"qcom,default-led-trigger",
				&fctrl->torch_trigger_name[i]);
			if (rc < 0) {
				rc = of_property_read_string(torch_src_node,
					"linux,default-trigger",
					&fctrl->torch_trigger_name[i]);
				if (rc < 0) {
					pr_err("default-trigger read failed\n");
					of_node_put(torch_src_node);
					continue;
				}
			}

			CDBG("default trigger %s\n",
				fctrl->torch_trigger_name[i]);

			/* Read operational-current */
			rc = of_property_read_u32(torch_src_node,
				"qcom,current",
				&fctrl->torch_op_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(torch_src_node);
				continue;
			}

			/* Read max-current */
			rc = of_property_read_u32(torch_src_node,
				"qcom,max-current",
				&fctrl->torch_max_current[i]);
			if (rc < 0) {
				pr_err("current: read failed\n");
				of_node_put(torch_src_node);
				continue;
			}

			of_node_put(torch_src_node);

			CDBG("max_current[%d] %d current[%d] %d\n",
				i, fctrl->torch_max_current[i], i, fctrl->torch_op_current[i]);

			led_trigger_register_simple(
				fctrl->torch_trigger_name[i],
				&fctrl->torch_trigger[i]);
		}
#if 0		
		if (fctrl->flash_driver_type == FLASH_DRIVER_DEFAULT)
			fctrl->flash_driver_type = FLASH_DRIVER_PMIC;
		CDBG("%s:%d fctrl->flash_driver_type = %d", __func__, __LINE__,
			fctrl->flash_driver_type);
#endif		
	}

	return 0;
}

static int32_t msm_flash_get_dt_data(struct device_node *of_node,
	struct msm_flash_ctrl_t *fctrl)
{
	int32_t rc = 0;
	uint32_t id_info[3];

	CDBG("msm_flash_get_dt_data E.\n");

	if (!of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	/* Read the sub device */
	rc = of_property_read_u32(of_node, "cell-index", &fctrl->pdev->id);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}
	fctrl->subdev_id = fctrl->pdev->id;
	
	CDBG("subdev id %d\n", fctrl->pdev->id);

	rc = of_property_read_u32(of_node, "qcom,flash-driver-type", 
		&fctrl->flash_driver_type);
	if (rc < 0) {
		pr_err("flash_driver_type: read failed. set default type=PMIC\n");
		fctrl->flash_driver_type = FLASH_DRIVER_PMIC;
	}
	
	CDBG("%s qcom,flash-driver-type=%d\n", __func__, fctrl->flash_driver_type);
	
	if(FLASH_DRIVER_PMIC == fctrl->flash_driver_type){
		/* Read the flash and torch source info from device tree node */
		rc = msm_flash_get_pmic_source_info(of_node, fctrl);
		if (rc < 0) {
			pr_err("%s:%d msm_flash_get_pmic_source_info failed rc %d\n",
				__func__, __LINE__, rc);			
		}

		fctrl->func_tbl = &flash_table[FLASH_DRIVER_PMIC]->func_tbl;
		CDBG("%s FLASH_DRIVER_PMIC X.\n", __func__);
		return rc;
	}
    
    if(FLASH_DRIVER_I2C == fctrl->flash_driver_type){
        fctrl->front_flash_node = false;
	    fctrl->front_flash_node = of_property_read_bool(of_node,
						"qcom,front_flash_node");
        CDBG("%s front_flash_node = %d\n",__func__,fctrl->front_flash_node);
    }

	/* Read the gpio information from device tree */
	rc = msm_flash_get_gpio_dt_data(of_node, fctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_gpio_dt_data failed rc %d\n",
			__func__, __LINE__, rc);
		return rc;
	}
	
	if(FLASH_DRIVER_GPIO == fctrl->flash_driver_type){		
		CDBG("%s FLASH_DRIVER_GPIO X.\n", __func__);
		return rc;		
	}
	
	/* Read the CCI master. Use M0 if not available in the node */
	rc = of_property_read_u32(of_node, "qcom,cci-master",
		&fctrl->cci_i2c_master);
	CDBG("%s qcom,cci-master %d, rc %d\n", __func__, 
		fctrl->cci_i2c_master, rc);
	if (rc < 0) {
		/* Set default master 0 */
		fctrl->cci_i2c_master = MASTER_0;
		rc = 0;
	}

	fctrl->slave_info =	kzalloc(sizeof(struct msm_camera_slave_info), GFP_KERNEL);
	if (!fctrl->slave_info) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto Error0;
	}
	
	rc = of_property_read_u32_array(of_node, "qcom,slave-id",
		id_info, 3);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		rc = -EINVAL;
		goto Error1;
	}
	
	fctrl->slave_info->sensor_slave_addr = id_info[0];
	fctrl->slave_info->sensor_id_reg_addr = id_info[1];
	fctrl->slave_info->sensor_id = id_info[2];
	CDBG("%s slave_info=<0x%x 0x%x 0x%x>\n", __func__, 
		id_info[0], id_info[1], id_info[2]);

	msm_flash_get_caps(of_node, fctrl);

	/*Update i2c flash driver's interface*/
	CDBG("%s FLASH_DRIVER_I2C X.\n", __func__);
	
	return 0;
	
Error1:
	if(fctrl->slave_info){
		kfree(fctrl->slave_info);
		fctrl->slave_info = NULL;
	}
	
Error0:
	
	return rc;
}

#ifdef CONFIG_COMPAT
static long msm_flash_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	int32_t i = 0;
	int32_t rc = 0;
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);
	struct msm_flash_cfg_data_t32 *u32 =
		(struct msm_flash_cfg_data_t32 *)arg;
	struct msm_flash_cfg_data_t flash_data;
	struct msm_flash_init_info_t32 flash_init_info32;
	struct msm_flash_init_info_t flash_init_info;

	CDBG("Enter");
	flash_data.cfg_type = u32->cfg_type;
	for (i = 0; i < MAX_LED_TRIGGERS; i++) {
		flash_data.flash_current[i] = u32->flash_current[i];
		flash_data.flash_duration[i] = u32->flash_duration[i];
	}
	switch (cmd) {
	case VIDIOC_MSM_FLASH_CFG32:
		cmd = VIDIOC_MSM_FLASH_CFG;
		switch (flash_data.cfg_type) {
		case CFG_FLASH_OFF:
		case CFG_FLASH_LOW:
		case CFG_FLASH_HIGH:
			flash_data.cfg.settings = compat_ptr(u32->cfg.settings);
			break;
		case CFG_FLASH_INIT:
			flash_data.cfg.flash_init_info = &flash_init_info;
			if (copy_from_user(&flash_init_info32,
				(void *)compat_ptr(u32->cfg.flash_init_info),
				sizeof(struct msm_flash_init_info_t32))) {
				pr_err("%s copy_from_user failed %d\n",
					__func__, __LINE__);
				return -EFAULT;
			}
			flash_init_info.flash_driver_type =
				flash_init_info32.flash_driver_type;
			flash_init_info.slave_addr =
				flash_init_info32.slave_addr;
			flash_init_info.i2c_freq_mode =
				flash_init_info32.i2c_freq_mode;
			flash_init_info.settings =
				compat_ptr(flash_init_info32.settings);
			flash_init_info.power_setting_array =
				compat_ptr(
				flash_init_info32.power_setting_array);
			break;
		default:
			break;
		}
		break;
	default:
		return msm_flash_subdev_ioctl(sd, cmd, arg);
	}

	rc =  msm_flash_subdev_ioctl(sd, cmd, &flash_data);
	for (i = 0; i < MAX_LED_TRIGGERS; i++) {
		u32->flash_current[i] = flash_data.flash_current[i];
		u32->flash_duration[i] = flash_data.flash_duration[i];
	}
	CDBG("Exit");
	return rc;
}

static long msm_flash_subdev_fops_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_flash_subdev_do_ioctl);
}
#endif


#ifdef CONFIG_HISENSE_CAMLEDS_FATE_TEST
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
static void msm_fate_torch_wake_lock(struct msm_flash_ctrl_t *flash_ctrl, int lock)
{
	if(lock){
		/*lock wake*/
		if (!wake_lock_active(&flash_ctrl->torch_wake_lock)) {
			wake_lock(&flash_ctrl->torch_wake_lock);
			CDBG("%s: torch wake lock!\n", __func__);
		}
	}else{
		/*unlock wake*/
		if (wake_lock_active(&flash_ctrl->torch_wake_lock)) {
			wake_unlock(&flash_ctrl->torch_wake_lock);
			CDBG("%s: torch wake unlock!\n", __func__);
		}
	}
}
#endif

static void msm_fate_brightness_set(struct led_classdev *led_cdev,
				unsigned int  value)
{
	int32_t i = 0;
	struct msm_flash_ctrl_t *flash_ctrl = container_of(led_cdev, struct msm_flash_ctrl_t, led_cl_dev);

	CDBG("%s: Enter.  value=%d\n", __func__, value);
	
	switch(value){
		case LED_TORCH1:{
			/* Turn off flash triggers */	
			for (i = 0; i < flash_ctrl->flash_num_sources; i++)
				if (flash_ctrl->flash_trigger[i])
					led_trigger_event(flash_ctrl->flash_trigger[i], 0);

			if(flash_ctrl->torch_trigger[0])
				led_trigger_event(flash_ctrl->torch_trigger[0],	FATE_TORCH_CUR);
			
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
			msm_fate_torch_wake_lock(flash_ctrl, 1);
#endif
		}
		break;
					
		case LED_TORCH2:{
			/* Turn off flash triggers */	
			for (i = 0; i < flash_ctrl->flash_num_sources; i++)
				if (flash_ctrl->flash_trigger[i])
					led_trigger_event(flash_ctrl->flash_trigger[i], 0);
			
			if(flash_ctrl->torch_trigger[1])
				led_trigger_event(flash_ctrl->torch_trigger[1], FATE_TORCH_CUR);
			
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
			msm_fate_torch_wake_lock(flash_ctrl, 1);
#endif
		}
		break;
			
		case LED_FLASH1:{
			/* Turn off torch triggers */
			for (i = 0; i < flash_ctrl->torch_num_sources; i++)
				if (flash_ctrl->torch_trigger[i])
					led_trigger_event(flash_ctrl->torch_trigger[i], 0);

			if(flash_ctrl->flash_trigger[0])
				led_trigger_event(flash_ctrl->flash_trigger[0],	FATE_FLASH_CUR);
			
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);
		}
		break;
		
		case LED_FLASH2:{
			/* Turn off torch triggers */
			for (i = 0; i < flash_ctrl->torch_num_sources; i++)
				if (flash_ctrl->torch_trigger[i])
					led_trigger_event(flash_ctrl->torch_trigger[i], 0);

			if(flash_ctrl->flash_trigger[1])
				led_trigger_event(flash_ctrl->flash_trigger[1],	FATE_FLASH_CUR);
			
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);			
		}
		break;


		case LED_FULL:{
			/* Turn off flash triggers */	
			for (i = 0; i < flash_ctrl->flash_num_sources; i++)
				if (flash_ctrl->flash_trigger[i])
					led_trigger_event(flash_ctrl->flash_trigger[i], 0);

			
			for (i = 0; i < flash_ctrl->flash_num_sources; i++) {
				if (flash_ctrl->torch_trigger[i]) {
					led_trigger_event(flash_ctrl->torch_trigger[i], FATE_TORCH_CUR);
				}
			}
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
			msm_fate_torch_wake_lock(flash_ctrl, 1);
#endif
		}
		break;

		case LED_FULL_FLASH:{ // 90 For Fate dual led flash 
			/* Turn off torch triggers */
			for (i = 0; i < flash_ctrl->torch_num_sources; i++)
				if (flash_ctrl->torch_trigger[i])
					led_trigger_event(flash_ctrl->torch_trigger[i], 0);

			for (i = 0; i < flash_ctrl->flash_num_sources; i++) {
				if (flash_ctrl->flash_trigger[i]) {
					led_trigger_event(flash_ctrl->flash_trigger[i], FATE_FLASH_CUR);
				}
			}		
			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 1);					
		
		}
		break;

		
		case LED_OFF:{
			/* Turn off flash triggers */	
			for (i = 0; i < flash_ctrl->flash_num_sources; i++)
				if (flash_ctrl->flash_trigger[i])
					led_trigger_event(flash_ctrl->flash_trigger[i], 0);
			
			
			/* Turn off torch triggers */
			for (i = 0; i < flash_ctrl->torch_num_sources; i++)
				if (flash_ctrl->torch_trigger[i])
					led_trigger_event(flash_ctrl->torch_trigger[i], 0);

			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 0);
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
			msm_fate_torch_wake_lock(flash_ctrl, 0);
#endif
		}
		break;
			
		default:{			
#ifdef TUNING_DUAL_LED_DEBUG
			static int trg_cnt = 0;
			int cnt = value / 100000000;
			int led0_cur = 0;
			int led1_cur = 0;
			int j = 0;
			
			pr_err("%s: value=%d\n", __func__, value);
			if(cnt){
				/*set triger count*/
				trg_cnt = value % 100000000;
				pr_err("%s: set trg_cnt = %d\n", __func__, trg_cnt);
				break;
			}else{
				/*set dual chroma leds current*/
				led0_cur = value / 10000;
				led1_cur = value % 10000;				
				pr_err("%s: set led0_cur = %d\n", __func__, led0_cur);
				pr_err("%s: set led1_cur = %d\n", __func__, led1_cur);
				if(led0_cur > 1000){
					pr_err("%s: set led0_cur = %d > max_cur 1000mA. fail!\n", __func__, led0_cur);
					break;
				}
				if(led1_cur > 1000){
					pr_err("%s: set led1_cur = %d > max_cur 1000mA. fail!\n", __func__, led1_cur);
					break;
				}
			}

			for(j = 0; j < trg_cnt; j++){
				
				if (flash_ctrl->flash_trigger[0]) {
					led_trigger_event(flash_ctrl->flash_trigger[0], led0_cur);
				}
				if (flash_ctrl->flash_trigger[1]) {
					led_trigger_event(flash_ctrl->flash_trigger[1], led1_cur);
				}
				if (flash_ctrl->switch_trigger)
					led_trigger_event(flash_ctrl->switch_trigger, 1);

				pr_err("%s: msleep(300)\n", __func__);

				msleep(300);
				
				/* Turn off flash triggers */	
				for (i = 0; i < flash_ctrl->flash_num_sources; i++)
					if (flash_ctrl->flash_trigger[i])
						led_trigger_event(flash_ctrl->flash_trigger[i], 0);

				
				if (flash_ctrl->switch_trigger)
					led_trigger_event(flash_ctrl->switch_trigger, 0);

				msleep(3600);
				pr_err("%s: msleep(3600)\n",  __func__);
			}	

#else
			
			/* Turn off flash triggers */	
			for (i = 0; i < flash_ctrl->flash_num_sources; i++)
				if (flash_ctrl->flash_trigger[i])
					led_trigger_event(flash_ctrl->flash_trigger[i], 0);


			/* Turn off torch triggers */
			for (i = 0; i < flash_ctrl->torch_num_sources; i++)
				if (flash_ctrl->torch_trigger[i])
					led_trigger_event(flash_ctrl->torch_trigger[i], 0);

			if (flash_ctrl->switch_trigger)
				led_trigger_event(flash_ctrl->switch_trigger, 0);
#endif

#ifdef CONFIG_HISENSE_TORCH_WAKEUP
			msm_fate_torch_wake_lock(flash_ctrl, 0);
#endif
			break;
		}
			
	}
	
	CDBG("%s: Exit\n", __func__);
	return;
};



static int msm_flash_create_classdev_4fate(struct platform_device *pdev,
				void *data)
{
	struct msm_flash_ctrl_t *fctrl = (struct msm_flash_ctrl_t *)data;

	if (!fctrl) {
		pr_err("Invalid fctrl\n");
		return -EINVAL;
	}
	fctrl->led_cl_dev.name = "flashlight";	
	fctrl->led_cl_dev.brightness = LED_OFF;
	fctrl->led_cl_dev.brightness_set = msm_fate_brightness_set;
	fctrl->led_cl_dev.max_brightness = 0x7FFFFFFF;//1000;

	msm_fate_brightness_set(&fctrl->led_cl_dev, LED_OFF);
	led_classdev_register(&pdev->dev, &fctrl->led_cl_dev);

	return 0;
}
#endif /* CONFIG_HISENSE_CAMLEDS_FATE_TEST */

/**********modified by hisense for dual led torch/flash control*********/
void msm_flash_i2c_rear_brightness_set(struct led_classdev *led_cdev,
				unsigned int value)
{
	int i;
	struct msm_flash_ctrl_t *fctrl = container_of(led_cdev, struct msm_flash_ctrl_t, led_cl_dev);
	struct msm_flash_cfg_data_t *flash_data = NULL;
    CDBG("%s E\n",__func__);
    CDBG("%s value = %d led_state = %d\n",__func__,
        value,fctrl->led_state);
	if(fctrl){
		if (value == LED_FULL) {
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->func_tbl->camera_flash_low) {
					for (i = 0; i < fctrl->torch_num_sources; i++) {
						if (fctrl->torch_max_current[i] > 0) {
							fctrl->torch_op_current[i] = fctrl->torch_max_current[i];
						}
					}
					fctrl->func_tbl->camera_flash_low(fctrl,flash_data);
				}
			}
		} else if (value == LED_FULL_FLASH) {
			if (fctrl->led_state != MSM_CAMERA_LED_INIT) {
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->func_tbl->camera_flash_high) {
					for (i = 0; i < fctrl->flash_num_sources; i++) {
						if (fctrl->flash_max_current[i] > 0) {
							fctrl->flash_op_current[i] = (fctrl->flash_max_current[i])/2;
						}
					}

					fctrl->func_tbl->camera_flash_high(fctrl,flash_data);
				}
			}
		} else if (value == LED_TORCH1) {
			if (fctrl->led_state != MSM_CAMERA_LED_INIT) {
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->reg_setting && fctrl->reg_setting->led1_low_setting) {
					fctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
						&fctrl->flash_i2c_client,
						fctrl->reg_setting->led1_low_setting);
				}

				if(fctrl->func_tbl->camera_flash_clear_status){
					fctrl->func_tbl->camera_flash_clear_status(fctrl);
				}
			}
		}
		else if (value == LED_TORCH2){
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->reg_setting && fctrl->reg_setting->led2_low_setting) {
					fctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
						&fctrl->flash_i2c_client,
						fctrl->reg_setting->led2_low_setting);
				}
				if(fctrl->func_tbl->camera_flash_clear_status){
					fctrl->func_tbl->camera_flash_clear_status(fctrl);
				}
			}
		}
		else if (value == LED_FLASH1){
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->reg_setting && fctrl->reg_setting->led1_high_setting) {
					fctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
						&fctrl->flash_i2c_client,
						fctrl->reg_setting->led1_high_setting);
			}
				if(fctrl->func_tbl->camera_flash_clear_status){
					fctrl->func_tbl->camera_flash_clear_status(fctrl);
				}
			}
		}
		else if (value == LED_FLASH2){
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
                if(fctrl->func_tbl->camera_flash_power_on){
                    fctrl->func_tbl->camera_flash_power_on(fctrl);
                }
				fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->reg_setting && fctrl->reg_setting->led2_high_setting) {
					fctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
						&fctrl->flash_i2c_client,
						fctrl->reg_setting->led2_high_setting);
				}
				if(fctrl->func_tbl->camera_flash_clear_status){
					fctrl->func_tbl->camera_flash_clear_status(fctrl);
				}
			}
		}
		else {
			if(fctrl->led_state != MSM_CAMERA_LED_RELEASE){
				if (fctrl->func_tbl->camera_flash_off)
					fctrl->func_tbl->camera_flash_off(fctrl,flash_data);
                if(fctrl->func_tbl->camera_flash_power_down){
                    fctrl->func_tbl->camera_flash_power_down(fctrl);
                }
				fctrl->func_tbl->camera_flash_release(fctrl);
			}
		}
	}
    CDBG("%s X\n",__func__);
};

void msm_flash_i2c_front_brightness_set(struct led_classdev *led_cdev,
				enum led_brightness value)
{
	struct msm_flash_ctrl_t *fctrl = container_of(led_cdev, struct msm_flash_ctrl_t, led_cl_dev);
	struct msm_flash_cfg_data_t *flash_data = NULL;
    CDBG("%s E\n",__func__);
    CDBG("%s value = %d flash_state = %d\n",__func__,
        value,fctrl->flash_state);
	if(fctrl){
		if (value == LED_FULL) {
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
				if (fctrl->func_tbl->camera_flash_init)
					fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->func_tbl->camera_flash_low)
					fctrl->func_tbl->camera_flash_low(fctrl,flash_data);
			}
		}
		else if (value == LED_FULL_FLASH){
			if(fctrl->led_state != MSM_CAMERA_LED_INIT){
				if (fctrl->func_tbl->camera_flash_init)
					fctrl->func_tbl->camera_flash_init(fctrl,flash_data);
				if (fctrl->func_tbl->camera_flash_high)
					fctrl->func_tbl->camera_flash_high(fctrl,flash_data);
			}
		}
		else {
			if(fctrl->led_state != MSM_CAMERA_LED_RELEASE){
				if (fctrl->func_tbl->camera_flash_off)
					fctrl->func_tbl->camera_flash_off(fctrl,flash_data);
				if (fctrl->func_tbl->camera_flash_release)
					fctrl->func_tbl->camera_flash_release(fctrl);
			}
		}
	}
    CDBG("%s X\n",__func__);
};

static int msm_camera_flash_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int32_t rc = 0;
	struct msm_flash_ctrl_t *flash_ctrl = NULL;
	CDBG("Enter\n");

	if (client == NULL) {
		pr_err("msm_flash_i2c_probe: client is null\n");
		return -EINVAL;
	}

	flash_ctrl = kzalloc(sizeof(struct msm_flash_ctrl_t), GFP_KERNEL);
	if (!flash_ctrl) {
		pr_err("%s:%d failed no memory\n", __func__, __LINE__);
		return -ENOMEM;
	}
	memset(flash_ctrl, 0, sizeof(struct msm_flash_ctrl_t));

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("i2c_check_functionality failed\n");
		kfree(flash_ctrl);
		return -EINVAL;
	}

	rc = msm_flash_get_dt_data(client->dev.of_node, flash_ctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_dt_data failed\n",
			__func__, __LINE__);
		kfree(flash_ctrl);
		return -EINVAL;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	flash_ctrl->power_info.dev = &client->dev;
	flash_ctrl->flash_device_type = MSM_CAMERA_I2C_DEVICE;
	flash_ctrl->flash_mutex = &msm_flash_mutex;
	flash_ctrl->flash_i2c_client.i2c_func_tbl = &msm_flash_qup_func_tbl;
	flash_ctrl->flash_i2c_client.client = client;

	/* Initialize sub device */
	v4l2_subdev_init(&flash_ctrl->msm_sd.sd, &msm_flash_subdev_ops);
	v4l2_set_subdevdata(&flash_ctrl->msm_sd.sd, flash_ctrl);

	flash_ctrl->msm_sd.sd.internal_ops = &msm_flash_internal_ops;
	flash_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(flash_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(flash_ctrl->msm_sd.sd.name),
		"msm_camera_flash");
	media_entity_init(&flash_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	flash_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	flash_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_FLASH;
	flash_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x1;
	msm_sd_register(&flash_ctrl->msm_sd);

	CDBG("%s:%d flash sd name = %s", __func__, __LINE__,
		flash_ctrl->msm_sd.sd.entity.name);
	msm_flash_v4l2_subdev_fops = v4l2_subdev_fops;
#ifdef CONFIG_COMPAT
	msm_flash_v4l2_subdev_fops.compat_ioctl32 =
		msm_flash_subdev_fops_ioctl;
#endif
	flash_ctrl->msm_sd.sd.devnode->fops = &msm_flash_v4l2_subdev_fops;

	CDBG("probe success\n");
	return rc;
}



static int32_t msm_camera_flash_probe(struct platform_device *pdev)
{
	int32_t rc = 0;
	struct msm_flash_ctrl_t *flash_ctrl = NULL;
	struct msm_camera_cci_client *cci_client = NULL;

	CDBG("Enter");
	if (!pdev->dev.of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	flash_ctrl = kzalloc(sizeof(struct msm_flash_ctrl_t), GFP_KERNEL);
	if (!flash_ctrl) {
		pr_err("%s:%d failed no memory\n", __func__, __LINE__);
		return -ENOMEM;
	}

	memset(flash_ctrl, 0, sizeof(struct msm_flash_ctrl_t));

	flash_ctrl->pdev = pdev;

	rc = msm_flash_get_dt_data(pdev->dev.of_node, flash_ctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_dt_data failed\n",
			__func__, __LINE__);
		kfree(flash_ctrl);
		return -EINVAL;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	flash_ctrl->power_info.dev = &flash_ctrl->pdev->dev;
	flash_ctrl->flash_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	flash_ctrl->flash_mutex = &msm_flash_mutex;
	flash_ctrl->flash_i2c_client.i2c_func_tbl = &msm_flash_cci_func_tbl;
	flash_ctrl->flash_i2c_client.cci_client = kzalloc(
		sizeof(struct msm_camera_cci_client), GFP_KERNEL);
	if (!flash_ctrl->flash_i2c_client.cci_client) {
		kfree(flash_ctrl);
		pr_err("failed no memory\n");
		return -ENOMEM;
	}

	cci_client = flash_ctrl->flash_i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = flash_ctrl->cci_i2c_master;

	/* Initialize sub device */
	v4l2_subdev_init(&flash_ctrl->msm_sd.sd, &msm_flash_subdev_ops);
	v4l2_set_subdevdata(&flash_ctrl->msm_sd.sd, flash_ctrl);

	flash_ctrl->msm_sd.sd.internal_ops = &msm_flash_internal_ops;
	flash_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(flash_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(flash_ctrl->msm_sd.sd.name),
		"msm_camera_flash");
	media_entity_init(&flash_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	flash_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	flash_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_FLASH;
	flash_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x1;
	msm_sd_register(&flash_ctrl->msm_sd);

	CDBG("%s:%d flash sd name = %s", __func__, __LINE__,
		flash_ctrl->msm_sd.sd.entity.name);
	msm_flash_v4l2_subdev_fops = v4l2_subdev_fops;
#ifdef CONFIG_COMPAT
	msm_flash_v4l2_subdev_fops.compat_ioctl32 =
		msm_flash_subdev_fops_ioctl;
#endif
	flash_ctrl->msm_sd.sd.devnode->fops = &msm_flash_v4l2_subdev_fops;

	if (flash_ctrl->flash_driver_type == FLASH_DRIVER_PMIC){
		rc = msm_torch_create_classdev(pdev, flash_ctrl);
#ifdef CONFIG_HISENSE_CAMLEDS_FATE_TEST
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
		wake_lock_init(&flash_ctrl->torch_wake_lock, WAKE_LOCK_SUSPEND,
		"qpnp_torch_lock");
#endif
		msm_flash_create_classdev_4fate(pdev, flash_ctrl);
#endif /* CONFIG_HISENSE_CAMLEDS_FATE_TEST */
	}
	CDBG("probe success\n");
	return rc;
}


int32_t msm_flash_platform_probe(struct platform_device *pdev,
	const void *data)
{
	int32_t rc = 0;
	struct msm_flash_ctrl_t *flash_ctrl = (struct msm_flash_ctrl_t *)data;
	struct msm_camera_cci_client *cci_client = NULL;

	CDBG("Enter");
	if (!pdev->dev.of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	if(!flash_ctrl){
		pr_err("%s:%d invalid flash_ctrl\n", __func__, __LINE__);
		return -EINVAL;
	}

	flash_ctrl->pdev = pdev;

	rc = msm_flash_get_dt_data(pdev->dev.of_node, flash_ctrl);
	if (rc < 0) {
		pr_err("%s:%d msm_flash_get_dt_data failed\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	flash_ctrl->flash_state = MSM_CAMERA_FLASH_RELEASE;
	flash_ctrl->power_info.dev = &flash_ctrl->pdev->dev;
	flash_ctrl->flash_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	flash_ctrl->flash_mutex = &msm_flash_mutex;
	flash_ctrl->flash_i2c_client.i2c_func_tbl = &msm_flash_cci_func_tbl;
	flash_ctrl->flash_i2c_client.cci_client = kzalloc(
		sizeof(struct msm_camera_cci_client), GFP_KERNEL);
	if (!flash_ctrl->flash_i2c_client.cci_client) {
		pr_err("failed no memory\n");
		return -ENOMEM;
	}

	cci_client = flash_ctrl->flash_i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = flash_ctrl->cci_i2c_master;
	cci_client->retries = 3;
	cci_client->id_map = 0;
	cci_client->i2c_freq_mode = I2C_FAST_MODE;
	cci_client->cid = 0;
	if (flash_ctrl->slave_info){
		cci_client->sid = flash_ctrl->slave_info->sensor_slave_addr >> 1;
	}


	/* Initialize sub device */
	v4l2_subdev_init(&flash_ctrl->msm_sd.sd, &msm_flash_subdev_ops);
	v4l2_set_subdevdata(&flash_ctrl->msm_sd.sd, flash_ctrl);

	flash_ctrl->msm_sd.sd.internal_ops = &msm_flash_internal_ops;
	flash_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(flash_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(flash_ctrl->msm_sd.sd.name),
		"%s", flash_ctrl->flash_name);
	
	media_entity_init(&flash_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	flash_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	flash_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_FLASH;
	flash_ctrl->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x1;
	msm_sd_register(&flash_ctrl->msm_sd);

	CDBG("%s:%d flash sd name = %s", __func__, __LINE__,
		flash_ctrl->msm_sd.sd.entity.name);
	msm_flash_v4l2_subdev_fops = v4l2_subdev_fops;
#ifdef CONFIG_COMPAT
	msm_flash_v4l2_subdev_fops.compat_ioctl32 =
		msm_flash_subdev_fops_ioctl;
#endif
	flash_ctrl->msm_sd.sd.devnode->fops = &msm_flash_v4l2_subdev_fops;

	if (flash_ctrl->flash_driver_type == FLASH_DRIVER_PMIC){
		rc = msm_torch_create_classdev(pdev, flash_ctrl);
#ifdef CONFIG_HISENSE_CAMLEDS_FATE_TEST
#ifdef CONFIG_HISENSE_TORCH_WAKEUP
		wake_lock_init(&flash_ctrl->torch_wake_lock, WAKE_LOCK_SUSPEND,
		"qpnp_torch_lock");
#endif
		msm_flash_create_classdev_4fate(pdev, flash_ctrl);
#endif /* CONFIG_HISENSE_CAMLEDS_FATE_TEST */
	}

	CDBG("probe success\n");
	return rc;
}

MODULE_DEVICE_TABLE(of, msm_flash_i2c_dt_match);

static struct i2c_driver msm_flash_i2c_driver = {
	.id_table = msm_flash_i2c_id,
	.probe  = msm_camera_flash_i2c_probe,
	.remove = __exit_p(msm_camera_flash_i2c_remove),
	.driver = {
		.name = "qcom,camera-flash",
		.owner = THIS_MODULE,
		.of_match_table = msm_flash_i2c_dt_match,
	},
};

MODULE_DEVICE_TABLE(of, msm_flash_dt_match);

static struct platform_driver msm_flash_platform_driver = {
	.probe = msm_camera_flash_probe,
	.driver = {
		.name = "qcom,camera-flash",
		.owner = THIS_MODULE,
		.of_match_table = msm_flash_dt_match,
	},
};

static int __init msm_flash_init_module(void)
{
	int32_t rc = 0;
	CDBG("Enter\n");
	rc = platform_driver_register(&msm_flash_platform_driver);
	if (!rc)
		return rc;

	pr_err("platform probe for flash failed");

	/* Perform i2c probe if platform probe fails. */
	rc = i2c_add_driver(&msm_flash_i2c_driver);
	if (rc)
		pr_err("i2c probe for flash failed");

	return rc;
}

static void __exit msm_flash_exit_module(void)
{
	platform_driver_unregister(&msm_flash_platform_driver);
	i2c_del_driver(&msm_flash_i2c_driver);
}

static struct msm_flash_table msm_pmic_flash_table = {
	.flash_driver_type = FLASH_DRIVER_PMIC,
	.func_tbl = {
		.camera_flash_init = msm_flash_off,
		.camera_flash_release = msm_flash_release,
		.camera_flash_off = msm_flash_off,
		.camera_flash_low = msm_flash_low,
		.camera_flash_high = msm_flash_high,
	},
};

static struct msm_flash_table msm_gpio_flash_table = {
	.flash_driver_type = FLASH_DRIVER_GPIO,
	.func_tbl = {
		.camera_flash_init = msm_flash_gpio_init,
		.camera_flash_release = msm_flash_release,
		.camera_flash_off = msm_flash_off,
		.camera_flash_low = msm_flash_low,
		.camera_flash_high = msm_flash_high,
	},
};

static struct msm_flash_table msm_i2c_flash_table = {
	.flash_driver_type = FLASH_DRIVER_I2C,
	.func_tbl = {
		.camera_flash_init = msm_flash_i2c_init,
		.camera_flash_release = msm_flash_i2c_release,
		.camera_flash_off = msm_flash_i2c_write_setting_array,
		.camera_flash_low = msm_flash_i2c_write_setting_array,
		.camera_flash_high = msm_flash_i2c_write_setting_array,
	},
};

module_init(msm_flash_init_module);
module_exit(msm_flash_exit_module);
MODULE_DESCRIPTION("MSM FLASH");
MODULE_LICENSE("GPL v2");

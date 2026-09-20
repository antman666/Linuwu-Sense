// SPDX-License-Identifier: GPL-2.0

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/units.h>

#include "linuwu_sense_gaming.h"
#include "linuwu_sense_hwmon.h"

#define ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS 0x0000
#define ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING 0x0001

#define ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK GENMASK_ULL(15, 8)
#define ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK GENMASK_ULL(23, 8)
#define ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK GENMASK_ULL(39, 24)

enum acer_wmi_predator_v4_sensor_id {
	ACER_WMID_SENSOR_CPU_TEMPERATURE = 0x01,
	ACER_WMID_SENSOR_CPU_FAN_SPEED = 0x02,
	ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2 = 0x03,
	ACER_WMID_SENSOR_GPU_FAN_SPEED = 0x06,
	ACER_WMID_SENSOR_GPU_TEMPERATURE = 0x0A,
};

struct acer_wmi_hwmon_data {
	struct acer_wmi *acer;
	u64 supported_sensors;
};

static const enum acer_wmi_predator_v4_sensor_id
	acer_wmi_temp_channel_to_sensor_id[] = {
		[0] = ACER_WMID_SENSOR_CPU_TEMPERATURE,
		[1] = ACER_WMID_SENSOR_GPU_TEMPERATURE,
		[2] = ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2,
	};

static const enum acer_wmi_predator_v4_sensor_id
	acer_wmi_fan_channel_to_sensor_id[] = {
		[0] = ACER_WMID_SENSOR_CPU_FAN_SPEED,
		[1] = ACER_WMID_SENSOR_GPU_FAN_SPEED,
	};

static umode_t acer_wmi_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	const struct acer_wmi_hwmon_data *hwmon = data;
	enum acer_wmi_predator_v4_sensor_id sensor_id;

	switch (type) {
	case hwmon_temp:
		sensor_id = acer_wmi_temp_channel_to_sensor_id[channel];
		break;
	case hwmon_fan:
		sensor_id = acer_wmi_fan_channel_to_sensor_id[channel];
		break;
	default:
		return 0;
	}

	if (hwmon->supported_sensors & BIT(sensor_id - 1))
		return 0444;

	return 0;
}

static int acer_wmi_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long *val)
{
	struct acer_wmi_hwmon_data *hwmon = dev_get_drvdata(dev);
	u64 command = ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING;
	u64 result;
	int ret;

	if (!hwmon || !hwmon->acer)
		return -ENODEV;

	switch (type) {
	case hwmon_temp:
		command |=
			FIELD_PREP(ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK,
				   acer_wmi_temp_channel_to_sensor_id[channel]);
		ret = linuwu_sense_gaming_get_sys_info(hwmon->acer, command, &result);
		if (ret < 0)
			return ret;

		result = FIELD_GET(ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK,
				   result);
		*val = result * MILLIDEGREE_PER_DEGREE;
		return 0;
	case hwmon_fan:
		command |=
			FIELD_PREP(ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK,
				   acer_wmi_fan_channel_to_sensor_id[channel]);

		ret = linuwu_sense_gaming_get_sys_info(hwmon->acer, command, &result);
		if (ret < 0)
			return ret;
		*val = FIELD_GET(ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK,
				 result);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const acer_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT), NULL
};

static const struct hwmon_ops acer_wmi_hwmon_ops = {
	.read = acer_wmi_hwmon_read,
	.is_visible = acer_wmi_hwmon_is_visible,
};

static const struct hwmon_chip_info acer_wmi_hwmon_chip_info = {
	.ops = &acer_wmi_hwmon_ops,
	.info = acer_wmi_hwmon_info,
};

int acer_wmi_hwmon_init(struct acer_wmi *acer, struct device *dev)
{
	struct acer_wmi_hwmon_data *hwmon_data;
	struct device *hwmon;
	u64 result;
	int ret;

	hwmon_data = devm_kzalloc(dev, sizeof(*hwmon_data), GFP_KERNEL);
	if (!hwmon_data)
		return -ENOMEM;

	hwmon_data->acer = acer;

	ret = linuwu_sense_gaming_get_sys_info(
		acer, ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS, &result);
	if (ret < 0)
		return ret;

	/* Return early if no sensors are available. */
	hwmon_data->supported_sensors =
		FIELD_GET(ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK, result);
	if (!hwmon_data->supported_sensors)
		return 0;

	hwmon = devm_hwmon_device_register_with_info(
		dev, "acer", hwmon_data, &acer_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register acer hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

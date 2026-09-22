// SPDX-License-Identifier: GPL-2.0
/*
 *  hwmon frontend for the Acer Predator/Nitro WMI driver.
 *
 *  This module maps the hwmon channels to the sensor readings provided by the
 *  Predator Gaming WMI backend. The backend owns the firmware command layout,
 *  the frontend only obeys the hwmon unit conventions.
 */

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/units.h>

#include "linuwu_sense.h"
#include "linuwu_sense_gaming.h"
#include "linuwu_sense_hwmon.h"

struct acer_wmi_hwmon_data {
	struct acer_wmi *acer;
	u64 supported_sensors;
};

static const enum linuwu_sense_gaming_sensor acer_wmi_temp_channel_to_sensor[] = {
	[0] = LINUWU_SENSE_GAMING_SENSOR_CPU_TEMPERATURE,
	[1] = LINUWU_SENSE_GAMING_SENSOR_GPU_TEMPERATURE,
	[2] = LINUWU_SENSE_GAMING_SENSOR_EXTERNAL_TEMPERATURE_2,
};

static const enum linuwu_sense_gaming_sensor acer_wmi_fan_channel_to_sensor[] = {
	[0] = LINUWU_SENSE_GAMING_SENSOR_CPU_FAN_SPEED,
	[1] = LINUWU_SENSE_GAMING_SENSOR_GPU_FAN_SPEED,
};

static umode_t acer_wmi_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	const struct acer_wmi_hwmon_data *hwmon = data;
	enum linuwu_sense_gaming_sensor sensor;

	switch (type) {
	case hwmon_temp:
		sensor = acer_wmi_temp_channel_to_sensor[channel];
		break;
	case hwmon_fan:
		sensor = acer_wmi_fan_channel_to_sensor[channel];
		break;
	default:
		return 0;
	}

	if (linuwu_sense_gaming_sensor_is_supported(hwmon->supported_sensors,
						    sensor))
		return 0444;

	return 0;
}

static int acer_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct acer_wmi_hwmon_data *hwmon = dev_get_drvdata(dev);
	enum linuwu_sense_gaming_sensor sensor;
	long value;
	int ret;

	if (!hwmon || !hwmon->acer)
		return -ENODEV;

	switch (type) {
	case hwmon_temp:
		sensor = acer_wmi_temp_channel_to_sensor[channel];
		break;
	case hwmon_fan:
		sensor = acer_wmi_fan_channel_to_sensor[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* The sensor query is a WMI operation of the Acer gaming backend. */
	mutex_lock(&hwmon->acer->lock);
	ret = linuwu_sense_gaming_read_sensor(hwmon->acer, sensor, &value);
	mutex_unlock(&hwmon->acer->lock);
	if (ret < 0)
		return ret;

	if (type == hwmon_temp)
		*val = value * MILLIDEGREE_PER_DEGREE;
	else
		*val = value;

	return 0;
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
	int ret;

	hwmon_data = devm_kzalloc(dev, sizeof(*hwmon_data), GFP_KERNEL);
	if (!hwmon_data)
		return -ENOMEM;

	hwmon_data->acer = acer;

	mutex_lock(&acer->lock);
	ret = linuwu_sense_gaming_get_supported_sensors(
		acer, &hwmon_data->supported_sensors);
	mutex_unlock(&acer->lock);
	if (ret < 0)
		return ret;

	/* Return early if no sensors are available. */
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

// SPDX-License-Identifier: GPL-2.0
/*
 *  hwmon frontend for the Acer Predator/Nitro WMI driver.
 *
 *  This module maps the hwmon channels to the sensor readings and fan controls
 *  provided by the Predator Gaming WMI backend. The backend owns the firmware
 *  command layout, the frontend only obeys the hwmon unit conventions:
 *
 *  - temp[1-3]_input: CPU, GPU and external temperature in millidegrees
 *  - fan[1-2]_input: CPU and GPU fan speed in RPM
 *  - pwm[1-2]: CPU and GPU fan duty cycle, scaled from the percentage the
 *    firmware takes to the hwmon 0..255 range
 *  - pwm[1-2]_enable: 0 = full speed, 1 = manual (pwmN), 2 = automatic,
 *    matching the Acer fan behavior modes
 *
 *  The firmware does not report the manual duty cycle back, so pwm[1-2] read
 *  the last duty cycle written through the hwmon interface.
 */

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/units.h>

#include "linuwu_sense.h"
#include "linuwu_sense_fan.h"
#include "linuwu_sense_gaming.h"
#include "linuwu_sense_hwmon.h"
#include "linuwu_sense_quirks.h"

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

static bool acer_wmi_hwmon_pwm_channel_visible(struct acer_wmi *acer,
					       int channel)
{
	if (!(acer->capability & ACER_CAP_TURBO_FAN))
		return false;

	if (channel == 0)
		return acer->quirks->cpu_fans > 0;

	return channel == 1 && acer->quirks->gpu_fans > 0;
}

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
	case hwmon_pwm:
		if (acer_wmi_hwmon_pwm_channel_visible(hwmon->acer, channel))
			return 0644;

		return 0;
	default:
		return 0;
	}

	if (linuwu_sense_gaming_sensor_is_supported(hwmon->supported_sensors,
						    sensor))
		return 0444;

	return 0;
}

static enum linuwu_sense_fan acer_wmi_hwmon_channel_to_fan(int channel)
{
	return channel == 0 ? LINUWU_SENSE_FAN_CPU : LINUWU_SENSE_FAN_GPU;
}

static int acer_wmi_hwmon_pwm_read(struct acer_wmi *acer, int channel,
				   long *val)
{
	if (channel == 0)
		*val = DIV_ROUND_CLOSEST(acer->cpu_fan_speed * 255, 100);
	else
		*val = DIV_ROUND_CLOSEST(acer->gpu_fan_speed * 255, 100);

	return 0;
}

static int acer_wmi_hwmon_pwm_enable_read(struct acer_wmi *acer, int channel,
					  long *val)
{
	enum linuwu_sense_fan_mode mode;
	struct wmi_device *wdev;
	int ret;

	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev)
		return -ENODEV;

	ret = linuwu_sense_fan_get_mode(
		wdev, acer_wmi_hwmon_channel_to_fan(channel), &mode);
	if (ret)
		return ret;

	switch (mode) {
	case LINUWU_SENSE_FAN_MODE_TURBO:
		*val = 0;
		break;
	case LINUWU_SENSE_FAN_MODE_CUSTOM:
		*val = 1;
		break;
	case LINUWU_SENSE_FAN_MODE_AUTO:
		*val = 2;
		break;
	default:
		return -EIO;
	}

	return 0;
}

static int acer_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct acer_wmi_hwmon_data *hwmon = dev_get_drvdata(dev);
	enum linuwu_sense_gaming_sensor sensor;
	struct acer_wmi *acer;
	long value;
	int ret;

	if (!hwmon || !hwmon->acer)
		return -ENODEV;

	acer = hwmon->acer;

	switch (type) {
	case hwmon_temp:
		sensor = acer_wmi_temp_channel_to_sensor[channel];
		break;
	case hwmon_fan:
		sensor = acer_wmi_fan_channel_to_sensor[channel];
		break;
	case hwmon_pwm:
		mutex_lock(&acer->lock);
		if (attr == hwmon_pwm_input)
			ret = acer_wmi_hwmon_pwm_read(acer, channel, val);
		else
			ret = acer_wmi_hwmon_pwm_enable_read(acer, channel,
							     val);
		mutex_unlock(&acer->lock);

		return ret;
	default:
		return -EOPNOTSUPP;
	}

	/* The sensor query is a WMI operation of the Acer gaming backend. */
	mutex_lock(&acer->lock);
	ret = linuwu_sense_gaming_read_sensor(acer, sensor, &value);
	mutex_unlock(&acer->lock);
	if (ret < 0)
		return ret;

	if (type == hwmon_temp)
		*val = value * MILLIDEGREE_PER_DEGREE;
	else
		*val = value;

	return 0;
}

static int acer_wmi_hwmon_pwm_write(struct acer_wmi *acer, int channel,
				    long val)
{
	enum linuwu_sense_fan fan = acer_wmi_hwmon_channel_to_fan(channel);
	struct wmi_device *wdev;
	bool cpu, gpu;
	int percent;
	int ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	percent = DIV_ROUND_CLOSEST(val * 100, 255);

	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev)
		return -ENODEV;

	cpu = fan == LINUWU_SENSE_FAN_CPU;
	gpu = !cpu;

	/* A duty cycle only takes effect in manual mode. */
	ret = linuwu_sense_fan_set_mode(wdev, cpu, gpu,
					LINUWU_SENSE_FAN_MODE_CUSTOM);
	if (ret)
		return ret;

	ret = linuwu_sense_fan_set_speed(wdev, fan, percent);
	if (ret)
		return ret;

	if (cpu)
		acer->cpu_fan_speed = percent;
	else
		acer->gpu_fan_speed = percent;

	return 0;
}

static int acer_wmi_hwmon_pwm_enable_write(struct acer_wmi *acer, int channel,
					   long val)
{
	enum linuwu_sense_fan_mode mode;
	struct wmi_device *wdev;
	bool cpu, gpu;

	switch (val) {
	case 0:
		mode = LINUWU_SENSE_FAN_MODE_TURBO;
		break;
	case 1:
		mode = LINUWU_SENSE_FAN_MODE_CUSTOM;
		break;
	case 2:
		mode = LINUWU_SENSE_FAN_MODE_AUTO;
		break;
	default:
		return -EINVAL;
	}

	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev)
		return -ENODEV;

	cpu = channel == 0;
	gpu = !cpu;

	return linuwu_sense_fan_set_mode(wdev, cpu, gpu, mode);
}

static int acer_wmi_hwmon_write(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, long val)
{
	struct acer_wmi_hwmon_data *hwmon = dev_get_drvdata(dev);
	struct acer_wmi *acer;
	int ret;

	if (!hwmon || !hwmon->acer)
		return -ENODEV;

	acer = hwmon->acer;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&acer->lock);
	if (attr == hwmon_pwm_input)
		ret = acer_wmi_hwmon_pwm_write(acer, channel, val);
	else if (attr == hwmon_pwm_enable)
		ret = acer_wmi_hwmon_pwm_enable_write(acer, channel, val);
	else
		ret = -EOPNOTSUPP;
	mutex_unlock(&acer->lock);

	return ret;
}

static const struct hwmon_channel_info *const acer_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops acer_wmi_hwmon_ops = {
	.read = acer_wmi_hwmon_read,
	.write = acer_wmi_hwmon_write,
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

	if (acer->capability & ACER_CAP_FAN_SPEED_READ) {
		mutex_lock(&acer->lock);
		ret = linuwu_sense_gaming_get_supported_sensors(
			acer, &hwmon_data->supported_sensors);
		mutex_unlock(&acer->lock);
		if (ret < 0)
			return ret;
	}

	/* Nothing to expose when neither sensors nor fan control exist. */
	if (!hwmon_data->supported_sensors &&
	    !(acer->capability & ACER_CAP_TURBO_FAN))
		return 0;

	hwmon = devm_hwmon_device_register_with_info(
		dev, "acer", hwmon_data, &acer_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register acer hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

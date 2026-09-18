// SPDX-License-Identifier: GPL-2.0
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense_fan.h"

#define ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID 14
#define ACER_WMID_SET_GAMING_FAN_SPEED_METHODID 16

static acpi_status linuwu_sense_fan_invoke(struct wmi_device *wdev,
					   u32 method_id, u64 input)
{
	struct wmi_buffer input_buf = {
		.length = sizeof(input),
		.data = &input,
	};
	struct wmi_buffer output_buf = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, method_id, &input_buf,
				   &output_buf, sizeof(u32));
	if (err)
		return AE_ERROR;

	/* Methods 14 and 16 return a uint32 status value. */
	kfree(output_buf.data);

	return AE_OK;
}

acpi_status linuwu_sense_fan_set_behavior(struct wmi_device *wdev,
					   u64 behavior)
{
	return linuwu_sense_fan_invoke(
		wdev, ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID, behavior);
}

acpi_status linuwu_sense_fan_set_mode(struct wmi_device *wdev,
					 u8 cpu_fans, u8 gpu_fans,
					 enum linuwu_sense_fan_mode fan_mode)
{
	u64 fan_config1 = 0;
	u64 fan_config2 = 0;
	int i;

	if (fan_mode != LINUWU_SENSE_FAN_MODE_AUTO &&
	    fan_mode != LINUWU_SENSE_FAN_MODE_TURBO)
		return AE_BAD_PARAMETER;

	if (cpu_fans > 0)
		fan_config2 |= 1;

	for (i = 0; i < cpu_fans + gpu_fans; i++)
		fan_config2 |= 1ULL << (i + 1);

	for (i = 0; i < gpu_fans; i++)
		fan_config2 |= 1ULL << (i + 3);

	if (cpu_fans > 0)
		fan_config1 |= fan_mode;

	for (i = 0; i < cpu_fans + gpu_fans; i++)
		fan_config1 |= (u64)fan_mode << (2 * i + 2);

	for (i = 0; i < gpu_fans; i++)
		fan_config1 |= (u64)fan_mode << (2 * i + 6);

	return linuwu_sense_fan_set_behavior(
		wdev, fan_config2 | (fan_config1 << 16));
}

static u64 linuwu_sense_fan_value(int percentage, int fan_index)
{
	return (((percentage * 25600) / 100) & 0xFF00) + fan_index;
}

acpi_status linuwu_sense_fan_set_speed(struct wmi_device *wdev,
					  enum linuwu_sense_fan fan,
					  int percentage)
{
	int fan_index;

	if (percentage < 0 || percentage > 100)
		return AE_BAD_PARAMETER;

	switch (fan) {
	case LINUWU_SENSE_FAN_CPU:
		fan_index = 1;
		break;
	case LINUWU_SENSE_FAN_GPU:
		fan_index = 4;
		break;
	default:
		return AE_BAD_PARAMETER;
	}

	return linuwu_sense_fan_invoke(
		wdev, ACER_WMID_SET_GAMING_FAN_SPEED_METHODID,
		linuwu_sense_fan_value(percentage, fan_index));
}

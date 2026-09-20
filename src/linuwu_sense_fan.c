// SPDX-License-Identifier: GPL-2.0
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense_fan.h"
#include "linuwu_sense_wmi.h"

#define ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID 14
#define ACER_WMID_GET_GAMING_FAN_BEHAVIOR_METHODID 15
#define ACER_WMID_SET_GAMING_FAN_SPEED_METHODID 16

#define ACER_GAMING_FAN_BEHAVIOR_CPU BIT(0)
#define ACER_GAMING_FAN_BEHAVIOR_GPU BIT(3)

#define ACER_GAMING_FAN_BEHAVIOR_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_FAN_BEHAVIOR_ID_MASK GENMASK_ULL(15, 0)
#define ACER_GAMING_FAN_BEHAVIOR_SET_CPU_MODE_MASK GENMASK(17, 16)
#define ACER_GAMING_FAN_BEHAVIOR_SET_GPU_MODE_MASK GENMASK(23, 22)
#define ACER_GAMING_FAN_BEHAVIOR_GET_CPU_MODE_MASK GENMASK(9, 8)
#define ACER_GAMING_FAN_BEHAVIOR_GET_GPU_MODE_MASK GENMASK(15, 14)

#define ACER_GAMING_FAN_SPEED_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_FAN_SPEED_ID_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_FAN_SPEED_VALUE_MASK GENMASK_ULL(15, 8)

#define ACER_GAMING_FAN_SPEED_CPU_ID 0x01
#define ACER_GAMING_FAN_SPEED_GPU_ID 0x04

acpi_status linuwu_sense_fan_set_behavior(struct wmi_device *wdev, u64 behavior)
{
	u64 result;
	acpi_status status;

	status = linuwu_sense_wmi_execute_u64(
		wdev, ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID, behavior,
		&result);
	if (ACPI_FAILURE(status))
		return status;

	/* The return status must be zero for the operation to have succeeded. */
	if (FIELD_GET(ACER_GAMING_FAN_BEHAVIOR_STATUS_MASK, result))
		return AE_ERROR;

	return AE_OK;
}

acpi_status linuwu_sense_fan_set_mode(struct wmi_device *wdev, bool cpu_fan,
				      bool gpu_fan,
				      enum linuwu_sense_fan_mode fan_mode)
{
	u16 fan_bitmap = 0;
	u64 behavior = 0;

	if (!cpu_fan && !gpu_fan)
		return AE_BAD_PARAMETER;

	if (fan_mode < LINUWU_SENSE_FAN_MODE_AUTO ||
	    fan_mode > LINUWU_SENSE_FAN_MODE_CUSTOM)
		return AE_BAD_PARAMETER;

	if (cpu_fan)
		fan_bitmap |= ACER_GAMING_FAN_BEHAVIOR_CPU;
	if (gpu_fan)
		fan_bitmap |= ACER_GAMING_FAN_BEHAVIOR_GPU;

	behavior |= FIELD_PREP(ACER_GAMING_FAN_BEHAVIOR_ID_MASK, fan_bitmap);
	if (cpu_fan)
		behavior |= FIELD_PREP(
			ACER_GAMING_FAN_BEHAVIOR_SET_CPU_MODE_MASK, fan_mode);
	if (gpu_fan)
		behavior |= FIELD_PREP(
			ACER_GAMING_FAN_BEHAVIOR_SET_GPU_MODE_MASK, fan_mode);

	return linuwu_sense_fan_set_behavior(wdev, behavior);
}

acpi_status linuwu_sense_fan_get_mode(struct wmi_device *wdev,
				      enum linuwu_sense_fan fan,
				      enum linuwu_sense_fan_mode *fan_mode)
{
	u16 fan_bitmap;
	unsigned int mode;
	u64 result;
	acpi_status status;

	if (!fan_mode)
		return AE_BAD_PARAMETER;

	switch (fan) {
	case LINUWU_SENSE_FAN_CPU:
		fan_bitmap = ACER_GAMING_FAN_BEHAVIOR_CPU;
		break;
	case LINUWU_SENSE_FAN_GPU:
		fan_bitmap = ACER_GAMING_FAN_BEHAVIOR_GPU;
		break;
	default:
		return AE_BAD_PARAMETER;
	}

	status = linuwu_sense_wmi_execute_u64(
		wdev, ACER_WMID_GET_GAMING_FAN_BEHAVIOR_METHODID,
		FIELD_PREP(ACER_GAMING_FAN_BEHAVIOR_ID_MASK, fan_bitmap),
		&result);
	if (ACPI_FAILURE(status))
		return status;

	if (FIELD_GET(ACER_GAMING_FAN_BEHAVIOR_STATUS_MASK, result))
		return AE_ERROR;

	if (fan == LINUWU_SENSE_FAN_CPU)
		mode = FIELD_GET(ACER_GAMING_FAN_BEHAVIOR_GET_CPU_MODE_MASK,
				 result);
	else
		mode = FIELD_GET(ACER_GAMING_FAN_BEHAVIOR_GET_GPU_MODE_MASK,
				 result);

	if (mode < LINUWU_SENSE_FAN_MODE_AUTO ||
	    mode > LINUWU_SENSE_FAN_MODE_CUSTOM)
		return AE_ERROR;

	*fan_mode = mode;
	return AE_OK;
}

acpi_status linuwu_sense_fan_set_speed(struct wmi_device *wdev,
				       enum linuwu_sense_fan fan,
				       int percentage)
{
	u64 input = 0;
	u64 result;
	u8 fan_id;
	acpi_status status;

	if (percentage < 0 || percentage > 100)
		return AE_BAD_PARAMETER;

	switch (fan) {
	case LINUWU_SENSE_FAN_CPU:
		fan_id = ACER_GAMING_FAN_SPEED_CPU_ID;
		break;
	case LINUWU_SENSE_FAN_GPU:
		fan_id = ACER_GAMING_FAN_SPEED_GPU_ID;
		break;
	default:
		return AE_BAD_PARAMETER;
	}

	input |= FIELD_PREP(ACER_GAMING_FAN_SPEED_ID_MASK, fan_id);
	input |= FIELD_PREP(ACER_GAMING_FAN_SPEED_VALUE_MASK, percentage);

	status = linuwu_sense_wmi_execute_u64(
		wdev, ACER_WMID_SET_GAMING_FAN_SPEED_METHODID, input, &result);
	if (ACPI_FAILURE(status))
		return status;

	switch (FIELD_GET(ACER_GAMING_FAN_SPEED_STATUS_MASK, result)) {
	case 0x00:
		return AE_OK;
	case 0x01:
		return AE_NOT_FOUND;
	case 0x02:
		return AE_BAD_PARAMETER;
	default:
		return AE_ERROR;
	}
}

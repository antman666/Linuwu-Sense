// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Predator Gaming WMI protocol access for the Acer WMI Laptop Extras driver.
 *
 *  This module implements the hardware commands of the Predator Gaming WMI
 *  interface, the Predator/Nitro sensor readings of its "get system info"
 *  command and the system function commands of the WMID APGE device. It only
 *  exposes semantic operations to the rest of the driver.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense.h"
#include "linuwu_sense_gaming.h"

/*
 * Method IDs of the Predator Gaming WMI device
 */
#define ACER_WMID_GET_GAMING_PROFILE_METHODID 3
#define ACER_WMID_SET_GAMING_PROFILE_METHODID 1
#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID 5
#define ACER_WMID_SET_GAMING_MISC_SETTING_METHODID 22
#define ACER_WMID_GET_GAMING_MISC_SETTING_METHODID 23

/*
 * Method IDs of the WMID APGE device
 */
#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

/*
 * Command values of the "get system info" method
 */
#define ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS 0x02
#define ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS 0x0000
#define ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING 0x0001

/*
 * The return status must be zero for a command to have succeeded
 */
#define ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK GENMASK_ULL(7, 0)

#define ACER_GAMING_MISC_SETTING_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_INDEX_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_VALUE_MASK GENMASK_ULL(15, 8)

/*
 * Bit fields of the "get system info" sensor commands
 */
#define ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK GENMASK_ULL(15, 8)
#define ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK GENMASK_ULL(23, 8)
#define ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK GENMASK_ULL(39, 24)

/*
 * Misc setting indices of the Predator Gaming WMI interface
 */
#define ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND 0x0006
#define ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES 0x000A
#define ACER_WMID_MISC_SETTING_PLATFORM_PROFILE 0x000B

/*
 * Decode the result of a Predator Gaming WMI method. Some of the commands are
 * only answered with a u32 value while others return a full u64 value.
 */
static int linuwu_sense_gaming_decode_result(const struct wmi_buffer *output,
					     u64 *result)
{
	if (output->length >= sizeof(*result))
		*result = get_unaligned_le64(output->data);
	else if (output->length >= sizeof(u32))
		*result = get_unaligned_le32(output->data);
	else
		return -EIO;

	return 0;
}

static int linuwu_sense_gaming_get_sys_info(struct acer_wmi *acer, u32 command,
					    u64 *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = command;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err)
		return err;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK, result))
		return -EIO;

	*out = result;

	return 0;
}

int linuwu_sense_gaming_get_power_source(struct acer_wmi *acer, bool *on_ac)
{
	u64 status;
	int err;

	err = linuwu_sense_gaming_get_sys_info(
		acer, ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS, &status);
	if (err)
		return err;

	*on_ac = !!status;

	return 0;
}

int linuwu_sense_gaming_get_supported_sensors(struct acer_wmi *acer,
					      u64 *sensors)
{
	u64 result;
	int err;

	err = linuwu_sense_gaming_get_sys_info(
		acer, ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS, &result);
	if (err)
		return err;

	*sensors =
		FIELD_GET(ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK, result);

	return 0;
}

bool linuwu_sense_gaming_sensor_is_supported(
	u64 sensors, enum linuwu_sense_gaming_sensor sensor)
{
	/* The firmware reports one bit per sensor ID, starting at bit 0 */
	return sensors & BIT(sensor - 1);
}

int linuwu_sense_gaming_read_sensor(struct acer_wmi *acer,
				    enum linuwu_sense_gaming_sensor sensor,
				    long *value)
{
	u64 command = ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING;
	u64 result;
	int err;

	command |= FIELD_PREP(ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK, sensor);

	err = linuwu_sense_gaming_get_sys_info(acer, command, &result);
	if (err)
		return err;

	*value = FIELD_GET(ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK, result);

	return 0;
}

static int linuwu_sense_gaming_set_misc_setting(struct acer_wmi *acer,
						u8 setting, u8 value)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = 0;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, value);

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_MISC_SETTING_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err)
		return err;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	return 0;
}

static int linuwu_sense_gaming_get_misc_setting(struct acer_wmi *acer,
						u8 setting, u8 *value)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u32 input_value = 0;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_MISC_SETTING_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err)
		return err;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	*value = FIELD_GET(ACER_GAMING_MISC_SETTING_VALUE_MASK, result);

	return 0;
}

int linuwu_sense_gaming_get_thermal_profile(struct acer_wmi *acer, u8 *profile)
{
	return linuwu_sense_gaming_get_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, profile);
}

int linuwu_sense_gaming_set_thermal_profile(struct acer_wmi *acer, u8 profile)
{
	return linuwu_sense_gaming_set_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, profile);
}

int linuwu_sense_gaming_get_supported_thermal_profiles(struct acer_wmi *acer,
						       unsigned long *profiles)
{
	/* The bitmap is returned as a single byte */
	return linuwu_sense_gaming_get_misc_setting(
		acer, ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES,
		(u8 *)profiles);
}

int linuwu_sense_gaming_get_lcd_override(struct acer_wmi *acer, int *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = 0x00;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_PROFILE_METHODID,
				   &input, &output, sizeof(u32));
	if (err) {
		pr_err("Error getting lcd override status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error getting lcd override status: %d\n", err);
		return err;
	}

	pr_info("lcd override get status: %llu\n", result);
	*state = result == 0x1000001000000 ? 1 : result == 0x1000000 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_lcd_override(struct acer_wmi *acer, bool enable)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = enable ? 0x1000000000010 : 0x10;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	pr_info("lcd_override set value: %d\n", enable);
	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_PROFILE_METHODID,
				   &input, &output, sizeof(u32));
	if (err) {
		pr_err("Error setting lcd override status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting lcd override status: %d\n", err);
		return err;
	}

	pr_info("lcd override set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_backlight_timeout(struct acer_wmi *acer, int *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	u64 input_value = 0x88401;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, ACER_WMID_GET_FUNCTION, &input,
				   &output, sizeof(u32));
	if (err) {
		pr_err("Error getting backlight_timeout status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error getting backlight_timeout status: %d\n", err);
		return err;
	}

	pr_info("backlight_timeout get status: %llu\n", result);
	*state = result == 0x1E0000080000 ? 1 : result == 0x80000 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_backlight_timeout(struct acer_wmi *acer,
					      bool enable)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	u64 input_value = enable ? 0x1E0000088402 : 0x88402;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	pr_info("bascklight_timeout set value: %d\n", enable);
	err = wmidev_invoke_method(wdev, 0, ACER_WMID_SET_FUNCTION, &input,
				   &output, sizeof(u32));
	if (err) {
		pr_err("Error setting backlight_timeout status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting backlight_timeout status: %d\n", err);
		return err;
	}

	pr_info("backlight_timeout set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_boot_animation_sound(struct acer_wmi *acer,
						 int *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = 0;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK,
				  ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND);
	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_MISC_SETTING_METHODID,
				   &input, &output, sizeof(u32));
	if (err) {
		pr_err("Error getting boot_animation_sound status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error getting boot_animation_sound status: %d\n", err);
		return err;
	}

	pr_info("boot_animation_sound get status: %llu\n", result);
	*state = result == 0x100 ? 1 : result == 0 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_boot_animation_sound(struct acer_wmi *acer,
						 bool enable)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input_value = 0;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	pr_info("boot_animation_sound set value: %d\n", enable);
	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK,
				  ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND);
	input_value |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, enable);
	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_MISC_SETTING_METHODID,
				   &input, &output, sizeof(u32));
	if (err) {
		pr_err("Error setting boot_animation_sound status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting boot_animation_sound status: %d\n", err);
		return err;
	}

	pr_info("boot_animation_sound set status: %llu\n", result);
	return 0;
}

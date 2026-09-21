// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Predator Gaming WMI protocol access for the Acer WMI Laptop Extras driver.
 *
 *  This module implements the hardware commands of the Predator Gaming WMI
 *  interface and the battery health and USB charging commands of the WMID
 *  battery and APGE devices. It only exposes semantic operations to the rest
 *  of the driver.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense.h"
#include "linuwu_sense_gaming.h"
#include "linuwu_sense_wmi.h"

/*
 * Method IDs of the Predator Gaming WMI device
 */
#define ACER_WMID_GET_GAMING_PROFILE_METHODID 3
#define ACER_WMID_SET_GAMING_PROFILE_METHODID 1
#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID 5
#define ACER_WMID_SET_GAMING_MISC_SETTING_METHODID 22
#define ACER_WMID_GET_GAMING_MISC_SETTING_METHODID 23
#define ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID 21
#define ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID 20
#define ACER_WMID_SET_GAMING_RGB_KB_METHODID 6
#define ACER_WMID_GET_GAMING_RGB_KB_METHODID 7

/*
 * Method IDs of the WMID APGE device
 */
#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

/*
 * Method IDs of the WMID battery device
 */
#define ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID 20
#define ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID 21

/*
 * Command values of the "get system info" method
 */
#define ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS 0x02

/*
 * The return status must be zero for a command to have succeeded
 */
#define ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK GENMASK_ULL(7, 0)

#define ACER_GAMING_MISC_SETTING_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_INDEX_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_VALUE_MASK GENMASK_ULL(15, 8)

/*
 * Misc setting indices of the Predator Gaming WMI interface
 */
#define ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND 0x0006
#define ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES 0x000A
#define ACER_WMID_MISC_SETTING_PLATFORM_PROFILE 0x000B

/*
 * Per zone selectors of the four zone keyboard backlight
 */
static const u8 acer_wmid_kb_zone_ids[LINUWU_SENSE_GAMING_KB_ZONE_COUNT] = {
	0x1, 0x2, 0x4, 0x8
};

struct get_four_zoned_kb_output {
	u8 gmReturn;
	u8 gmOutput[15];
} __packed;

struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

/*
 * Execute a Predator Gaming WMI method through the WMI transport. Some of the
 * commands are only answered with a u32 value, so the minimum result size has
 * to be a u32.
 */
static acpi_status linuwu_sense_gaming_execute(struct acer_wmi *acer,
					       enum acer_wmi_guid guid,
					       u32 method_id, u64 input,
					       u64 *result)
{
	struct wmi_device *wdev = acer->wdevs[guid];

	if (!wdev)
		return AE_ERROR;

	return linuwu_sense_wmi_execute_u64_min_size(wdev, method_id, input,
						     sizeof(u32), result);
}

int linuwu_sense_gaming_get_sys_info(struct acer_wmi *acer, u32 command,
				     u64 *out)
{
	acpi_status status;
	u64 result;

	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_GET_GAMING_SYS_INFO_METHODID, command, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

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

static int linuwu_sense_gaming_set_misc_setting(struct acer_wmi *acer,
						u8 setting, u8 value)
{
	acpi_status status;
	u64 input = 0;
	u64 result;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, value);

	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	return 0;
}

static int linuwu_sense_gaming_get_misc_setting(struct acer_wmi *acer,
						u8 setting, u8 *value)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 input = 0;
	u64 result;
	int ret;

	if (!wdev)
		return -ENODEV;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);

	ret = linuwu_sense_wmi_execute_u32_u64(
		wdev, ACER_WMID_GET_GAMING_MISC_SETTING_METHODID, input,
		&result);
	if (ret < 0)
		return ret;

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
	acpi_status status;
	u64 result;

	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_GET_GAMING_PROFILE_METHODID, 0x00, &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting lcd override status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("lcd override get status: %llu\n", result);
	*state = result == 0x1000001000000 ? 1 : result == 0x1000000 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_lcd_override(struct acer_wmi *acer, bool enable)
{
	acpi_status status;
	u64 result;

	pr_info("lcd_override set value: %d\n", enable);
	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_SET_GAMING_PROFILE_METHODID,
		enable ? 0x1000000000010 : 0x10, &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting lcd override status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("lcd override set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_backlight_timeout(struct acer_wmi *acer, int *state)
{
	acpi_status status;
	u64 result;

	status = linuwu_sense_gaming_execute(acer, ACER_WMI_GUID_WMID_APGE,
					     ACER_WMID_GET_FUNCTION, 0x88401,
					     &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting backlight_timeout status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("backlight_timeout get status: %llu\n", result);
	*state = result == 0x1E0000080000 ? 1 : result == 0x80000 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_backlight_timeout(struct acer_wmi *acer,
					      bool enable)
{
	acpi_status status;
	u64 result;

	pr_info("bascklight_timeout set value: %d\n", enable);
	status = linuwu_sense_gaming_execute(acer, ACER_WMI_GUID_WMID_APGE,
					     ACER_WMID_SET_FUNCTION,
					     enable ? 0x1E0000088402 : 0x88402,
					     &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting backlight_timeout status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("backlight_timeout set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_boot_animation_sound(struct acer_wmi *acer,
						 int *state)
{
	acpi_status status;
	u64 input = 0;
	u64 result;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK,
			    ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND);
	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_GET_GAMING_MISC_SETTING_METHODID, input, &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting boot_animation_sound status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("boot_animation_sound get status: %llu\n", result);
	*state = result == 0x100 ? 1 : result == 0 ? 0 : -1;
	return 0;
}

int linuwu_sense_gaming_set_boot_animation_sound(struct acer_wmi *acer,
						 bool enable)
{
	acpi_status status;
	u64 input = 0;
	u64 result;

	pr_info("boot_animation_sound set value: %d\n", enable);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK,
			    ACER_WMID_MISC_SETTING_BOOT_ANIMATION_SOUND);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, enable);
	status = linuwu_sense_gaming_execute(
		acer, ACER_WMI_GUID_WMID_GAMING,
		ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, input, &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting boot_animation_sound status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("boot_animation_sound set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_usb_charging(struct acer_wmi *acer, int *percent)
{
	acpi_status status;
	u64 result;

	status = linuwu_sense_gaming_execute(acer, ACER_WMI_GUID_WMID_APGE,
					     ACER_WMID_GET_FUNCTION, 0x4,
					     &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting usb charging status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("usb charging get status: %llu\n", result);
	/* -1 means unknown value */
	*percent = result == 663296  ? 0 :
		   result == 659200  ? 10 :
		   result == 1314560 ? 20 :
		   result == 1969920 ? 30 :
				       -1;
	return 0;
}

int linuwu_sense_gaming_set_usb_charging(struct acer_wmi *acer, u8 percent)
{
	acpi_status status;
	u64 result;
	u64 value;

	pr_info("usb charging set value: %d\n", percent);
	/* If the value is unknown then turn it off */
	value = percent == 0  ? 663300 :
		percent == 10 ? 659204 :
		percent == 20 ? 1314564 :
		percent == 30 ? 1969924 :
				663300;
	status = linuwu_sense_gaming_execute(acer, ACER_WMI_GUID_WMID_APGE,
					     ACER_WMID_SET_FUNCTION, value,
					     &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting usb charging status: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}
	pr_info("usb charging set status: %llu\n", result);
	return 0;
}

int linuwu_sense_gaming_get_battery_mode(
	struct acer_wmi *acer, enum linuwu_sense_gaming_battery_mode mode,
	int *enabled)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_BATTERY];
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = 0x1,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct get_battery_health_control_status_output ret;
	int err;

	pr_info("battery health query: %d\n", mode);

	if (!wdev)
		return -ENODEV;

	err = linuwu_sense_wmi_execute_buffer(
       wdev, ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
       &params, sizeof(params), sizeof(ret), &ret, sizeof(ret));
	if (err) {
		pr_err("Unexpected output getting battery health status: %d\n",
		       err);
		goto out;
	}

	switch (mode) {
	case LINUWU_SENSE_GAMING_BATTERY_MODE_HEALTH:
		*enabled = ret.uFunctionStatus[0];
		break;
	case LINUWU_SENSE_GAMING_BATTERY_MODE_CALIBRATION:
		*enabled = ret.uFunctionStatus[1];
		break;
	default:
		err = -EINVAL;
		break;
	}

out:
	return err;
}

int linuwu_sense_gaming_set_battery_mode(
	struct acer_wmi *acer, enum linuwu_sense_gaming_battery_mode mode,
	u8 status)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_BATTERY];
	struct set_battery_health_control_input params = {
		.uBatteryNo = 0x1,
		.uFunctionMask = mode,
		.uFunctionStatus = status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct set_battery_health_control_output ret;
	int err;

	pr_info("%s: %d | %d\n", __func__, mode, status);

	if (!wdev)
		return -ENODEV;

	err = linuwu_sense_wmi_execute_buffer(
       wdev, ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID,
       &params, sizeof(params), sizeof(ret), &ret, sizeof(ret));
	if (err) {
		pr_err("Unexpected output setting battery health status: %d\n",
		       err);
		goto out;
	}

	if (ret.uReturn != 0 && ret.uReservedOut != 0) {
		pr_err("Failed to set battery health status\n");
		err = -EIO;
		goto out;
	}

out:
	return err;
}

int linuwu_sense_gaming_get_kb_backlight(
	struct acer_wmi *acer, struct linuwu_sense_gaming_kb_backlight *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	struct get_four_zoned_kb_output out;
	u64 in = 1;
	int err;

	if (!wdev)
		return -ENODEV;

	err = linuwu_sense_wmi_execute_buffer(
       wdev, ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID,
       &in, sizeof(in), sizeof(out), &out, sizeof(out));
	if (err) {
		pr_err("Unexpected output getting kb zone status: %d\n", err);
		goto out;
	}

	state->mode = out.gmOutput[0];
	state->speed = out.gmOutput[1];
	state->brightness = out.gmOutput[2];
	state->direction = out.gmOutput[4];
	state->red = out.gmOutput[5];
	state->green = out.gmOutput[6];
	state->blue = out.gmOutput[7];

out:
	return err;
}

int linuwu_sense_gaming_set_kb_backlight(
	struct acer_wmi *acer,
	const struct linuwu_sense_gaming_kb_backlight *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u8 gmInput[16] = {};
	u64 resp = 0;
	int err;

	if (!wdev)
		return -ENODEV;

	gmInput[0] = state->mode;
	gmInput[1] = state->speed;
	gmInput[2] = state->brightness;
	gmInput[4] = state->direction;
	gmInput[5] = state->red;
	gmInput[6] = state->green;
	gmInput[7] = state->blue;
	gmInput[8] = 3;
	gmInput[9] = 1;

	err = linuwu_sense_wmi_execute_buffer(
       wdev, ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID,
       gmInput, sizeof(gmInput), sizeof(u32), &resp, sizeof(resp));
	if (err)
		goto out;

	if (resp != 0) {
		pr_err("failed to set keyboard rgb: %llu\n", resp);
		err = -EIO;
	}

out:
	return err;
}

int linuwu_sense_gaming_get_kb_zone_color(struct acer_wmi *acer,
					  enum linuwu_sense_gaming_kb_zone zone,
					  u64 *color)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 value = acer_wmid_kb_zone_ids[zone];
	int err;

	if (!wdev)
		return -ENODEV;

	err = linuwu_sense_wmi_execute_buffer(
       wdev, ACER_WMID_GET_GAMING_RGB_KB_METHODID, &value,
       sizeof(value), sizeof(u32), &value, sizeof(value));
	if (err)
		goto err_log;

	/* The color is stored in the upper three bytes of the response */
	*color = cpu_to_be64(value) >> 32;

	return 0;

err_log:
	pr_err("Error getting kb status (zone %d): %d\n", zone + 1, err);
	return err;
}

int linuwu_sense_gaming_set_kb_zone_color(struct acer_wmi *acer,
					  enum linuwu_sense_gaming_kb_zone zone,
					  u64 color)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 value;
	int err;

	if (!wdev)
		return -ENODEV;

	value = (cpu_to_be64(color) >> 32) | acer_wmid_kb_zone_ids[zone];

	err = linuwu_sense_wmi_execute_buffer(
		wdev, ACER_WMID_SET_GAMING_RGB_KB_METHODID, &value,
		sizeof(value), sizeof(u32), NULL, 0);
	if (err)
		pr_err("Error setting KB color (zone %d): %d\n", zone + 1, err);

	return err;
}

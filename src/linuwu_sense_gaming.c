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

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
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

int linuwu_sense_gaming_get_sys_info(struct acer_wmi *acer, u32 command,
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

int linuwu_sense_gaming_get_usb_charging(struct acer_wmi *acer, int *percent)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	u64 input_value = 0x4;
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
		pr_err("Error getting usb charging status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error getting usb charging status: %d\n", err);
		return err;
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
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	u64 input_value;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	pr_info("usb charging set value: %d\n", percent);
	/* If the value is unknown then turn it off */
	input_value = percent == 0  ? 663300 :
		      percent == 10 ? 659204 :
		      percent == 20 ? 1314564 :
		      percent == 30 ? 1969924 :
				      663300;
	err = wmidev_invoke_method(wdev, 0, ACER_WMID_SET_FUNCTION, &input,
				   &output, sizeof(u32));
	if (err) {
		pr_err("Error setting usb charging status: %d\n", err);
		return err;
	}

	err = linuwu_sense_gaming_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting usb charging status: %d\n", err);
		return err;
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
	struct wmi_buffer input = {
		.length = sizeof(params),
		.data = &params,
	};
	struct wmi_buffer output = {};
	int err;

	pr_info("battery health query: %d\n", mode);

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
				   &input, &output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output getting battery health status: %d\n",
		       err);
		goto out;
	}

	if (output.length < sizeof(ret)) {
		err = -EIO;
		goto out;
	}
	memcpy(&ret, output.data, sizeof(ret));

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
	kfree(output.data);
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
	struct wmi_buffer input = {
		.length = sizeof(params),
		.data = &params,
	};
	struct wmi_buffer output = {};
	int err;

	pr_info("%s: %d | %d\n", __func__, mode, status);

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID,
				   &input, &output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output setting battery health status: %d\n",
		       err);
		goto out;
	}

	if (output.length < sizeof(ret)) {
		err = -EIO;
		goto out;
	}
	memcpy(&ret, output.data, sizeof(ret));

	if (ret.uReturn != 0 && ret.uReservedOut != 0) {
		pr_err("Failed to set battery health status\n");
		err = -EIO;
		goto out;
	}

out:
	kfree(output.data);
	return err;
}

int linuwu_sense_gaming_get_kb_backlight(
	struct acer_wmi *acer, struct linuwu_sense_gaming_kb_backlight *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	struct get_four_zoned_kb_output out;
	struct wmi_buffer input = {};
	struct wmi_buffer output = {};
	u64 in = 1;
	int err;

	if (!wdev)
		return -ENODEV;

	input.length = sizeof(in);
	input.data = &in;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(out));
	if (err) {
		pr_err("Unexpected output getting kb zone status: %d\n", err);
		goto out;
	}

	if (output.length < sizeof(out)) {
		err = -EIO;
		goto out;
	}
	memcpy(&out, output.data, sizeof(out));

	state->mode = out.gmOutput[0];
	state->speed = out.gmOutput[1];
	state->brightness = out.gmOutput[2];
	state->direction = out.gmOutput[4];
	state->red = out.gmOutput[5];
	state->green = out.gmOutput[6];
	state->blue = out.gmOutput[7];

out:
	kfree(output.data);
	return err;
}

int linuwu_sense_gaming_set_kb_backlight(
	struct acer_wmi *acer,
	const struct linuwu_sense_gaming_kb_backlight *state)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u8 gmInput[16] = {};
	struct wmi_buffer input = {
		.length = sizeof(gmInput),
		.data = gmInput,
	};
	struct wmi_buffer output = {};
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

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	memcpy(&resp, output.data, min_t(size_t, output.length, sizeof(resp)));
	kfree(output.data);

	if (resp != 0) {
		pr_err("failed to set keyboard rgb: %llu\n", resp);
		err = -EIO;
	}

	return err;
}

int linuwu_sense_gaming_get_kb_zone_color(struct acer_wmi *acer,
					  enum linuwu_sense_gaming_kb_zone zone,
					  u64 *color)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 value = acer_wmid_kb_zone_ids[zone];
	u64 result = 0;
	struct wmi_buffer input = {
		.length = sizeof(value),
		.data = &value,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_RGB_KB_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		goto err_log;

	memcpy(&result, output.data, min_t(size_t, output.length, sizeof(result)));
	kfree(output.data);

	/* The color is stored in the upper three bytes of the response */
	*color = cpu_to_be64(result) >> 32;

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
	u64 value = (cpu_to_be64(color) >> 32) | acer_wmid_kb_zone_ids[zone];
	struct wmi_buffer input = {
		.length = sizeof(value),
		.data = &value,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_RGB_KB_METHODID,
				   &input, &output, sizeof(u32));
	kfree(output.data);
	if (err)
		pr_err("Error setting KB color (zone %d): %d\n", zone + 1, err);

	return err;
}

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
#include <linux/bitops.h>
#include <linux/kernel.h>
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

/*
 * Method IDs of the WMID APGE device
 */
#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

/*
 * The system function command of the WMID APGE device.
 *
 * Hotkey Customized Setting and Acer Application Status.
 * Set Device Default Value and Report Acer Application Status.
 * When Acer Application starts, it will run this method to inform
 * BIOS/EC that Acer Application is on.
 * App Status
 *	Bit[0]: Launch Manager Status
 *	Bit[1]: ePM Status
 *	Bit[2]: Device Control Status
 *	Bit[3]: Acer Power Button Utility Status
 *	Bit[4]: RF Button Status
 *	Bit[5]: ODD PM Status
 *	Bit[6]: Device Default Value Control
 *	Bit[7]: Hall Sensor Application Status
 */
struct func_input_params {
	u8 function_num; /* Function Number */
	u16 commun_devices; /* Communication type devices default status */
	u16 devices; /* Other type devices default status */
	u8 app_status; /* Acer Device Status. LM, ePM, RF Button... */
	u8 app_mask; /* Bit mask to app_status */
	u8 reserved;
} __packed;

struct func_return_value {
	u8 error_code; /* Error Code */
	u8 ec_return_value; /* EC Return Value */
	u16 reserved;
} __packed;

static int
linuwu_sense_gaming_set_function_mode(struct acer_wmi *acer,
				      struct func_input_params *params,
				      struct func_return_value *return_value)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	struct wmi_buffer input = {
		.length = sizeof(*params),
		.data = params,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, ACER_WMID_SET_FUNCTION, &input,
				   &output, sizeof(*return_value));
	if (err)
		return err;

	if (output.length < sizeof(*return_value)) {
		err = -EIO;
		goto out;
	}

	memcpy(return_value, output.data, sizeof(*return_value));

out:
	kfree(output.data);
	return err;
}

int linuwu_sense_gaming_enable_ec_raw(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x00, /* Launch Manager Deactive */
		.app_mask = 0x01,
	};
	int err;

	err = linuwu_sense_gaming_set_function_mode(acer, &params,
						    &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling EC raw mode failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
	else
		pr_info("Enabled EC raw mode\n");

	return 0;
}

int linuwu_sense_gaming_enable_launch_manager(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x01, /* Launch Manager Active */
		.app_mask = 0x01,
	};
	int err;

	err = linuwu_sense_gaming_set_function_mode(acer, &params,
						    &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling Launch Manager failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return 0;
}

int linuwu_sense_gaming_enable_rf_button(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x10, /* RF Button Active */
		.app_mask = 0x10,
	};
	int err;

	err = linuwu_sense_gaming_set_function_mode(acer, &params,
						    &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling RF Button failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return 0;
}

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

/*
 * Predator thermal profiles as understood by the Predator Gaming WMI
 * interface.
 */
enum acer_predator_v4_thermal_profile {
	ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET = 0x00,
	ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED = 0x01,
	ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE = 0x04,
	ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO = 0x05,
	ACER_PREDATOR_V4_THERMAL_PROFILE_ECO = 0x06,
};

static int
linuwu_sense_gaming_profile_to_acer(enum platform_profile_option profile,
				    u8 *acer_profile)
{
	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		*acer_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
		break;
	case PLATFORM_PROFILE_QUIET:
		*acer_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		*acer_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		*acer_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		*acer_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
linuwu_sense_gaming_profile_from_acer(u8 acer_profile,
				      enum platform_profile_option *profile)
{
	switch (acer_profile) {
	case ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_ECO:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

int linuwu_sense_gaming_get_thermal_profile(
	struct acer_wmi *acer, enum platform_profile_option *profile)
{
	u8 acer_profile;
	int err;

	err = linuwu_sense_gaming_get_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, &acer_profile);
	if (err)
		return err;

	return linuwu_sense_gaming_profile_from_acer(acer_profile, profile);
}

int linuwu_sense_gaming_set_thermal_profile(
	struct acer_wmi *acer, enum platform_profile_option profile)
{
	u8 acer_profile;
	int err;

	err = linuwu_sense_gaming_profile_to_acer(profile, &acer_profile);
	if (err)
		return err;

	return linuwu_sense_gaming_set_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, acer_profile);
}

int linuwu_sense_gaming_get_supported_thermal_profiles(struct acer_wmi *acer,
						       unsigned long *choices)
{
	u8 supported = 0;
	int err;

	/* The firmware bitmap is returned as a single byte */
	err = linuwu_sense_gaming_get_misc_setting(
		acer, ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES, &supported);
	if (err)
		return err;

	if (supported & BIT(ACER_PREDATOR_V4_THERMAL_PROFILE_ECO))
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	if (supported & BIT(ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET))
		set_bit(PLATFORM_PROFILE_QUIET, choices);
	if (supported & BIT(ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED))
		set_bit(PLATFORM_PROFILE_BALANCED, choices);
	if (supported & BIT(ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE))
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	if (supported & BIT(ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO))
		set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
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

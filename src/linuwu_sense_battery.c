// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Battery and USB charging WMI commands for the Acer Predator/Nitro driver.
 *
 *  This module implements the battery health control commands of the WMID
 *  battery device and the USB charging commands of the WMID APGE device.
 *  It keeps no driver state, the firmware is the only owner of the battery
 *  state.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense_battery.h"

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
 * Decode the result of one of the APGE function commands. Some of the
 * commands are only answered with a u32 value while others return a full u64
 * value.
 */
static int linuwu_sense_battery_decode_result(const struct wmi_buffer *output,
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

int linuwu_sense_battery_get_usb_charging(struct wmi_device *wdev, int *percent)
{
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

	err = linuwu_sense_battery_decode_result(&output, &result);
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

int linuwu_sense_battery_set_usb_charging(struct wmi_device *wdev, u8 percent)
{
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

	err = linuwu_sense_battery_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting usb charging status: %d\n", err);
		return err;
	}

	pr_info("usb charging set status: %llu\n", result);
	return 0;
}

int linuwu_sense_battery_get_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  int *enabled)
{
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

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
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
	case LINUWU_SENSE_BATTERY_MODE_HEALTH:
		*enabled = ret.uFunctionStatus[0];
		break;
	case LINUWU_SENSE_BATTERY_MODE_CALIBRATION:
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

int linuwu_sense_battery_set_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  u8 status)
{
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

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID, &input,
		&output, sizeof(ret));
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

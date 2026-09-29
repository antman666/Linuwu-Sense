// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer APGE WMI protocol access for the Acer Predator/Nitro driver.
 *
 *  This module implements the wire protocol of the APGE system function
 *  commands, shared by the gaming and battery feature backends.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense_apge.h"

/*
 * Method IDs of the WMID APGE device
 */
#define LINUWU_SENSE_APGE_SET_FUNCTION 1
#define LINUWU_SENSE_APGE_GET_FUNCTION 2

/*
 * Decode the result of an APGE function command. Some of the commands are
 * only answered with a u32 value while others return a full u64 value.
 */
static int linuwu_sense_apge_decode_result(const struct wmi_buffer *output,
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

int linuwu_sense_apge_set_app_status(
	struct wmi_device *wdev,
	const struct linuwu_sense_apge_app_status *status,
	struct linuwu_sense_apge_function_result *result)
{
	struct wmi_buffer input = {
		.length = sizeof(*status),
		.data = (void *)status,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, LINUWU_SENSE_APGE_SET_FUNCTION,
				   &input, &output, sizeof(*result));
	if (err)
		return err;

	if (output.length < sizeof(*result)) {
		err = -EIO;
		goto out;
	}

	memcpy(result, output.data, sizeof(*result));

out:
	kfree(output.data);
	return err;
}

int linuwu_sense_apge_get_function(struct wmi_device *wdev, u64 function,
				   u64 *value)
{
	struct wmi_buffer input = {
		.length = sizeof(function),
		.data = &function,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, LINUWU_SENSE_APGE_GET_FUNCTION,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	err = linuwu_sense_apge_decode_result(&output, &result);
	kfree(output.data);
	if (err)
		return err;

	*value = result;

	return 0;
}

int linuwu_sense_apge_set_function_value(struct wmi_device *wdev, u64 function,
					 u64 *status)
{
	struct wmi_buffer input = {
		.length = sizeof(function),
		.data = &function,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, LINUWU_SENSE_APGE_SET_FUNCTION,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	err = linuwu_sense_apge_decode_result(&output, &result);
	kfree(output.data);
	if (err)
		return err;

	*status = result;

	return 0;
}

// SPDX-License-Identifier: GPL-2.0

#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense_wmi.h"

acpi_status linuwu_sense_wmi_execute_u64(struct wmi_device *wdev,
                                         u32 method_id, u64 input,
                                         u64 *result)
{
	struct wmi_buffer input_buf = {
		.length = sizeof(input),
		.data = &input,
	};
	struct wmi_buffer output_buf = {};
	u64 value;
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, method_id, &input_buf, &output_buf,
				   sizeof(value));
	if (err) {
		kfree(output_buf.data);
		return AE_ERROR;
	}

	if (!output_buf.data)
		return AE_ERROR;

	if (output_buf.length >= sizeof(value))
		value = get_unaligned_le64(output_buf.data);
	else if (output_buf.length >= sizeof(u32))
		value = get_unaligned_le32(output_buf.data);
	else {
		kfree(output_buf.data);
		return AE_ERROR;
	}

	kfree(output_buf.data);

	if (result)
		*result = value;

	return AE_OK;
}

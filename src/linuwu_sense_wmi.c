// SPDX-License-Identifier: GPL-2.0

#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense_wmi.h"

int linuwu_sense_wmi_execute_buffer(struct wmi_device *wdev, u32 method_id,
				    const void *input, size_t input_len,
				    size_t min_size, void *output,
				    size_t output_size)
{
	struct wmi_buffer input_buf = {
		.length = input_len,
		.data = (void *)input,
	};
	struct wmi_buffer output_buf = {};
	size_t copy_len;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, method_id, &input_buf, &output_buf,
				   min_size);
	if (err)
		goto out;

	if (!output_buf.data && output && output_size) {
		err = -EIO;
		goto out;
	}

	if (output && output_size) {
		memset(output, 0, output_size);
		copy_len = output_size;
		if (copy_len > output_buf.length)
			copy_len = output_buf.length;
		memcpy(output, output_buf.data, copy_len);
	}

out:
	kfree(output_buf.data);
	return err;
}

int linuwu_sense_wmi_query_block(struct wmi_device *wdev, u8 instance,
				 size_t min_size, void *output,
				 size_t output_size)
{
	struct wmi_buffer output_buf = {};
	size_t copy_len;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_query_block(wdev, instance, &output_buf, min_size);
	if (err)
		goto out;

	if (!output_buf.data && output && output_size) {
		err = -EIO;
		goto out;
	}

	if (output && output_size) {
		memset(output, 0, output_size);
		copy_len = output_size;
		if (copy_len > output_buf.length)
			copy_len = output_buf.length;
		memcpy(output, output_buf.data, copy_len);
	}

out:
	kfree(output_buf.data);
	return err;
}

acpi_status linuwu_sense_wmi_execute_u64(struct wmi_device *wdev, u32 method_id,
					 u64 input, u64 *result)
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

static int linuwu_sense_wmi_invoke_u64(struct wmi_device *wdev, u32 method_id,
				       void *input, size_t input_len,
				       size_t min_size, u64 *result)
{
	struct wmi_buffer input_buf = {
		.length = input_len,
		.data = input,
	};
	struct wmi_buffer output_buf = {};
	u64 value;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, method_id, &input_buf, &output_buf,
				   min_size);
	if (err)
		goto out;

	if (!output_buf.data) {
		err = -EIO;
		goto out;
	}

	if (output_buf.length >= sizeof(value)) {
		value = get_unaligned_le64(output_buf.data);
	} else if (output_buf.length >= sizeof(u32)) {
		value = get_unaligned_le32(output_buf.data);
	} else {
		err = -EIO;
		goto out;
	}

	if (result)
		*result = value;

out:
	kfree(output_buf.data);
	return err;
}

acpi_status linuwu_sense_wmi_execute_u64_min_size(struct wmi_device *wdev,
						  u32 method_id, u64 input,
						  size_t min_size, u64 *result)
{
	return linuwu_sense_wmi_invoke_u64(wdev, method_id, &input,
					   sizeof(input), min_size, result) ?
		       AE_ERROR :
		       AE_OK;
}

int linuwu_sense_wmi_execute_u32_u64(struct wmi_device *wdev, u32 method_id,
				     u32 input, u64 *result)
{
	return linuwu_sense_wmi_invoke_u64(wdev, method_id, &input,
					   sizeof(input), sizeof(u32), result);
}

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUWU_SENSE_WMI_H
#define LINUWU_SENSE_WMI_H

#include <linux/acpi.h>
#include <linux/types.h>
#include <linux/wmi.h>

/*
 * Execute an Acer WMI method with arbitrary input/output buffers.
 *
 * @min_size is the minimum result size requested from the WMI subsystem.
 * If @output is non-NULL, up to @output_size bytes are copied from the
 * firmware result buffer. The destination is zero-filled before copying so
 * callers can safely consume short u32-or-u64 responses.
 *
 * Returns the raw errno returned by the WMI subsystem, or a local errno on
 * a missing output buffer when a caller requested one.
 */
int linuwu_sense_wmi_execute_buffer(struct wmi_device *wdev, u32 method_id,
				    const void *input, size_t input_len,
				    size_t min_size, void *output,
				    size_t output_size);

/*
 * Query an Acer WMI data block with arbitrary output buffers.
 *
 * @min_size is the minimum result size requested from the WMI subsystem.
 * If @output is non-NULL, up to @output_size bytes are copied from the
 * WMI result buffer. The destination is zero-filled before copying.
 *
 * Returns the raw errno returned by the WMI subsystem, or a local errno on
 * a missing output buffer when a caller requested one.
 */
int linuwu_sense_wmi_query_block(struct wmi_device *wdev, u8 instance,
				 size_t min_size, void *output,
				 size_t output_size);

/*
 * Execute an Acer WMI method whose input is a u64 and whose result is
 * returned as either a u32 or u64 value.
 */
acpi_status linuwu_sense_wmi_execute_u64(struct wmi_device *wdev, u32 method_id,
					 u64 input, u64 *result);

/*
 * Execute an Acer WMI method whose input is a u64 and whose result is
 * returned as either a u32 or u64 value. @min_size is the minimum size of the
 * result data in bytes, the WMI subsystem fails the call if the device
 * returns less data.
 *
 * This variant exists for commands which some firmware revisions only answer
 * with a u32 value.
 */
acpi_status linuwu_sense_wmi_execute_u64_min_size(struct wmi_device *wdev,
						  u32 method_id, u64 input,
						  size_t min_size, u64 *result);

/*
 * Execute an Acer WMI method whose input is a u32 and whose result is
 * returned as either a u32 or u64 value.
 *
 * Unlike linuwu_sense_wmi_execute_u64(), this returns the raw error code of
 * the WMI subsystem so that callers can propagate it.
 */
int linuwu_sense_wmi_execute_u32_u64(struct wmi_device *wdev, u32 method_id,
				     u32 input, u64 *result);

#endif /* LINUWU_SENSE_WMI_H */

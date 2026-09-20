/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUWU_SENSE_WMI_H
#define LINUWU_SENSE_WMI_H

#include <linux/acpi.h>
#include <linux/types.h>
#include <linux/wmi.h>

/*
 * Execute an Acer WMI method whose input is a u64 and whose result is
 * returned as either a u32 or u64 value.
 */
acpi_status linuwu_sense_wmi_execute_u64(struct wmi_device *wdev,
                                         u32 method_id, u64 input,
                                         u64 *result);

#endif /* LINUWU_SENSE_WMI_H */

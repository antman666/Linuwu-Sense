/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUWU_SENSE_FAN_H
#define LINUWU_SENSE_FAN_H

#include <linux/acpi.h>
#include <linux/types.h>
#include <linux/wmi.h>

enum linuwu_sense_fan {
	LINUWU_SENSE_FAN_CPU,
	LINUWU_SENSE_FAN_GPU,
};

enum linuwu_sense_fan_mode {
	LINUWU_SENSE_FAN_MODE_AUTO = 1,
	LINUWU_SENSE_FAN_MODE_TURBO = 2,
	LINUWU_SENSE_FAN_MODE_CUSTOM = 3,
};

acpi_status linuwu_sense_fan_set_behavior(struct wmi_device *wdev,
					  u64 behavior);

acpi_status linuwu_sense_fan_set_mode(struct wmi_device *wdev, bool cpu_fan,
				      bool gpu_fan,
				      enum linuwu_sense_fan_mode fan_mode);

acpi_status linuwu_sense_fan_get_mode(struct wmi_device *wdev,
				      enum linuwu_sense_fan fan,
				      enum linuwu_sense_fan_mode *fan_mode);

acpi_status linuwu_sense_fan_set_speed(struct wmi_device *wdev,
				       enum linuwu_sense_fan fan,
				       int percentage);

#endif /* LINUWU_SENSE_FAN_H */

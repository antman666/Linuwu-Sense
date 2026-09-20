/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUWU_SENSE_HWMON_H
#define LINUWU_SENSE_HWMON_H

#include <linux/types.h>

struct acer_wmi;
struct device;

int acer_wmi_get_sys_info(struct acer_wmi *acer, u32 command, u64 *out);
int acer_wmi_hwmon_init(struct acer_wmi *acer, struct device *dev);

#endif /* LINUWU_SENSE_HWMON_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUWU_SENSE_HWMON_H
#define LINUWU_SENSE_HWMON_H

#include <linux/types.h>

struct acer_wmi;
struct device;

/*
 * Register the hwmon frontend of an instance. Exposes the firmware sensor
 * readings and the fan control channels; nothing is registered when the
 * machine provides neither of them.
 */
int acer_wmi_hwmon_init(struct acer_wmi *acer, struct device *dev);

#endif /* LINUWU_SENSE_HWMON_H */

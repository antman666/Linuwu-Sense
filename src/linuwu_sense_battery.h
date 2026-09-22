/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Battery and USB charging WMI commands for the Acer Predator/Nitro driver.
 */
#ifndef _LINUWU_SENSE_BATTERY_H_
#define _LINUWU_SENSE_BATTERY_H_

#include <linux/types.h>

struct wmi_device;

/*
 * Battery health control functions of the WMID battery device.
 */
enum linuwu_sense_battery_mode {
	LINUWU_SENSE_BATTERY_MODE_HEALTH = 1,
	LINUWU_SENSE_BATTERY_MODE_CALIBRATION = 2,
};

/*
 * Locking: the caller resolves the WMI device and serializes the calls, the
 * core driver holds acer->lock around them.
 */

/* Battery health control status */
int linuwu_sense_battery_get_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  int *enabled);
int linuwu_sense_battery_set_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  u8 status);

/* USB charging, in percent of the maximum charging current */
int linuwu_sense_battery_get_usb_charging(struct wmi_device *wdev,
					  int *percent);
int linuwu_sense_battery_set_usb_charging(struct wmi_device *wdev, u8 percent);

#endif /* _LINUWU_SENSE_BATTERY_H_ */

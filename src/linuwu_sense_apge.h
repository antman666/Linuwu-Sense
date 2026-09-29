/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Acer APGE WMI protocol access for the Acer Predator/Nitro driver.
 *
 *  The APGE device (WMID_GUID3) carries the Acer system function commands:
 *  the application status handshake and the raw function value commands used
 *  by the USB charging and backlight timeout features. This file only encodes
 *  and decodes that protocol, the semantic operations stay in the feature
 *  backends.
 */
#ifndef _LINUWU_SENSE_APGE_H_
#define _LINUWU_SENSE_APGE_H_

#include <linux/types.h>

struct wmi_device;

/*
 * Locking: the caller holds acer->lock while it resolves the WMI device and
 * issues the command, so the endpoint cannot be removed in between. These
 * functions neither take nor release any driver lock themselves.
 */

/*
 * Application status input of the APGE system function command.
 *
 * Hotkey Customized Setting and Acer Application Status. The app status bits
 * are:
 *	Bit[0]: Launch Manager Status
 *	Bit[1]: ePM Status
 *	Bit[2]: Device Control Status
 *	Bit[3]: Acer Power Button Utility Status
 *	Bit[4]: RF Button Status
 *	Bit[5]: ODD PM Status
 *	Bit[6]: Device Default Value Control
 *	Bit[7]: Hall Sensor Application Status
 */
struct linuwu_sense_apge_app_status {
	u8 function_num; /* Function Number */
	u16 commun_devices; /* Communication type devices default status */
	u16 devices; /* Other type devices default status */
	u8 app_status; /* Acer Device Status. LM, ePM, RF Button... */
	u8 app_mask; /* Bit mask to app_status */
	u8 reserved;
} __packed;

/* Result of the APGE system function command. */
struct linuwu_sense_apge_function_result {
	u8 error_code; /* Error Code */
	u8 ec_return_value; /* EC Return Value */
	u16 reserved;
} __packed;

/* Set the application status through the APGE system function command. */
int linuwu_sense_apge_set_app_status(
	struct wmi_device *wdev,
	const struct linuwu_sense_apge_app_status *status,
	struct linuwu_sense_apge_function_result *result);

/*
 * Raw APGE function value commands. The meaning of the value is defined by
 * the individual feature command. get_function returns the firmware answer,
 * set_function_value returns the firmware status which is zero on success.
 */
int linuwu_sense_apge_get_function(struct wmi_device *wdev, u64 function,
				   u64 *value);
int linuwu_sense_apge_set_function_value(struct wmi_device *wdev, u64 function,
					 u64 *status);

#endif /* _LINUWU_SENSE_APGE_H_ */

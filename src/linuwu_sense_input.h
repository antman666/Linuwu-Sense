/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Input (hotkey) frontend for the Acer Predator/Nitro driver.
 */
#ifndef _LINUWU_SENSE_INPUT_H_
#define _LINUWU_SENSE_INPUT_H_

#include <linux/types.h>

struct acer_wmi;

/*
 * Register the hotkey input device. Does nothing when the machine has no WMI
 * event device. Must be called from the platform device probe.
 */
int linuwu_sense_input_init(struct acer_wmi *acer);

/*
 * Report a firmware hotkey event through the input subsystem. Does nothing
 * when the input device is not registered.
 */
void linuwu_sense_input_report_hotkey(struct acer_wmi *acer, u8 key_num,
				      u16 device_state);

#endif /* _LINUWU_SENSE_INPUT_H_ */

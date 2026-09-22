/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Four zone keyboard backlight for the Acer Predator/Nitro driver.
 */
#ifndef _LINUWU_SENSE_KEYBOARD_H_
#define _LINUWU_SENSE_KEYBOARD_H_

struct acer_wmi;

/*
 * Set up the four zone keyboard backlight: allocate the driver side state,
 * register the sysfs interface and restore the state saved by the previous
 * module load. Must be called from the platform device probe. Does nothing
 * when the machine has no four zone keyboard.
 */
int linuwu_sense_keyboard_init(struct acer_wmi *acer);

/*
 * Persist the current keyboard state for the next module load. Best effort,
 * failures are only reported in the kernel log. Must be called before the
 * platform device resources are released.
 */
void linuwu_sense_keyboard_save_state(struct acer_wmi *acer);

#endif /* _LINUWU_SENSE_KEYBOARD_H_ */

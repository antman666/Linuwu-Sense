/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Four zone keyboard backlight for the Acer Predator/Nitro driver.
 */
#ifndef _LINUWU_SENSE_KEYBOARD_H_
#define _LINUWU_SENSE_KEYBOARD_H_

struct acer_wmi;

/*
 * Probe the four zone keyboard without changing its state. Used to decide
 * whether a feature= request may enable the keyboard frontend. Must be called
 * with acer->lock held; the function issues the WMI commands itself.
 */
int linuwu_sense_keyboard_probe(struct acer_wmi *acer);

/*
 * Set up the four zone keyboard backlight: allocate the driver side state,
 * register the sysfs interface, read the current firmware state and apply the
 * static zone colours from options=. Must be called from the logical platform
 * device probe without acer->lock held; the function takes the lock itself.
 */
int linuwu_sense_keyboard_init(struct acer_wmi *acer);

#endif /* _LINUWU_SENSE_KEYBOARD_H_ */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Four zone keyboard backlight for the Acer Predator/Nitro driver.
 */
#ifndef _LINUWU_SENSE_KEYBOARD_H_
#define _LINUWU_SENSE_KEYBOARD_H_

struct acer_wmi;
struct linuwu_sense_evidence;

/*
 * Probe independent keyboard GET operations without changing device state.
 * Partial read support is retained in @evidence; one unsupported zone does
 * not veto the remaining keyboard controls. The function takes acer->lock.
 */
int linuwu_sense_keyboard_probe(struct acer_wmi *acer,
				struct linuwu_sense_evidence *evidence);

/*
 * Set up the four zone keyboard backlight: allocate the driver side state,
 * register the sysfs interface, read the current firmware state and apply the
 * static zone colours from options=. Must be called from the logical platform
 * device probe without acer->lock held; the function takes the lock itself.
 */
int linuwu_sense_keyboard_init(struct acer_wmi *acer);

#endif /* _LINUWU_SENSE_KEYBOARD_H_ */

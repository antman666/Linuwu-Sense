/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  platform_profile adapter for the Acer WMI Laptop Extras driver.
 */
#ifndef _LINUWU_SENSE_PROFILE_H_
#define _LINUWU_SENSE_PROFILE_H_

#include <linux/types.h>

struct acer_wmi;

/*
 * Register the platform_profile handler and restore the thermal profile
 * selected by the firmware. Must be called from the platform device probe.
 */
int linuwu_sense_profile_init(struct acer_wmi *acer);

/*
 * Cycle to the next thermal profile, as requested by the turbo key.
 */
int linuwu_sense_profile_cycle(struct acer_wmi *acer);

/*
 * Switch the thermal profile after the power source changed. Has to be called
 * without acer->lock held, the WMI event lock may be held.
 */
int linuwu_sense_profile_power_source_changed(struct acer_wmi *acer,
					      bool on_ac);

#endif /* _LINUWU_SENSE_PROFILE_H_ */

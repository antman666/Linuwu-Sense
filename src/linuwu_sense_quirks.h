/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Model quirks for the Acer Predator/Nitro WMI driver.
 */
#ifndef _LINUWU_SENSE_QUIRKS_H_
#define _LINUWU_SENSE_QUIRKS_H_

#include <linux/types.h>

/*
 * Model specific static configuration. Entries are selected from the DMI
 * table by linuwu_sense_quirks_match() and never modified afterwards.
 */
struct linuwu_sense_quirks {
	u8 cpu_fans;
	u8 gpu_fans;
	u8 predator_v4;
	u8 nitro_v4;
	u8 nitro_sense;
	u8 four_zone_kb;
};

/*
 * Select the quirk entry for the running machine. Returns NULL when the
 * machine is not a supported Acer Predator/Nitro model.
 */
const struct linuwu_sense_quirks *linuwu_sense_quirks_match(void);

#endif /* _LINUWU_SENSE_QUIRKS_H_ */

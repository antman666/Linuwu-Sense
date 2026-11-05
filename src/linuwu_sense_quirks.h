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
 *
 * The table no longer describes which features a model supports: features are
 * discovered from the hardware at load time. What remains are the interface
 * family (which selects the userspace group and the thermal profile
 * behaviour) and a negative veto for known firmware bugs.
 */
struct linuwu_sense_quirks {
	u8 predator_v4;
	u8 nitro_v4;
	u8 nitro_sense;

	/*
	 * Features that must not be enabled on this model even when the
	 * verification says they are present. This is a negative quirk: it
	 * exists for firmware that reports a capability it does not really
	 * implement. Leave zero unless a specific model needs the veto.
	 */
	u32 deny;
};

/*
 * Select the quirk entry for the running machine. Returns NULL when the
 * machine is not a supported Acer Predator/Nitro model.
 */
const struct linuwu_sense_quirks *linuwu_sense_quirks_match(void);

#endif /* _LINUWU_SENSE_QUIRKS_H_ */

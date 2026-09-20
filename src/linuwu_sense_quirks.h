/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Model quirks for the Acer WMI Laptop Extras driver.
 */
#ifndef _LINUWU_SENSE_QUIRKS_H_
#define _LINUWU_SENSE_QUIRKS_H_

#include <linux/types.h>

/*
 * Model specific static configuration. Entries are selected from the DMI
 * tables by linuwu_sense_quirks_match() and never modified afterwards.
 *
 * The wireless, mailled and brightness fields describe machines where those
 * features are not available through WMI and have to be accessed through the
 * Embedded Controller directly.
 */
struct linuwu_sense_quirks {
	u8 wireless;
	u8 mailled;
	s8 brightness;
	u8 bluetooth;
	u8 cpu_fans;
	u8 gpu_fans;
	u8 predator_v4;
	u8 nitro_v4;
	u8 nitro_sense;
	u8 four_zone_kb;
	/* Capability override for this model, 0 if there is none */
	u32 force_caps;
};

/*
 * True when the running machine must not be driven by this module at all.
 * Only valid during module initialization.
 */
bool __init linuwu_sense_quirks_blacklisted(void);

/* True when the machine is a known AMW0_GUID1 carrier of the Acer family. */
bool linuwu_sense_quirks_amw0_whitelisted(void);

/* True when no model specific quirk entry matched the machine. */
bool linuwu_sense_quirks_is_unknown(const struct linuwu_sense_quirks *quirks);

/*
 * Select the quirk entry for the running machine. Never returns NULL, an
 * unmatched machine falls back to an all-zero entry.
 */
const struct linuwu_sense_quirks *linuwu_sense_quirks_match(void);

/*
 * The capability bitmap to force for the running machine, -1 if neither the
 * module parameter nor a DMI entry requested an override.
 */
int linuwu_sense_quirks_force_caps(void);

#endif /* _LINUWU_SENSE_QUIRKS_H_ */

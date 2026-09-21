// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Model quirks and DMI matching for the Acer WMI Laptop Extras driver.
 *
 *  Copyright (C) 2007-2009	Carlos Corbacho <carlos@strangeworlds.co.uk>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

#include "linuwu_sense.h"
#include "linuwu_sense_quirks.h"

/*
 * Model override parameters. They are only consumed while the quirk entry of
 * a device instance is selected.
 */
static bool predator_v4;
static bool nitro_v4;
static int force_series;
static int force_caps = -1;

module_param(force_series, int, 0444);
module_param(force_caps, int, 0444);
module_param(predator_v4, bool, 0444);
module_param(nitro_v4, bool, 0444);
MODULE_PARM_DESC(force_series, "Force a different laptop series");
MODULE_PARM_DESC(force_caps, "Force the capability bitmask to this value");
MODULE_PARM_DESC(
	predator_v4,
	"Enable features for predator laptops that use predator sense v4");
MODULE_PARM_DESC(nitro_v4,
		 "Enable features for nitro laptops that use nitro sense v4");

static const struct linuwu_sense_quirks quirk_unknown = {};

static const struct linuwu_sense_quirks quirk_acer_aspire_1520 = {
	.brightness = -1,
};

static const struct linuwu_sense_quirks quirk_acer_travelmate_2490 = {
	.mailled = 1,
};

static const struct linuwu_sense_quirks quirk_acer_predator_ph315_53 = {
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static const struct linuwu_sense_quirks quirk_acer_predator_phn16_71 = {
	.cpu_fans = 1,
	.gpu_fans = 1,
	.predator_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_predator_phn16_72 = {
	.predator_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_an16_41 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_an16_42 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_anv16_41 = {
	.nitro_v4 = 1,
	.four_zone_kb = 0,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_an16_43 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_legacy = {
	.nitro_sense = 2,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_an515_58 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static const struct linuwu_sense_quirks quirk_acer_nitro = {
	.nitro_sense = 1,
};

static const struct linuwu_sense_quirks quirk_acer_predator_v4 = {
	.predator_v4 = 1,
};

/* This AMW0 laptop has no bluetooth */
static const struct linuwu_sense_quirks quirk_medion_md_98300 = {
	.wireless = 1,
};

static const struct linuwu_sense_quirks quirk_fujitsu_amilo_li_1718 = {
	.wireless = 2,
};

static const struct linuwu_sense_quirks quirk_lenovo_ideapad_s205 = {
	.wireless = 3,
};

static const struct linuwu_sense_quirks quirk_acer_nitro_v4 = {
	.nitro_v4 = 1,
};

/*
 * These models advertise capabilities which do not match the hardware, so the
 * capability bitmap has to be forced instead of trusting the firmware.
 */
static const struct linuwu_sense_quirks quirk_acer_switch_10e = {
	.force_caps = ACER_CAP_KBD_DOCK,
};

static const struct linuwu_sense_quirks quirk_acer_switch_10 = {
	.force_caps = ACER_CAP_KBD_DOCK,
};

static const struct linuwu_sense_quirks quirk_acer_switch_v10_017 = {
	.force_caps = ACER_CAP_KBD_DOCK,
};

static const struct linuwu_sense_quirks quirk_acer_one_10_s1003 = {
	.force_caps = ACER_CAP_KBD_DOCK,
};

/* The Aspire One has a dummy ACPI-WMI interface - disable it */
static const struct dmi_system_id acer_blacklist[] __initconst = {
	{
		.ident = "Acer Aspire One (SSD)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AOA110"),
		},
	},
	{
		.ident = "Acer Aspire One (HDD)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AOA150"),
		},
	},
	{}
};

static const struct dmi_system_id amw0_whitelist[] = {
	{
		.ident = "Acer",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
		},
	},
	{
		.ident = "Gateway",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Gateway"),
		},
	},
	{
		.ident = "Packard Bell",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Packard Bell"),
		},
	},
	{}
};

/*
 * This quirk table is only for Acer/Gateway/Packard Bell family
 * that those machines are supported by acer-wmi driver.
 */
static const struct dmi_system_id acer_quirks[] = {
	{
		.ident = "Acer Nitro AN16-43",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-43"),
		},
		.driver_data = (void *)&quirk_acer_nitro_an16_43,
	},
	{
		.ident = "Acer Nitro AN16-42",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-42"),
		},
		.driver_data = (void *)&quirk_acer_nitro_an16_42,
	},
	{
		.ident = "Acer Nitro AN515-58",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN515-58"),
		},
		.driver_data = (void *)&quirk_acer_nitro_an515_58,
	},
	{
		.ident = "Acer Nitro AN16-41",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-41"),
		},
		.driver_data = (void *)&quirk_acer_nitro_an16_41,
	},
	{
		.ident = "Acer Nitro ANV16-41",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV16-41"),
		},
		.driver_data = (void *)&quirk_acer_nitro_anv16_41,
	},
	{
		.ident = "Acer Nitro ANV15-41",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV15-41"),
		},
		.driver_data = (void *)&quirk_acer_nitro,
	},
	{
		.ident = "Acer Nitro ANV15-51",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV15-51"),
		},
		.driver_data = (void *)&quirk_acer_nitro,
	},
	{
		.ident = "Acer Nitro AN515-55",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN515-55"),
		},
		.driver_data = (void *)&quirk_acer_nitro_legacy,
	},
	{
		.ident = "Acer Aspire 1360",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1360"),
		},
		.driver_data = (void *)&quirk_acer_aspire_1520,
	},
	{
		.ident = "Acer Aspire 1520",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1520"),
		},
		.driver_data = (void *)&quirk_acer_aspire_1520,
	},
	{
		.ident = "Acer Aspire 3100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3100"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 3610",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3610"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 5100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5100"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 5610",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5610"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 5630",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5630"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 5650",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5650"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 5680",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5680"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Aspire 9110",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 9110"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer TravelMate 2490",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 2490"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer TravelMate 4200",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 4200"),
		},
		.driver_data = (void *)&quirk_acer_travelmate_2490,
	},
	{
		.ident = "Acer Predator PH315-53",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-53"),
		},
		.driver_data = (void *)&quirk_acer_predator_ph315_53,
	},
	{
		.ident = "Acer Predator PHN16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PHN16-71"),
		},
		.driver_data = (void *)&quirk_acer_predator_phn16_71,
	},
	{
		.ident = "Acer Predator PHN16-72",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PHN16-72"),
		},
		.driver_data = (void *)&quirk_acer_predator_phn16_72,
	},
	{
		.ident = "Acer Predator PH16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH16-71"),
		},
		.driver_data = (void *)&quirk_acer_predator_v4,
	},
	{
		.ident = "Acer Predator PH18-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH18-71"),
		},
		.driver_data = (void *)&quirk_acer_predator_v4,
	},
	{
		.ident = "Acer Predator PTX17-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PTX17-71"),
		},
		.driver_data = (void *)&quirk_acer_predator_v4,
	},
	{
		.ident = "Acer Aspire Switch 10E SW3-016",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW3-016"),
		},
		.driver_data = (void *)&quirk_acer_switch_10e,
	},
	{
		.ident = "Acer Aspire Switch 10 SW5-012",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW5-012"),
		},
		.driver_data = (void *)&quirk_acer_switch_10,
	},
	{
		.ident = "Acer Aspire Switch V 10 SW5-017",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SW5-017"),
		},
		.driver_data = (void *)&quirk_acer_switch_v10_017,
	},
	{
		.ident = "Acer One 10 (S1003)",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "One S1003"),
		},
		.driver_data = (void *)&quirk_acer_one_10_s1003,
	},
	{}
};

/*
 * This quirk list is for those non-acer machines that have AMW0_GUID1
 * but supported by acer-wmi in past days. Keeping this quirk list here
 * is only for backward compatible. Please do not add new machine to
 * here anymore. Those non-acer machines should be supported by
 * appropriate wmi drivers.
 */
static const struct dmi_system_id non_acer_quirks[] = {
	{
		.ident = "Fujitsu Siemens Amilo Li 1718",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AMILO Li 1718"),
		},
		.driver_data = (void *)&quirk_fujitsu_amilo_li_1718,
	},
	{
		.ident = "Medion MD 98300",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "WAM2030"),
		},
		.driver_data = (void *)&quirk_medion_md_98300,
	},
	{
		.ident = "Lenovo Ideapad S205",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "10382LG"),
		},
		.driver_data = (void *)&quirk_lenovo_ideapad_s205,
	},
	{
		.ident = "Lenovo Ideapad S205 (Brazos)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Brazos"),
		},
		.driver_data = (void *)&quirk_lenovo_ideapad_s205,
	},
	{
		.ident = "Lenovo 3000 N200",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "0687A31"),
		},
		.driver_data = (void *)&quirk_fujitsu_amilo_li_1718,
	},
	{
		.ident = "Lenovo Ideapad S205-10382JG",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "10382JG"),
		},
		.driver_data = (void *)&quirk_lenovo_ideapad_s205,
	},
	{
		.ident = "Lenovo Ideapad S205-1038DPG",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "1038DPG"),
		},
		.driver_data = (void *)&quirk_lenovo_ideapad_s205,
	},
	{}
};

/*
 * Return the quirk entry of the first matching DMI entry. A capability
 * override does not select a quirk entry. Its effective value is queried
 * separately so DMI matching remains free of side effects.
 */
static const struct linuwu_sense_quirks *
linuwu_sense_quirks_from_table(const struct dmi_system_id *table)
{
	const struct dmi_system_id *entry;
	const struct linuwu_sense_quirks *quirks;

	entry = dmi_first_match(table);
	if (!entry)
		return NULL;

	quirks = entry->driver_data;

	/* Keep the historical behavior: this model uses only force_caps. */
	if (quirks->force_caps)
		return NULL;

	return quirks;
}

bool __init linuwu_sense_quirks_blacklisted(void)
{
	return !!dmi_first_match(acer_blacklist);
}

bool linuwu_sense_quirks_amw0_whitelisted(void)
{
	return !!dmi_first_match(amw0_whitelist);
}

bool linuwu_sense_quirks_is_unknown(const struct linuwu_sense_quirks *quirks)
{
	return quirks == &quirk_unknown;
}

const struct linuwu_sense_quirks *linuwu_sense_quirks_match(void)
{
	const struct linuwu_sense_quirks *quirks = NULL;

	if (predator_v4) {
		quirks = &quirk_acer_predator_v4;
	} else if (nitro_v4) {
		quirks = &quirk_acer_nitro_v4;
	} else if (!force_series) {
		quirks = linuwu_sense_quirks_from_table(acer_quirks);
		if (!quirks)
			quirks =
				linuwu_sense_quirks_from_table(non_acer_quirks);
	} else if (force_series == 2490) {
		quirks = &quirk_acer_travelmate_2490;
	}

	if (!quirks)
		quirks = &quirk_unknown;

	return quirks;
}

static int linuwu_sense_quirks_dmi_force_caps(const struct dmi_system_id *table)
{
	const struct dmi_system_id *entry;
	const struct linuwu_sense_quirks *quirks;

	entry = dmi_first_match(table);
	if (!entry)
		return -1;

	quirks = entry->driver_data;
	if (!quirks->force_caps)
		return -1;

	return quirks->force_caps;
}

int linuwu_sense_quirks_force_caps(void)
{
	int caps;

	/* An explicit module parameter always takes precedence. */
	if (force_caps >= 0)
		return force_caps;

	/* Preserve the original DMI table priority and first-match behavior. */
	caps = linuwu_sense_quirks_dmi_force_caps(acer_quirks);
	if (caps >= 0)
		return caps;

	return linuwu_sense_quirks_dmi_force_caps(non_acer_quirks);
}

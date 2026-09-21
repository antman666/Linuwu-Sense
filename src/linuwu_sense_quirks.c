// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Model quirks and DMI matching for the Acer Predator/Nitro WMI driver.
 *
 *  Copyright (C) 2007-2009	Carlos Corbacho <carlos@strangeworlds.co.uk>
 */

#include <linux/dmi.h>
#include <linux/types.h>

#include "linuwu_sense_quirks.h"

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

/*
 * This quirk table only contains the supported Acer Predator/Nitro models.
 * Every entry matches the exact system vendor and product name, there is no
 * catch-all match for the Acer vendor.
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
	{}
};

const struct linuwu_sense_quirks *linuwu_sense_quirks_match(void)
{
	const struct dmi_system_id *entry;

	entry = dmi_first_match(acer_quirks);
	if (!entry)
		return NULL;

	return entry->driver_data;
}

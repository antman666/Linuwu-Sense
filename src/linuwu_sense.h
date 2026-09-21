/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Acer Predator/Nitro WMI Laptop Extras
 *
 *  Copyright (C) 2007-2009	Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  Based on acer_acpi:
 *    Copyright (C) 2005-2007	E.M. Smith
 *    Copyright (C) 2007-2008	Carlos Corbacho <cathectic@gmail.com>
 */
#ifndef _LINUWU_SENSE_H_
#define _LINUWU_SENSE_H_

#include <linux/bits.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct device;
struct input_dev;
struct linuwu_sense_quirks;
struct platform_device;
struct wmi_device;

/*
 * Interface capability flags
 */
#define ACER_CAP_SET_FUNCTION_MODE BIT(5)
#define ACER_CAP_KBD_DOCK BIT(6)
#define ACER_CAP_TURBO_FAN BIT(9)
#define ACER_CAP_PLATFORM_PROFILE BIT(10)
#define ACER_CAP_FAN_SPEED_READ BIT(11)
#define ACER_CAP_PREDATOR_SENSE BIT(12)
#define ACER_CAP_NITRO_SENSE BIT(13)
#define ACER_CAP_NITRO_SENSE_V4 BIT(14)

/*
 * The WMI devices used by this driver. Acer firmware spreads the different
 * parts of the Predator/Nitro interface over several WMI devices, which are
 * all children of the same WMI bus device.
 */
enum acer_wmi_guid {
	ACER_WMI_GUID_WMID_APGE, /* WMID_GUID3 */
	ACER_WMI_GUID_WMID_GAMING, /* WMID_GUID4 */
	ACER_WMI_GUID_WMID_BATTERY, /* WMID_GUID5 */
	ACER_WMI_GUID_EVENT, /* ACERWMID_EVENT_GUID */
	ACER_WMI_GUID_COUNT,
};

struct per_zone_color {
	u64 zone1, zone2, zone3, zone4;
	int brightness;
} __packed;

struct kb_state {
	u8 per_zone;
	u8 mode;
	u8 speed;
	u8 brightness;
	u8 direction;
	u8 red;
	u8 green;
	u8 blue;
	struct per_zone_color zones;
} __packed;

/*
 * Per physical device driver state. This structure is shared by all WMI
 * devices belonging to the same WMI bus device. It is allocated by devres on
 * the WMI bus device, so the memory outlives every WMI device and is released
 * only when the WMI bus device itself goes away. The platform device keeps a
 * pointer to it via platform_set_drvdata().
 */
struct acer_wmi {
	struct device *dev; /* the platform device */
	struct platform_device *pdev;
	struct device *parent; /* the WMI bus device */
	struct list_head node;
	struct list_head wdev_list;

	struct wmi_device *wdevs[ACER_WMI_GUID_COUNT];

	/*
	 * Number of WMI devices of this instance that are currently bound to
	 * the driver, plus the instance setup state. All three fields are
	 * protected by acer_wmi_instances_lock.
	 */
	unsigned int wdev_count;
	bool setup_done;
	/* Last setup attempt failed; cleared when a new WMI probe joins. */
	bool setup_failed;

	/* The capabilities this interface provides */
	u32 capability;

	/* The model specific quirks which matched this machine */
	const struct linuwu_sense_quirks *quirks;

	/*
	 * Protects the cached fan speed pair, keyboard state, power-source
	 * state and thermal profile shared by the sysfs and WMI event paths.
	 * May be held across ACPI/WMI operations, so it must stay a mutex.
	 */
	struct mutex lock;

	/*
	 * Serializes WMI event processing and prevents it from running while
	 * the platform device is being torn down. Must be a mutex because the
	 * notify path may sleep.
	 */
	struct mutex event_lock;
	bool ready;

	struct input_dev *input_dev;

	struct device *platform_profile_dev;
	bool platform_profile_support;

	bool on_ac;
	u8 thermal_profile;

	int cpu_fan_speed;
	int gpu_fan_speed;
	struct kb_state current_kb_state;
};

#endif /* _LINUWU_SENSE_H_ */

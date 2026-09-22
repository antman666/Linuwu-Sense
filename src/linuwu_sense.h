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
struct linuwu_sense_input;
struct linuwu_sense_keyboard;
struct linuwu_sense_profile;
struct linuwu_sense_quirks;
struct platform_device;
struct wmi_device;

/*
 * Interface capability flags
 */
#define ACER_CAP_SET_FUNCTION_MODE BIT(5)
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

/*
 * Per physical device driver state. This structure is shared by all WMI
 * devices belonging to the same WMI bus device. It is allocated by the first
 * WMI device probe of an instance and freed once the last WMI device of that
 * instance has been removed, or at module exit. It therefore outlives every
 * individual WMI device of the instance.
 *
 * The driver also owns one logical platform device per instance
 * ("linuwu-sense"), which hosts the userspace control surface and the
 * devres-managed subsystem devices (input, hwmon, platform profile). The
 * platform device keeps a pointer to this structure via
 * platform_set_drvdata().
 */
struct acer_wmi {
	struct device *dev; /* the platform device */
	struct platform_device *pdev;
	struct device *parent; /* the WMI bus device */
	struct list_head node;
	struct list_head wdev_list;

	struct wmi_device *wdevs[ACER_WMI_GUID_COUNT];

	/*
	 * Opaque state owned by the Linux subsystem frontends. The core
	 * driver only stores the pointers, the component that allocated the
	 * state is responsible for its content.
	 */
	struct linuwu_sense_input *input;
	struct linuwu_sense_keyboard *keyboard;
	struct linuwu_sense_profile *profile;

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
	 * Protects the cached fan speed pair and serializes the WMI
	 * operations issued by the control surface, the WMI event path and
	 * the subsystem frontends. May be held across ACPI/WMI operations,
	 * so it must stay a mutex.
	 */
	struct mutex lock;

	/*
	 * Serializes WMI event processing and prevents it from running while
	 * the platform device is being torn down. Must be a mutex because the
	 * notify path may sleep.
	 */
	struct mutex event_lock;
	bool ready;

	int cpu_fan_speed;
	int gpu_fan_speed;
};

#endif /* _LINUWU_SENSE_H_ */

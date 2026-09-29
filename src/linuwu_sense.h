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
 * Aggregate setup state of one WMI bus device.
 *
 * COLLECTING: the driver instance exists, but not every expected endpoint
 * probe has run yet.
 * ACTIVE: the complete endpoint set was set up and the logical platform
 * device with all its frontends is published.
 * FAILED: the endpoint set or the logical device setup failed. Terminal for
 * this instance: all WMI devices of the WMI bus device were already registered
 * when probing started, so no later probe can repair the device set.
 *
 * There is no separate SETTING_UP state because the setup runs synchronously
 * under acer_wmi_instances_lock: the state only changes once it returned.
 * There is no TEARING_DOWN state either because the logical device is
 * unregistered synchronously before the instance is released.
 */
enum acer_wmi_state {
	ACER_WMI_COLLECTING,
	ACER_WMI_ACTIVE,
	ACER_WMI_FAILED,
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

	/*
	 * The WMI device that claimed each Acer GUID of this instance. A GUID
	 * is claimed by exactly one WMI device; the first device to bind wins
	 * and further devices with the same GUID are refused. A NULL entry
	 * means that the endpoint is currently not usable. Protected by
	 * @lock.
	 */
	struct wmi_device *wdevs[ACER_WMI_GUID_COUNT];

	/*
	 * Opaque state owned by the Linux subsystem frontends. The pointers
	 * are published by the logical platform device probe before @ready is
	 * set and released by its removal after @ready was cleared, so the WMI
	 * event path may use them under @event_lock while the subsystem
	 * frontends use them under @lock. The core driver only stores the
	 * pointers, the component that allocated the state is responsible for
	 * its content.
	 */
	struct linuwu_sense_input *input;
	struct linuwu_sense_keyboard *keyboard;
	struct linuwu_sense_profile *profile;

	/*
	 * Number of distinct Acer GUIDs exposed by the WMI bus device this
	 * instance belongs to. This is the number of endpoints that have to be
	 * claimed before the instance can be set up. Captured when the
	 * instance is created. Protected by acer_wmi_instances_lock.
	 */
	unsigned int wdev_expected;

	/*
	 * Number of matching WMI devices below the WMI bus device when the
	 * instance was created, and the number of probe callbacks of this
	 * instance seen so far. Once every expected probe has run, no later
	 * probe can complete the endpoint set. Protected by
	 * acer_wmi_instances_lock.
	 */
	unsigned int probes_expected;
	unsigned int probes_seen;

	/*
	 * Number of endpoints of this instance that are currently claimed.
	 * Protected by acer_wmi_instances_lock, each claim and release
	 * additionally by @lock.
	 */
	unsigned int wdev_count;

	/*
	 * Aggregate setup state. Runs from COLLECTING to ACTIVE when the
	 * complete endpoint set was set up, or to FAILED when the endpoint
	 * set or the setup of the logical device failed. FAILED is terminal:
	 * once every expected probe has run no later probe can repair the set.
	 * Protected by acer_wmi_instances_lock.
	 */
	enum acer_wmi_state state;

	/*
	 * Set by the logical platform device probe once all frontends were
	 * published. Read by the setup path directly after
	 * platform_device_add() triggered the probe.
	 */
	bool platform_probe_ok;

	/* The capabilities this interface provides */
	u32 capability;

	/* The model specific quirks which matched this machine */
	const struct linuwu_sense_quirks *quirks;

	/*
	 * Protects the endpoint claims in @wdevs[] and the cached fan speed
	 * pair, and serializes the Acer WMI method invocations issued by the
	 * control surface, the WMI event path and the subsystem frontends.
	 * May be held across ACPI/WMI operations, so it must stay a mutex.
	 *
	 * Lock order: acer_wmi_instances_lock -> @event_lock -> @lock.
	 */
	struct mutex lock;

	/*
	 * Serializes the WMI event path and protects @ready. The event path
	 * acquires it before @lock, so it must never be taken while @lock is
	 * held. Must be a mutex because the notify path may sleep.
	 */
	struct mutex event_lock;
	bool ready;

	int cpu_fan_speed;
	int gpu_fan_speed;
};

/*
 * Resolve the WMI device that claimed @guid for @acer. Returns NULL when the
 * endpoint is not usable right now, either because the device never bound or
 * because it was removed again.
 *
 * The caller must hold acer->lock for the whole resolve and use sequence, so
 * that the returned device cannot be removed while it is in use.
 */
struct wmi_device *linuwu_sense_endpoint_get(struct acer_wmi *acer,
					     enum acer_wmi_guid guid);

#endif /* _LINUWU_SENSE_H_ */

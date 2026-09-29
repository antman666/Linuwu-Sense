// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Battery and USB charging WMI commands for the Acer Predator/Nitro driver.
 *
 *  This module implements the battery health control commands of the WMID
 *  battery device and the USB charging commands of the WMID APGE device. It
 *  keeps no driver state, the firmware is the only owner of the battery
 *  state.
 *
 *  The battery charge limit is exposed through the power_supply extension API
 *  of the ACPI battery: this driver extends the battery power supply owned by
 *  the ACPI battery driver instead of registering a competing battery device.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <acpi/battery.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "linuwu_sense.h"
#include "linuwu_sense_battery.h"

/*
 * Method IDs of the WMID APGE device
 */
#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

/*
 * Method IDs of the WMID battery device
 */
#define ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID 20
#define ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID 21

struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

/*
 * Decode the result of one of the APGE function commands. Some of the
 * commands are only answered with a u32 value while others return a full u64
 * value.
 */
static int linuwu_sense_battery_decode_result(const struct wmi_buffer *output,
					      u64 *result)
{
	if (output->length >= sizeof(*result))
		*result = get_unaligned_le64(output->data);
	else if (output->length >= sizeof(u32))
		*result = get_unaligned_le32(output->data);
	else
		return -EIO;

	return 0;
}

int linuwu_sense_battery_get_usb_charging(struct wmi_device *wdev, int *percent)
{
	u64 input_value = 0x4;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, ACER_WMID_GET_FUNCTION, &input,
				   &output, sizeof(u32));
	if (err) {
		pr_err("Error getting usb charging status: %d\n", err);
		return err;
	}

	err = linuwu_sense_battery_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error getting usb charging status: %d\n", err);
		return err;
	}

	pr_info("usb charging get status: %llu\n", result);
	/* -1 means unknown value */
	*percent = result == 663296  ? 0 :
		   result == 659200  ? 10 :
		   result == 1314560 ? 20 :
		   result == 1969920 ? 30 :
				       -1;
	return 0;
}

int linuwu_sense_battery_set_usb_charging(struct wmi_device *wdev, u8 percent)
{
	u64 input_value;
	struct wmi_buffer input = {
		.length = sizeof(input_value),
		.data = &input_value,
	};
	struct wmi_buffer output = {};
	u64 result;
	int err;

	if (!wdev)
		return -ENODEV;

	pr_info("usb charging set value: %d\n", percent);
	/* If the value is unknown then turn it off */
	input_value = percent == 0  ? 663300 :
		      percent == 10 ? 659204 :
		      percent == 20 ? 1314564 :
		      percent == 30 ? 1969924 :
				      663300;
	err = wmidev_invoke_method(wdev, 0, ACER_WMID_SET_FUNCTION, &input,
				   &output, sizeof(u32));
	if (err) {
		pr_err("Error setting usb charging status: %d\n", err);
		return err;
	}

	err = linuwu_sense_battery_decode_result(&output, &result);
	kfree(output.data);
	if (err) {
		pr_err("Error setting usb charging status: %d\n", err);
		return err;
	}

	pr_info("usb charging set status: %llu\n", result);
	return 0;
}

int linuwu_sense_battery_get_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  int *enabled)
{
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = 0x1,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct get_battery_health_control_status_output ret;
	struct wmi_buffer input = {
		.length = sizeof(params),
		.data = &params,
	};
	struct wmi_buffer output = {};
	int err;

	pr_info("battery health query: %d\n", mode);

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
		&input, &output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output getting battery health status: %d\n",
		       err);
		goto out;
	}

	if (output.length < sizeof(ret)) {
		err = -EIO;
		goto out;
	}
	memcpy(&ret, output.data, sizeof(ret));

	switch (mode) {
	case LINUWU_SENSE_BATTERY_MODE_HEALTH:
		*enabled = ret.uFunctionStatus[0];
		break;
	case LINUWU_SENSE_BATTERY_MODE_CALIBRATION:
		*enabled = ret.uFunctionStatus[1];
		break;
	default:
		err = -EINVAL;
		break;
	}

out:
	kfree(output.data);
	return err;
}

int linuwu_sense_battery_set_mode(struct wmi_device *wdev,
				  enum linuwu_sense_battery_mode mode,
				  u8 status)
{
	struct set_battery_health_control_input params = {
		.uBatteryNo = 0x1,
		.uFunctionMask = mode,
		.uFunctionStatus = status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct set_battery_health_control_output ret;
	struct wmi_buffer input = {
		.length = sizeof(params),
		.data = &params,
	};
	struct wmi_buffer output = {};
	int err;

	pr_info("%s: %d | %d\n", __func__, mode, status);

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID, &input,
		&output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output setting battery health status: %d\n",
		       err);
		goto out;
	}

	if (output.length < sizeof(ret)) {
		err = -EIO;
		goto out;
	}
	memcpy(&ret, output.data, sizeof(ret));

	if (ret.uReturn != 0 && ret.uReservedOut != 0) {
		pr_err("Failed to set battery health status\n");
		err = -EIO;
		goto out;
	}

out:
	kfree(output.data);
	return err;
}

/*
 * Charge limit extension for the ACPI battery power supply.
 *
 * The firmware only knows two states: limit the charge to 80 percent or do
 * not limit it. They are mapped onto the standard charge_control_end_threshold
 * property, which the power_supply core exposes on the existing battery
 * device.
 */

/* The fixed threshold the firmware limit mode corresponds to. */
#define ACER_BATTERY_LIMIT_THRESHOLD 80
#define ACER_BATTERY_FULL_THRESHOLD 100

struct linuwu_sense_battery_ext {
	struct acpi_battery_hook hook;
	struct acer_wmi *acer;
	struct list_head batteries;
	/* Protects the list of batteries this extension was added to. */
	struct mutex lock;
};

struct linuwu_sense_battery_entry {
	struct list_head node;
	struct power_supply *battery;
};

static int acer_battery_ext_get_property(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *data,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct linuwu_sense_battery_ext *battery = data;
	struct acer_wmi *acer = battery->acer;
	int enabled;
	int ret;

	if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
		return -EINVAL;

	mutex_lock(&acer->lock);
	ret = linuwu_sense_battery_get_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_HEALTH, &enabled);
	mutex_unlock(&acer->lock);
	if (ret)
		return ret;

	val->intval = enabled ? ACER_BATTERY_LIMIT_THRESHOLD :
				ACER_BATTERY_FULL_THRESHOLD;
	return 0;
}

static int acer_battery_ext_set_property(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *data,
					 enum power_supply_property psp,
					 const union power_supply_propval *val)
{
	struct linuwu_sense_battery_ext *battery = data;
	struct acer_wmi *acer = battery->acer;
	u8 status;
	int ret;

	if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
		return -EINVAL;

	if (val->intval < 1 || val->intval > 100)
		return -EINVAL;

	/*
	 * The firmware cannot set an arbitrary threshold, so round the value
	 * to the nearest one it supports: 80 percent or no limit.
	 */
	status = val->intval > 90 ? 0 : 1;

	mutex_lock(&acer->lock);
	ret = linuwu_sense_battery_set_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_HEALTH, status);
	mutex_unlock(&acer->lock);

	return ret;
}

static int acer_battery_ext_property_is_writeable(
	struct power_supply *psy, const struct power_supply_ext *ext,
	void *data, enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static const enum power_supply_property acer_battery_ext_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext acer_battery_ext = {
	.name = "linuwu-sense",
	.properties = acer_battery_ext_properties,
	.num_properties = ARRAY_SIZE(acer_battery_ext_properties),
	.get_property = acer_battery_ext_get_property,
	.set_property = acer_battery_ext_set_property,
	.property_is_writeable = acer_battery_ext_property_is_writeable,
};

static int acer_battery_add(struct power_supply *psy,
			    struct acpi_battery_hook *hook)
{
	struct linuwu_sense_battery_ext *battery =
		container_of(hook, struct linuwu_sense_battery_ext, hook);
	struct linuwu_sense_battery_entry *entry;
	int ret;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;

	ret = power_supply_register_extension(psy, &acer_battery_ext,
					      battery->acer->dev, battery);
	if (ret) {
		kfree(entry);

		/*
		 * Another driver already provides the charge control for this
		 * battery. Skip the battery but keep the hook for the others.
		 */
		if (ret == -EEXIST) {
			dev_warn(&psy->dev,
				 "charge control already provided, skipping\n");
			return 0;
		}

		return ret;
	}

	entry->battery = psy;

	mutex_lock(&battery->lock);
	list_add(&entry->node, &battery->batteries);
	mutex_unlock(&battery->lock);

	return 0;
}

static int acer_battery_remove(struct power_supply *psy,
			       struct acpi_battery_hook *hook)
{
	struct linuwu_sense_battery_ext *battery =
		container_of(hook, struct linuwu_sense_battery_ext, hook);
	struct linuwu_sense_battery_entry *entry, *tmp;
	bool extended = false;

	mutex_lock(&battery->lock);
	list_for_each_entry_safe(entry, tmp, &battery->batteries, node) {
		if (entry->battery != psy)
			continue;

		list_del(&entry->node);
		kfree(entry);
		extended = true;
		break;
	}
	mutex_unlock(&battery->lock);

	if (extended)
		power_supply_unregister_extension(psy, &acer_battery_ext);

	return 0;
}

int linuwu_sense_battery_init(struct acer_wmi *acer)
{
	struct linuwu_sense_battery_ext *battery;
	int err;

	if (!(acer->capability &
	      (ACER_CAP_PREDATOR_SENSE | ACER_CAP_NITRO_SENSE |
	       ACER_CAP_NITRO_SENSE_V4)))
		return 0;

	battery = devm_kzalloc(acer->dev, sizeof(*battery), GFP_KERNEL);
	if (!battery)
		return -ENOMEM;

	battery->acer = acer;
	battery->hook.name = "Linuwu-Sense Battery Charge Limit";
	battery->hook.add_battery = acer_battery_add;
	battery->hook.remove_battery = acer_battery_remove;
	INIT_LIST_HEAD(&battery->batteries);
	INIT_LIST_HEAD(&battery->hook.list);
	mutex_init(&battery->lock);

	err = devm_battery_hook_register(acer->dev, &battery->hook);
	if (err) {
		dev_err(acer->dev, "Failed to register battery hook: %d\n",
			err);
		return err;
	}

	return 0;
}

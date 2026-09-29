// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer WMI Laptop Extras
 *
 *  Copyright (C) 2007-2009	Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  Based on acer_acpi:
 *    Copyright (C) 2005-2007	E.M. Smith
 *    Copyright (C) 2007-2008	Carlos Corbacho <cathectic@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense.h"
#include "linuwu_sense_battery.h"
#include "linuwu_sense_event.h"
#include "linuwu_sense_fan.h"
#include "linuwu_sense_gaming.h"
#include "linuwu_sense_hwmon.h"
#include "linuwu_sense_input.h"
#include "linuwu_sense_keyboard.h"
#include "linuwu_sense_profile.h"
#include "linuwu_sense_quirks.h"

MODULE_AUTHOR("Carlos Corbacho");
MODULE_DESCRIPTION("Acer Laptop WMI Extras Driver");
MODULE_LICENSE("GPL");

/*
 * Acer ACPI method GUIDs used by the Predator/Nitro interface
 */
#define WMID_GUID3 "61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
#define WMID_GUID4 "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"
#define WMID_GUID5 "79772EC5-04B1-4bfd-843C-61E7F77B6CC9"

/*
 * Acer ACPI event GUIDs
 */
#define ACERWMID_EVENT_GUID "676AA15E-6A47-4D9F-A2CC-1E6D18D14026"

static bool ec_raw_mode;

module_param(ec_raw_mode, bool, 0444);
MODULE_PARM_DESC(ec_raw_mode, "Enable EC raw mode");

/*
 * Per WMI device state. Every matching WMI device that claimed its GUID gets
 * its own instance of this structure, bound to the WMI device via
 * dev_set_drvdata().
 */
struct acer_wmi_wdev {
	struct acer_wmi *acer;
	enum acer_wmi_guid guid;
};

/*
 * Registry of the driver states, one entry per WMI bus device. New entries
 * are created during WMI device probing and removed again once the last WMI
 * device of an instance is gone, or when the driver is unloaded.
 */
static LIST_HEAD(acer_wmi_instances);
static DEFINE_MUTEX(acer_wmi_instances_lock);
static bool acer_wmi_shutting_down;

/*
 * Model quirks of the running machine, selected once at module load. The
 * module refuses to initialize when no entry matches.
 */
static const struct linuwu_sense_quirks *acer_wmi_quirks;

static void set_quirks(struct acer_wmi *acer)
{
	const struct linuwu_sense_quirks *quirks = acer->quirks;

	if (quirks->cpu_fans || quirks->gpu_fans)
		acer->capability |= ACER_CAP_TURBO_FAN;

	/*
	 * Some acer nitro laptops don't have features like lcd override , boot
	 * animation sound so this is used. Think wisely before using any quirks
	 * validate your features.
	 */
	if (quirks->nitro_sense == 1) {
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE;
	} else if (quirks->nitro_sense == 2) {
		/*
		 * Platform Profile is not found on some older acer nitro
		 * models, so we exclude it
		 */
		acer->capability |= ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE;
	}

	if (quirks->predator_v4)
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_PREDATOR_SENSE;

	/* Includes all feature that predatorv4 have*/
	if (quirks->nitro_v4)
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE_V4;
}

/*
 * General interface convenience methods
 */

static bool has_cap(const struct acer_wmi *acer, u32 cap)
{
	return acer->capability & cap;
}

/*
 * Resolve the WMI device that claimed @guid. See linuwu_sense.h for the
 * locking contract.
 */
struct wmi_device *linuwu_sense_endpoint_get(struct acer_wmi *acer,
					     enum acer_wmi_guid guid)
{
	lockdep_assert_held(&acer->lock);

	return acer->wdevs[guid];
}

/* Fan Speed */
static int acer_set_fan_speed(struct acer_wmi *acer, int t_cpu_fan_speed,
			      int t_gpu_fan_speed);

static bool acer_turbo_fan_supported(struct acer_wmi *acer)
{
	return has_cap(acer, ACER_CAP_TURBO_FAN) &&
	       (acer->quirks->cpu_fans > 0 || acer->quirks->gpu_fans > 0);
}

static int acer_set_turbo_fan_mode_locked(struct acer_wmi *acer, bool turbo)
{
	struct wmi_device *wdev;
	int err;

	if (!acer_turbo_fan_supported(acer))
		return -EOPNOTSUPP;
	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev)
		return -ENODEV;

	err = linuwu_sense_fan_set_mode(wdev, acer->quirks->cpu_fans > 0,
					acer->quirks->gpu_fans > 0,
					turbo ? LINUWU_SENSE_FAN_MODE_TURBO :
						LINUWU_SENSE_FAN_MODE_AUTO);
	if (err)
		return err;

	return 0;
}

static void acer_wmi_notify(struct wmi_device *wdev,
			    const struct wmi_buffer *data)
{
	struct acer_wmi_wdev *wdev_data = dev_get_drvdata(&wdev->dev);
	struct acer_wmi *acer;
	struct linuwu_sense_event event;
	int err;

	if (!wdev_data || wdev_data->guid != ACER_WMI_GUID_EVENT)
		return;

	acer = wdev_data->acer;
	if (!acer)
		return;

	err = linuwu_sense_event_parse(data, &event);
	if (err) {
		pr_warn("Unknown buffer length %zu\n", data->length);
		return;
	}

	mutex_lock(&acer->event_lock);
	if (!acer->ready) {
		mutex_unlock(&acer->event_lock);
		return;
	}

	switch (event.type) {
	case LINUWU_SENSE_EVENT_HOTKEY:
		pr_info("device state: 0x%x\n", event.device_state);
		linuwu_sense_input_report_hotkey(acer, event.key_num,
						 event.device_state);
		break;
	case LINUWU_SENSE_EVENT_PROFILE_CYCLE:
		if (has_cap(acer, ACER_CAP_PLATFORM_PROFILE))
			linuwu_sense_profile_cycle(acer);
		break;
	case LINUWU_SENSE_EVENT_POWER_SOURCE:
		if (acer->quirks->predator_v4 &&
		    has_cap(acer, ACER_CAP_PLATFORM_PROFILE))
			linuwu_sense_profile_power_source_changed(acer,
								  event.on_ac);
		break;
	case LINUWU_SENSE_EVENT_BATTERY_BOOST:
		break;
	case LINUWU_SENSE_EVENT_CALIBRATION:
		if (has_cap(acer, ACER_CAP_PREDATOR_SENSE) ||
		    has_cap(acer, ACER_CAP_NITRO_SENSE) ||
		    has_cap(acer, ACER_CAP_NITRO_SENSE_V4)) {
			mutex_lock(&acer->lock);
			err = linuwu_sense_battery_set_mode(
				linuwu_sense_endpoint_get(
					acer, ACER_WMI_GUID_WMID_BATTERY),
				LINUWU_SENSE_BATTERY_MODE_CALIBRATION,
				event.enabled);
			mutex_unlock(&acer->lock);
			if (err)
				pr_err("Error changing calibration state\n");
		}
		break;
	default:
		pr_warn("Unknown event\n");
		break;
	}

	mutex_unlock(&acer->event_lock);
}

/*
 * USB Charging
 */
static ssize_t predator_usb_charging_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int percent;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_get_usb_charging(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE),
		&percent);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", percent);
}

static ssize_t predator_usb_charging_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 10) && (val != 20) && (val != 30))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_set_usb_charging(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE), val);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

/*
 * Battery Limit (80%)
 * Battery Calibration
 */
static ssize_t predator_battery_limit_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int enabled;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_get_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_HEALTH, &enabled);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t predator_battery_limit_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;

	if ((val != 0) && (val != 1))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_set_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_HEALTH, val);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

static ssize_t predator_battery_calibration_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int enabled;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_get_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_CALIBRATION, &enabled);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t preadtor_battery_calibration_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;

	if ((val != 0) && (val != 1))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_set_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_CALIBRATION, val);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

/*
 * FAN CONTROLS
 */
static int acer_set_fan_speed(struct acer_wmi *acer, int t_cpu_fan_speed,
			      int t_gpu_fan_speed)
{
	struct wmi_device *wdev =
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	int err;

	if (!wdev)
		return -ENODEV;

	if (t_cpu_fan_speed == 100 && t_gpu_fan_speed == 100) {
		pr_info("MAX FAN MODE!\n");
		err = linuwu_sense_fan_set_mode(wdev,
						acer->quirks->cpu_fans > 0,
						acer->quirks->gpu_fans > 0,
						LINUWU_SENSE_FAN_MODE_TURBO);
		if (err) {
			pr_err("Error setting fan speed status: %d\n", err);
			return err;
		}
	} else if (t_cpu_fan_speed == 0 && t_gpu_fan_speed == 0) {
		pr_info("AUTO FAN MODE!\n");
		err = linuwu_sense_fan_set_mode(wdev,
						acer->quirks->cpu_fans > 0,
						acer->quirks->gpu_fans > 0,
						LINUWU_SENSE_FAN_MODE_AUTO);
		if (err) {
			pr_err("Error setting fan speed status: %d\n", err);
			return err;
		}
	} else if (t_cpu_fan_speed <= 100 && t_gpu_fan_speed <= 100) {
		if (t_cpu_fan_speed == 0) {
			pr_info("CUSTOM FAN MODE (GPU)\n");
			err = linuwu_sense_fan_set_mode(
				wdev, acer->quirks->cpu_fans > 0, false,
				LINUWU_SENSE_FAN_MODE_AUTO);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_mode(
				wdev, false, acer->quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_GPU, t_gpu_fan_speed);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}
		} else if (t_gpu_fan_speed == 0) {
			pr_info("CUSTOM FAN MODE (CPU)\n");
			err = linuwu_sense_fan_set_mode(
				wdev, false, acer->quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_AUTO);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_mode(
				wdev, acer->quirks->cpu_fans > 0, false,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_CPU, t_cpu_fan_speed);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}
		} else {
			pr_info("CUSTOM FAN MODE (MIXED)!\n");
			err = linuwu_sense_fan_set_mode(
				wdev, acer->quirks->cpu_fans > 0,
				acer->quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_CPU, t_cpu_fan_speed);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}

			err = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_GPU, t_gpu_fan_speed);
			if (err) {
				pr_err("Error setting fan speed status: %d\n",
				       err);
				return err;
			}
		}
	} else {
		return -EIO;
	}

	acer->cpu_fan_speed = t_cpu_fan_speed;
	acer->gpu_fan_speed = t_gpu_fan_speed;
	pr_info("Fan speeds updated: CPU=%d, GPU=%d\n", acer->cpu_fan_speed,
		acer->gpu_fan_speed);

	return 0;
}

static ssize_t predator_fan_speed_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&acer->lock);
	ret = sysfs_emit(buf, "%d,%d\n", acer->cpu_fan_speed,
			 acer->gpu_fan_speed);
	mutex_unlock(&acer->lock);

	return ret;
}

static ssize_t predator_fan_speed_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int t_cpu_fan_speed, t_gpu_fan_speed;

	char input[9];
	char *token;
	char *input_ptr = input;
	ssize_t len;

	len = strscpy(input, buf, sizeof(input));
	if (len < 0)
		return len;

	if (len > 0 && input[len - 1] == '\n')
		input[len - 1] = '\0';

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_cpu_fan_speed) ||
	    t_cpu_fan_speed < 0 || t_cpu_fan_speed > 100) {
		pr_err("Invalid CPU speed value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_gpu_fan_speed) ||
	    t_gpu_fan_speed < 0 || t_gpu_fan_speed > 100) {
		pr_err("Invalid GPU speed value.\n");
		return -EINVAL;
	}

	int err;

	mutex_lock(&acer->lock);
	err = acer_set_fan_speed(acer, t_cpu_fan_speed, t_gpu_fan_speed);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

static ssize_t predator_turbo_mode_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct wmi_device *wdev;
	enum linuwu_sense_fan_mode mode;
	bool enabled = true;
	bool have_fan = false;
	int err;

	if (!acer_turbo_fan_supported(acer))
		return -EOPNOTSUPP;

	mutex_lock(&acer->lock);
	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev) {
		err = -ENODEV;
		goto out;
	}

	if (acer->quirks->cpu_fans > 0) {
		err = linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_CPU,
						&mode);
		if (err)
			goto out;
		have_fan = true;
		if (mode != LINUWU_SENSE_FAN_MODE_TURBO)
			enabled = false;
	}

	if (acer->quirks->gpu_fans > 0) {
		err = linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_GPU,
						&mode);
		if (err)
			goto out;
		have_fan = true;
		if (mode != LINUWU_SENSE_FAN_MODE_TURBO)
			enabled = false;
	}

	err = have_fan ? 0 : -EIO;
out:
	mutex_unlock(&acer->lock);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", enabled ? 1 : 0);
}

static ssize_t predator_turbo_mode_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (!acer_turbo_fan_supported(acer))
		return -EOPNOTSUPP;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = acer_set_turbo_fan_mode_locked(acer, val);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

	return count;
}
/*
 * LCD OVERRIDE CONTROLS
 */
static ssize_t predator_lcd_override_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_lcd_override(acer, &state);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", state);
}

static ssize_t predator_lcd_override_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_lcd_override(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

/*
 * BACKLIGHT 30 SEC TIMEOUT
 */

static ssize_t predator_backlight_timeout_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_backlight_timeout(acer, &state);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", state);
}

static ssize_t predator_backlight_timeout_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_backlight_timeout(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

/*
 * System Boot Animation & Sound
 */
static ssize_t predator_boot_animation_sound_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_boot_animation_sound(acer, &state);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", state);
}

static ssize_t
predator_boot_animation_sound_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_boot_animation_sound(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return count;
}

/*
 * predator sense attributes
 */
static struct device_attribute boot_animation_sound =
	__ATTR(boot_animation_sound, 0644, predator_boot_animation_sound_show,
	       predator_boot_animation_sound_store);
static struct device_attribute backlight_timeout =
	__ATTR(backlight_timeout, 0644, predator_backlight_timeout_show,
	       predator_backlight_timeout_store);
static struct device_attribute usb_charging =
	__ATTR(usb_charging, 0644, predator_usb_charging_show,
	       predator_usb_charging_store);
static struct device_attribute battery_calibration =
	__ATTR(battery_calibration, 0644, predator_battery_calibration_show,
	       preadtor_battery_calibration_store);
static struct device_attribute battery_limiter =
	__ATTR(battery_limiter, 0644, predator_battery_limit_show,
	       predator_battery_limit_store);
static struct device_attribute fan_speed = __ATTR(
	fan_speed, 0644, predator_fan_speed_show, predator_fan_speed_store);
static struct device_attribute turbo_mode = __ATTR(
	turbo_mode, 0644, predator_turbo_mode_show, predator_turbo_mode_store);
static struct device_attribute lcd_override =
	__ATTR(lcd_override, 0644, predator_lcd_override_show,
	       predator_lcd_override_store);
static struct attribute *predator_sense_attrs[] = { &lcd_override.attr,
						    &fan_speed.attr,
						    &turbo_mode.attr,
						    &battery_limiter.attr,
						    &battery_calibration.attr,
						    &usb_charging.attr,
						    &backlight_timeout.attr,
						    &boot_animation_sound.attr,
						    NULL };

static umode_t predator_sense_attr_is_visible(struct kobject *kobj,
					      struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct acer_wmi *acer = dev_get_drvdata(dev);

	if (attr == &turbo_mode.attr &&
	    (!acer || !acer_turbo_fan_supported(acer)))
		return 0;

	return attr->mode;
}

static struct attribute_group preadtor_sense_attr_group = {
	.name = "predator_sense",
	.attrs = predator_sense_attrs,
	.is_visible = predator_sense_attr_is_visible,
};

static struct attribute *nitro_sense_v4_attrs[] = {
	&lcd_override.attr,	    &fan_speed.attr,
	&battery_limiter.attr,	    &battery_calibration.attr,
	&usb_charging.attr,	    &backlight_timeout.attr,
	&boot_animation_sound.attr, NULL
};

static struct attribute_group nitro_sense_v4_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_v4_attrs
};

/* nitro sense attributes */
static struct attribute *nitro_sense_attrs[] = {
	&fan_speed.attr,    &battery_limiter.attr,   &battery_calibration.attr,
	&usb_charging.attr, &backlight_timeout.attr, NULL
};
static struct attribute_group nitro_sense_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_attrs
};

/*
 * Platform device
 */

static int acer_platform_probe(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);
	int err;

	if (!acer)
		return -ENODEV;

	err = linuwu_sense_input_init(acer);
	if (err) {
		pr_err("Unable to set up input device\n");
		return err;
	}

	if (has_cap(acer, ACER_CAP_PLATFORM_PROFILE)) {
		err = linuwu_sense_profile_init(acer);
		if (err)
			return err;
	}

	if (has_cap(acer, ACER_CAP_TURBO_FAN)) {
		mutex_lock(&acer->lock);
		err = acer_set_turbo_fan_mode_locked(acer, false);
		mutex_unlock(&acer->lock);
		if (err && err != -EOPNOTSUPP)
			pr_warn("Unable to initialize gaming fan mode: %d\n",
				err);
	}

	if (has_cap(acer, ACER_CAP_PREDATOR_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &preadtor_sense_attr_group);
		if (err)
			return err;
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE_V4)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_v4_attr_group);
		if (err)
			return err;
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_attr_group);
		if (err)
			return err;
	}

	err = linuwu_sense_keyboard_init(acer);
	if (err)
		return err;

	if (has_cap(acer, ACER_CAP_FAN_SPEED_READ)) {
		err = acer_wmi_hwmon_init(acer, acer->dev);
		if (err)
			return err;
	}

	mutex_lock(&acer->event_lock);
	acer->ready = true;
	mutex_unlock(&acer->event_lock);

	return 0;
}

static void acer_platform_remove(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);

	if (!acer)
		return;

	/*
	 * Stop the WMI notify path before any resource is released. The
	 * event_lock makes sure that no notify_new() callback is still
	 * running when the devres-managed resources are torn down below.
	 */
	mutex_lock(&acer->event_lock);
	acer->ready = false;
	mutex_unlock(&acer->event_lock);

	linuwu_sense_keyboard_save_state(acer);

	/*
	 * All remaining resources (input, hwmon, platform profile and sysfs
	 * groups) are devres-managed and are released by the driver core
	 * after this callback returned.
	 */
}

/*
 * The logical platform device and its driver are named after this driver
 * instead of "acer-wmi": the upstream acer-wmi driver registers a platform
 * driver of that name, and two drivers with the same name can never be
 * registered on the platform bus at the same time. The unique name keeps this
 * driver independent of the acer-wmi driver's load state.
 */
static struct platform_driver acer_platform_driver = {
	.driver = {
		.name = "linuwu-sense",
	},
	.probe = acer_platform_probe,
	.remove = acer_platform_remove,
};

/*
 * WMI device handling
 *
 * A WMI bus device of an Acer Predator/Nitro laptop exposes the Acer
 * interface as several WMI devices with different GUIDs. The driver needs one
 * device per GUID: the APGE, GAMING and BATTERY endpoints are required for
 * the aggregate to work at all, while the event endpoint only provides the
 * hotkey input frontend.
 *
 * The WMI core guarantees that all WMI devices of a WMI bus device are
 * registered before any of them is probed: wmi_add_device() adds a device link
 * from each WMI device to the platform device which owns the _WDG block, and
 * such a consumer is not probed before the supplier finished probing.
 *
 * An endpoint that disappears after the aggregate was set up does not tear
 * the aggregate down: the operations that need it fail with -ENODEV while the
 * remaining frontends keep working. The aggregate itself is only released
 * together with the last WMI device of its WMI bus device.
 */
struct acer_wmi_endpoint {
	enum acer_wmi_guid guid;
	const char *guid_string;
	bool required;
};

static const struct acer_wmi_endpoint acer_wmi_endpoints[ACER_WMI_GUID_COUNT] = {
	[ACER_WMI_GUID_WMID_APGE] = {
		.guid = ACER_WMI_GUID_WMID_APGE,
		.guid_string = WMID_GUID3,
		.required = true,
	},
	[ACER_WMI_GUID_WMID_GAMING] = {
		.guid = ACER_WMI_GUID_WMID_GAMING,
		.guid_string = WMID_GUID4,
		.required = true,
	},
	[ACER_WMI_GUID_WMID_BATTERY] = {
		.guid = ACER_WMI_GUID_WMID_BATTERY,
		.guid_string = WMID_GUID5,
		.required = true,
	},
	[ACER_WMI_GUID_EVENT] = {
		.guid = ACER_WMI_GUID_EVENT,
		.guid_string = ACERWMID_EVENT_GUID,
		.required = false,
	},
};

static const struct wmi_device_id acer_wmi_id_table[] = {
	{
		.guid_string = WMID_GUID3,
		.context = &acer_wmi_endpoints[ACER_WMI_GUID_WMID_APGE],
	},
	{
		.guid_string = WMID_GUID4,
		.context = &acer_wmi_endpoints[ACER_WMI_GUID_WMID_GAMING],
	},
	{
		.guid_string = WMID_GUID5,
		.context = &acer_wmi_endpoints[ACER_WMI_GUID_WMID_BATTERY],
	},
	{
		.guid_string = ACERWMID_EVENT_GUID,
		.context = &acer_wmi_endpoints[ACER_WMI_GUID_EVENT],
	},
	{}
};
MODULE_DEVICE_TABLE(wmi, acer_wmi_id_table);

static int acer_wmi_instance_setup(struct acer_wmi *acer);

/*
 * The Acer GUIDs exposed by one WMI bus device.
 */
struct acer_wmi_topology {
	/* Bitmap of the GUIDs exposed below the WMI bus device. */
	unsigned long guids;
	/* Number of distinct GUIDs in @guids. */
	unsigned int endpoints;
};

static int acer_wmi_topology_scan(struct device *dev, void *data)
{
	struct acer_wmi_topology *topo = data;
	const char *name = dev_name(dev);
	const struct wmi_device_id *id;

	for (id = acer_wmi_id_table; id->guid_string[0]; id++) {
		const struct acer_wmi_endpoint *endpoint = id->context;

		if (strncasecmp(name, id->guid_string, strlen(id->guid_string)))
			continue;

		if (!test_and_set_bit(endpoint->guid, &topo->guids))
			topo->endpoints++;

		break;
	}

	return 0;
}

static bool acer_wmi_topology_is_supported(struct device *dev,
					   const struct acer_wmi_topology *topo)
{
	enum acer_wmi_guid guid;

	for (guid = 0; guid < ACER_WMI_GUID_COUNT; guid++) {
		if (!acer_wmi_endpoints[guid].required ||
		    test_bit(guid, &topo->guids))
			continue;

		dev_warn(
			dev,
			"required Acer WMI interface %s is missing, refusing to bind\n",
			acer_wmi_endpoints[guid].guid_string);
		return false;
	}

	return true;
}

static struct acer_wmi *acer_wmi_instance_find(struct device *parent)
{
	struct acer_wmi *acer;

	lockdep_assert_held(&acer_wmi_instances_lock);

	list_for_each_entry(acer, &acer_wmi_instances, node)
		if (acer->parent == parent)
			return acer;

	return NULL;
}

static struct acer_wmi *acer_wmi_instance_create(struct device *parent,
						 unsigned int endpoints)
{
	struct acer_wmi *acer;

	lockdep_assert_held(&acer_wmi_instances_lock);

	/*
	 * The state is shared by all WMI devices of this WMI bus device and
	 * its lifetime is owned by the driver instance, not by any of those
	 * devices: it is freed once the last WMI device of this instance has
	 * been removed (or at module exit). The WMI core guarantees that no
	 * notify callback can run before or after the driver's remove
	 * callbacks, and the platform device is unregistered synchronously
	 * before the state is freed.
	 */
	acer = kzalloc_obj(struct acer_wmi);
	if (!acer)
		return NULL;

	acer->parent = parent;
	acer->quirks = acer_wmi_quirks;
	acer->wdev_expected = endpoints;
	mutex_init(&acer->lock);
	mutex_init(&acer->event_lock);
	INIT_LIST_HEAD(&acer->node);
	list_add_tail(&acer->node, &acer_wmi_instances);

	return acer;
}

/*
 * Claim the endpoint @guid for @wdev. A GUID identifies a WMI device type,
 * not a unique device: the WMI bus allows several WMI devices to share a GUID
 * and the WMI core only makes a driver with no_singleton bind all of them.
 * This driver uses one device per GUID, so the first device to bind claims
 * the GUID and every further device of the same GUID is refused.
 *
 * The caller must hold acer->lock, so that the claim is serialized with every
 * lookup of acer->wdevs[].
 */
static bool acer_wmi_endpoint_claim(struct acer_wmi *acer,
				    enum acer_wmi_guid guid,
				    struct wmi_device *wdev)
{
	lockdep_assert_held(&acer->lock);

	if (acer->wdevs[guid])
		return false;

	acer->wdevs[guid] = wdev;
	acer->wdev_count++;

	return true;
}

/*
 * Release a claim. The claim is never handed over to another WMI device with
 * the same GUID: a replacement would silently change the device used for the
 * firmware method invocations. The caller must hold acer->lock.
 */
static void acer_wmi_endpoint_release(struct acer_wmi *acer,
				      enum acer_wmi_guid guid,
				      struct wmi_device *wdev)
{
	lockdep_assert_held(&acer->lock);

	if (acer->wdevs[guid] != wdev)
		return;

	acer->wdevs[guid] = NULL;
	acer->wdev_count--;
}

/*
 * Remove the platform-facing interfaces of an instance while keeping the
 * shared state allocated: the WMI devices of the instance still reference it
 * through wdev_data->acer.
 */
static void acer_wmi_instance_deactivate(struct acer_wmi *acer)
{
	lockdep_assert_held(&acer_wmi_instances_lock);

	if (acer->pdev) {
		platform_device_unregister(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
	}
}

/*
 * Free an instance which has no bound WMI device left. The WMI core
 * guarantees that notify callbacks are synchronized with the driver's remove
 * callbacks, and platform_device_unregister() drains the sysfs and subsystem
 * resources synchronously, so no reference can be left behind here.
 */
static void acer_wmi_instance_free(struct acer_wmi *acer)
{
	lockdep_assert_held(&acer_wmi_instances_lock);

	acer_wmi_instance_deactivate(acer);
	list_del_init(&acer->node);
	mutex_destroy(&acer->lock);
	mutex_destroy(&acer->event_lock);
	kfree(acer);
}

static int acer_wmi_wdev_probe(struct wmi_device *wdev, const void *context)
{
	const struct acer_wmi_endpoint *endpoint = context;
	struct acer_wmi_topology topo = {};
	struct acer_wmi_wdev *wdev_data;
	struct acer_wmi *acer;
	int err;

	mutex_lock(&acer_wmi_instances_lock);

	if (acer_wmi_shutting_down) {
		err = -ENODEV;
		goto out_unlock;
	}

	acer = acer_wmi_instance_find(wdev->dev.parent);
	if (!acer) {
		/*
		 * All WMI devices of a WMI bus device are already registered
		 * when the first of them is probed, so the complete topology
		 * can be validated before the instance is created. Machines
		 * without all required Acer interfaces are not supported.
		 */
		device_for_each_child(wdev->dev.parent, &topo,
				      acer_wmi_topology_scan);
		if (!acer_wmi_topology_is_supported(&wdev->dev, &topo)) {
			err = -ENODEV;
			goto out_unlock;
		}

		acer = acer_wmi_instance_create(wdev->dev.parent,
						topo.endpoints);
		if (!acer) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}

	wdev_data = devm_kzalloc(&wdev->dev, sizeof(*wdev_data), GFP_KERNEL);
	if (!wdev_data) {
		err = -ENOMEM;
		goto out_maybe_destroy;
	}

	wdev_data->acer = acer;
	wdev_data->guid = endpoint->guid;

	mutex_lock(&acer->lock);
	if (!acer_wmi_endpoint_claim(acer, endpoint->guid, wdev)) {
		mutex_unlock(&acer->lock);
		dev_warn(
			&wdev->dev,
			"GUID %s is already claimed by another WMI device, refusing this device\n",
			endpoint->guid_string);
		err = -ENODEV;
		goto out_maybe_destroy;
	}
	mutex_unlock(&acer->lock);

	dev_set_drvdata(&wdev->dev, wdev_data);

	if (acer->setup_done || acer->setup_failed) {
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	/* Wait for the remaining endpoints of this instance to be claimed. */
	if (acer->wdev_count < acer->wdev_expected) {
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	err = acer_wmi_instance_setup(acer);
	if (err) {
		/*
		 * The aggregate cannot be set up. All WMI devices of a WMI bus
		 * device are registered before any of them is probed, so a
		 * later probe can never complete the set: the instance stays
		 * inactive without publishing any interface and is only
		 * released once its last WMI device is removed.
		 */
		dev_err(&wdev->dev, "Unable to set up the logical device: %d\n",
			err);
		acer->setup_failed = true;
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	acer->setup_done = true;
	mutex_unlock(&acer_wmi_instances_lock);
	return 0;

out_maybe_destroy:
	/* Drop an instance which never got any WMI device */
	if (!acer->wdev_count)
		acer_wmi_instance_free(acer);
out_unlock:
	mutex_unlock(&acer_wmi_instances_lock);
	return err;
}

static void acer_wmi_wdev_remove(struct wmi_device *wdev)
{
	struct acer_wmi_wdev *wdev_data = dev_get_drvdata(&wdev->dev);
	struct acer_wmi *acer;

	if (!wdev_data)
		return;

	acer = wdev_data->acer;
	dev_set_drvdata(&wdev->dev, NULL);

	mutex_lock(&acer_wmi_instances_lock);

	/*
	 * Take acer->lock to serialize the claim release with every path that
	 * resolves an endpoint through acer->wdevs[], including the WMI
	 * notify path and the subsystem frontends. The caller of this
	 * function is synchronized with the notify path by the WMI core.
	 */
	mutex_lock(&acer->lock);
	acer_wmi_endpoint_release(acer, wdev_data->guid, wdev);
	mutex_unlock(&acer->lock);

	/* Free the instance once its last endpoint device is gone. */
	if (!acer->wdev_count)
		acer_wmi_instance_free(acer);

	mutex_unlock(&acer_wmi_instances_lock);
}

static struct wmi_driver acer_wmi_driver = {
	.driver = {
		.name = "linuwu-sense",
	},
	.id_table = acer_wmi_id_table,
	/*
	 * The event payload is validated in acer_wmi_notify(). A non-zero
	 * value would make the WMI core refuse to bind the event device on
	 * firmware without a _WED method, which would prevent the instance
	 * from ever seeing the complete set of WMI devices.
	 */
	.min_event_size = 0,
	.no_singleton = true,
	.probe = acer_wmi_wdev_probe,
	.remove = acer_wmi_wdev_remove,
	.notify_new = acer_wmi_notify,
};

static int acer_wmi_instance_setup(struct acer_wmi *acer)
{
	struct acer_wmi *other;
	int err, id = PLATFORM_DEVID_NONE;

	lockdep_assert_held(&acer_wmi_instances_lock);

	if (!acer->quirks)
		return -ENODEV;

	mutex_lock(&acer->lock);
	if (linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE))
		acer->capability |= ACER_CAP_SET_FUNCTION_MODE;
	mutex_unlock(&acer->lock);

	set_quirks(acer);

	/*
	 * Bring up the firmware side of the Acer interface before any
	 * interface is published to the rest of the system.
	 */
	err = 0;
	mutex_lock(&acer->lock);
	if (linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE) &&
	    (acer->capability & ACER_CAP_SET_FUNCTION_MODE)) {
		if (linuwu_sense_gaming_enable_rf_button(acer))
			pr_warn("Cannot enable RF Button Driver\n");

		if (ec_raw_mode) {
			if (linuwu_sense_gaming_enable_ec_raw(acer)) {
				pr_err("Cannot enable EC raw mode\n");
				err = -ENODEV;
			}
		} else if (linuwu_sense_gaming_enable_launch_manager(acer)) {
			pr_err("Cannot enable Launch Manager mode\n");
			err = -ENODEV;
		}
	} else if (ec_raw_mode) {
		pr_info("No WMID EC raw mode enable method\n");
	}
	mutex_unlock(&acer->lock);

	if (err)
		return err;

	/*
	 * Keep a stable device name for the logical device of the only
	 * instance; additional instances get an auto-assigned id.
	 */
	list_for_each_entry(other, &acer_wmi_instances, node) {
		if (other != acer && other->pdev) {
			id = PLATFORM_DEVID_AUTO;
			break;
		}
	}

	acer->pdev = platform_device_alloc("linuwu-sense", id);
	if (!acer->pdev)
		return -ENOMEM;

	platform_set_drvdata(acer->pdev, acer);
	acer->dev = &acer->pdev->dev;

	err = platform_device_add(acer->pdev);
	if (err) {
		platform_device_put(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
		return err;
	}

	return 0;
}

static void acer_wmi_teardown(void)
{
	struct acer_wmi *acer, *tmp;

	mutex_lock(&acer_wmi_instances_lock);
	acer_wmi_shutting_down = true;

	/*
	 * Unregister the platform devices while all WMI devices are still
	 * bound, so that the state is saved before the WMI devices are gone.
	 * The instances themselves stay allocated until the WMI devices have
	 * been removed by wmi_driver_unregister() below.
	 */
	list_for_each_entry_safe(acer, tmp, &acer_wmi_instances, node)
		acer_wmi_instance_deactivate(acer);
	mutex_unlock(&acer_wmi_instances_lock);

	platform_driver_unregister(&acer_platform_driver);
	wmi_driver_unregister(&acer_wmi_driver);

	/*
	 * Drop any leftover registry entry, e.g. for an instance whose WMI
	 * devices never bound to the driver.
	 */
	mutex_lock(&acer_wmi_instances_lock);
	list_for_each_entry_safe(acer, tmp, &acer_wmi_instances, node)
		acer_wmi_instance_free(acer);
	mutex_unlock(&acer_wmi_instances_lock);
}

static int __init acer_wmi_init(void)
{
	int err;

	pr_info("Acer Laptop ACPI-WMI Extras\n");

	/*
	 * This driver only controls Acer Predator/Nitro machines. Reject
	 * everything else before any WMI device can be bound.
	 */
	acer_wmi_quirks = linuwu_sense_quirks_match();
	if (!acer_wmi_quirks) {
		pr_info("Unsupported hardware detected - not loading\n");
		return -ENODEV;
	}

	mutex_lock(&acer_wmi_instances_lock);
	acer_wmi_shutting_down = false;
	mutex_unlock(&acer_wmi_instances_lock);

	err = platform_driver_register(&acer_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		return err;
	}

	err = wmi_driver_register(&acer_wmi_driver);
	if (err) {
		pr_err("Unable to register WMI driver\n");
		platform_driver_unregister(&acer_platform_driver);
		return err;
	}

	/*
	 * The instances are created and set up from the WMI device probe
	 * callbacks, so no WMI device has to be present at this point. The
	 * driver core also probes matching WMI devices which appear later.
	 */
	return 0;
}

static void __exit acer_wmi_exit(void)
{
	acer_wmi_teardown();

	pr_info("Acer Laptop WMI Extras unloaded\n");
}

module_init(acer_wmi_init);
module_exit(acer_wmi_exit);

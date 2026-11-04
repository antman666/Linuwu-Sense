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

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
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
 * Verbose logging is opt-in. It only raises the existing driver detail lines
 * from dev_dbg() to dev_info(), it never changes the effective feature set.
 */
static bool linuwu_sense_test_mode;
module_param_named(test_mode, linuwu_sense_test_mode, bool, 0644);
MODULE_PARM_DESC(test_mode, "Enable verbose driver logging (default: 0)");

void linuwu_sense_dbg(struct device *dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (linuwu_sense_test_mode)
		dev_info(dev, "%pV", &vaf);
	else
		dev_dbg(dev, "%pV", &vaf);

	va_end(args);
}

void linuwu_sense_pr_dbg(const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (linuwu_sense_test_mode)
		pr_info("%pV", &vaf);
	else
		pr_debug("%pV", &vaf);

	va_end(args);
}

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

/*
 * Static feature configuration. Both containers are load time only; the
 * kernel never reads or writes a file for them.
 */
static char feature_param[512];
static char options_param[512];
static struct linuwu_sense_config linuwu_sense_config;
static u32 linuwu_sense_default_on;
static u32 linuwu_sense_allowed;

struct linuwu_sense_feature_desc {
	const char *name;
	u32 bit;
};

static const struct linuwu_sense_feature_desc linuwu_sense_features[] = {
	{ "fan_tach", LS_FEAT_FAN_TACH },
	{ "fan_control", LS_FEAT_FAN_CONTROL },
	{ "thermal_profile", LS_FEAT_THERMAL_PROFILE },
	{ "keyboard_4zone", LS_FEAT_KEYBOARD_4ZONE },
	{ "battery_health", LS_FEAT_BATTERY_HEALTH },
	{ "usb_charging", LS_FEAT_USB_CHARGING },
	{ "backlight_timeout", LS_FEAT_BACKLIGHT_TIMEOUT },
	{ "boot_animation", LS_FEAT_BOOT_ANIMATION },
	{ "lcd_override", LS_FEAT_LCD_OVERRIDE },
};

static const struct linuwu_sense_feature_desc *
ls_feature_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(linuwu_sense_features); i++)
		if (!strcmp(name, linuwu_sense_features[i].name))
			return &linuwu_sense_features[i];

	return NULL;
}

static const struct linuwu_sense_feature_desc *
ls_feature_by_bit(u32 bit)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(linuwu_sense_features); i++)
		if (linuwu_sense_features[i].bit == bit)
			return &linuwu_sense_features[i];

	return NULL;
}

static int ls_feature_index(u32 bit)
{
	return __ffs(bit);
}

static int ls_param_store(char *dst, size_t size, const char *val,
			  const char *name)
{
	size_t len;

	if (!val) {
		pr_warn("%s= without a value, using defaults\n", name);
		dst[0] = '\0';
		return 0;
	}

	len = strlen(val);
	if (len >= size) {
		pr_warn("%s= value truncated to %zu bytes\n", name, size - 1);
		len = size - 1;
	}

	memcpy(dst, val, len);
	dst[len] = '\0';

	return 0;
}

static int feature_param_set(const char *val, const struct kernel_param *kp)
{
	return ls_param_store(feature_param, sizeof(feature_param), val,
			      "feature");
}

static int feature_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s\n", feature_param);
}

static const struct kernel_param_ops feature_param_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = feature_param_set,
	.get = feature_param_get,
};

module_param_cb(feature, &feature_param_ops, NULL, 0444);
MODULE_PARM_DESC(feature, "Feature switches: <feature>.enable=0|1,...");

static int options_param_set(const char *val, const struct kernel_param *kp)
{
	return ls_param_store(options_param, sizeof(options_param), val,
			      "options");
}

static int options_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s\n", options_param);
}

static const struct kernel_param_ops options_param_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = options_param_set,
	.get = options_param_get,
};

module_param_cb(options, &options_param_ops, NULL, 0444);
MODULE_PARM_DESC(options, "Feature options: <feature>.<option>=<value>,...");

/*
 * Features enabled by the historical behaviour of the matched model. This is
 * the compatibility baseline: without feature= / options= nothing changes.
 */
static u32 linuwu_sense_default_features(const struct linuwu_sense_quirks *q)
{
	u32 features = 0;

	if (q->cpu_fans || q->gpu_fans)
		features |= LS_FEAT_FAN_CONTROL;

	if (q->predator_v4 || q->nitro_v4 || q->nitro_sense)
		features |= LS_FEAT_FAN_TACH;

	if (q->predator_v4 || q->nitro_v4 || q->nitro_sense == 1)
		features |= LS_FEAT_THERMAL_PROFILE;

	if (q->four_zone_kb)
		features |= LS_FEAT_KEYBOARD_4ZONE;

	if (q->predator_v4 || q->nitro_v4 || q->nitro_sense)
		features |= LS_FEAT_BATTERY_HEALTH |
			    LS_FEAT_USB_CHARGING |
			    LS_FEAT_BACKLIGHT_TIMEOUT;

	if (q->predator_v4 || q->nitro_v4)
		features |= LS_FEAT_BOOT_ANIMATION |
			    LS_FEAT_LCD_OVERRIDE;

	return features;
}

/*
 * Hard boundary of the model. Features outside this mask cannot be enabled by
 * the user and are never probed. The boundary is deliberately wider than the
 * historical default so that a validated feature can be opted in without
 * changing the default behaviour of existing machines.
 */
static u32 linuwu_sense_allowed_features(const struct linuwu_sense_quirks *q)
{
	u32 allowed = linuwu_sense_default_features(q);

	allowed |= LS_FEAT_FAN_TACH | LS_FEAT_FAN_CONTROL |
		   LS_FEAT_THERMAL_PROFILE | LS_FEAT_KEYBOARD_4ZONE |
		   LS_FEAT_BATTERY_HEALTH | LS_FEAT_USB_CHARGING |
		   LS_FEAT_BACKLIGHT_TIMEOUT;

	if (q->predator_v4 || q->nitro_v4)
		allowed |= LS_FEAT_BOOT_ANIMATION | LS_FEAT_LCD_OVERRIDE;

	return allowed;
}

static int ls_parse_rgb(const char *value, u32 *rgb)
{
	u32 val;

	if (kstrtou32(value, 16, &val) || val > 0xffffff)
		return -EINVAL;

	*rgb = val;
	return 0;
}

static void ls_parse_feature_param(u32 default_on, u32 allowed)
{
	char buf[sizeof(feature_param)];
	char *token, *next, *eq, *dot;
	const struct linuwu_sense_feature_desc *feature;
	u32 requested = default_on;
	u8 enable;

	strscpy(buf, feature_param, sizeof(buf));

	for (token = strsep(&next, ","); token; token = strsep(&next, ",")) {
		token = strim(token);
		if (!*token)
			continue;

		eq = strchr(token, '=');
		if (!eq) {
			pr_warn("feature: '%s' has no value, ignored\n", token);
			continue;
		}
		*eq++ = '\0';
		token = strim(token);
		eq = strim(eq);

		dot = strrchr(token, '.');
		if (!dot || strcmp(dot, ".enable")) {
			pr_warn("feature: '%s' ignored, only <feature>.enable is accepted\n",
				token);
			continue;
		}
		*dot = '\0';

		feature = ls_feature_by_name(token);
		if (!feature) {
			pr_warn("feature: unknown feature '%s', ignored\n", token);
			continue;
		}

		if (!(allowed & feature->bit)) {
			pr_warn("feature: '%s' is outside the model boundary, using default\n",
				token);
			continue;
		}

		if (kstrtou8(eq, 10, &enable) || enable > 1) {
			pr_warn("feature: invalid enable value for '%s', using default\n",
				token);
			continue;
		}

		if (enable) {
			requested |= feature->bit;
			linuwu_sense_config.enabled |= feature->bit;
			linuwu_sense_config.disabled &= ~feature->bit;
		} else {
			requested &= ~feature->bit;
			linuwu_sense_config.disabled |= feature->bit;
			linuwu_sense_config.enabled &= ~feature->bit;
		}
	}

	linuwu_sense_config.requested = requested & allowed;
}

static void ls_parse_options_param(u32 requested, u32 allowed)
{
	char buf[sizeof(options_param)];
	char *token, *next, *eq, *dot;
	const struct linuwu_sense_feature_desc *feature;
	u32 rgb;
	int zone;

	strscpy(buf, options_param, sizeof(buf));

	for (token = strsep(&next, ","); token; token = strsep(&next, ",")) {
		token = strim(token);
		if (!*token)
			continue;

		eq = strchr(token, '=');
		if (!eq) {
			pr_warn("options: '%s' has no value, ignored\n", token);
			continue;
		}
		*eq++ = '\0';
		token = strim(token);
		eq = strim(eq);

		dot = strrchr(token, '.');
		if (!dot) {
			pr_warn("options: '%s' is not <feature>.<option>, ignored\n",
				token);
			continue;
		}
		*dot++ = '\0';

		feature = ls_feature_by_name(token);
		if (!feature) {
			pr_warn("options: unknown feature '%s', ignored\n", token);
			continue;
		}

		if (!(allowed & feature->bit)) {
			pr_warn("options: '%s' is outside the model boundary, ignored\n",
				token);
			continue;
		}

		if (!strcmp(dot, "enable")) {
			pr_warn("options: '%s.enable' belongs in feature=, ignored\n",
				token);
			continue;
		}

		if (!(requested & feature->bit)) {
			pr_warn("options: '%s.%s' ignored, feature is disabled; use %s.enable=1\n",
				token, dot, token);
			continue;
		}

		if (feature->bit == LS_FEAT_KEYBOARD_4ZONE) {
			if (!strcmp(dot, "firstzone"))
				zone = 0;
			else if (!strcmp(dot, "secondzone"))
				zone = 1;
			else if (!strcmp(dot, "thirdzone"))
				zone = 2;
			else if (!strcmp(dot, "fourthzone"))
				zone = 3;
			else {
				pr_warn("options: unknown keyboard_4zone option '%s', ignored\n",
					dot);
				continue;
			}

			if (ls_parse_rgb(eq, &rgb)) {
				pr_warn("options: invalid colour for keyboard_4zone.%s, ignored\n",
					dot);
				continue;
			}

			linuwu_sense_config.keyboard.valid[zone] = true;
			linuwu_sense_config.keyboard.rgb[zone] = rgb;
			continue;
		}

		pr_warn("options: unknown option '%s' for feature '%s', ignored\n",
			dot, token);
	}
}

static int linuwu_sense_config_init(const struct linuwu_sense_quirks *quirks)
{
	memset(&linuwu_sense_config, 0, sizeof(linuwu_sense_config));

	linuwu_sense_default_on = linuwu_sense_default_features(quirks);
	linuwu_sense_allowed = linuwu_sense_allowed_features(quirks);

	if (linuwu_sense_default_on & ~linuwu_sense_allowed) {
		pr_warn("model table error: default features exceed the boundary, clamping\n");
		linuwu_sense_default_on &= linuwu_sense_allowed;
	}

	linuwu_sense_config.requested = linuwu_sense_default_on;

	ls_parse_feature_param(linuwu_sense_default_on, linuwu_sense_allowed);
	ls_parse_options_param(linuwu_sense_config.requested,
			       linuwu_sense_allowed);

	return 0;
}

/*
 * Family bits select the userspace frontend group and are not switchable.
 * The individual controls are derived from caps.effective by
 * ls_update_capability().
 */
static void set_quirks(struct acer_wmi *acer)
{
	const struct linuwu_sense_quirks *quirks = acer->quirks;

	if (quirks->nitro_sense)
		acer->capability |= ACER_CAP_NITRO_SENSE;
	if (quirks->predator_v4)
		acer->capability |= ACER_CAP_PREDATOR_SENSE;
	if (quirks->nitro_v4)
		acer->capability |= ACER_CAP_NITRO_SENSE_V4;

	acer->cpu_fan_control = quirks->cpu_fans > 0;
	acer->gpu_fan_control = quirks->gpu_fans > 0;
}

static void ls_update_capability(struct acer_wmi *acer)
{
	acer->capability &= ~(ACER_CAP_TURBO_FAN |
			      ACER_CAP_PLATFORM_PROFILE |
			      ACER_CAP_FAN_SPEED_READ);

	if (acer->caps.effective & LS_FEAT_FAN_CONTROL)
		acer->capability |= ACER_CAP_TURBO_FAN;
	if (acer->caps.effective & LS_FEAT_THERMAL_PROFILE)
		acer->capability |= ACER_CAP_PLATFORM_PROFILE;
	if (acer->caps.effective & LS_FEAT_FAN_TACH)
		acer->capability |= ACER_CAP_FAN_SPEED_READ;
}

static void ls_clear_feature(struct acer_wmi *acer, u32 feature)
{
	acer->caps.effective &= ~feature;
	ls_update_capability(acer);
}

/*
 * General interface convenience methods
 */

static bool has_cap(const struct acer_wmi *acer, u32 cap)
{
	return acer->capability & cap;
}

static bool has_feature(const struct acer_wmi *acer, u32 feature)
{
	return acer->caps.effective & feature;
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

/*
 * Capability probe. Only features the user explicitly enabled outside the
 * historical default are probed; the defaults keep their current behaviour.
 */
static void ls_probe_record(struct acer_wmi *acer, u32 bit, int err)
{
	const struct linuwu_sense_feature_desc *feature = ls_feature_by_bit(bit);
	int index = ls_feature_index(bit);

	if (!feature || index < 0 || index >= LS_FEAT_COUNT)
		return;

	acer->caps.last_err[index] = err;

	if (!err) {
		acer->caps.state[index] = LS_PROBE_SUPPORTED;
		linuwu_sense_dbg(acer->parent, "feature %s: probe supported\n",
				 feature->name);
		return;
	}

	if (err == -ENODEV)
		acer->caps.state[index] = LS_PROBE_DEFERRED;
	else if (err == -EOPNOTSUPP)
		acer->caps.state[index] = LS_PROBE_UNSUPPORTED;
	else
		acer->caps.state[index] = LS_PROBE_INCONCLUSIVE;

	acer->caps.effective &= ~bit;
	dev_warn(acer->parent, "feature %s: probe failed (%d), disabled\n",
		 feature->name, err);
}

static int ls_probe_fan_tach(struct acer_wmi *acer)
{
	u64 sensors;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_supported_sensors(acer, &sensors);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

	acer->caps.sensors = sensors;

	if (!linuwu_sense_gaming_sensor_is_supported(
		    sensors, LINUWU_SENSE_GAMING_SENSOR_CPU_FAN_SPEED) &&
	    !linuwu_sense_gaming_sensor_is_supported(
		    sensors, LINUWU_SENSE_GAMING_SENSOR_GPU_FAN_SPEED))
		return -EOPNOTSUPP;

	return 0;
}

static int ls_probe_fan_control(struct acer_wmi *acer)
{
	enum linuwu_sense_fan_mode mode;
	struct wmi_device *wdev;
	bool cpu = false;
	bool gpu = false;
	int err;

	mutex_lock(&acer->lock);
	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	if (!wdev) {
		err = -ENODEV;
		goto out_unlock;
	}

	if (!linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_CPU, &mode))
		cpu = true;
	if (!linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_GPU, &mode))
		gpu = true;

	err = (cpu || gpu) ? 0 : -EOPNOTSUPP;

out_unlock:
	mutex_unlock(&acer->lock);

	acer->cpu_fan_control |= cpu;
	acer->gpu_fan_control |= gpu;

	return err;
}

static int ls_probe_thermal_profile(struct acer_wmi *acer)
{
	unsigned long profiles = 0;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_supported_thermal_profiles(acer,
								 &profiles);
	mutex_unlock(&acer->lock);

	if (!err && !profiles)
		err = -EOPNOTSUPP;

	acer->caps.profiles = profiles;
	return err;
}

static int ls_probe_battery_health(struct acer_wmi *acer)
{
	struct wmi_device *wdev;
	int enabled;
	int err;

	mutex_lock(&acer->lock);
	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY);
	if (wdev)
		err = linuwu_sense_battery_get_mode(
			wdev, LINUWU_SENSE_BATTERY_MODE_HEALTH, &enabled);
	else
		err = -ENODEV;
	mutex_unlock(&acer->lock);

	return err;
}

static int ls_probe_usb_charging(struct acer_wmi *acer)
{
	struct wmi_device *wdev;
	int percent;
	int err;

	mutex_lock(&acer->lock);
	wdev = linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE);
	if (wdev)
		err = linuwu_sense_battery_get_usb_charging(wdev, &percent);
	else
		err = -ENODEV;
	mutex_unlock(&acer->lock);

	if (!err && percent < 0)
		err = -EOPNOTSUPP;

	return err;
}

static int ls_probe_backlight_timeout(struct acer_wmi *acer)
{
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_backlight_timeout(acer, &state);
	mutex_unlock(&acer->lock);

	if (!err && state < 0)
		err = -EOPNOTSUPP;

	return err;
}

static int ls_probe_boot_animation(struct acer_wmi *acer)
{
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_boot_animation_sound(acer, &state);
	mutex_unlock(&acer->lock);

	if (!err && state < 0)
		err = -EOPNOTSUPP;

	return err;
}

static int ls_probe_lcd_override(struct acer_wmi *acer)
{
	int state;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_lcd_override(acer, &state);
	mutex_unlock(&acer->lock);

	if (!err && state < 0)
		err = -EOPNOTSUPP;

	return err;
}

static void acer_wmi_caps_probe(struct acer_wmi *acer)
{
	u32 want = acer->caps.requested & ~acer->caps.default_on;
	unsigned int i;

	for (i = 0; i < LS_FEAT_COUNT; i++) {
		acer->caps.state[i] = LS_PROBE_NOT_PROBED;
		acer->caps.last_err[i] = 0;
	}

	acer->caps.effective = acer->caps.requested;

	if (want & LS_FEAT_FAN_TACH)
		ls_probe_record(acer, LS_FEAT_FAN_TACH,
				ls_probe_fan_tach(acer));
	if (want & LS_FEAT_FAN_CONTROL)
		ls_probe_record(acer, LS_FEAT_FAN_CONTROL,
				ls_probe_fan_control(acer));
	if (want & LS_FEAT_THERMAL_PROFILE)
		ls_probe_record(acer, LS_FEAT_THERMAL_PROFILE,
				ls_probe_thermal_profile(acer));
	if (want & LS_FEAT_KEYBOARD_4ZONE)
		ls_probe_record(acer, LS_FEAT_KEYBOARD_4ZONE,
				linuwu_sense_keyboard_probe(acer));
	if (want & LS_FEAT_BATTERY_HEALTH)
		ls_probe_record(acer, LS_FEAT_BATTERY_HEALTH,
				ls_probe_battery_health(acer));
	if (want & LS_FEAT_USB_CHARGING)
		ls_probe_record(acer, LS_FEAT_USB_CHARGING,
				ls_probe_usb_charging(acer));
	if (want & LS_FEAT_BACKLIGHT_TIMEOUT)
		ls_probe_record(acer, LS_FEAT_BACKLIGHT_TIMEOUT,
				ls_probe_backlight_timeout(acer));
	if (want & LS_FEAT_BOOT_ANIMATION)
		ls_probe_record(acer, LS_FEAT_BOOT_ANIMATION,
				ls_probe_boot_animation(acer));
	if (want & LS_FEAT_LCD_OVERRIDE)
		ls_probe_record(acer, LS_FEAT_LCD_OVERRIDE,
				ls_probe_lcd_override(acer));

	ls_update_capability(acer);
}

/* Fan Speed */
static bool acer_turbo_fan_supported(struct acer_wmi *acer)
{
	return has_cap(acer, ACER_CAP_TURBO_FAN) &&
	       (acer->cpu_fan_control || acer->gpu_fan_control);
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

	err = linuwu_sense_fan_set_mode(wdev, acer->cpu_fan_control,
					acer->gpu_fan_control,
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
		linuwu_sense_dbg(&wdev->dev, "unknown event buffer length %zu\n",
				 data->length);
		return;
	}

	mutex_lock(&acer->event_lock);
	if (!acer->ready) {
		mutex_unlock(&acer->event_lock);
		return;
	}

	switch (event.type) {
	case LINUWU_SENSE_EVENT_HOTKEY:
		linuwu_sense_dbg(&wdev->dev, "hotkey %u, device state 0x%x\n",
				 event.key_num, event.device_state);
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
		if (has_feature(acer, LS_FEAT_BATTERY_HEALTH)) {
			mutex_lock(&acer->lock);
			err = linuwu_sense_battery_set_mode(
				linuwu_sense_endpoint_get(
					acer, ACER_WMI_GUID_WMID_BATTERY),
				LINUWU_SENSE_BATTERY_MODE_CALIBRATION,
				event.enabled);
			mutex_unlock(&acer->lock);
			if (err)
				dev_err(&wdev->dev,
					"error changing calibration state: %d\n",
					err);
		}
		break;
	default:
		linuwu_sense_dbg(&wdev->dev, "unknown event type %d\n",
				 event.type);
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
		return err;

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
	if (val != 0 && val != 10 && val != 20 && val != 30)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_set_usb_charging(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE), val);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

	return count;
}

/*
 * Battery Calibration
 */
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
		return err;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t predator_battery_calibration_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;
	int err;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;

	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_battery_set_mode(
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_BATTERY),
		LINUWU_SENSE_BATTERY_MODE_CALIBRATION, val);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

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

	if (acer->cpu_fan_control) {
		err = linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_CPU,
						&mode);
		if (err)
			goto out;
		have_fan = true;
		if (mode != LINUWU_SENSE_FAN_MODE_TURBO)
			enabled = false;
	}

	if (acer->gpu_fan_control) {
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
		return err;

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
	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_lcd_override(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

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
		return err;

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
	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_backlight_timeout(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

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
		return err;

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
	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_set_boot_animation_sound(acer, val == 1);
	mutex_unlock(&acer->lock);
	if (err)
		return err;

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
	       predator_battery_calibration_store);
static struct device_attribute turbo_mode = __ATTR(
	turbo_mode, 0644, predator_turbo_mode_show, predator_turbo_mode_store);
static struct device_attribute lcd_override =
	__ATTR(lcd_override, 0644, predator_lcd_override_show,
	       predator_lcd_override_store);
static struct attribute *predator_sense_attrs[] = { &lcd_override.attr,
						    &turbo_mode.attr,
						    &battery_calibration.attr,
						    &usb_charging.attr,
						    &backlight_timeout.attr,
						    &boot_animation_sound.attr,
						    NULL };

static umode_t ls_sense_attr_is_visible(struct acer_wmi *acer,
					struct attribute *attr)
{
	if (!acer)
		return 0;

	if (attr == &turbo_mode.attr)
		return acer_turbo_fan_supported(acer) ? attr->mode : 0;
	if (attr == &lcd_override.attr)
		return has_feature(acer, LS_FEAT_LCD_OVERRIDE) ? attr->mode : 0;
	if (attr == &battery_calibration.attr)
		return has_feature(acer, LS_FEAT_BATTERY_HEALTH) ? attr->mode : 0;
	if (attr == &usb_charging.attr)
		return has_feature(acer, LS_FEAT_USB_CHARGING) ? attr->mode : 0;
	if (attr == &backlight_timeout.attr)
		return has_feature(acer, LS_FEAT_BACKLIGHT_TIMEOUT) ? attr->mode : 0;
	if (attr == &boot_animation_sound.attr)
		return has_feature(acer, LS_FEAT_BOOT_ANIMATION) ? attr->mode : 0;

	return attr->mode;
}

static umode_t predator_sense_attr_is_visible(struct kobject *kobj,
					      struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);

	return ls_sense_attr_is_visible(dev_get_drvdata(dev), attr);
}

static umode_t nitro_sense_attr_is_visible(struct kobject *kobj,
					   struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);

	return ls_sense_attr_is_visible(dev_get_drvdata(dev), attr);
}

static struct attribute_group predator_sense_attr_group = {
	.name = "predator_sense",
	.attrs = predator_sense_attrs,
	.is_visible = predator_sense_attr_is_visible,
};

static struct attribute *nitro_sense_v4_attrs[] = {
	&lcd_override.attr,	 &battery_calibration.attr,  &usb_charging.attr,
	&backlight_timeout.attr, &boot_animation_sound.attr, NULL
};

static struct attribute_group nitro_sense_v4_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_v4_attrs,
	.is_visible = nitro_sense_attr_is_visible,
};

/* nitro sense attributes */
static struct attribute *nitro_sense_attrs[] = { &battery_calibration.attr,
						 &usb_charging.attr,
						 &backlight_timeout.attr,
						 NULL };
static struct attribute_group nitro_sense_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_attrs,
	.is_visible = nitro_sense_attr_is_visible,
};

/*
 * Platform device
 */

/*
 * Clear the shared frontend pointers. This action is registered before any
 * frontend resource is allocated, so devres releases it last, after every
 * frontend was unregistered and its state released. Until then a frontend
 * callback may still dereference these pointers, so they must stay valid.
 */
static void acer_platform_frontends_released(void *data)
{
	struct acer_wmi *acer = data;

	acer->input = NULL;
	acer->keyboard = NULL;
	acer->profile = NULL;
}

/*
 * Publish the userspace frontends of an instance. Called by the logical
 * platform device probe, which acer_wmi_instance_setup() triggers after the
 * endpoint topology was confirmed and the firmware side was initialized.
 *
 * Optional frontends are best effort: a failure is logged and the matching
 * feature is cleared, but it never makes the whole aggregate unusable. Only
 * the devres setup itself is fatal.
 *
 * Event handling (@ready) is enabled last, once every frontend is published.
 */
static int acer_platform_probe(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);
	int err;

	if (!acer)
		return -ENODEV;

	acer->platform_probe_ok = false;

	err = devm_add_action_or_reset(&pdev->dev,
				       acer_platform_frontends_released, acer);
	if (err)
		return err;

	err = linuwu_sense_input_init(acer);
	if (err)
		dev_warn(&pdev->dev, "input frontend disabled: %d\n", err);

	if (has_feature(acer, LS_FEAT_THERMAL_PROFILE)) {
		err = linuwu_sense_profile_init(acer);
		if (err) {
			dev_warn(&pdev->dev,
				 "platform profile frontend disabled: %d\n", err);
			ls_clear_feature(acer, LS_FEAT_THERMAL_PROFILE);
		}
	}

	if (has_cap(acer, ACER_CAP_TURBO_FAN)) {
		mutex_lock(&acer->lock);
		err = acer_set_turbo_fan_mode_locked(acer, false);
		mutex_unlock(&acer->lock);
		if (err && err != -EOPNOTSUPP)
			dev_warn(&pdev->dev,
				 "unable to initialize fan mode: %d\n", err);
	}

	if (has_cap(acer, ACER_CAP_PREDATOR_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &predator_sense_attr_group);
		if (err)
			dev_warn(&pdev->dev,
				 "predator_sense attributes disabled: %d\n", err);
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE_V4)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_v4_attr_group);
		if (err)
			dev_warn(&pdev->dev,
				 "nitro_sense attributes disabled: %d\n", err);
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_attr_group);
		if (err)
			dev_warn(&pdev->dev,
				 "nitro_sense attributes disabled: %d\n", err);
	}

	if (has_feature(acer, LS_FEAT_KEYBOARD_4ZONE)) {
		err = linuwu_sense_keyboard_init(acer);
		if (err) {
			dev_warn(&pdev->dev,
				 "keyboard frontend disabled: %d\n", err);
			ls_clear_feature(acer, LS_FEAT_KEYBOARD_4ZONE);
		}
	}

	if (has_feature(acer, LS_FEAT_BATTERY_HEALTH)) {
		err = linuwu_sense_battery_init(acer);
		if (err) {
			dev_warn(&pdev->dev,
				 "battery health frontend disabled: %d\n", err);
			ls_clear_feature(acer, LS_FEAT_BATTERY_HEALTH);
		}
	}

	/*
	 * The hwmon frontend provides the sensor readings and the fan control:
	 * register it when the machine has at least one of them.
	 */
	if (has_cap(acer, ACER_CAP_FAN_SPEED_READ) ||
	    has_cap(acer, ACER_CAP_TURBO_FAN)) {
		err = acer_wmi_hwmon_init(acer, acer->dev);
		if (err) {
			dev_warn(&pdev->dev,
				 "hwmon frontend disabled: %d\n", err);
			ls_clear_feature(acer, LS_FEAT_FAN_TACH |
						 LS_FEAT_FAN_CONTROL);
		}
	}

	mutex_lock(&acer->event_lock);
	acer->ready = true;
	mutex_unlock(&acer->event_lock);

	/*
	 * All frontends are published and event handling is enabled: report
	 * the logical device as successfully probed.
	 */
	acer->platform_probe_ok = true;

	return 0;
}

static void acer_platform_remove(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);

	if (!acer)
		return;

	/*
	 * Stop the WMI notify path. acer->ready only closes the event path;
	 * the sysfs, LED and platform profile callbacks are stopped later by
	 * their own devres release actions, so the frontend pointers must
	 * stay valid until then.
	 */
	mutex_lock(&acer->event_lock);
	acer->ready = false;
	mutex_unlock(&acer->event_lock);

	/*
	 * All remaining resources (input, hwmon, platform profile, LED
	 * devices and sysfs groups) are devres-managed and stay live until
	 * the driver core releases them after this callback returned. Their
	 * callbacks may still dereference the state behind acer->input,
	 * acer->keyboard and acer->profile, so the pointers must not be
	 * cleared here. The devres action registered first in probe clears
	 * them once every frontend has been unregistered.
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
	/* Number of matching WMI devices below the WMI bus device. */
	unsigned int devices;
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

		topo->devices++;

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

static struct acer_wmi *
acer_wmi_instance_create(struct device *parent,
			 const struct acer_wmi_topology *topo)
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
	acer->wdev_expected = topo->endpoints;
	acer->probes_expected = topo->devices;
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

/*
 * An instance may only be released once no endpoint is claimed any more and
 * every matching WMI device of its WMI bus device has been probed: only then
 * no later probe can still reference it. The caller must hold
 * acer_wmi_instances_lock.
 */
static bool acer_wmi_instance_is_exhausted(const struct acer_wmi *acer)
{
	return !acer->wdev_count && acer->probes_seen >= acer->probes_expected;
}

static void acer_wmi_instance_drop(struct acer_wmi *acer)
{
	lockdep_assert_held(&acer_wmi_instances_lock);

	if (acer_wmi_instance_is_exhausted(acer))
		acer_wmi_instance_free(acer);
}

/*
 * Advance a COLLECTING instance: set up the logical device once the complete
 * endpoint set is claimed, or mark the instance FAILED once every expected
 * probe has run without completing the set. Must be called with
 * acer_wmi_instances_lock held.
 */
static void acer_wmi_instance_advance(struct acer_wmi *acer, struct device *dev)
{
	int err;

	if (acer->state != ACER_WMI_COLLECTING)
		return;

	if (acer->wdev_count == acer->wdev_expected) {
		/*
		 * The complete endpoint set is bound: bring up the logical
		 * platform device. It only succeeds when all its frontends
		 * were registered, otherwise the aggregate fails without
		 * leaving a partial interface behind.
		 */
		err = acer_wmi_instance_setup(acer);
		if (err) {
			dev_err(dev,
				"unable to set up the logical device: %d\n",
				err);
			acer->state = ACER_WMI_FAILED;
		} else {
			acer->state = ACER_WMI_ACTIVE;
		}

		return;
	}

	/*
	 * The endpoint set is not complete yet. Once every expected probe has
	 * run the set can no longer be completed, because all WMI devices of
	 * a WMI bus device were already registered when probing started.
	 */
	if (acer->probes_seen >= acer->probes_expected) {
		dev_err(dev,
			"only %u of %u Acer WMI endpoints bound, aggregate disabled\n",
			acer->wdev_count, acer->wdev_expected);
		acer->state = ACER_WMI_FAILED;
	}
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

		acer = acer_wmi_instance_create(wdev->dev.parent, &topo);
		if (!acer) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}

	/*
	 * FAILED is terminal: every expected probe already ran when the
	 * aggregate failed, so this can only be a rebind of an endpoint. It
	 * cannot make the aggregate work again.
	 */
	if (acer->state == ACER_WMI_FAILED) {
		dev_warn(&wdev->dev, "aggregate setup already failed\n");
		err = -ENODEV;
		goto out_unlock;
	}

	acer->probes_seen++;

	wdev_data = devm_kzalloc(&wdev->dev, sizeof(*wdev_data), GFP_KERNEL);
	if (!wdev_data) {
		err = -ENOMEM;
		goto out_maybe_drop;
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
		goto out_maybe_drop;
	}
	mutex_unlock(&acer->lock);

	dev_set_drvdata(&wdev->dev, wdev_data);

	if (acer->state == ACER_WMI_ACTIVE) {
		/* Rebind of an endpoint of an already running aggregate. */
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	acer_wmi_instance_advance(acer, &wdev->dev);

	mutex_unlock(&acer_wmi_instances_lock);
	return 0;

out_maybe_drop:
	/*
	 * A probe which failed before claiming an endpoint can still be the
	 * last expected one, so the instance state has to advance here too.
	 */
	acer_wmi_instance_advance(acer, &wdev->dev);
	acer_wmi_instance_drop(acer);
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

	/* Drop the instance once it can never be referenced again. */
	acer_wmi_instance_drop(acer);

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

	acer->config = &linuwu_sense_config;
	acer->caps.allowed = linuwu_sense_allowed;
	acer->caps.default_on = linuwu_sense_default_on;
	acer->caps.requested = linuwu_sense_config.requested;
	acer->caps.effective = acer->caps.requested;
	acer_wmi_caps_probe(acer);

	linuwu_sense_dbg(acer->parent,
			 "model boundary 0x%x, defaults 0x%x, requested 0x%x, effective 0x%x\n",
			 acer->caps.allowed, acer->caps.default_on,
			 acer->caps.requested, acer->caps.effective);

	/*
	 * Bring up the firmware side of the Acer interface before any
	 * interface is published to the rest of the system. These handshakes
	 * are best effort: a failure is reported but must not turn the whole
	 * aggregate into a terminal failure.
	 */
	mutex_lock(&acer->lock);
	if (linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_APGE) &&
	    (acer->capability & ACER_CAP_SET_FUNCTION_MODE)) {
		if (linuwu_sense_gaming_enable_rf_button(acer))
			dev_warn(acer->parent, "cannot enable RF button\n");

		if (ec_raw_mode) {
			if (linuwu_sense_gaming_enable_ec_raw(acer))
				dev_warn(acer->parent,
					 "cannot enable EC raw mode\n");
		} else if (linuwu_sense_gaming_enable_launch_manager(acer)) {
			dev_warn(acer->parent,
				 "cannot enable launch manager mode\n");
		}
	} else if (ec_raw_mode) {
		linuwu_sense_dbg(acer->parent,
				 "no WMID EC raw mode enable method\n");
	}
	mutex_unlock(&acer->lock);

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

	/*
	 * This probe is triggered synchronously by the platform_device_add()
	 * below, so @platform_probe_ok tells whether the logical device
	 * published all its frontends.
	 */
	acer->platform_probe_ok = false;
	err = platform_device_add(acer->pdev);
	if (err) {
		platform_device_put(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
		return err;
	}

	if (!acer->platform_probe_ok) {
		/*
		 * The logical device probe failed and released its
		 * devres-managed frontends again, including the action which
		 * clears the shared frontend pointers. Remove the unbound
		 * device so that no incomplete interface stays behind.
		 */
		platform_device_unregister(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
		return -ENODEV;
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
	 * bound, so that the frontend teardown runs in the normal order. The
	 * instances themselves stay allocated until the WMI devices have been
	 * removed by wmi_driver_unregister() below.
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

static const char *ls_probe_state_name(const struct acer_wmi *acer, u32 bit)
{
	int index = ls_feature_index(bit);

	if (!(acer->caps.requested & bit))
		return "disabled";
	if (index < 0 || index >= LS_FEAT_COUNT)
		return "invalid";

	switch (acer->caps.state[index]) {
	case LS_PROBE_NOT_PROBED:
		return "default";
	case LS_PROBE_SUPPORTED:
		return "supported";
	case LS_PROBE_UNSUPPORTED:
		return "unsupported";
	case LS_PROBE_INCONCLUSIVE:
		return "inconclusive";
	case LS_PROBE_DEFERRED:
		return "deferred";
	default:
		return "unknown";
	}
}

static void ls_caps_show_one(struct seq_file *m, const struct acer_wmi *acer)
{
	const char *model = dmi_get_system_info(DMI_PRODUCT_NAME);
	unsigned int i;

	seq_printf(m, "device: %s\n",
		   acer->parent ? dev_name(acer->parent) : "unknown");
	seq_printf(m, "model: %s\n", model ? model : "unknown");
	seq_printf(m, "allowed:    0x%08x\n", acer->caps.allowed);
	seq_printf(m, "default_on: 0x%08x\n", acer->caps.default_on);
	seq_printf(m, "requested:  0x%08x\n", acer->caps.requested);
	seq_printf(m, "effective:  0x%08x\n",
		   acer->caps.effective);
	seq_printf(m, "sensors:    0x%016llx\n",
		   (unsigned long long)acer->caps.sensors);
	seq_printf(m, "profiles:   0x%016lx\n", acer->caps.profiles);
	seq_puts(m, "\n");

	seq_printf(m, "%-20s %-7s %-12s %-7s %s\n", "feature", "enabled",
		   "probe", "error", "configured");
	for (i = 0; i < ARRAY_SIZE(linuwu_sense_features); i++) {
		const struct linuwu_sense_feature_desc *feature =
			&linuwu_sense_features[i];
		int index = ls_feature_index(feature->bit);

		seq_printf(m, "%-20s %-7s %-12s %-7d %s\n", feature->name,
			   (acer->caps.effective & feature->bit) ? "yes" : "no",
			   ls_probe_state_name(acer, feature->bit),
			   (index >= 0 && index < LS_FEAT_COUNT) ?
				   acer->caps.last_err[index] :
				   0,
			   (acer->caps.requested & feature->bit) ? "yes" : "no");
	}
	seq_puts(m, "\n");
}

static int linuwu_sense_caps_show(struct seq_file *m, void *v)
{
	struct acer_wmi *acer;

	mutex_lock(&acer_wmi_instances_lock);
	list_for_each_entry(acer, &acer_wmi_instances, node)
		ls_caps_show_one(m, acer);
	mutex_unlock(&acer_wmi_instances_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(linuwu_sense_caps);

static struct dentry *linuwu_sense_debugfs;

static int __init acer_wmi_init(void)
{
	int err;

	pr_info("loading\n");

	/*
	 * This driver only controls Acer Predator/Nitro machines. Reject
	 * everything else before any WMI device can be bound.
	 */
	acer_wmi_quirks = linuwu_sense_quirks_match();
	if (!acer_wmi_quirks) {
		pr_warn("unsupported hardware, not loading\n");
		return -ENODEV;
	}

	linuwu_sense_config_init(acer_wmi_quirks);

	linuwu_sense_debugfs = debugfs_create_dir("linuwu_sense", NULL);
	if (!IS_ERR_OR_NULL(linuwu_sense_debugfs))
		debugfs_create_file("capabilities", 0444, linuwu_sense_debugfs,
				    NULL, &linuwu_sense_caps_fops);
	else
		linuwu_sense_debugfs = NULL;

	mutex_lock(&acer_wmi_instances_lock);
	acer_wmi_shutting_down = false;
	mutex_unlock(&acer_wmi_instances_lock);

	err = platform_driver_register(&acer_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		goto err_debugfs;
	}

	err = wmi_driver_register(&acer_wmi_driver);
	if (err) {
		pr_err("Unable to register WMI driver\n");
		platform_driver_unregister(&acer_platform_driver);
		goto err_debugfs;
	}

	/*
	 * The instances are created and set up from the WMI device probe
	 * callbacks, so no WMI device has to be present at this point. The
	 * driver core also probes matching WMI devices which appear later.
	 */
	return 0;

err_debugfs:
	debugfs_remove_recursive(linuwu_sense_debugfs);
	linuwu_sense_debugfs = NULL;
	return err;
}

static void __exit acer_wmi_exit(void)
{
	debugfs_remove_recursive(linuwu_sense_debugfs);
	linuwu_sense_debugfs = NULL;
	acer_wmi_teardown();
}

module_init(acer_wmi_init);
module_exit(acer_wmi_exit);

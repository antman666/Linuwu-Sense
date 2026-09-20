/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Predator Gaming WMI protocol access for the Acer WMI Laptop Extras driver.
 */
#ifndef _LINUWU_SENSE_GAMING_H_
#define _LINUWU_SENSE_GAMING_H_

#include <linux/types.h>

struct acer_wmi;

/*
 * Predator thermal profiles as understood by the Predator Gaming WMI
 * interface.
 */
enum acer_predator_v4_thermal_profile {
	ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET = 0x00,
	ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED = 0x01,
	ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE = 0x04,
	ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO = 0x05,
	ACER_PREDATOR_V4_THERMAL_PROFILE_ECO = 0x06,
};

/* Keyboard backlight state as understood by the Predator Gaming interface */
struct linuwu_sense_gaming_kb_backlight {
	u8 mode;
	u8 speed;
	u8 brightness;
	u8 direction;
	u8 red;
	u8 green;
	u8 blue;
};

enum linuwu_sense_gaming_kb_zone {
	LINUWU_SENSE_GAMING_KB_ZONE_1,
	LINUWU_SENSE_GAMING_KB_ZONE_2,
	LINUWU_SENSE_GAMING_KB_ZONE_3,
	LINUWU_SENSE_GAMING_KB_ZONE_4,
	LINUWU_SENSE_GAMING_KB_ZONE_COUNT,
};

/* Battery health control functions */
enum linuwu_sense_gaming_battery_mode {
	LINUWU_SENSE_GAMING_BATTERY_MODE_HEALTH = 1,
	LINUWU_SENSE_GAMING_BATTERY_MODE_CALIBRATION = 2,
};

/*
 * Locking: every function below has to be called with acer->lock held. The
 * only exceptions are the two read-only system info queries:
 *
 *  - linuwu_sense_gaming_get_sys_info() is also called from the hwmon read
 *    path without holding acer->lock.
 *  - linuwu_sense_gaming_get_supported_thermal_profiles() is called from the
 *    platform_profile probe callback without holding acer->lock.
 *
 * Both commands only read a value from the firmware and neither of them
 * touches any cached driver state.
 */

/*
 * Execute a Predator Gaming "get system info" command. The command values are
 * described by the hwmon adapter for sensor readings.
 */
int linuwu_sense_gaming_get_sys_info(struct acer_wmi *acer, u32 command,
				     u64 *out);

/* Whether the machine currently runs on AC power. */
int linuwu_sense_gaming_get_power_source(struct acer_wmi *acer, bool *on_ac);

/* Predator thermal profile */
int linuwu_sense_gaming_get_thermal_profile(struct acer_wmi *acer, u8 *profile);
int linuwu_sense_gaming_set_thermal_profile(struct acer_wmi *acer, u8 profile);
int linuwu_sense_gaming_get_supported_thermal_profiles(struct acer_wmi *acer,
						       unsigned long *profiles);

/* LCD override */
int linuwu_sense_gaming_get_lcd_override(struct acer_wmi *acer, int *state);
int linuwu_sense_gaming_set_lcd_override(struct acer_wmi *acer, bool enable);

/* Backlight timeout */
int linuwu_sense_gaming_get_backlight_timeout(struct acer_wmi *acer,
					      int *state);
int linuwu_sense_gaming_set_backlight_timeout(struct acer_wmi *acer,
					      bool enable);

/* Boot animation sound */
int linuwu_sense_gaming_get_boot_animation_sound(struct acer_wmi *acer,
						 int *state);
int linuwu_sense_gaming_set_boot_animation_sound(struct acer_wmi *acer,
						 bool enable);

/* USB charging, in percent of the maximum charging current */
int linuwu_sense_gaming_get_usb_charging(struct acer_wmi *acer, int *percent);
int linuwu_sense_gaming_set_usb_charging(struct acer_wmi *acer, u8 percent);

/* Battery health control */
int linuwu_sense_gaming_get_battery_mode(struct acer_wmi *acer,
					 enum linuwu_sense_gaming_battery_mode mode,
					 int *enabled);
int linuwu_sense_gaming_set_battery_mode(struct acer_wmi *acer,
					 enum linuwu_sense_gaming_battery_mode mode,
					 u8 status);

/* Four zone keyboard backlight */
int linuwu_sense_gaming_get_kb_backlight(
	struct acer_wmi *acer, struct linuwu_sense_gaming_kb_backlight *state);
int linuwu_sense_gaming_set_kb_backlight(
	struct acer_wmi *acer,
	const struct linuwu_sense_gaming_kb_backlight *state);

/*
 * Per zone RGB colors. The value uses the byte order of the sysfs interface,
 * so it is opaque for the core driver.
 */
int linuwu_sense_gaming_get_kb_zone_color(struct acer_wmi *acer,
					  enum linuwu_sense_gaming_kb_zone zone,
					  u64 *color);
int linuwu_sense_gaming_set_kb_zone_color(struct acer_wmi *acer,
					  enum linuwu_sense_gaming_kb_zone zone,
					  u64 color);

#endif /* _LINUWU_SENSE_GAMING_H_ */

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

/*
 * Sensors of the Predator Gaming "get system info" command. The values are
 * the sensor IDs used by the firmware.
 */
enum linuwu_sense_gaming_sensor {
	LINUWU_SENSE_GAMING_SENSOR_CPU_TEMPERATURE = 0x01,
	LINUWU_SENSE_GAMING_SENSOR_CPU_FAN_SPEED = 0x02,
	LINUWU_SENSE_GAMING_SENSOR_EXTERNAL_TEMPERATURE_2 = 0x03,
	LINUWU_SENSE_GAMING_SENSOR_GPU_FAN_SPEED = 0x06,
	LINUWU_SENSE_GAMING_SENSOR_GPU_TEMPERATURE = 0x0A,
};

/*
 * Locking: every function below has to be called with acer->lock held. The
 * only exceptions are the read-only queries:
 *
 *  - linuwu_sense_gaming_get_supported_sensors() and
 *    linuwu_sense_gaming_read_sensor() are also called from the hwmon path
 *    without holding acer->lock.
 *  - linuwu_sense_gaming_sensor_is_supported() is a pure predicate.
 *  - linuwu_sense_gaming_get_supported_thermal_profiles() is called from the
 *    platform_profile probe callback without holding acer->lock.
 *
 * None of these commands touches any cached driver state. The WMI device is
 * resolved at call time, and the ACPI interpreter serializes the AML method
 * execution.
 */

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

/*
 * Predator/Nitro sensor readings. linuwu_sense_gaming_read_sensor() returns
 * the value in the unit of the firmware sensor: degrees Celsius for
 * temperature sensors and revolutions per minute for fan sensors.
 */
int linuwu_sense_gaming_get_supported_sensors(struct acer_wmi *acer,
					      u64 *sensors);
bool linuwu_sense_gaming_sensor_is_supported(
	u64 sensors, enum linuwu_sense_gaming_sensor sensor);
int linuwu_sense_gaming_read_sensor(struct acer_wmi *acer,
				    enum linuwu_sense_gaming_sensor sensor,
				    long *value);

#endif /* _LINUWU_SENSE_GAMING_H_ */

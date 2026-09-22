/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Predator Gaming WMI protocol access for the Acer WMI Laptop Extras driver.
 */
#ifndef _LINUWU_SENSE_GAMING_H_
#define _LINUWU_SENSE_GAMING_H_

#include <linux/platform_profile.h>
#include <linux/types.h>

struct acer_wmi;

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
 * only exception is linuwu_sense_gaming_sensor_is_supported(), which is a
 * pure predicate.
 *
 * None of these commands touches any cached driver state: the WMI device is
 * resolved at call time, and the Acer firmware profile encoding stays inside
 * this backend. acer->lock is the serialization domain for the Acer WMI
 * operations of the control surface, the WMI event path and the Linux
 * subsystem frontends.
 */

/* Whether the machine currently runs on AC power. */
int linuwu_sense_gaming_get_power_source(struct acer_wmi *acer, bool *on_ac);

/* The system function commands of the WMID APGE device. */
int linuwu_sense_gaming_enable_ec_raw(struct acer_wmi *acer);
int linuwu_sense_gaming_enable_launch_manager(struct acer_wmi *acer);
int linuwu_sense_gaming_enable_rf_button(struct acer_wmi *acer);

/*
 * Predator thermal profile. The frontend uses the Linux platform_profile
 * choices, the mapping onto the firmware profile IDs happens inside the
 * backend.
 */
int linuwu_sense_gaming_get_thermal_profile(
	struct acer_wmi *acer, enum platform_profile_option *profile);
int linuwu_sense_gaming_set_thermal_profile(
	struct acer_wmi *acer, enum platform_profile_option profile);
int linuwu_sense_gaming_get_supported_thermal_profiles(struct acer_wmi *acer,
						       unsigned long *choices);

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

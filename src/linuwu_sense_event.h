/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Acer WMI event parsing for the Acer Predator/Nitro WMI driver.
 */
#ifndef _LINUWU_SENSE_EVENT_H_
#define _LINUWU_SENSE_EVENT_H_

#include <linux/types.h>

struct wmi_buffer;

/*
 * Semantic events decoded from the Acer WMID event payload. The mapping from
 * the firmware event encoding onto these types is owned by the parser in
 * linuwu_sense_event.c, the core driver only dispatches on the result.
 */
enum linuwu_sense_event_type {
	LINUWU_SENSE_EVENT_UNKNOWN,
	LINUWU_SENSE_EVENT_HOTKEY,
	LINUWU_SENSE_EVENT_PROFILE_CYCLE,
	LINUWU_SENSE_EVENT_POWER_SOURCE,
	LINUWU_SENSE_EVENT_BATTERY_BOOST,
	LINUWU_SENSE_EVENT_CALIBRATION,
};

/*
 * Decoded representation of an Acer WMID event.
 *
 * @key_num and @device_state are only valid for LINUWU_SENSE_EVENT_HOTKEY.
 * They keep the firmware encoding because the input frontend owns the hotkey
 * keymap. @on_ac is only valid for LINUWU_SENSE_EVENT_POWER_SOURCE and
 * @enabled is only valid for LINUWU_SENSE_EVENT_CALIBRATION.
 */
struct linuwu_sense_event {
	enum linuwu_sense_event_type type;
	u8 key_num;
	u16 device_state;
	bool on_ac;
	bool enabled;
};

/*
 * Validate and decode the payload of an Acer WMID event. The buffer stays
 * owned by the caller and is never modified.
 *
 * Returns 0 on success, -ENODATA when the payload is too short for an event.
 */
int linuwu_sense_event_parse(const struct wmi_buffer *data,
			     struct linuwu_sense_event *event);

#endif /* _LINUWU_SENSE_EVENT_H_ */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Acer WMI event parsing for the Acer Predator/Nitro WMI driver.
 */
#ifndef _LINUWU_SENSE_EVENT_H_
#define _LINUWU_SENSE_EVENT_H_

#include <linux/types.h>

struct wmi_buffer;

/*
 * Decoded representation of an Acer WMID event. @function and @key_num keep
 * the firmware encoding, the interpretation of these values is left to the
 * core driver.
 */
struct linuwu_sense_event {
	u8 function;
	u8 key_num;
	u16 device_state;
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

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer WMI event parsing for the Acer Predator/Nitro WMI driver.
 *
 *  This parser only validates and decodes the firmware event payload. It
 *  neither modifies any driver state nor triggers any action.
 */

#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense_event.h"

/*
 * Payload layout of an Acer WMID event. The layout matches the event data
 * described by the firmware and used by the upstream acer-wmi driver.
 */
struct linuwu_sense_event_data {
	u8 function;
	u8 key_num;
	u16 device_state;
	u16 reserved1;
	u8 kbd_dock_state;
	u8 reserved2;
} __packed;

int linuwu_sense_event_parse(const struct wmi_buffer *data,
			     struct linuwu_sense_event *event)
{
	const struct linuwu_sense_event_data *value;

	if (data->length < sizeof(*value))
		return -ENODATA;

	/* The WMI core guarantees that the event data is 8-byte aligned. */
	value = data->data;

	event->function = value->function;
	event->key_num = value->key_num;
	event->device_state = value->device_state;

	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer WMI event parsing for the Acer Predator/Nitro WMI driver.
 *
 *  This parser validates and decodes the firmware event payload into the
 *  semantic events of linuwu_sense_event.h. It neither modifies any driver
 *  state nor triggers any action.
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

/*
 * Function numbers of the Acer WMID event interface.
 */
enum linuwu_sense_event_function {
	WMID_HOTKEY_EVENT = 0x1,
	WMID_GAMING_TURBO_KEY_EVENT = 0x7,
	WMID_AC_EVENT = 0x8,
	WMID_BATTERY_BOOST_EVENT = 0x9,
	WMID_CALIBRATION_EVENT = 0x0B,
};

/*
 * Key number of the turbo key which cycles the platform profile. The other
 * key numbers of the turbo key event are not handled by this driver.
 */
#define WMID_PROFILE_CYCLE_KEY 0x5

int linuwu_sense_event_parse(const struct wmi_buffer *data,
			     struct linuwu_sense_event *event)
{
	const struct linuwu_sense_event_data *value;

	if (data->length < sizeof(*value))
		return -ENODATA;

	/* The WMI core guarantees that the event data is 8-byte aligned. */
	value = data->data;

	event->type = LINUWU_SENSE_EVENT_UNKNOWN;
	event->key_num = 0;
	event->device_state = 0;
	event->on_ac = false;
	event->enabled = false;

	switch (value->function) {
	case WMID_HOTKEY_EVENT:
		event->type = LINUWU_SENSE_EVENT_HOTKEY;
		event->key_num = value->key_num;
		event->device_state = value->device_state;
		break;
	case WMID_GAMING_TURBO_KEY_EVENT:
		if (value->key_num == WMID_PROFILE_CYCLE_KEY)
			event->type = LINUWU_SENSE_EVENT_PROFILE_CYCLE;
		break;
	case WMID_AC_EVENT:
		/*
		 * The event reports key_num 1 while the AC adapter is
		 * connected and 0 while it is disconnected.
		 */
		if (value->key_num <= 1) {
			event->type = LINUWU_SENSE_EVENT_POWER_SOURCE;
			event->on_ac = value->key_num != 0;
		}
		break;
	case WMID_BATTERY_BOOST_EVENT:
		event->type = LINUWU_SENSE_EVENT_BATTERY_BOOST;
		break;
	case WMID_CALIBRATION_EVENT:
		event->type = LINUWU_SENSE_EVENT_CALIBRATION;
		event->enabled = value->key_num != 0;
		break;
	default:
		break;
	}

	return 0;
}

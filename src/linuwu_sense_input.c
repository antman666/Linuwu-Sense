// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Input (hotkey) frontend for the Acer Predator/Nitro driver.
 *
 *  This module registers the input device for the firmware hotkey events and
 *  reports the events through the input subsystem. It does not decide what a
 *  hotkey does, that policy stays in the core driver.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "linuwu_sense.h"
#include "linuwu_sense_input.h"

/*
 * GUID3 device flags
 */
#define ACER_WMID3_GDS_TOUCHPAD (1 << 1) /* Touchpad */

static const struct key_entry acer_wmi_keymap[] = {
	{ KE_KEY, 0x01, { KEY_WLAN } }, /* WiFi */
	{ KE_KEY, 0x03, { KEY_WLAN } }, /* WiFi */
	{ KE_KEY, 0x04, { KEY_WLAN } }, /* WiFi */
	{ KE_KEY, 0x12, { KEY_BLUETOOTH } }, /* BT */
	{ KE_KEY, 0x21, { KEY_PROG1 } }, /* Backup */
	{ KE_KEY, 0x22, { KEY_PROG2 } }, /* Arcade */
	{ KE_KEY, 0x23, { KEY_PROG3 } }, /* P_Key */
	{ KE_KEY, 0x24, { KEY_PROG4 } }, /* Social networking_Key */
	{ KE_KEY, 0x27, { KEY_HELP } },
	{ KE_KEY, 0x29, { KEY_PROG3 } }, /* P_Key for TM8372 */
	{ KE_IGNORE, 0x41, { KEY_MUTE } },
	{ KE_IGNORE, 0x42, { KEY_PREVIOUSSONG } },
	{ KE_IGNORE, 0x4d, { KEY_PREVIOUSSONG } },
	{ KE_IGNORE, 0x43, { KEY_NEXTSONG } },
	{ KE_IGNORE, 0x4e, { KEY_NEXTSONG } },
	{ KE_IGNORE, 0x44, { KEY_PLAYPAUSE } },
	{ KE_IGNORE, 0x4f, { KEY_PLAYPAUSE } },
	{ KE_IGNORE, 0x45, { KEY_STOP } },
	{ KE_IGNORE, 0x50, { KEY_STOP } },
	{ KE_IGNORE, 0x48, { KEY_VOLUMEUP } },
	{ KE_IGNORE, 0x49, { KEY_VOLUMEDOWN } },
	{ KE_IGNORE, 0x4a, { KEY_VOLUMEDOWN } },
	/*
	 * 0x61 is KEY_SWITCHVIDEOMODE. Usually this is a duplicate input event
	 * with the "Video Bus" input device events. But sometimes it is not
	 * a dup. Map it to KEY_UNKNOWN instead of using KE_IGNORE so that
	 * udev/hwdb can override it on systems where it is not a dup.
	 */
	{ KE_KEY, 0x61, { KEY_UNKNOWN } },
	{ KE_IGNORE, 0x62, { KEY_BRIGHTNESSUP } },
	{ KE_IGNORE, 0x63, { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x64, { KEY_SWITCHVIDEOMODE } }, /* Display Switch */
	{ KE_IGNORE, 0x81, { KEY_SLEEP } },
	{ KE_KEY, 0x82, { KEY_TOUCHPAD_TOGGLE } }, /* Touch Pad Toggle */
	/*
	 * Automatic keyboard background light toggle.
	 */
	{ KE_IGNORE, 0x84, { KEY_KBDILLUMTOGGLE } },
	{ KE_KEY, KEY_TOUCHPAD_ON, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, KEY_TOUCHPAD_OFF, { KEY_TOUCHPAD_OFF } },
	{ KE_IGNORE, 0x83, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY, 0x85, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY, 0x86, { KEY_WLAN } },
	{ KE_KEY, 0x87, { KEY_POWER } },
	{ KE_END, 0 }
};

/* Driver state private to the input frontend */
struct linuwu_sense_input {
	struct input_dev *input_dev;
};

int linuwu_sense_input_init(struct acer_wmi *acer)
{
	struct linuwu_sense_input *input;
	struct input_dev *input_dev;
	int err;

	if (!acer->wdevs[ACER_WMI_GUID_EVENT])
		return 0;

	input = devm_kzalloc(acer->dev, sizeof(*input), GFP_KERNEL);
	if (!input)
		return -ENOMEM;

	input_dev = devm_input_allocate_device(acer->dev);
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = "Acer WMI hotkeys";
	input_dev->phys = "wmi/input0";
	input_dev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(input_dev, acer_wmi_keymap, NULL);
	if (err)
		return err;

	err = input_register_device(input_dev);
	if (err)
		return err;

	input->input_dev = input_dev;
	acer->input = input;

	return 0;
}

void linuwu_sense_input_report_hotkey(struct acer_wmi *acer, u8 key_num,
				      u16 device_state)
{
	struct linuwu_sense_input *input = acer->input;
	const struct key_entry *key;
	u32 scancode;

	if (!input || !input->input_dev)
		return;

	key = sparse_keymap_entry_from_scancode(input->input_dev, key_num);
	if (!key) {
		pr_warn("Unknown key number - 0x%x\n", key_num);
		return;
	}

	scancode = key_num;
	if (key->keycode == KEY_TOUCHPAD_TOGGLE)
		scancode = (device_state & ACER_WMID3_GDS_TOUCHPAD) ?
				   KEY_TOUCHPAD_ON :
				   KEY_TOUCHPAD_OFF;
	sparse_keymap_report_event(input->input_dev, scancode, 1, true);
}

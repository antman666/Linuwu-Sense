// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Four zone keyboard backlight for the Acer Predator/Nitro driver.
 *
 *  This module implements the keyboard backlight commands of the Predator
 *  Gaming WMI interface, one multicolor LED class device per keyboard zone
 *  and the persistence of the backlight state across module loads. The Acer
 *  specific effect settings which the LED subsystem cannot express stay
 *  available through a vendor sysfs attribute. The core driver only calls the
 *  init and save entry points, the state itself is private to this module.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "linuwu_sense.h"
#include "linuwu_sense_keyboard.h"
#include "linuwu_sense_quirks.h"

/*
 * Method IDs of the four zone keyboard backlight
 */
#define ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID 21
#define ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID 20
#define ACER_WMID_SET_GAMING_RGB_KB_METHODID 6
#define ACER_WMID_GET_GAMING_RGB_KB_METHODID 7

#define KB_STATE_FILE "/etc/four_zone_kb_state"

enum linuwu_sense_keyboard_zone {
	LINUWU_SENSE_KEYBOARD_ZONE_1,
	LINUWU_SENSE_KEYBOARD_ZONE_2,
	LINUWU_SENSE_KEYBOARD_ZONE_3,
	LINUWU_SENSE_KEYBOARD_ZONE_4,
	LINUWU_SENSE_KEYBOARD_ZONE_COUNT,
};

/*
 * Per zone selectors of the four zone keyboard backlight
 */
static const u8 acer_wmid_kb_zone_ids[LINUWU_SENSE_KEYBOARD_ZONE_COUNT] = {
	0x1, 0x2, 0x4, 0x8
};

/* Keyboard backlight state as understood by the Predator Gaming interface */
struct linuwu_sense_keyboard_backlight {
	u8 mode;
	u8 speed;
	u8 brightness;
	u8 direction;
	u8 red;
	u8 green;
	u8 blue;
};

struct get_four_zoned_kb_output {
	u8 gmReturn;
	u8 gmOutput[15];
} __packed;

struct per_zone_color {
	u64 zone1, zone2, zone3, zone4;
	int brightness;
} __packed;

struct kb_state {
	u8 per_zone;
	u8 mode;
	u8 speed;
	u8 brightness;
	u8 direction;
	u8 red;
	u8 green;
	u8 blue;
	struct per_zone_color zones;
} __packed;

/*
 * Driver state private to the keyboard backlight.
 *
 * One multicolor LED class device per keyboard zone. The firmware has a
 * single global brightness for all zones, but the LED class models one
 * brightness per LED class device, so the driver maps the LED brightness in
 * software onto the per zone color intensity and keeps the firmware
 * brightness at its maximum while per zone colors are active.
 */
struct linuwu_sense_keyboard_led {
	struct led_classdev_mc mc;
	struct mc_subled subled[3];
	struct linuwu_sense_keyboard *kb;
	enum linuwu_sense_keyboard_zone zone;
};

struct linuwu_sense_keyboard {
	struct acer_wmi *acer;
	struct kb_state state;
	struct linuwu_sense_keyboard_led leds[LINUWU_SENSE_KEYBOARD_ZONE_COUNT];
};

/*
 * LED class device names of the four zones, following the zoned keyboard
 * backlight naming from Documentation/leds/leds-class.rst. The zone names are
 * positional, in the order of the firmware zone selectors.
 */
static const char *const acer_kb_zone_names[LINUWU_SENSE_KEYBOARD_ZONE_COUNT] = {
	"zone1", "zone2", "zone3", "zone4"
};

static int linuwu_sense_keyboard_get_backlight(
	struct acer_wmi *acer, struct linuwu_sense_keyboard_backlight *state)
{
	struct wmi_device *wdev =
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	struct get_four_zoned_kb_output out;
	struct wmi_buffer input = {};
	struct wmi_buffer output = {};
	u64 in = 1;
	int err;

	if (!wdev)
		return -ENODEV;

	input.length = sizeof(in);
	input.data = &in;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(out));
	if (err) {
		pr_err("Unexpected output getting kb zone status: %d\n", err);
		goto out;
	}

	if (output.length < sizeof(out)) {
		err = -EIO;
		goto out;
	}
	memcpy(&out, output.data, sizeof(out));

	state->mode = out.gmOutput[0];
	state->speed = out.gmOutput[1];
	state->brightness = out.gmOutput[2];
	state->direction = out.gmOutput[4];
	state->red = out.gmOutput[5];
	state->green = out.gmOutput[6];
	state->blue = out.gmOutput[7];

out:
	kfree(output.data);
	return err;
}

static int linuwu_sense_keyboard_set_backlight(
	struct acer_wmi *acer,
	const struct linuwu_sense_keyboard_backlight *state)
{
	struct wmi_device *wdev =
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	u8 gmInput[16] = {};
	struct wmi_buffer input = {
		.length = sizeof(gmInput),
		.data = gmInput,
	};
	struct wmi_buffer output = {};
	u64 resp = 0;
	int err;

	if (!wdev)
		return -ENODEV;

	gmInput[0] = state->mode;
	gmInput[1] = state->speed;
	gmInput[2] = state->brightness;
	gmInput[4] = state->direction;
	gmInput[5] = state->red;
	gmInput[6] = state->green;
	gmInput[7] = state->blue;
	gmInput[8] = 3;
	gmInput[9] = 1;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return err;

	memcpy(&resp, output.data, min_t(size_t, output.length, sizeof(resp)));
	kfree(output.data);

	if (resp != 0) {
		pr_err("failed to set keyboard rgb: %llu\n", resp);
		err = -EIO;
	}

	return err;
}

static int linuwu_sense_keyboard_get_zone_color(
	struct acer_wmi *acer, enum linuwu_sense_keyboard_zone zone, u64 *color)
{
	struct wmi_device *wdev =
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	u64 value = acer_wmid_kb_zone_ids[zone];
	u64 result = 0;
	struct wmi_buffer input = {
		.length = sizeof(value),
		.data = &value,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_RGB_KB_METHODID, &input,
				   &output, sizeof(u32));
	if (err)
		goto err_log;

	memcpy(&result, output.data,
	       min_t(size_t, output.length, sizeof(result)));
	kfree(output.data);

	/* The color is stored in the upper three bytes of the response */
	*color = cpu_to_be64(result) >> 32;

	return 0;

err_log:
	pr_err("Error getting kb status (zone %d): %d\n", zone + 1, err);
	return err;
}

static int linuwu_sense_keyboard_set_zone_color(
	struct acer_wmi *acer, enum linuwu_sense_keyboard_zone zone, u64 color)
{
	struct wmi_device *wdev =
		linuwu_sense_endpoint_get(acer, ACER_WMI_GUID_WMID_GAMING);
	u64 value = (cpu_to_be64(color) >> 32) | acer_wmid_kb_zone_ids[zone];
	struct wmi_buffer input = {
		.length = sizeof(value),
		.data = &value,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_RGB_KB_METHODID, &input,
				   &output, sizeof(u32));
	kfree(output.data);
	if (err)
		pr_err("Error setting KB color (zone %d): %d\n", zone + 1, err);

	return err;
}

/*
 * Scale the zone color down to the requested LED brightness and apply it.
 * The firmware brightness is set to its maximum: the per zone brightness is
 * carried by the color intensity, as in the multicolor software scaling
 * model.
 */
static int acer_kb_led_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness brightness)
{
	struct linuwu_sense_keyboard_led *zone = container_of(
		led_cdev, struct linuwu_sense_keyboard_led, mc.led_cdev);
	struct linuwu_sense_keyboard *kb = zone->kb;
	struct acer_wmi *acer = kb->acer;
	struct linuwu_sense_keyboard_backlight state = {
		.brightness = 100,
	};
	u64 color;
	int err;

	led_mc_calc_color_components(&zone->mc, brightness);

	color = ((u64)zone->subled[0].brightness << 16) |
		((u64)zone->subled[1].brightness << 8) |
		(u64)zone->subled[2].brightness;

	mutex_lock(&acer->lock);
	err = linuwu_sense_keyboard_set_backlight(acer, &state);
	if (!err)
		err = linuwu_sense_keyboard_set_zone_color(acer, zone->zone,
							   color);
	mutex_unlock(&acer->lock);

	if (err)
		pr_err("Error applying LED zone %d: %d\n", zone->zone + 1, err);

	return err;
}

static u64 acer_kb_zone_color(const struct per_zone_color *zones, int zone)
{
	switch (zone) {
	case 0:
		return zones->zone1;
	case 1:
		return zones->zone2;
	case 2:
		return zones->zone3;
	default:
		return zones->zone4;
	}
}

static int acer_kb_zone_led_register(struct acer_wmi *acer, int index)
{
	struct linuwu_sense_keyboard *kb = acer->keyboard;
	struct linuwu_sense_keyboard_led *zone = &kb->leds[index];
	unsigned int intensity[3];
	u64 color;
	int i;

	zone->kb = kb;
	zone->zone = index;

	zone->mc.led_cdev.name = devm_kasprintf(acer->dev, GFP_KERNEL,
						"%s:rgb:kbd_zoned_backlight-%s",
						dev_name(acer->dev),
						acer_kb_zone_names[index]);
	if (!zone->mc.led_cdev.name)
		return -ENOMEM;

	color = acer_kb_zone_color(&kb->state.zones, index);
	intensity[0] = (color >> 16) & 0xff;
	intensity[1] = (color >> 8) & 0xff;
	intensity[2] = color & 0xff;

	zone->mc.led_cdev.max_brightness = 255;
	zone->mc.led_cdev.brightness =
		DIV_ROUND_CLOSEST(kb->state.brightness * 255, 100);
	zone->mc.led_cdev.brightness_set_blocking = acer_kb_led_brightness_set;

	zone->subled[0].color_index = LED_COLOR_ID_RED;
	zone->subled[1].color_index = LED_COLOR_ID_GREEN;
	zone->subled[2].color_index = LED_COLOR_ID_BLUE;
	for (i = 0; i < 3; i++) {
		zone->subled[i].intensity = intensity[i];
		zone->subled[i].max_intensity = 255;
		zone->subled[i].channel = i;
	}

	zone->mc.num_colors = 3;
	zone->mc.subled_info = zone->subled;

	return devm_led_classdev_multicolor_register(acer->dev, &zone->mc);
}

static int acer_kb_leds_init(struct acer_wmi *acer)
{
	int i, err;

	for (i = 0; i < LINUWU_SENSE_KEYBOARD_ZONE_COUNT; i++) {
		err = acer_kb_zone_led_register(acer, i);
		if (err)
			return err;
	}

	return 0;
}

/* four zone mode */
static ssize_t four_zoned_rgb_kb_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct linuwu_sense_keyboard_backlight output;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_keyboard_get_backlight(acer, &output);
	mutex_unlock(&acer->lock);
	if (err)
		return -ENODEV;

	return sysfs_emit(buf, "%d,%d,%d,%d,%d,%d,%d\n", output.mode,
			  output.speed, output.brightness, output.direction,
			  output.red, output.green, output.blue);
}

static ssize_t four_zoned_rgb_kb_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct linuwu_sense_keyboard_backlight state;
	int mode, speed, brightness, direction, red, green, blue;
	char input_buf[30];
	char *token;
	char *input_ptr = input_buf;
	ssize_t len;
	int err;

	len = strscpy(input_buf, buf, sizeof(input_buf));
	if (len < 0)
		return len;

	if (len > 0 && input_buf[len - 1] == '\n')
		input_buf[len - 1] = '\0';

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &mode) || mode < 0 || mode > 7) {
		pr_err("Invalid mode value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &speed) || speed < 0 || speed > 9) {
		pr_err("Invalid speed value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &brightness) || brightness < 0 ||
	    brightness > 100) {
		pr_err("Invalid brightness value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &direction) ||
	    (direction <= 0 && (mode == 0x3 || mode == 0x4)) || direction < 0 ||
	    direction > 2) {
		pr_err("Invalid direction value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &red) || red < 0 || red > 255) {
		pr_err("Invalid red value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &green) || green < 0 ||
	    green > 255) {
		pr_err("Invalid green value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &blue) || blue < 0 || blue > 255) {
		pr_err("Invalid blue value.\n");
		return -EINVAL;
	}

	switch (mode) {
	case 0x0: /* Static mode: Ignore speed and direction */
		speed = 0;
		direction = 0;
		break;
	case 0x1: /* Breathing mode: Ignore speed */
		speed = 0;
		direction = 0;
		break;
	case 0x2: /* Neon mode: Ignore red, green, blue, and direction */
		red = 0;
		green = 0;
		blue = 0;
		direction = 0;
		break;
	case 0x3: /* Wave mode: Ignore red, green, and blue */
		red = 0;
		green = 0;
		blue = 0;
		break;
	case 0x4: /* Shifting mode: No restrictions (all values allowed) */
		break;
	case 0x5: /* Zoom mode: Ignore direction */
		direction = 0;
		break;
	case 0x6: /* Meteor mode: Ignore direction */
		direction = 0;
		break;
	case 0x7: /* Twinkling mode: Ignore direction */
		direction = 0;
		break;
	default:
		pr_err("Invalid mode value.\n");
		return -EINVAL;
	}

	state.mode = mode;
	state.speed = speed;
	state.brightness = brightness;
	state.direction = direction;
	state.red = red;
	state.green = green;
	state.blue = blue;

	mutex_lock(&acer->lock);
	err = linuwu_sense_keyboard_set_backlight(acer, &state);
	if (err) {
		mutex_unlock(&acer->lock);
		pr_err("Error setting RGB KB status.\n");
		return -ENODEV;
	}

	/* Set per_zone to 0 */
	acer->keyboard->state.per_zone = 0;
	mutex_unlock(&acer->lock);

	return count;
}

/* Per Zone Mode */

static int get_per_zone_color(struct acer_wmi *acer,
			      struct per_zone_color *output)
{
	struct linuwu_sense_keyboard_backlight state;
	u64 *zones[] = { &output->zone1, &output->zone2, &output->zone3,
			 &output->zone4 };
	int i, err;

	for (i = 0; i < LINUWU_SENSE_KEYBOARD_ZONE_COUNT; i++) {
		err = linuwu_sense_keyboard_get_zone_color(acer, i, zones[i]);
		if (err)
			return err;
	}

	/* Fetching Brighness Value */
	err = linuwu_sense_keyboard_get_backlight(acer, &state);
	if (err) {
		pr_err("get kb status failed!");
		return err;
	}
	output->brightness = state.brightness;

	return 0;
}

static int set_per_zone_color(struct acer_wmi *acer,
			      struct per_zone_color *input)
{
	struct linuwu_sense_keyboard_backlight state = {
		.brightness = input->brightness,
	};
	u64 *zones[] = { &input->zone1, &input->zone2, &input->zone3,
			 &input->zone4 };
	int i, err;

	err = linuwu_sense_keyboard_set_backlight(acer, &state);
	if (err) {
		pr_err("Error setting KB status.\n");
		return err;
	}

	for (i = 0; i < LINUWU_SENSE_KEYBOARD_ZONE_COUNT; i++) {
		err = linuwu_sense_keyboard_set_zone_color(acer, i, *zones[i]);
		if (err)
			return err;
	}
	/* set per_zone to 1*/

	acer->keyboard->state.per_zone = 1;

	return 0;
}

/* BackLight State */

static bool kb_state_valid(const struct kb_state *state)
{
	if (state->per_zone > 1)
		return false;
	if (state->mode > 7 || state->speed > 9)
		return false;
	if (state->brightness > 100 || state->direction > 2)
		return false;
	if (state->zones.brightness < 0 || state->zones.brightness > 100)
		return false;
	if (state->zones.zone1 >= BIT_ULL(24) ||
	    state->zones.zone2 >= BIT_ULL(24) ||
	    state->zones.zone3 >= BIT_ULL(24) ||
	    state->zones.zone4 >= BIT_ULL(24))
		return false;

	return true;
}

static int four_zone_kb_state_update(struct acer_wmi *acer)
{
	struct linuwu_sense_keyboard *kb = acer->keyboard;
	struct linuwu_sense_keyboard_backlight out;
	struct kb_state state = kb->state;
	int err;

	/* Get keyboard status */
	err = linuwu_sense_keyboard_get_backlight(acer, &out);
	if (err) {
		pr_err("get kb status failed!");
		return -EIO;
	}

	state.mode = out.mode;
	state.speed = out.speed;
	state.brightness = out.brightness;
	state.direction = out.direction;
	state.red = out.red;
	state.green = out.green;
	state.blue = out.blue;

	/* Get per-zone color data */
	err = get_per_zone_color(acer, &state.zones);
	if (err) {
		pr_err("get_per_zone_color failed!");
		return -EIO;
	}

	kb->state = state;

	return 0;
}

static int four_zone_kb_state_save(struct acer_wmi *acer)
{
	struct file *file;
	ssize_t len;
	int err;
	struct kb_state state;

	mutex_lock(&acer->lock);
	err = four_zone_kb_state_update(acer);
	if (err) {
		mutex_unlock(&acer->lock);
		return err;
	}
	state = acer->keyboard->state;
	mutex_unlock(&acer->lock);

	file = filp_open(KB_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(file)) {
		pr_err("kb_state_access - Error opening file\n");
		return PTR_ERR(file);
	}

	len = kernel_write(file, (char *)&state, sizeof(state), &file->f_pos);
	if (len < 0)
		pr_err("kb_state_access - Error writing to file: %ld\n", len);

	filp_close(file, NULL);

	if (len != sizeof(state)) {
		pr_err("Failed to write complete state to file\n");
		return -EIO;
	}

	pr_info("kb states saved successfully\n");
	return 0;
}

static int four_zone_kb_state_load(struct acer_wmi *acer)
{
	struct file *file;
	ssize_t len;
	int err;
	struct kb_state state;

	mutex_lock(&acer->lock);

	file = filp_open(KB_STATE_FILE, O_RDONLY, 0);
	if (!IS_ERR(file)) {
		len = kernel_read(file, (char *)&state, sizeof(state),
				  &file->f_pos);
		filp_close(file, NULL);

		if (len != sizeof(state)) {
			pr_err("Incomplete state read\n");
			err = -EIO;
			goto out;
		}

		if (!kb_state_valid(&state)) {
			pr_err("Invalid KB state data\n");
			err = -EINVAL;
			goto out;
		}

		pr_info("KB states loaded\n");
	} else {
		pr_info("KB state file not found!\n");
		err = -ENOENT;
		goto out;
	}

	if (state.per_zone) {
		struct per_zone_color zones = state.zones;

		err = set_per_zone_color(acer, &zones);
		if (err) {
			pr_err("Error setting RGB KB status.\n");
			err = -EIO;
			goto out;
		}
	} else {
		struct linuwu_sense_keyboard_backlight kb = {
			.mode = state.mode,
			.speed = state.speed,
			.brightness = state.brightness,
			.direction = state.direction,
			.red = state.red,
			.green = state.green,
			.blue = state.blue,
		};

		err = linuwu_sense_keyboard_set_backlight(acer, &kb);
		if (err) {
			pr_err("Error setting KB status.\n");
			err = -EIO;
			goto out;
		}
	}

	acer->keyboard->state = state;

	pr_info("KB states restored successfully\n");
	err = 0;

out:
	mutex_unlock(&acer->lock);
	return err;
}

/*
 * Four Zoned Keyboard effect attributes. The zone colors and brightness are
 * exposed through the LED class devices, this attribute only carries the
 * Acer specific effect settings which the LED subsystem cannot express.
 */
static struct device_attribute four_zoned_rgb_mode = __ATTR(
	four_zone_mode, 0644, four_zoned_rgb_kb_show, four_zoned_rgb_kb_store);
static struct attribute *four_zoned_kb_attrs[] = { &four_zoned_rgb_mode.attr,
						   NULL };

/* Four Zoned RGB Keyboard */
static struct attribute_group four_zoned_kb_attr_group = {
	.name = "four_zoned_kb",
	.attrs = four_zoned_kb_attrs
};

int linuwu_sense_keyboard_init(struct acer_wmi *acer)
{
	struct linuwu_sense_keyboard *kb;
	int err;

	if (!acer->quirks->four_zone_kb)
		return 0;

	kb = devm_kzalloc(acer->dev, sizeof(*kb), GFP_KERNEL);
	if (!kb)
		return -ENOMEM;

	kb->acer = acer;
	acer->keyboard = kb;

	err = devm_device_add_group(acer->dev, &four_zoned_kb_attr_group);
	if (err)
		goto err_clear;

	/* Restore the persisted state, then sync the driver view with the EC. */
	four_zone_kb_state_load(acer);

	mutex_lock(&acer->lock);
	if (four_zone_kb_state_update(acer))
		pr_warn("Could not read back the keyboard state\n");
	mutex_unlock(&acer->lock);

	err = acer_kb_leds_init(acer);
	if (err)
		goto err_clear;

	return 0;

err_clear:
	acer->keyboard = NULL;
	return err;
}

void linuwu_sense_keyboard_save_state(struct acer_wmi *acer)
{
	if (!acer->keyboard)
		return;

	four_zone_kb_state_save(acer);
}

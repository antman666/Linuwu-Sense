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

#include <acpi/video.h>
#include <linux/acpi.h>
#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/i8042.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/rfkill.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/units.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>

#include "linuwu_sense_fan.h"

MODULE_AUTHOR("Carlos Corbacho");
MODULE_DESCRIPTION("Acer Laptop WMI Extras Driver");
MODULE_LICENSE("GPL");

/*
 * Magic Number
 * Meaning is unknown - this number is required for writing to ACPI for AMW0
 * (it's also used in acerhk when directly accessing the BIOS)
 */
#define ACER_AMW0_WRITE 0x9610

/*
 * Bit masks for the AMW0 interface
 */
#define ACER_AMW0_WIRELESS_MASK 0x35
#define ACER_AMW0_BLUETOOTH_MASK 0x34
#define ACER_AMW0_MAILLED_MASK 0x31

/*
 * Method IDs for WMID interface
 */
#define ACER_WMID_GET_WIRELESS_METHODID 1
#define ACER_WMID_GET_BLUETOOTH_METHODID 2
#define ACER_WMID_GET_BRIGHTNESS_METHODID 3
#define ACER_WMID_SET_WIRELESS_METHODID 4
#define ACER_WMID_SET_BLUETOOTH_METHODID 5
#define ACER_WMID_SET_BRIGHTNESS_METHODID 6
#define ACER_WMID_GET_THREEG_METHODID 10
#define ACER_WMID_SET_THREEG_METHODID 11
#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

#define ACER_WMID_GET_GAMING_PROFILE_METHODID 3
#define ACER_WMID_SET_GAMING_PROFILE_METHODID 1
#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID 5
#define ACER_WMID_SET_GAMING_MISC_SETTING_METHODID 22
#define ACER_WMID_GET_GAMING_MISC_SETTING_METHODID 23
#define ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID 20
#define ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID 21
#define ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID 21
#define ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID 20
#define ACER_WMID_SET_GAMING_RGB_KB_METHODID 6
#define ACER_WMID_GET_GAMING_RGB_KB_METHODID 7

#define ACER_PREDATOR_V4_FAN_SPEED_READ_BIT_MASK GENMASK(20, 8)
#define ACER_GAMING_MISC_SETTING_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_INDEX_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_VALUE_MASK GENMASK_ULL(15, 8)

#define ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK GENMASK_ULL(7, 0)
#define ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK GENMASK_ULL(15, 8)
#define ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK GENMASK_ULL(23, 8)
#define ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK GENMASK_ULL(39, 24)

/*
 * Acer ACPI method GUIDs
 */
#define AMW0_GUID1 "67C3371D-95A3-4C37-BB61-DD47B491DAAB"
#define AMW0_GUID2 "431F16ED-0C2B-444C-B267-27DEB140CF9C"
#define WMID_GUID1 "6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3"
#define WMID_GUID2 "95764E09-FB56-4E83-B31A-37761F60994A"
#define WMID_GUID3 "61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
#define WMID_GUID4 "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"
#define WMID_GUID5 "79772EC5-04B1-4bfd-843C-61E7F77B6CC9"

#define KB_STATE_FILE "/etc/four_zone_kb_state"
/*
 * Acer ACPI event GUIDs
 */
#define ACERWMID_EVENT_GUID "676AA15E-6A47-4D9F-A2CC-1E6D18D14026"

enum acer_wmi_event_ids {
	WMID_HOTKEY_EVENT = 0x1,
	WMID_ACCEL_OR_KBD_DOCK_EVENT = 0x5,
	WMID_GAMING_TURBO_KEY_EVENT = 0x7,
	WMID_AC_EVENT = 0x8,
	WMID_BATTERY_BOOST_EVENT = 0x9,
	WMID_CALIBRATION_EVENT = 0x0B,
};

enum battery_mode { HEALTH_MODE = 1, CALIBRATION_MODE = 2 };

enum acer_wmi_predator_v4_sys_info_command {
	ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS = 0x0000,
	ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS = 0x02,
	ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING = 0x0001,
	ACER_WMID_CMD_GET_PREDATOR_V4_CPU_FAN_SPEED = 0x0201,
	ACER_WMID_CMD_GET_PREDATOR_V4_GPU_FAN_SPEED = 0x0601,
};

enum acer_wmi_predator_v4_sensor_id {
	ACER_WMID_SENSOR_CPU_TEMPERATURE = 0x01,
	ACER_WMID_SENSOR_CPU_FAN_SPEED = 0x02,
	ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2 = 0x03,
	ACER_WMID_SENSOR_GPU_FAN_SPEED = 0x06,
	ACER_WMID_SENSOR_GPU_TEMPERATURE = 0x0A,
};

enum acer_wmi_gaming_misc_setting {
	ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES = 0x000A,
	ACER_WMID_MISC_SETTING_PLATFORM_PROFILE = 0x000B,
};

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
	{ KE_IGNORE, 0x84, { KEY_KBDILLUMTOGGLE } }, /* Automatic Keyboard
                                                    background light toggle */
	{ KE_KEY, KEY_TOUCHPAD_ON, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, KEY_TOUCHPAD_OFF, { KEY_TOUCHPAD_OFF } },
	{ KE_IGNORE, 0x83, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY, 0x85, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY, 0x86, { KEY_WLAN } },
	{ KE_KEY, 0x87, { KEY_POWER } },
	{ KE_END, 0 }
};

struct event_return_value {
	u8 function;
	u8 key_num;
	u16 device_state;
	u16 reserved1;
	u8 kbd_dock_state;
	u8 reserved2;
} __packed;

/*
 * GUID3 Get Device Status device flags
 */
#define ACER_WMID3_GDS_WIRELESS (1 << 0) /* WiFi */
#define ACER_WMID3_GDS_THREEG (1 << 6) /* 3G */
#define ACER_WMID3_GDS_WIMAX (1 << 7) /* WiMAX */
#define ACER_WMID3_GDS_BLUETOOTH (1 << 11) /* BT */
#define ACER_WMID3_GDS_RFBTN (1 << 14) /* RF Button */

#define ACER_WMID3_GDS_TOUCHPAD (1 << 1) /* Touchpad */

/* Hotkey Customized Setting and Acer Application Status.
 * Set Device Default Value and Report Acer Application Status.
 * When Acer Application starts, it will run this method to inform
 * BIOS/EC that Acer Application is on.
 * App Status
 *	Bit[0]: Launch Manager Status
 *	Bit[1]: ePM Status
 *	Bit[2]: Device Control Status
 *	Bit[3]: Acer Power Button Utility Status
 *	Bit[4]: RF Button Status
 *	Bit[5]: ODD PM Status
 *	Bit[6]: Device Default Value Control
 *	Bit[7]: Hall Sensor Application Status
 */
struct func_input_params {
	u8 function_num; /* Function Number */
	u16 commun_devices; /* Communication type devices default status */
	u16 devices; /* Other type devices default status */
	u8 app_status; /* Acer Device Status. LM, ePM, RF Button... */
	u8 app_mask; /* Bit mask to app_status */
	u8 reserved;
} __packed;

struct func_return_value {
	u8 error_code; /* Error Code */
	u8 ec_return_value; /* EC Return Value */
	u16 reserved;
} __packed;

struct wmid3_gds_set_input_param { /* Set Device Status input parameter */
	u8 function_num; /* Function Number */
	u8 hotkey_number; /* Hotkey Number */
	u16 devices; /* Set Device */
	u8 volume_value; /* Volume Value */
} __packed;

struct wmid3_gds_get_input_param { /* Get Device Status input parameter */
	u8 function_num; /* Function Number */
	u8 hotkey_number; /* Hotkey Number */
	u16 devices; /* Get Device */
} __packed;

struct wmid3_gds_return_value { /* Get Device Status return value*/
	u8 error_code; /* Error Code */
	u8 ec_return_value; /* EC Return Value */
	u16 devices; /* Current Device Status */
	u32 reserved;
} __packed;

struct hotkey_function_type_aa {
	u8 type;
	u8 length;
	u16 handle;
	u16 commun_func_bitmap;
	u16 application_func_bitmap;
	u16 media_func_bitmap;
	u16 display_func_bitmap;
	u16 others_func_bitmap;
	u8 commun_fn_key_number;
} __packed;

/*
 * Interface capability flags
 */
#define ACER_CAP_MAILLED BIT(0)
#define ACER_CAP_WIRELESS BIT(1)
#define ACER_CAP_BLUETOOTH BIT(2)
#define ACER_CAP_BRIGHTNESS BIT(3)
#define ACER_CAP_THREEG BIT(4)
#define ACER_CAP_SET_FUNCTION_MODE BIT(5)
#define ACER_CAP_KBD_DOCK BIT(6)
#define ACER_CAP_TURBO_FAN BIT(9)
#define ACER_CAP_PLATFORM_PROFILE BIT(10)
#define ACER_CAP_FAN_SPEED_READ BIT(11)
#define ACER_CAP_PREDATOR_SENSE BIT(12)
#define ACER_CAP_NITRO_SENSE BIT(13)
#define ACER_CAP_NITRO_SENSE_V4 BIT(14)

/*
 * Interface type flags
 */
enum interface_flags {
	ACER_AMW0,
	ACER_AMW0_V2,
	ACER_WMID,
	ACER_WMID_v2,
};

static int max_brightness = 0xF;

static int mailled = -1;
static int brightness = -1;
static int threeg = -1;
static int force_series;
static int force_caps = -1;
static bool ec_raw_mode;
static bool has_type_aa;
static u16 commun_func_bitmap;
static u8 commun_fn_key_number;
static bool predator_v4;
static bool nitro_v4;

module_param(mailled, int, 0444);
module_param(brightness, int, 0444);
module_param(threeg, int, 0444);
module_param(force_series, int, 0444);
module_param(force_caps, int, 0444);
module_param(ec_raw_mode, bool, 0444);
module_param(predator_v4, bool, 0444);
module_param(nitro_v4, bool, 0444);
MODULE_PARM_DESC(mailled, "Set initial state of Mail LED");
MODULE_PARM_DESC(brightness, "Set initial LCD backlight brightness");
MODULE_PARM_DESC(threeg, "Set initial state of 3G hardware");
MODULE_PARM_DESC(force_series, "Force a different laptop series");
MODULE_PARM_DESC(force_caps, "Force the capability bitmask to this value");
MODULE_PARM_DESC(ec_raw_mode, "Enable EC raw mode");
MODULE_PARM_DESC(
	predator_v4,
	"Enable features for predator laptops that use predator sense v4");
MODULE_PARM_DESC(nitro_v4,
		 "Enable features for nitro laptops that use nitro sense v4");

struct acer_data {
	int mailled;
	int threeg;
	int brightness;
};

struct acer_debug {
	struct dentry *root;
	u32 wmid_devices;
};

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
 * The WMI devices used by this driver. Acer firmware spreads the different
 * parts of the embedded controller interface over several WMI devices, which
 * are all children of the same WMI bus device.
 */
enum acer_wmi_guid {
	ACER_WMI_GUID_AMW0, /* AMW0_GUID1 */
	ACER_WMI_GUID_AMW0_2, /* AMW0_GUID2 */
	ACER_WMI_GUID_WMID, /* WMID_GUID1 */
	ACER_WMI_GUID_WMID_DATA, /* WMID_GUID2 */
	ACER_WMI_GUID_WMID_APGE, /* WMID_GUID3 */
	ACER_WMI_GUID_WMID_GAMING, /* WMID_GUID4 */
	ACER_WMI_GUID_WMID_BATTERY, /* WMID_GUID5 */
	ACER_WMI_GUID_EVENT, /* ACERWMID_EVENT_GUID */
	ACER_WMI_GUID_COUNT,
};

struct acer_wmi;

/*
 * Per WMI device state. Every matching WMI device gets its own instance of
 * this structure, bound to the WMI device via dev_set_drvdata().
 */
struct acer_wmi_wdev {
	struct acer_wmi *acer;
	struct wmi_device *wdev;
	enum acer_wmi_guid guid;
	struct list_head node;
};

/*
 * Per physical device driver state. This structure is shared by all WMI
 * devices belonging to the same WMI bus device. It is allocated by devres on
 * the WMI bus device, so the memory outlives every WMI device and is released
 * only when the WMI bus device itself goes away. The platform device keeps a
 * pointer to it via platform_set_drvdata().
 */
struct acer_wmi {
	struct device *dev; /* the platform device */
	struct platform_device *pdev;
	struct device *parent; /* the WMI bus device */
	struct list_head node;
	struct list_head wdev_list;

	struct wmi_device *wdevs[ACER_WMI_GUID_COUNT];

	/*
	 * Number of WMI devices of this instance that are currently bound to
	 * the driver, plus the instance setup state. All three fields are
	 * protected by acer_wmi_instances_lock.
	 */
	unsigned int wdev_count;
	bool setup_done;
	/* Last setup attempt failed; cleared when a new WMI probe joins. */
	bool setup_failed;

	/* The WMI interface type */
	u32 type;

	/* The capabilities this interface provides */
	u32 capability;

	/* Private data for the current interface */
	struct acer_data data;

	/* debugfs entries associated with this interface */
	struct acer_debug debug;

	/*
	 * Protects the cached fan speed pair, keyboard state, power-source
	 * state and thermal profile shared by the sysfs and WMI event paths.
	 * May be held across ACPI/WMI operations, so it must stay a mutex.
	 */
	struct mutex lock;

	/*
	 * Serializes WMI event processing and prevents it from running while
	 * the platform device is being torn down. Must be a mutex because the
	 * notify path may sleep.
	 */
	struct mutex event_lock;
	bool ready;

	struct input_dev *input_dev;
	struct input_dev *accel_dev;
	acpi_handle gsensor_handle;

	struct led_classdev mail_led;
	struct backlight_device *backlight;

	struct rfkill *wireless_rfkill;
	struct rfkill *bluetooth_rfkill;
	struct rfkill *threeg_rfkill;
	bool rfkill_inited;

	struct device *platform_profile_dev;
	bool platform_profile_support;

	bool on_ac;
	u8 thermal_profile;

	int cpu_fan_speed;
	int gpu_fan_speed;
	struct kb_state current_kb_state;

	u64 supported_sensors;

	struct delayed_work rfkill_work;
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
 * Embedded Controller quirks
 * Some laptops require us to directly access the EC to either enable or query
 * features that are not available through WMI.
 */

struct quirk_entry {
	u8 wireless;
	u8 mailled;
	s8 brightness;
	u8 bluetooth;
	u8 cpu_fans;
	u8 gpu_fans;
	u8 predator_v4;
	u8 nitro_v4;
	u8 nitro_sense;
	u8 four_zone_kb;
};

static struct quirk_entry *quirks;

static void set_quirks(struct acer_wmi *acer)
{
	if (quirks->mailled)
		acer->capability |= ACER_CAP_MAILLED;

	if (quirks->brightness)
		acer->capability |= ACER_CAP_BRIGHTNESS;

	if (quirks->cpu_fans || quirks->gpu_fans)
		acer->capability |= ACER_CAP_TURBO_FAN;

	/* Some acer nitro laptops don't have features like lcd override , boot
     * animation sound so this is used. Think wisely before using any quirks
     * validate your features. */
	if (quirks->nitro_sense == 1) {
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE;
	} else if (quirks->nitro_sense == 2) {
		/* Platform Profile is not found on some older acer nitro models,
             * so we exclude it */
		acer->capability |= ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE;
	}

	if (quirks->predator_v4)
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_PREDATOR_SENSE;

	/* Includes all feature that predatorv4 have*/
	if (quirks->nitro_v4)
		acer->capability |= ACER_CAP_PLATFORM_PROFILE |
				    ACER_CAP_FAN_SPEED_READ |
				    ACER_CAP_NITRO_SENSE_V4;
}

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static int __init set_force_caps(const struct dmi_system_id *dmi)
{
	if (force_caps == -1) {
		force_caps = (uintptr_t)dmi->driver_data;
		pr_info("Found %s, set force_caps to 0x%x\n", dmi->ident,
			force_caps);
	}
	return 1;
}

static struct quirk_entry quirk_unknown = {};

static struct quirk_entry quirk_acer_aspire_1520 = {
	.brightness = -1,
};

static struct quirk_entry quirk_acer_travelmate_2490 = {
	.mailled = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_53 = {
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_phn16_71 = {
	.cpu_fans = 1,
	.gpu_fans = 1,
	.predator_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_predator_phn16_72 = {
	.predator_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_nitro_an16_41 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_nitro_an16_42 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_nitro_anv16_41 = {
	.nitro_v4 = 1,
	.four_zone_kb = 0,
};

static struct quirk_entry quirk_acer_nitro_an16_43 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_nitro_legacy = {
	.nitro_sense = 2,
};

static struct quirk_entry quirk_acer_nitro_an515_58 = {
	.nitro_v4 = 1,
	.four_zone_kb = 1,
};

static struct quirk_entry quirk_acer_nitro = {
	.nitro_sense = 1,
};

static struct quirk_entry quirk_acer_predator_v4 = {
	.predator_v4 = 1,
};

/* This AMW0 laptop has no bluetooth */
static struct quirk_entry quirk_medion_md_98300 = {
	.wireless = 1,
};

static struct quirk_entry quirk_fujitsu_amilo_li_1718 = {
	.wireless = 2,
};

static struct quirk_entry quirk_lenovo_ideapad_s205 = {
	.wireless = 3,
};

static struct quirk_entry quirk_acer_nitro_v4 = {
	.nitro_v4 = 1,
};

/* The Aspire One has a dummy ACPI-WMI interface - disable it */
static const struct dmi_system_id acer_blacklist[] __initconst = {
     {
         .ident = "Acer Aspire One (SSD)",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "AOA110"),
         },
     },
     {
         .ident = "Acer Aspire One (HDD)",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "AOA150"),
         },
     },
     {}
 };

static const struct dmi_system_id amw0_whitelist[] = {
     {
         .ident = "Acer",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
         },
     },
     {
         .ident = "Gateway",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Gateway"),
         },
     },
     {
         .ident = "Packard Bell",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Packard Bell"),
         },
     },
     {}
 };

/*
 * This quirk table is only for Acer/Gateway/Packard Bell family
 * that those machines are supported by acer-wmi driver.
 */
static const struct dmi_system_id acer_quirks[] __initconst = {
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro AN16-43",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-43"),
         },
         .driver_data = &quirk_acer_nitro_an16_43,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro AN16-42",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-42"),
         },
         .driver_data = &quirk_acer_nitro_an16_42,
     },
     {
        .callback = dmi_matched,
        .ident = "Acer Nitro AN515-58",
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
            DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN515-58"),
        },
        .driver_data = &quirk_acer_nitro_an515_58,
    },
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro AN16-41",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN16-41"),
         },
         .driver_data = &quirk_acer_nitro_an16_41,
     },     
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro ANV16-41",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV16-41"),
         },
         .driver_data = &quirk_acer_nitro_anv16_41,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro ANV15-41",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV15-41"),
         },
         .driver_data = &quirk_acer_nitro,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro ANV15-51",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV15-51"),
         },
         .driver_data = &quirk_acer_nitro,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Nitro AN515-55",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN515-55"),
         },
         .driver_data = &quirk_acer_nitro_legacy,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 1360",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1360"),
         },
         .driver_data = &quirk_acer_aspire_1520,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 1520",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1520"),
         },
         .driver_data = &quirk_acer_aspire_1520,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 3100",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3100"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 3610",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3610"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 5100",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5100"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 5610",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5610"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 5630",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5630"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 5650",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5650"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 5680",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5680"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Aspire 9110",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 9110"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer TravelMate 2490",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 2490"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer TravelMate 4200",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 4200"),
         },
         .driver_data = &quirk_acer_travelmate_2490,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Predator PH315-53",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-53"),
         },
         .driver_data = &quirk_acer_predator_ph315_53,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Predator PHN16-71",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Predator PHN16-71"),
         },
         .driver_data = &quirk_acer_predator_phn16_71,
     },
     {
        .callback = dmi_matched,
        .ident = "Acer Predator PHN16-72",
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
            DMI_MATCH(DMI_PRODUCT_NAME, "Predator PHN16-72"),
        },
        .driver_data = &quirk_acer_predator_phn16_72,
    },
     {
         .callback = dmi_matched,
         .ident = "Acer Predator PH16-71",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH16-71"),
         },
         .driver_data = &quirk_acer_predator_v4,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Predator PH18-71",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH18-71"),
         },
         .driver_data = &quirk_acer_predator_v4,
     },
     {
         .callback = dmi_matched,
         .ident = "Acer Predator PTX17-71",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Predator PTX17-71"),
         },
         .driver_data = &quirk_acer_predator_v4,
     },
     {
         .callback = set_force_caps,
         .ident = "Acer Aspire Switch 10E SW3-016",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW3-016"),
         },
         .driver_data = (void *)ACER_CAP_KBD_DOCK,
     },
     {
         .callback = set_force_caps,
         .ident = "Acer Aspire Switch 10 SW5-012",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW5-012"),
         },
         .driver_data = (void *)ACER_CAP_KBD_DOCK,
     },
     {
         .callback = set_force_caps,
         .ident = "Acer Aspire Switch V 10 SW5-017",
         .matches = {
             DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SW5-017"),
         },
         .driver_data = (void *)ACER_CAP_KBD_DOCK,
     },
     {
         .callback = set_force_caps,
         .ident = "Acer One 10 (S1003)",
         .matches = {
             DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
             DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "One S1003"),
         },
         .driver_data = (void *)ACER_CAP_KBD_DOCK,
     },
     {}
 };

/*
 * This quirk list is for those non-acer machines that have AMW0_GUID1
 * but supported by acer-wmi in past days. Keeping this quirk list here
 * is only for backward compatible. Please do not add new machine to
 * here anymore. Those non-acer machines should be supported by
 * appropriate wmi drivers.
 */
static const struct dmi_system_id non_acer_quirks[] __initconst = {
     {
         .callback = dmi_matched,
         .ident = "Fujitsu Siemens Amilo Li 1718",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
             DMI_MATCH(DMI_PRODUCT_NAME, "AMILO Li 1718"),
         },
         .driver_data = &quirk_fujitsu_amilo_li_1718,
     },
     {
         .callback = dmi_matched,
         .ident = "Medion MD 98300",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
             DMI_MATCH(DMI_PRODUCT_NAME, "WAM2030"),
         },
         .driver_data = &quirk_medion_md_98300,
     },
     {
         .callback = dmi_matched,
         .ident = "Lenovo Ideapad S205",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
             DMI_MATCH(DMI_PRODUCT_NAME, "10382LG"),
         },
         .driver_data = &quirk_lenovo_ideapad_s205,
     },
     {
         .callback = dmi_matched,
         .ident = "Lenovo Ideapad S205 (Brazos)",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
             DMI_MATCH(DMI_PRODUCT_NAME, "Brazos"),
         },
         .driver_data = &quirk_lenovo_ideapad_s205,
     },
     {
         .callback = dmi_matched,
         .ident = "Lenovo 3000 N200",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
             DMI_MATCH(DMI_PRODUCT_NAME, "0687A31"),
         },
         .driver_data = &quirk_fujitsu_amilo_li_1718,
     },
     {
         .callback = dmi_matched,
         .ident = "Lenovo Ideapad S205-10382JG",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
             DMI_MATCH(DMI_PRODUCT_NAME, "10382JG"),
         },
         .driver_data = &quirk_lenovo_ideapad_s205,
     },
     {
         .callback = dmi_matched,
         .ident = "Lenovo Ideapad S205-1038DPG",
         .matches = {
             DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
             DMI_MATCH(DMI_PRODUCT_NAME, "1038DPG"),
         },
         .driver_data = &quirk_lenovo_ideapad_s205,
     },
     {}
 };

enum acer_predator_v4_thermal_profile {
	ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET = 0x00,
	ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED = 0x01,
	ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE = 0x04,
	ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO = 0x05,
	ACER_PREDATOR_V4_THERMAL_PROFILE_ECO = 0x06,
};

/* Find which quirks are needed for a particular vendor/ model pair */
static void __init find_quirks(void)
{
	if (predator_v4) {
		quirks = &quirk_acer_predator_v4;
	} else if (nitro_v4) {
		quirks = &quirk_acer_nitro_v4;
	} else if (!force_series) {
		dmi_check_system(acer_quirks);
		dmi_check_system(non_acer_quirks);
	} else if (force_series == 2490) {
		quirks = &quirk_acer_travelmate_2490;
	}

	if (quirks == NULL)
		quirks = &quirk_unknown;
}

/*
 * General interface convenience methods
 */

static bool has_cap(const struct acer_wmi *acer, u32 cap)
{
	return acer->capability & cap;
}

/*
 * AMW0 (V1) interface
 */
struct wmab_args {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
};

struct wmab_ret {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
	u32 eex;
};

static int wmab_execute(struct acer_wmi *acer, struct wmab_args *regbuf,
			struct wmab_ret *ret)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_AMW0];
	struct wmi_buffer in = { .length = sizeof(*regbuf), .data = regbuf };
	struct wmi_buffer out = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, 1, &in, &out,
				   ret ? sizeof(*ret) : 0);
	if (err)
		return err;

	if (ret) {
		if (out.length < sizeof(*ret))
			err = -ENOMSG;
		else
			memcpy(ret, out.data, sizeof(*ret));
	}

	kfree(out.data);

	return err;
}

static acpi_status AMW0_get_u32(struct acer_wmi *acer, u32 *value, u32 cap)
{
	int err;
	u8 result;

	switch (cap) {
	case ACER_CAP_MAILLED:
		switch (quirks->mailled) {
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 7) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_WIRELESS:
		switch (quirks->wireless) {
		case 1:
			err = ec_read(0x7B, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		case 2:
			err = ec_read(0x71, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		case 3:
			err = ec_read(0x78, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 2) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_BLUETOOTH:
		switch (quirks->bluetooth) {
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 4) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_BRIGHTNESS:
		switch (quirks->brightness) {
		default:
			err = ec_read(0x83, &result);
			if (err)
				return AE_ERROR;
			*value = result;
			return AE_OK;
		}
		break;
	default:
		return AE_ERROR;
	}
	return AE_OK;
}

static acpi_status AMW0_set_u32(struct acer_wmi *acer, u32 value, u32 cap)
{
	struct wmab_args args;
	int err;

	args.eax = ACER_AMW0_WRITE;
	args.ebx = value ? (1 << 8) : 0;
	args.ecx = args.edx = 0;

	switch (cap) {
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_MAILLED_MASK;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_WIRELESS_MASK;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_BLUETOOTH_MASK;
		break;
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		switch (quirks->brightness) {
		default:
			if (ec_write(0x83, value))
				return AE_ERROR;
			return AE_OK;
		}
	default:
		return AE_ERROR;
	}

	/* Actually do the set */
	err = wmab_execute(acer, &args, NULL);

	return err ? AE_ERROR : AE_OK;
}

static int AMW0_find_mailled(struct acer_wmi *acer)
{
	struct wmab_args args;
	struct wmab_ret ret;
	int err;

	args.eax = 0x86;
	args.ebx = args.ecx = args.edx = 0;

	err = wmab_execute(acer, &args, &ret);
	if (err)
		return err;

	if (ret.eex & 0x1)
		acer->capability |= ACER_CAP_MAILLED;

	return 0;
}

static const struct acpi_device_id norfkill_ids[] = {
	{ "VPC2004", 0 }, { "IBM0068", 0 },
	{ "LEN0068", 0 }, { "SNY5001", 0 }, /* sony-laptop in charge */
	{ "HPQ6601", 0 }, { "", 0 },
};

static int AMW0_set_cap_acpi_check_device(void)
{
	const struct acpi_device_id *id;

	for (id = norfkill_ids; id->id[0]; id++)
		if (acpi_dev_found(id->id))
			return true;

	return false;
}

static int AMW0_set_capabilities(struct acer_wmi *acer)
{
	struct wmab_args args;
	struct wmab_ret ret;
	int err;

	/*
     * On laptops with this strange GUID (non Acer), normal probing doesn't
     * work.
     */
	if (acer->wdevs[ACER_WMI_GUID_AMW0_2]) {
		if ((quirks != &quirk_unknown) ||
		    !AMW0_set_cap_acpi_check_device())
			acer->capability |= ACER_CAP_WIRELESS;
		return 0;
	}

	args.eax = ACER_AMW0_WRITE;
	args.ecx = args.edx = 0;

	args.ebx = 0xa2 << 8;
	args.ebx |= ACER_AMW0_WIRELESS_MASK;

	err = wmab_execute(acer, &args, &ret);
	if (err)
		return err;

	if (ret.eax & 0x1)
		acer->capability |= ACER_CAP_WIRELESS;

	args.ebx = 2 << 8;
	args.ebx |= ACER_AMW0_BLUETOOTH_MASK;

	err = wmab_execute(acer, &args, &ret);
	if (err)
		return err;

	if (ret.eax & 0x1)
		acer->capability |= ACER_CAP_BLUETOOTH;

	/*
     * This appears to be safe to enable, since all Wistron based laptops
     * appear to use the same EC register for brightness, even if they
     * differ for wireless, etc
     */
	if (quirks->brightness >= 0)
		acer->capability |= ACER_CAP_BRIGHTNESS;

	return 0;
}

/*
 * New interface (The WMID interface)
 */
static acpi_status WMI_execute_u32(struct acer_wmi *acer, u32 method_id, u32 in,
				   u32 *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID];
	struct wmi_buffer input = { .length = sizeof(in), .data = &in };
	struct wmi_buffer result = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, method_id, &input, &result,
				   sizeof(u32));
	if (err)
		return AE_ERROR;

	if (out)
		*out = get_unaligned_le32(result.data);

	kfree(result.data);

	return AE_OK;
}

static acpi_status WMID_get_u32(struct acer_wmi *acer, u32 *value, u32 cap)
{
	acpi_status status;
	u8 tmp;
	u32 result, method_id = 0;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		method_id = ACER_WMID_GET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		method_id = ACER_WMID_GET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_BRIGHTNESS:
		method_id = ACER_WMID_GET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_THREEG:
		method_id = ACER_WMID_GET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (quirks->mailled == 1) {
			ec_read(0x9f, &tmp);
			*value = tmp & 0x1;
			return 0;
		}
		fallthrough;
	default:
		return AE_ERROR;
	}
	status = WMI_execute_u32(acer, method_id, 0, &result);

	if (ACPI_SUCCESS(status))
		*value = (u8)result;

	return status;
}

static acpi_status WMID_set_u32(struct acer_wmi *acer, u32 value, u32 cap)
{
	u32 method_id = 0;
	char param;

	switch (cap) {
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_THREEG:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		if (quirks->mailled == 1) {
			param = value ? 0x92 : 0x93;
			i8042_lock_chip();
			i8042_command(&param, 0x1059);
			i8042_unlock_chip();
			return 0;
		}
		break;
	default:
		return AE_ERROR;
	}
	return WMI_execute_u32(acer, method_id, (u32)value, NULL);
}

static acpi_status wmid3_get_device_status(struct acer_wmi *acer, u32 *value,
					   u16 device)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	struct wmid3_gds_return_value return_value;
	struct wmid3_gds_get_input_param params = {
		.function_num = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices = device,
	};
	struct wmi_buffer input = {
		.length = sizeof(struct wmid3_gds_get_input_param),
		.data = &params,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, 0x2, &input, &output,
				   sizeof(return_value));
	if (err)
		return AE_ERROR;

	memcpy(&return_value, output.data, sizeof(return_value));
	kfree(output.data);

	if (return_value.error_code || return_value.ec_return_value) {
		pr_warn("Get 0x%x Device Status failed: 0x%x - 0x%x\n", device,
			return_value.error_code, return_value.ec_return_value);
		return AE_ERROR;
	}

	*value = !!(return_value.devices & device);

	return AE_OK;
}

static acpi_status wmid_v2_get_u32(struct acer_wmi *acer, u32 *value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_get_device_status(acer, value, device);
}

static acpi_status wmid3_set_device_status(struct acer_wmi *acer, u32 value,
					   u16 device)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	struct wmid3_gds_return_value return_value;
	u16 devices;
	struct wmid3_gds_get_input_param get_params = {
		.function_num = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices = commun_func_bitmap,
	};
	struct wmid3_gds_set_input_param set_params = {
		.function_num = 0x2,
		.hotkey_number = commun_fn_key_number,
		.devices = commun_func_bitmap,
	};
	struct wmi_buffer get_input = {
		.length = sizeof(struct wmid3_gds_get_input_param),
		.data = &get_params,
	};
	struct wmi_buffer set_input = {
		.length = sizeof(struct wmid3_gds_set_input_param),
		.data = &set_params,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, 0x2, &get_input, &output,
				   sizeof(return_value));
	if (err)
		return AE_ERROR;

	memcpy(&return_value, output.data, sizeof(return_value));
	kfree(output.data);

	if (return_value.error_code || return_value.ec_return_value) {
		pr_warn("Get Current Device Status failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
		return AE_ERROR;
	}

	devices = return_value.devices;
	set_params.devices = (value) ? (devices | device) : (devices & ~device);

	err = wmidev_invoke_method(wdev, 0, 0x1, &set_input, &output,
				   sizeof(u32));
	if (err)
		return AE_ERROR;

	return_value.error_code = ((u8 *)output.data)[0];
	return_value.ec_return_value = ((u8 *)output.data)[1];
	kfree(output.data);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Set Device Status failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return AE_OK;
}

static acpi_status wmid_v2_set_u32(struct acer_wmi *acer, u32 value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_set_device_status(acer, value, device);
}

static void type_aa_dmi_decode(const struct dmi_header *header, void *d)
{
	struct acer_wmi *acer = d;
	struct hotkey_function_type_aa *type_aa;

	/* We are looking for OEM-specific Type AAh */
	if (header->type != 0xAA)
		return;

	has_type_aa = true;
	type_aa = (struct hotkey_function_type_aa *)header;

	pr_info("Function bitmap for Communication Button: 0x%x\n",
		type_aa->commun_func_bitmap);
	commun_func_bitmap = type_aa->commun_func_bitmap;

	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_WIRELESS)
		acer->capability |= ACER_CAP_WIRELESS;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_THREEG)
		acer->capability |= ACER_CAP_THREEG;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_BLUETOOTH)
		acer->capability |= ACER_CAP_BLUETOOTH;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_RFBTN)
		commun_func_bitmap &= ~ACER_WMID3_GDS_RFBTN;

	commun_fn_key_number = type_aa->commun_fn_key_number;
}

static int WMID_set_capabilities(struct acer_wmi *acer)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_DATA];
	struct wmi_buffer out = {};
	u32 devices;
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_query_block(wdev, 0, &out, sizeof(u32));
	if (err)
		return err;

	devices = get_unaligned_le32(out.data);
	kfree(out.data);

	pr_info("Function bitmap for Communication Device: 0x%x\n", devices);
	if (devices & 0x07)
		acer->capability |= ACER_CAP_WIRELESS;
	if (devices & 0x40)
		acer->capability |= ACER_CAP_THREEG;
	if (devices & 0x10)
		acer->capability |= ACER_CAP_BLUETOOTH;

	if (!(devices & 0x20))
		max_brightness = 0x9;

	return 0;
}

/*
 * WMID ApgeAction interface
 */

static acpi_status WMI_apgeaction_execute_u64(struct acer_wmi *acer,
					      u32 method_id, u64 in, u64 *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	struct wmi_buffer input = { .length = sizeof(in), .data = &in };
	struct wmi_buffer result = {};
	u64 tmp;
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, method_id, &input, &result,
				   sizeof(u32));
	if (err)
		return AE_ERROR;

	if (result.length >= sizeof(u64))
		tmp = get_unaligned_le64(result.data);
	else
		tmp = get_unaligned_le32(result.data);

	kfree(result.data);

	if (out)
		*out = tmp;

	return AE_OK;
}
/*
 * WMID Gaming interface
 */

static acpi_status WMI_gaming_execute_u64(struct acer_wmi *acer, u32 method_id,
					  u64 in, u64 *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	struct wmi_buffer input = { .length = sizeof(in), .data = &in };
	struct wmi_buffer result = {};
	u64 tmp;
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0, method_id, &input, &result,
				   sizeof(u32));
	if (err)
		return AE_ERROR;

	if (result.length >= sizeof(u64))
		tmp = get_unaligned_le64(result.data);
	else
		tmp = get_unaligned_le32(result.data);

	kfree(result.data);

	if (out)
		*out = tmp;

	return AE_OK;
}

static int WMI_gaming_execute_u32_u64(struct acer_wmi *acer, u32 method_id,
				      u32 in, u64 *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	struct wmi_buffer input = { .length = sizeof(in), .data = &in };
	struct wmi_buffer result = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, method_id, &input, &result,
				   sizeof(u32));
	if (err)
		return err;

	if (result.length >= sizeof(u64))
		*out = get_unaligned_le64(result.data);
	else
		*out = get_unaligned_le32(result.data);

	kfree(result.data);

	return 0;
}

static int WMID_gaming_get_sys_info(struct acer_wmi *acer, u32 command,
				    u64 *out)
{
	acpi_status status;
	u64 result;

	status = WMI_gaming_execute_u64(
		acer, ACER_WMID_GET_GAMING_SYS_INFO_METHODID, command, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK, result))
		return -EIO;

	*out = result;

	return 0;
}

static int
WMID_gaming_set_misc_setting(struct acer_wmi *acer,
			     enum acer_wmi_gaming_misc_setting setting,
			     u8 value)
{
	acpi_status status;
	u64 input = 0;
	u64 result;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, value);

	status = WMI_gaming_execute_u64(
		acer, ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, input,
		&result);
	if (ACPI_FAILURE(status))
		return -EIO;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	return 0;
}

static int
WMID_gaming_get_misc_setting(struct acer_wmi *acer,
			     enum acer_wmi_gaming_misc_setting setting,
			     u8 *value)
{
	u64 input = 0;
	u64 result;
	int ret;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);

	ret = WMI_gaming_execute_u32_u64(
		acer, ACER_WMID_GET_GAMING_MISC_SETTING_METHODID, input,
		&result);
	if (ret < 0)
		return ret;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	*value = FIELD_GET(ACER_GAMING_MISC_SETTING_VALUE_MASK, result);

	return 0;
}

/*
 * Generic Device (interface-independent)
 */

static acpi_status get_u32(struct acer_wmi *acer, u32 *value, u32 cap)
{
	acpi_status status = AE_ERROR;

	switch (acer->type) {
	case ACER_AMW0:
		status = AMW0_get_u32(acer, value, cap);
		break;
	case ACER_AMW0_V2:
		if (cap == ACER_CAP_MAILLED) {
			status = AMW0_get_u32(acer, value, cap);
			break;
		}
		fallthrough;
	case ACER_WMID:
		status = WMID_get_u32(acer, value, cap);
		break;
	case ACER_WMID_v2:
		if (cap &
		    (ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG))
			status = wmid_v2_get_u32(acer, value, cap);
		else if (acer->wdevs[ACER_WMI_GUID_WMID_DATA])
			status = WMID_get_u32(acer, value, cap);
		break;
	}

	return status;
}

static acpi_status set_u32(struct acer_wmi *acer, u32 value, u32 cap)
{
	acpi_status status;

	if (has_cap(acer, cap)) {
		switch (acer->type) {
		case ACER_AMW0:
			return AMW0_set_u32(acer, value, cap);
		case ACER_AMW0_V2:
			if (cap == ACER_CAP_MAILLED)
				return AMW0_set_u32(acer, value, cap);

			/*
                     * On some models, some WMID methods don't toggle
                     * properly. For those cases, we want to run the AMW0
                     * method afterwards to be certain we've really toggled
                     * the device state.
                     */
			if (cap == ACER_CAP_WIRELESS ||
			    cap == ACER_CAP_BLUETOOTH) {
				status = WMID_set_u32(acer, value, cap);
				if (ACPI_FAILURE(status))
					return status;

				return AMW0_set_u32(acer, value, cap);
			}
			fallthrough;
		case ACER_WMID:
			return WMID_set_u32(acer, value, cap);
		case ACER_WMID_v2:
			if (cap & (ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH |
				   ACER_CAP_THREEG))
				return wmid_v2_set_u32(acer, value, cap);
			else if (acer->wdevs[ACER_WMI_GUID_WMID_DATA])
				return WMID_set_u32(acer, value, cap);
			fallthrough;
		default:
			return AE_BAD_PARAMETER;
		}
	}
	return AE_BAD_PARAMETER;
}

static void acer_commandline_init(struct acer_wmi *acer)
{
	/*
     * These will all fail silently if the value given is invalid, or the
     * capability isn't available on the given interface
     */
	if (mailled >= 0)
		set_u32(acer, mailled, ACER_CAP_MAILLED);
	if (!has_type_aa && threeg >= 0)
		set_u32(acer, threeg, ACER_CAP_THREEG);
	if (brightness >= 0)
		set_u32(acer, brightness, ACER_CAP_BRIGHTNESS);
}

/*
 * LED device (Mail LED only, no other LEDs known yet)
 */
static void mail_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value)
{
	struct acer_wmi *acer = dev_get_drvdata(led_cdev->dev->parent);

	set_u32(acer, value, ACER_CAP_MAILLED);
}

static void acer_led_off(void *data)
{
	struct acer_wmi *acer = data;

	set_u32(acer, LED_OFF, ACER_CAP_MAILLED);
}

static int acer_led_init(struct acer_wmi *acer)
{
	int err;

	acer->mail_led.name = "acer-wmi::mail";
	acer->mail_led.brightness_set = mail_led_set;

	err = devm_led_classdev_register(acer->dev, &acer->mail_led);
	if (err)
		return err;

	return devm_add_action_or_reset(acer->dev, acer_led_off, acer);
}

/*
 * Backlight device
 */
static int read_brightness(struct backlight_device *bd)
{
	struct acer_wmi *acer = bl_get_data(bd);
	acpi_status status;
	u32 value;

	status = get_u32(acer, &value, ACER_CAP_BRIGHTNESS);
	if (ACPI_FAILURE(status))
		return -EIO;

	return value;
}

static int update_bl_status(struct backlight_device *bd)
{
	struct acer_wmi *acer = bl_get_data(bd);
	int intensity = backlight_get_brightness(bd);
	acpi_status status;

	status = set_u32(acer, intensity, ACER_CAP_BRIGHTNESS);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static const struct backlight_ops acer_bl_ops = {
	.get_brightness = read_brightness,
	.update_status = update_bl_status,
};

static int acer_backlight_init(struct acer_wmi *acer)
{
	struct backlight_properties props;
	struct backlight_device *bd;
	int err;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = max_brightness;
	bd = devm_backlight_device_register(acer->dev, "acer-wmi", acer->dev,
					    acer, &acer_bl_ops, &props);
	if (IS_ERR(bd)) {
		pr_err("Could not register Acer backlight device\n");
		return PTR_ERR(bd);
	}

	acer->backlight = bd;

	bd->props.power = BACKLIGHT_POWER_ON;

	err = read_brightness(bd);
	if (err < 0) {
		pr_err("Could not read initial brightness\n");
		return err;
	}

	bd->props.brightness = err;
	backlight_update_status(bd);
	return 0;
}

/*
 * Accelerometer device
 */
static int acer_gsensor_init(struct acer_wmi *acer)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj;

	output.length = sizeof(out_obj);
	output.pointer = &out_obj;
	status = acpi_evaluate_object(acer->gsensor_handle, "_INI", NULL,
				      &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int acer_gsensor_open(struct input_dev *input)
{
	struct acer_wmi *acer = input_get_drvdata(input);

	return acer_gsensor_init(acer);
}

static int acer_gsensor_event(struct acer_wmi *acer)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj[5];

	if (!acer->accel_dev)
		return -ENODEV;

	output.length = sizeof(out_obj);
	output.pointer = out_obj;

	status = acpi_evaluate_object(acer->gsensor_handle, "RDVL", NULL,
				      &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (out_obj->type != ACPI_TYPE_PACKAGE)
		return -EIO;

	if (out_obj->package.count != 4)
		return -EIO;

	if (out_obj->package.elements[0].type != ACPI_TYPE_INTEGER ||
	    out_obj->package.elements[1].type != ACPI_TYPE_INTEGER ||
	    out_obj->package.elements[2].type != ACPI_TYPE_INTEGER)
		return -EIO;

	input_report_abs(acer->accel_dev, ABS_X,
			 (s16)out_obj->package.elements[0].integer.value);
	input_report_abs(acer->accel_dev, ABS_Y,
			 (s16)out_obj->package.elements[1].integer.value);
	input_report_abs(acer->accel_dev, ABS_Z,
			 (s16)out_obj->package.elements[2].integer.value);
	input_sync(acer->accel_dev);
	return 0;
}
/* Fan Speed */
static acpi_status acer_set_fan_speed(struct acer_wmi *acer,
				      int t_cpu_fan_speed, int t_gpu_fan_speed);

static acpi_status battery_health_set(struct acer_wmi *acer, u8 function,
				      u8 function_status);

//  static int acer_get_fan_speed(int fan) {
//      if (quirks->predator_v4 || quirks->nitro_sense) {
//          acpi_status status;
//          u64 fanspeed;

//          status = WMI_gaming_execute_u64(
//              ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
//              fan == 0 ? ACER_WMID_CMD_GET_PREDATOR_V4_CPU_FAN_SPEED :
//                     ACER_WMID_CMD_GET_PREDATOR_V4_GPU_FAN_SPEED,
//              &fanspeed);

//          if (ACPI_FAILURE(status))
//              return -EIO;

//          return FIELD_GET(ACER_PREDATOR_V4_FAN_SPEED_READ_BIT_MASK,
//          fanspeed);
//      }
//      return -EOPNOTSUPP;
//  }

static int acer_power_source_refresh_locked(struct acer_wmi *acer)
{
	u64 on_ac;
	int err;

	err = WMID_gaming_get_sys_info(
		acer, ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS, &on_ac);
	if (err)
		return err;

	acer->on_ac = !!on_ac;
	return 0;
}

static u8 acer_default_thermal_profile(bool on_ac)
{
	return on_ac ? ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED :
		       ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
}

static u8 acer_thermal_profile_for_power_transition(bool old_on_ac,
						    bool new_on_ac,
						    u8 current_profile)
{
	if (old_on_ac && !new_on_ac)
		return current_profile ==
				       ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET ?
			       ACER_PREDATOR_V4_THERMAL_PROFILE_ECO :
			       ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;

	if (!old_on_ac && new_on_ac)
		return current_profile == ACER_PREDATOR_V4_THERMAL_PROFILE_ECO ?
			       ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET :
			       ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;

	return current_profile;
}

static u8 acer_next_thermal_profile(bool on_ac, u8 current_profile)
{
	if (!on_ac) {
		if (current_profile == ACER_PREDATOR_V4_THERMAL_PROFILE_ECO)
			return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		return ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
	}

	switch (current_profile) {
	case ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
	default:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	}
}

static u8
acer_platform_profile_to_thermal_profile(enum platform_profile_option profile)
{
	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
	case PLATFORM_PROFILE_QUIET:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
	case PLATFORM_PROFILE_BALANCED:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
	case PLATFORM_PROFILE_PERFORMANCE:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
	default:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	}
}

static u8 acer_normalize_platform_profile(bool on_ac,
					  enum platform_profile_option profile)
{
	if (on_ac) {
		switch (profile) {
		case PLATFORM_PROFILE_LOW_POWER:
		case PLATFORM_PROFILE_QUIET:
			return ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
		case PLATFORM_PROFILE_BALANCED:
			return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
			return ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
		case PLATFORM_PROFILE_PERFORMANCE:
			return ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
		default:
			return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		}
	}

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
	case PLATFORM_PROFILE_QUIET:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
	case PLATFORM_PROFILE_BALANCED:
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
	case PLATFORM_PROFILE_PERFORMANCE:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	default:
		return ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
	}
}

static int acer_apply_thermal_profile_locked(struct acer_wmi *acer, u8 profile)
{
	int err;

	err = WMID_gaming_set_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, profile);
	if (err)
		return err;

	acer->thermal_profile = profile;

	return 0;
}

static bool acer_turbo_fan_supported(struct acer_wmi *acer)
{
	return has_cap(acer, ACER_CAP_TURBO_FAN) &&
	       (quirks->cpu_fans > 0 || quirks->gpu_fans > 0);
}

static int acer_set_turbo_fan_mode_locked(struct acer_wmi *acer, bool turbo)
{
	struct wmi_device *wdev;
	acpi_status status;

	if (!acer_turbo_fan_supported(acer))
		return -EOPNOTSUPP;
	wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	if (!wdev)
		return -ENODEV;

	status = linuwu_sense_fan_set_mode(wdev, quirks->cpu_fans > 0,
					   quirks->gpu_fans > 0,
					   turbo ? LINUWU_SENSE_FAN_MODE_TURBO :
						   LINUWU_SENSE_FAN_MODE_AUTO);

	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int
acer_predator_v4_platform_profile_get(struct device *dev,
				      enum platform_profile_option *profile)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 tp;
	int err;

	mutex_lock(&acer->lock);
	err = WMID_gaming_get_misc_setting(
		acer, ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, &tp);
	if (!err) {
		switch (tp) {
		case ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO:
			*profile = PLATFORM_PROFILE_PERFORMANCE;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE:
			*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED:
			*profile = PLATFORM_PROFILE_BALANCED;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET:
			*profile = PLATFORM_PROFILE_QUIET;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_ECO:
			*profile = PLATFORM_PROFILE_LOW_POWER;
			break;
		default:
			err = -EOPNOTSUPP;
			break;
		}
	}
	if (!err)
		acer->thermal_profile = tp;
	mutex_unlock(&acer->lock);

	return err;
}

static int
acer_predator_v4_platform_profile_set(struct device *dev,
				      enum platform_profile_option profile)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	bool old_on_ac;
	u8 tp;
	int err;

	mutex_lock(&acer->lock);

	if (!quirks->predator_v4) {
		tp = acer_platform_profile_to_thermal_profile(profile);
		err = acer_apply_thermal_profile_locked(acer, tp);
		goto out;
	}

	old_on_ac = acer->on_ac;
	err = acer_power_source_refresh_locked(acer);
	if (err)
		goto out;

	if (old_on_ac != acer->on_ac)
		acer->thermal_profile =
			acer_thermal_profile_for_power_transition(
				old_on_ac, acer->on_ac, acer->thermal_profile);

	tp = acer_normalize_platform_profile(acer->on_ac, profile);
	err = acer_apply_thermal_profile_locked(acer, tp);

out:
	mutex_unlock(&acer->lock);
	return err;
}

static int acer_predator_v4_platform_profile_probe(void *drvdata,
						   unsigned long *choices)
{
	unsigned long supported_profiles = 0;
	int err;

	err = WMID_gaming_get_misc_setting(
		drvdata, ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES,
		(u8 *)&supported_profiles);
	if (err)
		return err;

	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_ECO, &supported_profiles))
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET,
		     &supported_profiles))
		set_bit(PLATFORM_PROFILE_QUIET, choices);
	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED,
		     &supported_profiles))
		set_bit(PLATFORM_PROFILE_BALANCED, choices);
	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE,
		     &supported_profiles))
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO,
		     &supported_profiles))
		set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int acer_thermal_profile_init(struct acer_wmi *acer)
{
	u8 profile;
	int err;

	mutex_lock(&acer->lock);
	err = acer_power_source_refresh_locked(acer);
	if (!err) {
		profile = acer_default_thermal_profile(acer->on_ac);
		err = acer_apply_thermal_profile_locked(acer, profile);
	}
	mutex_unlock(&acer->lock);

	return err;
}

static int acer_thermal_profile_change(struct acer_wmi *acer)
{
	bool old_on_ac;
	u8 next;
	int err;

	if (!quirks->predator_v4)
		return 0;

	mutex_lock(&acer->lock);
	old_on_ac = acer->on_ac;
	err = acer_power_source_refresh_locked(acer);
	if (err)
		goto out;

	if (old_on_ac != acer->on_ac)
		acer->thermal_profile =
			acer_thermal_profile_for_power_transition(
				old_on_ac, acer->on_ac, acer->thermal_profile);

	next = acer_next_thermal_profile(acer->on_ac, acer->thermal_profile);
	err = acer_apply_thermal_profile_locked(acer, next);
out:
	mutex_unlock(&acer->lock);

	if (!err && acer->platform_profile_support)
		platform_profile_notify(acer->platform_profile_dev);

	return err;
}

static const struct platform_profile_ops acer_predator_v4_platform_profile_ops = {
	.probe = acer_predator_v4_platform_profile_probe,
	.profile_get = acer_predator_v4_platform_profile_get,
	.profile_set = acer_predator_v4_platform_profile_set,
};

static int acer_platform_profile_setup(struct acer_wmi *acer)
{
	const int max_retries = 10;
	int delay_ms = 100;

	if (!quirks->predator_v4 && !quirks->nitro_sense && !quirks->nitro_v4)
		return 0;

	for (int attempt = 1; attempt <= max_retries; attempt++) {
		struct device *profile_dev;

		profile_dev = devm_platform_profile_register(
			acer->dev, "acer-wmi", acer,
			&acer_predator_v4_platform_profile_ops);
		if (!IS_ERR(profile_dev)) {
			acer->platform_profile_dev = profile_dev;
			acer->platform_profile_support = true;
			pr_info("Platform profile registered successfully "
				"(attempt %d)\n",
				attempt);
			return 0;
		}
		pr_warn("Platform profile registration failed (attempt %d/%d), "
			"error: %ld\n",
			attempt, max_retries, PTR_ERR(profile_dev));
		if (attempt < max_retries) {
			msleep(delay_ms);
			delay_ms = min(delay_ms * 2, 1000);
		}
	}
	pr_warn("Platform profile setup failed. Continuing to load without "
		"profile support.\n");
	acer->platform_profile_dev = NULL;
	acer->platform_profile_support = false;

	return 0;
}

/*
 * Switch series keyboard dock status
 */
static int acer_kbd_dock_state_to_sw_tablet_mode(u8 kbd_dock_state)
{
	switch (kbd_dock_state) {
	case 0x01: /* Docked, traditional clamshell laptop mode */
		return 0;
	case 0x04: /* Stand-alone tablet */
	case 0x40: /* Docked, tent mode, keyboard not usable */
		return 1;
	default:
		pr_warn("Unknown kbd_dock_state 0x%02x\n", kbd_dock_state);
	}

	return 0;
}

static void acer_kbd_dock_get_initial_state(struct acer_wmi *acer)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	u8 input[8] = {
		0x05,
		0x00,
	};
	struct wmi_buffer input_buf = { .length = sizeof(input),
					.data = input };
	struct wmi_buffer output_buf = {};
	u8 *output;
	int err;
	int sw_tablet_mode;

	if (!wdev)
		return;

	err = wmidev_invoke_method(wdev, 0, 0x2, &input_buf, &output_buf,
				   sizeof(input));
	if (err) {
		pr_err("Error getting keyboard-dock initial status: %d\n", err);
		return;
	}

	output = output_buf.data;
	if (output[0] != 0x00 || (output[3] != 0x05 && output[3] != 0x45)) {
		pr_err("Unexpected output [0]=0x%02x [3]=0x%02x getting "
		       "keyboard-dock initial status\n",
		       output[0], output[3]);
		goto out_free;
	}

	sw_tablet_mode = acer_kbd_dock_state_to_sw_tablet_mode(output[4]);
	if (acer->input_dev)
		input_report_switch(acer->input_dev, SW_TABLET_MODE,
				    sw_tablet_mode);

out_free:
	kfree(output_buf.data);
}

static void acer_kbd_dock_event(struct acer_wmi *acer,
				const struct event_return_value *event)
{
	int sw_tablet_mode;

	if (!has_cap(acer, ACER_CAP_KBD_DOCK) || !acer->input_dev)
		return;

	sw_tablet_mode =
		acer_kbd_dock_state_to_sw_tablet_mode(event->kbd_dock_state);
	input_report_switch(acer->input_dev, SW_TABLET_MODE, sw_tablet_mode);
	input_sync(acer->input_dev);
}

/*
 * Rfkill devices
 */
static void acer_rfkill_update(struct work_struct *work)
{
	struct acer_wmi *acer =
		container_of(work, struct acer_wmi, rfkill_work.work);
	u32 state;
	acpi_status status;

	/*
	 * Serialize with WMI device removal and the notify path, which both
	 * manipulate the sibling WMI devices in acer->wdevs[].
	 */
	mutex_lock(&acer->event_lock);

	if (has_cap(acer, ACER_CAP_WIRELESS)) {
		status = get_u32(acer, &state, ACER_CAP_WIRELESS);
		if (ACPI_SUCCESS(status)) {
			if (quirks->wireless == 3)
				rfkill_set_hw_state(acer->wireless_rfkill,
						    !state);
			else
				rfkill_set_sw_state(acer->wireless_rfkill,
						    !state);
		}
	}

	if (has_cap(acer, ACER_CAP_BLUETOOTH)) {
		status = get_u32(acer, &state, ACER_CAP_BLUETOOTH);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(acer->bluetooth_rfkill, !state);
	}

	if (has_cap(acer, ACER_CAP_THREEG) &&
	    acer->wdevs[ACER_WMI_GUID_WMID_APGE]) {
		status = get_u32(acer, &state, ACER_CAP_THREEG);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(acer->threeg_rfkill, !state);
	}

	mutex_unlock(&acer->event_lock);

	schedule_delayed_work(&acer->rfkill_work, round_jiffies_relative(HZ));
}

struct acer_rfkill_data {
	struct acer_wmi *acer;
	u32 cap;
};

static int acer_rfkill_set(void *data, bool blocked)
{
	struct acer_rfkill_data *rfkill_data = data;
	acpi_status status;

	if (rfkill_data->acer->rfkill_inited) {
		status = set_u32(rfkill_data->acer, !blocked, rfkill_data->cap);
		if (ACPI_FAILURE(status))
			return -ENODEV;
	}

	return 0;
}

static const struct rfkill_ops acer_rfkill_ops = {
	.set_block = acer_rfkill_set,
};

static void acer_rfkill_cleanup(void *data)
{
	struct rfkill *rfkill = data;

	rfkill_unregister(rfkill);
	rfkill_destroy(rfkill);
}

static void acer_rfkill_cancel_work(void *data)
{
	struct acer_wmi *acer = data;

	cancel_delayed_work_sync(&acer->rfkill_work);
}

static struct rfkill *acer_rfkill_register(struct acer_wmi *acer,
					   enum rfkill_type type,
					   const char *name, u32 cap)
{
	struct acer_rfkill_data *rfkill_data;
	struct rfkill *rfkill_dev;
	u32 state;
	acpi_status status;
	int err;

	rfkill_data = devm_kzalloc(acer->dev, sizeof(*rfkill_data), GFP_KERNEL);
	if (!rfkill_data)
		return ERR_PTR(-ENOMEM);

	rfkill_data->acer = acer;
	rfkill_data->cap = cap;

	rfkill_dev = rfkill_alloc(name, acer->dev, type, &acer_rfkill_ops,
				  rfkill_data);
	if (!rfkill_dev)
		return ERR_PTR(-ENOMEM);

	status = get_u32(acer, &state, cap);

	err = rfkill_register(rfkill_dev);
	if (err) {
		rfkill_destroy(rfkill_dev);
		return ERR_PTR(err);
	}

	err = devm_add_action_or_reset(acer->dev, acer_rfkill_cleanup,
				       rfkill_dev);
	if (err)
		return ERR_PTR(err);

	if (ACPI_SUCCESS(status))
		rfkill_set_sw_state(rfkill_dev, !state);

	return rfkill_dev;
}

static int acer_rfkill_init(struct acer_wmi *acer)
{
	int err;

	if (has_cap(acer, ACER_CAP_WIRELESS)) {
		acer->wireless_rfkill = acer_rfkill_register(acer,
							     RFKILL_TYPE_WLAN,
							     "acer-wireless",
							     ACER_CAP_WIRELESS);
		if (IS_ERR(acer->wireless_rfkill))
			return PTR_ERR(acer->wireless_rfkill);
	}

	if (has_cap(acer, ACER_CAP_BLUETOOTH)) {
		acer->bluetooth_rfkill = acer_rfkill_register(
			acer, RFKILL_TYPE_BLUETOOTH, "acer-bluetooth",
			ACER_CAP_BLUETOOTH);
		if (IS_ERR(acer->bluetooth_rfkill))
			return PTR_ERR(acer->bluetooth_rfkill);
	}

	if (has_cap(acer, ACER_CAP_THREEG)) {
		acer->threeg_rfkill = acer_rfkill_register(
			acer, RFKILL_TYPE_WWAN, "acer-threeg", ACER_CAP_THREEG);
		if (IS_ERR(acer->threeg_rfkill))
			return PTR_ERR(acer->threeg_rfkill);
	}

	acer->rfkill_inited = true;

	if ((ec_raw_mode || !acer->wdevs[ACER_WMI_GUID_EVENT]) &&
	    has_cap(acer,
		    ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG)) {
		err = devm_add_action_or_reset(acer->dev,
					       acer_rfkill_cancel_work, acer);
		if (err)
			return err;

		schedule_delayed_work(&acer->rfkill_work,
				      round_jiffies_relative(HZ));
	}

	return 0;
}

static void acer_wmi_notify(struct wmi_device *wdev,
			    const struct wmi_buffer *data)
{
	struct acer_wmi_wdev *wdev_data = dev_get_drvdata(&wdev->dev);
	struct acer_wmi *acer;
	struct event_return_value return_value;
	u16 device_state;
	const struct key_entry *key;
	u32 scancode;

	if (!wdev_data || wdev_data->guid != ACER_WMI_GUID_EVENT)
		return;

	acer = wdev_data->acer;
	if (!acer)
		return;

	if (data->length < sizeof(return_value)) {
		pr_warn("Unknown buffer length %zu\n", data->length);
		return;
	}

	mutex_lock(&acer->event_lock);
	if (!acer->ready) {
		mutex_unlock(&acer->event_lock);
		return;
	}

	return_value = *((const struct event_return_value *)data->data);

	switch (return_value.function) {
	case WMID_HOTKEY_EVENT:
		device_state = return_value.device_state;
		pr_info("device state: 0x%x\n", device_state);

		if (!acer->input_dev)
			break;

		key = sparse_keymap_entry_from_scancode(acer->input_dev,
							return_value.key_num);
		if (!key) {
			pr_warn("Unknown key number - 0x%x\n",
				return_value.key_num);
		} else {
			scancode = return_value.key_num;
			switch (key->keycode) {
			case KEY_WLAN:
			case KEY_BLUETOOTH:
				if (has_cap(acer, ACER_CAP_WIRELESS))
					rfkill_set_sw_state(
						acer->wireless_rfkill,
						!(device_state &
						  ACER_WMID3_GDS_WIRELESS));
				if (has_cap(acer, ACER_CAP_THREEG))
					rfkill_set_sw_state(
						acer->threeg_rfkill,
						!(device_state &
						  ACER_WMID3_GDS_THREEG));
				if (has_cap(acer, ACER_CAP_BLUETOOTH))
					rfkill_set_sw_state(
						acer->bluetooth_rfkill,
						!(device_state &
						  ACER_WMID3_GDS_BLUETOOTH));
				break;
			case KEY_TOUCHPAD_TOGGLE:
				scancode = (device_state &
					    ACER_WMID3_GDS_TOUCHPAD) ?
						   KEY_TOUCHPAD_ON :
						   KEY_TOUCHPAD_OFF;
			}
			sparse_keymap_report_event(acer->input_dev, scancode, 1,
						   true);
		}
		break;
	case WMID_ACCEL_OR_KBD_DOCK_EVENT:
		acer_gsensor_event(acer);
		acer_kbd_dock_event(acer, &return_value);
		break;
	case WMID_GAMING_TURBO_KEY_EVENT:
		if (return_value.key_num == 0x5 &&
		    has_cap(acer, ACER_CAP_PLATFORM_PROFILE))
			acer_thermal_profile_change(acer);
		break;
	case WMID_AC_EVENT:
		if (quirks->predator_v4 &&
		    has_cap(acer, ACER_CAP_PLATFORM_PROFILE)) {
			bool old_on_ac, new_on_ac;
			u8 target;
			int err;

			if (return_value.key_num > 1) {
				pr_info("Unknown AC event key number - %d\n",
					return_value.key_num);
				break;
			}

			new_on_ac = return_value.key_num == 0;

			mutex_lock(&acer->lock);
			old_on_ac = acer->on_ac;
			if (old_on_ac == new_on_ac) {
				err = 0;
			} else {
				target =
					acer_thermal_profile_for_power_transition(
						old_on_ac, new_on_ac,
						acer->thermal_profile);
				acer->on_ac = new_on_ac;
				err = acer_apply_thermal_profile_locked(acer,
									target);
			}
			mutex_unlock(&acer->lock);

			if (!err && old_on_ac != new_on_ac &&
			    acer->platform_profile_support)
				platform_profile_notify(
					acer->platform_profile_dev);
		}
		break;
	case WMID_BATTERY_BOOST_EVENT:
		break;
	case WMID_CALIBRATION_EVENT:
		if (has_cap(acer, ACER_CAP_PREDATOR_SENSE) ||
		    has_cap(acer, ACER_CAP_NITRO_SENSE) ||
		    has_cap(acer, ACER_CAP_NITRO_SENSE_V4)) {
			if (battery_health_set(acer, CALIBRATION_MODE,
					       return_value.key_num) != AE_OK) {
				pr_err("Error changing calibration state\n");
			}
		}
		break;
	default:
		pr_warn("Unknown function number - %d - %d\n",
			return_value.function, return_value.key_num);
		break;
	}

	mutex_unlock(&acer->event_lock);
}

static int wmid3_set_function_mode(struct acer_wmi *acer,
				   struct func_input_params *params,
				   struct func_return_value *return_value)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_APGE];
	struct wmi_buffer input = {
		.length = sizeof(struct func_input_params),
		.data = params,
	};
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return -ENODEV;

	err = wmidev_invoke_method(wdev, 0, 0x1, &input, &output,
				   sizeof(*return_value));
	if (err)
		return err;

	memcpy(return_value, output.data, sizeof(*return_value));
	kfree(output.data);

	return 0;
}

static int acer_wmi_enable_ec_raw(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x00, /* Launch Manager Deactive */
		.app_mask = 0x01,
	};
	int err;

	err = wmid3_set_function_mode(acer, &params, &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling EC raw mode failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
	else
		pr_info("Enabled EC raw mode\n");

	return 0;
}

static int acer_wmi_enable_lm(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x01, /* Launch Manager Active */
		.app_mask = 0x01,
	};
	int err;

	err = wmid3_set_function_mode(acer, &params, &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling Launch Manager failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return 0;
}

static int acer_wmi_enable_rf_button(struct acer_wmi *acer)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x10, /* RF Button Active */
		.app_mask = 0x10,
	};
	int err;

	err = wmid3_set_function_mode(acer, &params, &return_value);
	if (err)
		return err;

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling RF Button failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return 0;
}

static int acer_wmi_accel_setup(struct acer_wmi *acer)
{
	struct acpi_device *adev;
	int err;

	adev = acpi_dev_get_first_match_dev("BST0001", NULL, -1);
	if (!adev)
		return -ENODEV;

	acer->gsensor_handle = acpi_device_handle(adev);
	acpi_dev_put(adev);

	acer->accel_dev = devm_input_allocate_device(acer->dev);
	if (!acer->accel_dev)
		return -ENOMEM;

	acer->accel_dev->open = acer_gsensor_open;

	acer->accel_dev->name = "Acer BMA150 accelerometer";
	acer->accel_dev->phys = "wmi/input1";
	acer->accel_dev->id.bustype = BUS_HOST;
	acer->accel_dev->evbit[0] = BIT_MASK(EV_ABS);
	input_set_abs_params(acer->accel_dev, ABS_X, -16384, 16384, 0, 0);
	input_set_abs_params(acer->accel_dev, ABS_Y, -16384, 16384, 0, 0);
	input_set_abs_params(acer->accel_dev, ABS_Z, -16384, 16384, 0, 0);
	input_set_drvdata(acer->accel_dev, acer);

	err = input_register_device(acer->accel_dev);
	if (err)
		return err;

	return 0;
}

static int acer_wmi_input_setup(struct acer_wmi *acer)
{
	int err;

	acer->input_dev = devm_input_allocate_device(acer->dev);
	if (!acer->input_dev)
		return -ENOMEM;

	acer->input_dev->name = "Acer WMI hotkeys";
	acer->input_dev->phys = "wmi/input0";
	acer->input_dev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(acer->input_dev, acer_wmi_keymap, NULL);
	if (err)
		return err;

	if (has_cap(acer, ACER_CAP_KBD_DOCK))
		input_set_capability(acer->input_dev, EV_SW, SW_TABLET_MODE);

	if (has_cap(acer, ACER_CAP_KBD_DOCK))
		acer_kbd_dock_get_initial_state(acer);

	err = input_register_device(acer->input_dev);
	if (err)
		return err;

	return 0;
}

/*
 * debugfs functions
 */
static u32 get_wmid_devices(struct acer_wmi *acer)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_DATA];
	struct wmi_buffer out = {};
	u32 devices = 0;
	int err;

	if (!wdev)
		return 0;

	err = wmidev_query_block(wdev, 0, &out, sizeof(u32));
	if (err)
		return 0;

	devices = get_unaligned_le32(out.data);
	kfree(out.data);

	return devices;
}

static int acer_wmi_hwmon_init(struct acer_wmi *acer);
static void acer_wmi_debugfs_init(struct acer_wmi *acer);

/*
 * USB Charging
 */
static ssize_t predator_usb_charging_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;

	mutex_lock(&acer->lock);
	status = WMI_apgeaction_execute_u64(acer, ACER_WMID_GET_FUNCTION, 0x4,
					    &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting usb charging status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("usb charging get status: %llu\n", result);
	return sysfs_emit(buf, "%d\n",
			  result == 663296  ? 0 :
			  result == 659200  ? 10 :
			  result == 1314560 ? 20 :
			  result == 1969920 ? 30 :
					      -1); //-1 means unknown value
}

static ssize_t predator_usb_charging_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 10) && (val != 20) && (val != 30))
		return -EINVAL;
	pr_info("usb charging set value: %d\n", val);
	mutex_lock(&acer->lock);
	status = WMI_apgeaction_execute_u64(
		acer, ACER_WMID_SET_FUNCTION,
		val == 0  ? 663300 :
		val == 10 ? 659204 :
		val == 20 ? 1314564 :
		val == 30 ? 1969924 :
			    663300,
		&result); // if unkown value then turn it off.
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting usb charging status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("usb charging set status: %llu\n", result);
	return count;
}

/*
 * Battery Limit (80%)
 * Battery Calibration
 */
struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

static acpi_status battery_health_query(struct acer_wmi *acer, int mode,
					int *enabled)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_BATTERY];
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = 0x1,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct get_battery_health_control_status_output ret;
	struct wmi_buffer input = {
		.length =
			sizeof(struct get_battery_health_control_status_input),
		.data = &params,
	};
	struct wmi_buffer output = {};
	acpi_status status = AE_ERROR;
	int err;

	pr_info("battery health query: %d\n", mode);

	if (!wdev)
		return AE_ERROR;

	mutex_lock(&acer->lock);

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
		&input, &output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output getting battery health status: %d\n",
		       err);
		goto out;
	}

	memcpy(&ret, output.data, sizeof(ret));

	if (mode == HEALTH_MODE)
		*enabled = ret.uFunctionStatus[0];
	else if (mode == CALIBRATION_MODE)
		*enabled = ret.uFunctionStatus[1];
	else
		goto out;

	status = AE_OK;

out:
	kfree(output.data);
	mutex_unlock(&acer->lock);
	return status;
}

static acpi_status battery_health_set(struct acer_wmi *acer, u8 function,
				      u8 function_status)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_BATTERY];
	struct set_battery_health_control_input params = {
		.uBatteryNo = 0x1,
		.uFunctionMask = function,
		.uFunctionStatus = function_status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct set_battery_health_control_output ret;
	struct wmi_buffer input = {
		.length = sizeof(struct set_battery_health_control_input),
		.data = &params,
	};
	struct wmi_buffer output = {};
	acpi_status status = AE_ERROR;
	int err;

	pr_info("%s: %d | %d\n", __func__, function, function_status);

	if (!wdev)
		return AE_ERROR;

	mutex_lock(&acer->lock);

	err = wmidev_invoke_method(
		wdev, 0, ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID, &input,
		&output, sizeof(ret));
	if (err) {
		pr_err("Unexpected output setting battery health status: %d\n",
		       err);
		goto out;
	}

	memcpy(&ret, output.data, sizeof(ret));

	if (ret.uReturn != 0 && ret.uReservedOut != 0) {
		pr_err("Failed to set battery health status\n");
		goto out;
	}

	status = AE_OK;

out:
	kfree(output.data);
	mutex_unlock(&acer->lock);
	return status;
}

static ssize_t predator_battery_limit_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int enabled;
	acpi_status status = battery_health_query(acer, HEALTH_MODE, &enabled);

	if (ACPI_FAILURE(status))
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t predator_battery_limit_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;

	if ((val != 0) && (val != 1))
		return -EINVAL;

	if (battery_health_set(acer, HEALTH_MODE, val) != AE_OK)
		return -ENODEV;

	return count;
}

static ssize_t predator_battery_calibration_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int enabled;
	acpi_status status =
		battery_health_query(acer, CALIBRATION_MODE, &enabled);

	if (ACPI_FAILURE(status))
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t preadtor_battery_calibration_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;

	if ((val != 0) && (val != 1))
		return -EINVAL;

	if (battery_health_set(acer, CALIBRATION_MODE, val) != AE_OK)
		return -ENODEV;

	return count;
}

/*
 * FAN CONTROLS
 */
static acpi_status acer_set_fan_speed(struct acer_wmi *acer,
				      int t_cpu_fan_speed, int t_gpu_fan_speed)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	acpi_status status;

	if (!wdev)
		return AE_ERROR;

	if (t_cpu_fan_speed == 100 && t_gpu_fan_speed == 100) {
		pr_info("MAX FAN MODE!\n");
		status = linuwu_sense_fan_set_mode(wdev, quirks->cpu_fans > 0,
						   quirks->gpu_fans > 0,
						   LINUWU_SENSE_FAN_MODE_TURBO);
		if (ACPI_FAILURE(status)) {
			pr_err("Error setting fan speed status: %s\n",
			       acpi_format_exception(status));
			return AE_ERROR;
		}
	} else if (t_cpu_fan_speed == 0 && t_gpu_fan_speed == 0) {
		pr_info("AUTO FAN MODE!\n");
		status = linuwu_sense_fan_set_mode(wdev, quirks->cpu_fans > 0,
						   quirks->gpu_fans > 0,
						   LINUWU_SENSE_FAN_MODE_AUTO);
		if (ACPI_FAILURE(status)) {
			pr_err("Error setting fan speed status: %s\n",
			       acpi_format_exception(status));
			return AE_ERROR;
		}
	} else if (t_cpu_fan_speed <= 100 && t_gpu_fan_speed <= 100) {
		if (t_cpu_fan_speed == 0) {
			pr_info("CUSTOM FAN MODE (GPU)\n");
			status = linuwu_sense_fan_set_mode(
				wdev, quirks->cpu_fans > 0, false,
				LINUWU_SENSE_FAN_MODE_AUTO);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_mode(
				wdev, false, quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_GPU, t_gpu_fan_speed);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}
		} else if (t_gpu_fan_speed == 0) {
			pr_info("CUSTOM FAN MODE (CPU)\n");
			status = linuwu_sense_fan_set_mode(
				wdev, false, quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_AUTO);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_mode(
				wdev, quirks->cpu_fans > 0, false,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_CPU, t_cpu_fan_speed);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}
		} else {
			pr_info("CUSTOM FAN MODE (MIXED)!\n");
			status = linuwu_sense_fan_set_mode(
				wdev, quirks->cpu_fans > 0,
				quirks->gpu_fans > 0,
				LINUWU_SENSE_FAN_MODE_CUSTOM);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_CPU, t_cpu_fan_speed);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}

			status = linuwu_sense_fan_set_speed(
				wdev, LINUWU_SENSE_FAN_GPU, t_gpu_fan_speed);
			if (ACPI_FAILURE(status)) {
				pr_err("Error setting fan speed status: %s\n",
				       acpi_format_exception(status));
				return AE_ERROR;
			}
		}
	} else {
		return AE_ERROR;
	}

	acer->cpu_fan_speed = t_cpu_fan_speed;
	acer->gpu_fan_speed = t_gpu_fan_speed;
	pr_info("Fan speeds updated: CPU=%d, GPU=%d\n", acer->cpu_fan_speed,
		acer->gpu_fan_speed);

	return AE_OK;
}

static ssize_t predator_fan_speed_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&acer->lock);
	ret = sysfs_emit(buf, "%d,%d\n", acer->cpu_fan_speed,
			 acer->gpu_fan_speed);
	mutex_unlock(&acer->lock);

	return ret;
}

static ssize_t predator_fan_speed_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int t_cpu_fan_speed, t_gpu_fan_speed;

	char input[9];
	char *token;
	char *input_ptr = input;
	ssize_t len;

	len = strscpy(input, buf, sizeof(input));
	if (len < 0)
		return len;

	if (len > 0 && input[len - 1] == '\n')
		input[len - 1] = '\0';

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_cpu_fan_speed) ||
	    t_cpu_fan_speed < 0 || t_cpu_fan_speed > 100) {
		pr_err("Invalid CPU speed value.\n");
		return -EINVAL;
	}

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_gpu_fan_speed) ||
	    t_gpu_fan_speed < 0 || t_gpu_fan_speed > 100) {
		pr_err("Invalid GPU speed value.\n");
		return -EINVAL;
	}

	acpi_status status;

	mutex_lock(&acer->lock);
	status = acer_set_fan_speed(acer, t_cpu_fan_speed, t_gpu_fan_speed);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		return -ENODEV;
	}

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
	acpi_status status;

	if (!acer_turbo_fan_supported(acer))
		return -EOPNOTSUPP;

	mutex_lock(&acer->lock);
	wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	if (!wdev) {
		status = AE_ERROR;
		goto out;
	}

	if (quirks->cpu_fans > 0) {
		status = linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_CPU,
						   &mode);
		if (ACPI_FAILURE(status))
			goto out;
		have_fan = true;
		if (mode != LINUWU_SENSE_FAN_MODE_TURBO)
			enabled = false;
	}

	if (quirks->gpu_fans > 0) {
		status = linuwu_sense_fan_get_mode(wdev, LINUWU_SENSE_FAN_GPU,
						   &mode);
		if (ACPI_FAILURE(status))
			goto out;
		have_fan = true;
		if (mode != LINUWU_SENSE_FAN_MODE_TURBO)
			enabled = false;
	}

	status = have_fan ? AE_OK : AE_BAD_PARAMETER;
out:
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status))
		return -EIO;

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
 *LCD OVERRIDE CONTROLS
 */
static ssize_t predator_lcd_override_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;

	mutex_lock(&acer->lock);
	status = WMI_gaming_execute_u64(
		acer, ACER_WMID_GET_GAMING_PROFILE_METHODID, 0x00, &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting lcd override status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("lcd override get status: %llu\n", result);
	return sysfs_emit(buf, "%d\n",
			  result == 0x1000001000000 ? 1 :
			  result == 0x1000000	    ? 0 :
						      -1);
}

static ssize_t predator_lcd_override_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;
	pr_info("lcd_override set value: %d\n", val);
	mutex_lock(&acer->lock);
	status = WMI_gaming_execute_u64(acer,
					ACER_WMID_SET_GAMING_PROFILE_METHODID,
					val == 1 ? 0x1000000000010 : 0x10,
					&result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting lcd override status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("lcd override set status: %llu\n", result);
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
	acpi_status status;
	u64 result;

	mutex_lock(&acer->lock);
	status = WMI_apgeaction_execute_u64(acer, ACER_WMID_GET_FUNCTION,
					    0x88401, &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting backlight_timeout status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("backlight_timeout get status: %llu\n", result);
	return sysfs_emit(buf, "%d\n",
			  result == 0x1E0000080000 ? 1 :
			  result == 0x80000	   ? 0 :
						     -1);
}

static ssize_t predator_backlight_timeout_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;
	pr_info("bascklight_timeout set value: %d\n", val);
	mutex_lock(&acer->lock);
	status = WMI_apgeaction_execute_u64(acer, ACER_WMID_SET_FUNCTION,
					    val == 1 ? 0x1E0000088402 : 0x88402,
					    &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting backlight_timeout status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("backlight_timeout set status: %llu\n", result);
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
	acpi_status status;
	u64 result;

	mutex_lock(&acer->lock);
	status = WMI_gaming_execute_u64(
		acer, ACER_WMID_GET_GAMING_MISC_SETTING_METHODID, 0x6, &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting boot_animation_sound status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("boot_animation_sound get status: %llu\n", result);
	return sysfs_emit(buf, "%d\n",
			  result == 0x100 ? 1 :
			  result == 0	  ? 0 :
					    -1);
}

static ssize_t
predator_boot_animation_sound_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	u64 result;
	u8 val;

	if (kstrtou8(buf, 10, &val))
		return -EINVAL;
	if ((val != 0) && (val != 1))
		return -EINVAL;
	pr_info("boot_animation_sound set value: %d\n", val);
	mutex_lock(&acer->lock);
	status = WMI_gaming_execute_u64(
		acer, ACER_WMID_SET_GAMING_MISC_SETTING_METHODID,
		val == 1 ? 0x106 : 0x6, &result);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting boot_animation_sound status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	pr_info("boot_animation_sound set status: %llu\n", result);
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
	       preadtor_battery_calibration_store);
static struct device_attribute battery_limiter =
	__ATTR(battery_limiter, 0644, predator_battery_limit_show,
	       predator_battery_limit_store);
static struct device_attribute fan_speed = __ATTR(
	fan_speed, 0644, predator_fan_speed_show, predator_fan_speed_store);
static struct device_attribute turbo_mode = __ATTR(
	turbo_mode, 0644, predator_turbo_mode_show, predator_turbo_mode_store);
static struct device_attribute lcd_override =
	__ATTR(lcd_override, 0644, predator_lcd_override_show,
	       predator_lcd_override_store);
static struct attribute *predator_sense_attrs[] = { &lcd_override.attr,
						    &fan_speed.attr,
						    &turbo_mode.attr,
						    &battery_limiter.attr,
						    &battery_calibration.attr,
						    &usb_charging.attr,
						    &backlight_timeout.attr,
						    &boot_animation_sound.attr,
						    NULL };

static umode_t predator_sense_attr_is_visible(struct kobject *kobj,
					      struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct acer_wmi *acer = dev_get_drvdata(dev);

	if (attr == &turbo_mode.attr &&
	    (!acer || !acer_turbo_fan_supported(acer)))
		return 0;

	return attr->mode;
}

static struct attribute_group preadtor_sense_attr_group = {
	.name = "predator_sense",
	.attrs = predator_sense_attrs,
	.is_visible = predator_sense_attr_is_visible,
};

static struct attribute *nitro_sense_v4_attrs[] = {
	&lcd_override.attr,	    &fan_speed.attr,
	&battery_limiter.attr,	    &battery_calibration.attr,
	&usb_charging.attr,	    &backlight_timeout.attr,
	&boot_animation_sound.attr, NULL
};

static struct attribute_group nitro_sense_v4_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_v4_attrs
};

/* nitro sense attributes */
static struct attribute *nitro_sense_attrs[] = {
	&fan_speed.attr,    &battery_limiter.attr,   &battery_calibration.attr,
	&usb_charging.attr, &backlight_timeout.attr, NULL
};
static struct attribute_group nitro_sense_attr_group = {
	.name = "nitro_sense",
	.attrs = nitro_sense_attrs
};

/* Four Zoned Keyboard  */

struct get_four_zoned_kb_output {
	u8 gmReturn;
	u8 gmOutput[15];
} __packed;

static acpi_status set_kb_status(struct acer_wmi *acer, int mode, int speed,
				 int brightness, int direction, int red,
				 int green, int blue)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 resp = 0;
	u8 gmInput[16] = { mode,  speed, brightness, 0, direction, red,
			   green, blue,	 3,	     1, 0,	   0,
			   0,	  0,	 0,	     0 };
	struct wmi_buffer input = { .length = sizeof(gmInput),
				    .data = gmInput };
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_SET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(u32));
	if (err)
		return AE_ERROR;

	if (output.length >= sizeof(u64))
		resp = get_unaligned_le64(output.data);
	else
		resp = get_unaligned_le32(output.data);

	kfree(output.data);

	if (resp != 0) {
		pr_err("failed to set keyboard rgb: %llu\n", resp);
		return AE_ERROR;
	}

	return AE_OK;
}

static acpi_status get_kb_status(struct acer_wmi *acer,
				 struct get_four_zoned_kb_output *out)
{
	struct wmi_device *wdev = acer->wdevs[ACER_WMI_GUID_WMID_GAMING];
	u64 in = 1;
	struct wmi_buffer input = { .length = sizeof(in), .data = &in };
	struct wmi_buffer output = {};
	int err;

	if (!wdev)
		return AE_ERROR;

	err = wmidev_invoke_method(wdev, 0,
				   ACER_WMID_GET_GAMING_KB_BACKLIGHT_METHODID,
				   &input, &output, sizeof(*out));
	if (err) {
		pr_err("Unexpected output getting kb zone status: %d\n", err);
		return AE_ERROR;
	}

	memcpy(out, output.data, sizeof(*out));
	kfree(output.data);

	return AE_OK;
}

/* KB Backlight State  */;

/* four zone mode */
static ssize_t four_zoned_rgb_kb_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	struct get_four_zoned_kb_output output;

	mutex_lock(&acer->lock);
	status = get_kb_status(acer, &output);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting kb status: %s\n",
		       acpi_format_exception(status));
		return -ENODEV;
	}
	return sysfs_emit(buf, "%d,%d,%d,%d,%d,%d,%d\n", output.gmOutput[0],
			  output.gmOutput[1], output.gmOutput[2],
			  output.gmOutput[4], output.gmOutput[5],
			  output.gmOutput[6], output.gmOutput[7]);
}

static ssize_t four_zoned_rgb_kb_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;

	int mode, speed, brightness, direction, red, green, blue;
	char input_buf[30];
	char *token;
	char *input_ptr = input_buf;
	ssize_t len;

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
	    ((direction <= 0) && (mode == 0x3 || mode == 0x4)) ||
	    direction < 0 || direction > 2) {
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
	case 0x0: // Static mode: Ignore speed and direction
		speed = 0;
		direction = 0;
		break;
	case 0x1: // Breathing mode: Ignore speed
		speed = 0;
		direction = 0;
		break;
	case 0x2: // Neon mode: Ignore red, green, blue, and direction
		red = 0;
		green = 0;
		blue = 0;
		direction = 0;
		break;
	case 0x3: // Wave mode: Ignore red, green, and blue
		red = 0;
		green = 0;
		blue = 0;
		break;
	case 0x4: // Shifting mode: No restrictions (all values allowed)
		break;
	case 0x5: // Zoom mode: Ignore direction
		direction = 0;
		break;
	case 0x6: // Meteor mode: Ignore direction
		direction = 0;
		break;
	case 0x7: // Twinkling mode: Ignore direction
		direction = 0;
		break;
	default:
		pr_err("Invalid mode value.\n");
		return -EINVAL;
	}

	mutex_lock(&acer->lock);
	status = set_kb_status(acer, mode, speed, brightness, direction, red,
			       green, blue);
	if (ACPI_FAILURE(status)) {
		mutex_unlock(&acer->lock);
		pr_err("Error setting RGB KB status.\n");
		return -ENODEV;
	}

	/* Set per_zone to 0 */
	acer->current_kb_state.per_zone = 0;
	mutex_unlock(&acer->lock);

	return count;
}

/* Per Zone Mode */

static acpi_status get_per_zone_color(struct acer_wmi *acer,
				      struct per_zone_color *output)
{
	acpi_status status;
	u64 *zones[] = { &output->zone1, &output->zone2, &output->zone3,
			 &output->zone4 };
	u8 zone_ids[] = { 0x1, 0x2, 0x4, 0x8 };

	for (int i = 0; i < 4; i++) {
		status = WMI_gaming_execute_u64(
			acer, ACER_WMID_GET_GAMING_RGB_KB_METHODID, zone_ids[i],
			zones[i]);
		if (ACPI_FAILURE(status)) {
			pr_err("Error getting kb status (zone %d): %s\n", i + 1,
			       acpi_format_exception(status));
			return status;
		}
		*zones[i] = cpu_to_be64(*zones[i]) >> 32;
	}

	/* Fetching Brighness Value */
	struct get_four_zoned_kb_output out;
	status = get_kb_status(acer, &out);
	if (ACPI_FAILURE(status)) {
		pr_err("get kb status failed!");
		return status;
	}
	output->brightness = out.gmOutput[2];

	return AE_OK;
}

static acpi_status set_per_zone_color(struct acer_wmi *acer,
				      struct per_zone_color *input)
{
	acpi_status status;
	u64 *zones[] = { &input->zone1, &input->zone2, &input->zone3,
			 &input->zone4 };
	u8 zone_ids[] = { 0x1, 0x2, 0x4, 0x8 };

	status = set_kb_status(acer, 0, 0, input->brightness, 0, 0, 0, 0);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting KB status.\n");
		return status;
	}

	for (int i = 0; i < 4; i++) {
		*zones[i] = (cpu_to_be64(*zones[i]) >> 32) | zone_ids[i];
		status = WMI_gaming_execute_u64(
			acer, ACER_WMID_SET_GAMING_RGB_KB_METHODID, *zones[i],
			NULL);
		if (ACPI_FAILURE(status)) {
			pr_err("Error setting KB color (zone %d): %s\n", i + 1,
			       acpi_format_exception(status));
			return status;
		}
	}
	/* set per_zone to 1*/

	acer->current_kb_state.per_zone = 1;

	return status;
}

static ssize_t per_zoned_rgb_kb_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct per_zone_color output;
	acpi_status status;

	mutex_lock(&acer->lock);
	status = get_per_zone_color(acer, &output);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		return -ENODEV;
	}
	return sysfs_emit(buf, "%06llx,%06llx,%06llx,%06llx,%d\n", output.zone1,
			  output.zone2, output.zone3, output.zone4,
			  output.brightness);
}

static ssize_t per_zoned_rgb_kb_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	int i = 0;
	ssize_t len;
	char *token;
	char str_buf[34];
	struct per_zone_color colors;
	char *input_ptr = str_buf;

	len = strscpy(str_buf, buf, sizeof(str_buf));
	if (len < 0)
		return len;

	if (len > 0 && str_buf[len - 1] == '\n')
		str_buf[len - 1] = '\0';

	acpi_status status;

	/* zone1,zone2,zone3,zone4 */

	while ((token = strsep(&input_ptr, ",")) && i < 4) {
		if (strlen(token) != 6) {
			pr_err("Invalid rgb length: %s (%lu) (must be 3 bytes)\n",
			       token, strlen(token));
			return -EINVAL;
		}
		if (kstrtoull(token, 16, &((u64 *)&colors)[i])) {
			pr_err("Invalid hex value: %s\n", token);
			return -EINVAL;
		}
		i++;
	}

	if (!token || kstrtoint(token, 10, &colors.brightness) ||
	    colors.brightness < 0 || colors.brightness > 100) {
		pr_err("Invalid brightness value.\n");
		return -EINVAL;
	}

	/* set per zone colors */
	mutex_lock(&acer->lock);
	status = set_per_zone_color(acer, &colors);
	mutex_unlock(&acer->lock);
	if (ACPI_FAILURE(status)) {
		pr_err("Error setting RGB KB status.\n");
		return -ENODEV;
	}
	return count;
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
	acpi_status status;
	struct get_four_zoned_kb_output out;
	struct kb_state state = acer->current_kb_state;

	// Get keyboard status
	status = get_kb_status(acer, &out);
	if (ACPI_FAILURE(status)) {
		pr_err("get kb status failed!");
		return -EIO;
	}

	state.mode = out.gmOutput[0];
	state.speed = out.gmOutput[1];
	state.brightness = out.gmOutput[2];
	state.direction = out.gmOutput[4];
	state.red = out.gmOutput[5];
	state.green = out.gmOutput[6];
	state.blue = out.gmOutput[7];

	// Get per-zone color data
	status = get_per_zone_color(acer, &state.zones);
	if (ACPI_FAILURE(status)) {
		pr_err("get_per_zone_color failed!");
		return -EIO;
	}

	acer->current_kb_state = state;

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
	state = acer->current_kb_state;
	mutex_unlock(&acer->lock);

	file = filp_open(KB_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(file)) {
		pr_err("kb_state_access - Error opening file\n");
		return PTR_ERR(file);
	}

	len = kernel_write(file, (char *)&state, sizeof(state), &file->f_pos);
	if (len < 0) {
		pr_err("kb_state_access - Error writing to file: %ld\n", len);
	}

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
	acpi_status status;
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

		status = set_per_zone_color(acer, &zones);
		if (ACPI_FAILURE(status)) {
			pr_err("Error setting RGB KB status.\n");
			err = -EIO;
			goto out;
		}
	} else {
		status = set_kb_status(acer, state.mode, state.speed,
				       state.brightness, state.direction,
				       state.red, state.green, state.blue);
		if (ACPI_FAILURE(status)) {
			pr_err("Error setting KB status.\n");
			err = -EIO;
			goto out;
		}
	}

	acer->current_kb_state = state;

	pr_info("KB states restored successfully\n");
	err = 0;

out:
	mutex_unlock(&acer->lock);
	return err;
}

/* Four Zoned Keyboard Attributes */
static struct device_attribute four_zoned_rgb_mode = __ATTR(
	four_zone_mode, 0644, four_zoned_rgb_kb_show, four_zoned_rgb_kb_store);
static struct device_attribute per_zoned_rgb_mode = __ATTR(
	per_zone_mode, 0644, per_zoned_rgb_kb_show, per_zoned_rgb_kb_store);
static struct attribute *four_zoned_kb_attrs[] = { &four_zoned_rgb_mode.attr,
						   &per_zoned_rgb_mode.attr,
						   NULL };

/* Four Zoned RGB Keyboard */
static struct attribute_group four_zoned_kb_attr_group = {
	.name = "four_zoned_kb",
	.attrs = four_zoned_kb_attrs
};
/*
 * Platform device
 */

static int acer_platform_probe(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);
	int err;

	if (!acer)
		return -ENODEV;

	if (acer->wdevs[ACER_WMI_GUID_EVENT]) {
		err = acer_wmi_input_setup(acer);
		if (err) {
			pr_err("Unable to set up input device\n");
			return err;
		}

		err = acer_wmi_accel_setup(acer);
		if (err && err != -ENODEV)
			pr_warn("Cannot enable accelerometer\n");
	}

	if (has_cap(acer, ACER_CAP_MAILLED)) {
		err = acer_led_init(acer);
		if (err)
			return err;
	}

	if (has_cap(acer, ACER_CAP_BRIGHTNESS)) {
		err = acer_backlight_init(acer);
		if (err)
			return err;
	}

	err = acer_rfkill_init(acer);
	if (err)
		return err;

	if (has_cap(acer, ACER_CAP_PLATFORM_PROFILE)) {
		if (quirks->predator_v4) {
			err = acer_thermal_profile_init(acer);
			if (err)
				return err;
		}
		acer_platform_profile_setup(acer);
	}

	if (has_cap(acer, ACER_CAP_TURBO_FAN)) {
		mutex_lock(&acer->lock);
		err = acer_set_turbo_fan_mode_locked(acer, false);
		mutex_unlock(&acer->lock);
		if (err && err != -EOPNOTSUPP)
			pr_warn("Unable to initialize gaming fan mode: %d\n",
				err);
	}

	if (has_cap(acer, ACER_CAP_PREDATOR_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &preadtor_sense_attr_group);
		if (err)
			return err;
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE_V4)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_v4_attr_group);
		if (err)
			return err;
	}
	if (has_cap(acer, ACER_CAP_NITRO_SENSE)) {
		err = devm_device_add_group(&pdev->dev,
					    &nitro_sense_attr_group);
		if (err)
			return err;
	}

	if (quirks->four_zone_kb) {
		err = devm_device_add_group(&pdev->dev,
					    &four_zoned_kb_attr_group);
		if (err)
			return err;
		four_zone_kb_state_load(acer);
	}

	if (has_cap(acer, ACER_CAP_FAN_SPEED_READ)) {
		err = acer_wmi_hwmon_init(acer);
		if (err)
			return err;
	}

	if (acer->wdevs[ACER_WMI_GUID_WMID_DATA])
		acer_wmi_debugfs_init(acer);

	/* Override any initial settings with values from the commandline */
	acer_commandline_init(acer);

	mutex_lock(&acer->event_lock);
	acer->ready = true;
	mutex_unlock(&acer->event_lock);

	return 0;
}

static void acer_platform_remove(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);

	if (!acer)
		return;

	/*
	 * Stop the WMI notify path before any resource is released. The
	 * event_lock makes sure that no notify_new() callback is still
	 * running when the devres-managed resources are torn down below.
	 */
	mutex_lock(&acer->event_lock);
	acer->ready = false;
	mutex_unlock(&acer->event_lock);

	if (quirks->four_zone_kb)
		four_zone_kb_state_save(acer);

	/*
	 * All remaining resources (input, LED, backlight, rfkill, hwmon,
	 * platform profile, sysfs groups and debugfs) are devres-managed and
	 * are released by the driver core after this callback returned.
	 */
}

#ifdef CONFIG_PM_SLEEP
static int acer_suspend(struct device *dev)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u32 value;
	acpi_status status;
	struct acer_data *data;

	if (!acer)
		return -ENODEV;

	data = &acer->data;

	if (has_cap(acer, ACER_CAP_MAILLED)) {
		status = get_u32(acer, &value, ACER_CAP_MAILLED);
		if (ACPI_FAILURE(status)) {
			pr_err("Error reading mail LED state: %s\n",
			       acpi_format_exception(status));
			return -EIO;
		}

		status = set_u32(acer, LED_OFF, ACER_CAP_MAILLED);
		if (ACPI_FAILURE(status)) {
			pr_err("Error turning off mail LED: %s\n",
			       acpi_format_exception(status));
			return -EIO;
		}

		data->mailled = value;
	}

	if (has_cap(acer, ACER_CAP_BRIGHTNESS)) {
		status = get_u32(acer, &value, ACER_CAP_BRIGHTNESS);
		if (ACPI_FAILURE(status)) {
			pr_err("Error reading brightness: %s\n",
			       acpi_format_exception(status));
			return -EIO;
		}

		data->brightness = value;
	}

	return 0;
}

static int acer_resume(struct device *dev)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	acpi_status status;
	struct acer_data *data;

	if (!acer)
		return -ENODEV;

	data = &acer->data;

	if (has_cap(acer, ACER_CAP_MAILLED)) {
		status = set_u32(acer, data->mailled, ACER_CAP_MAILLED);
		if (ACPI_FAILURE(status)) {
			pr_err("Error restoring mail LED state: %s\n",
			       acpi_format_exception(status));
			return -EIO;
		}
	}

	if (has_cap(acer, ACER_CAP_BRIGHTNESS)) {
		status = set_u32(acer, data->brightness, ACER_CAP_BRIGHTNESS);
		if (ACPI_FAILURE(status)) {
			pr_err("Error restoring brightness: %s\n",
			       acpi_format_exception(status));
			return -EIO;
		}
	}

	if (acer->accel_dev) {
		int err = acer_gsensor_init(acer);

		if (err) {
			pr_err("Error initializing accelerometer: %d\n", err);
			return err;
		}
	}

	return 0;
}
#else
#define acer_suspend NULL
#define acer_resume NULL
#endif

static SIMPLE_DEV_PM_OPS(acer_pm, acer_suspend, acer_resume);

static void acer_platform_shutdown(struct platform_device *pdev)
{
	struct acer_wmi *acer = platform_get_drvdata(pdev);

	if (!acer)
		return;

	if (has_cap(acer, ACER_CAP_MAILLED))
		set_u32(acer, LED_OFF, ACER_CAP_MAILLED);
}

static struct platform_driver acer_platform_driver = {
	.driver = {
		.name = "acer-wmi",
		.pm = &acer_pm,
	},
	.probe = acer_platform_probe,
	.remove = acer_platform_remove,
	.shutdown = acer_platform_shutdown,
};

/*
 * debugfs functions
 */
static void acer_wmi_debugfs_remove(void *data)
{
	struct acer_wmi *acer = data;

	debugfs_remove_recursive(acer->debug.root);
	acer->debug.root = NULL;
}

static void acer_wmi_debugfs_init(struct acer_wmi *acer)
{
	int err;

	acer->debug.wmid_devices = get_wmid_devices(acer);
	acer->debug.root = debugfs_create_dir("acer-wmi", NULL);
	if (IS_ERR_OR_NULL(acer->debug.root)) {
		acer->debug.root = NULL;
		return;
	}

	debugfs_create_u32("devices", 0444, acer->debug.root,
			   &acer->debug.wmid_devices);

	err = devm_add_action_or_reset(acer->dev, acer_wmi_debugfs_remove,
				       acer);
	if (err)
		dev_warn(acer->dev, "Unable to register debugfs cleanup\n");
}

static const enum acer_wmi_predator_v4_sensor_id
	acer_wmi_temp_channel_to_sensor_id[] = {
		[0] = ACER_WMID_SENSOR_CPU_TEMPERATURE,
		[1] = ACER_WMID_SENSOR_GPU_TEMPERATURE,
		[2] = ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2,
	};

static const enum acer_wmi_predator_v4_sensor_id
	acer_wmi_fan_channel_to_sensor_id[] = {
		[0] = ACER_WMID_SENSOR_CPU_FAN_SPEED,
		[1] = ACER_WMID_SENSOR_GPU_FAN_SPEED,
	};

static umode_t acer_wmi_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	enum acer_wmi_predator_v4_sensor_id sensor_id;
	const struct acer_wmi *acer = data;

	switch (type) {
	case hwmon_temp:
		sensor_id = acer_wmi_temp_channel_to_sensor_id[channel];
		break;
	case hwmon_fan:
		sensor_id = acer_wmi_fan_channel_to_sensor_id[channel];
		break;
	default:
		return 0;
	}

	if (acer->supported_sensors & BIT(sensor_id - 1))
		return 0444;

	return 0;
}

static int acer_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	u64 command = ACER_WMID_CMD_GET_PREDATOR_V4_SENSOR_READING;
	u64 result;
	int ret;

	if (!acer)
		return -ENODEV;

	switch (type) {
	case hwmon_temp:
		command |=
			FIELD_PREP(ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK,
				   acer_wmi_temp_channel_to_sensor_id[channel]);

		ret = WMID_gaming_get_sys_info(acer, command, &result);
		if (ret < 0)
			return ret;

		result = FIELD_GET(ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK,
				   result);
		*val = result * MILLIDEGREE_PER_DEGREE;
		return 0;
	case hwmon_fan:
		command |=
			FIELD_PREP(ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK,
				   acer_wmi_fan_channel_to_sensor_id[channel]);

		ret = WMID_gaming_get_sys_info(acer, command, &result);
		if (ret < 0)
			return ret;

		*val = FIELD_GET(ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK,
				 result);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const acer_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT), NULL
};

static const struct hwmon_ops acer_wmi_hwmon_ops = {
	.read = acer_wmi_hwmon_read,
	.is_visible = acer_wmi_hwmon_is_visible,
};

static const struct hwmon_chip_info acer_wmi_hwmon_chip_info = {
	.ops = &acer_wmi_hwmon_ops,
	.info = acer_wmi_hwmon_info,
};

static int acer_wmi_hwmon_init(struct acer_wmi *acer)
{
	struct device *hwmon;
	u64 result;
	int ret;

	ret = WMID_gaming_get_sys_info(
		acer, ACER_WMID_CMD_GET_PREDATOR_V4_SUPPORTED_SENSORS, &result);
	if (ret < 0)
		return ret;

	/* Return early if no sensors are available */
	acer->supported_sensors =
		FIELD_GET(ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK, result);
	if (!acer->supported_sensors)
		return 0;

	hwmon = devm_hwmon_device_register_with_info(
		acer->dev, "acer", acer, &acer_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon)) {
		dev_err(acer->dev, "Could not register acer hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

/*
 * WMI device handling
 */
static const struct wmi_device_id acer_wmi_id_table[] = {
	{ AMW0_GUID1, (const void *)(uintptr_t)ACER_WMI_GUID_AMW0 },
	{ AMW0_GUID2, (const void *)(uintptr_t)ACER_WMI_GUID_AMW0_2 },
	{ WMID_GUID1, (const void *)(uintptr_t)ACER_WMI_GUID_WMID },
	{ WMID_GUID2, (const void *)(uintptr_t)ACER_WMI_GUID_WMID_DATA },
	{ WMID_GUID3, (const void *)(uintptr_t)ACER_WMI_GUID_WMID_APGE },
	{ WMID_GUID4, (const void *)(uintptr_t)ACER_WMI_GUID_WMID_GAMING },
	{ WMID_GUID5, (const void *)(uintptr_t)ACER_WMI_GUID_WMID_BATTERY },
	{ ACERWMID_EVENT_GUID, (const void *)(uintptr_t)ACER_WMI_GUID_EVENT },
	{}
};
MODULE_DEVICE_TABLE(wmi, acer_wmi_id_table);

static int acer_wmi_instance_setup(struct acer_wmi *acer);

static struct acer_wmi *acer_wmi_instance_find(struct device *parent)
{
	struct acer_wmi *acer;

	lockdep_assert_held(&acer_wmi_instances_lock);

	list_for_each_entry(acer, &acer_wmi_instances, node)
		if (acer->parent == parent)
			return acer;

	return NULL;
}

static struct acer_wmi *acer_wmi_instance_create(struct device *parent)
{
	struct acer_wmi *acer;

	lockdep_assert_held(&acer_wmi_instances_lock);

	/*
	 * The state is shared by all WMI devices of this WMI bus device, so
	 * it is owned by the WMI bus device itself. This guarantees that the
	 * memory stays around until the last WMI device has been removed.
	 */
	acer = devm_kzalloc(parent, sizeof(*acer), GFP_KERNEL);
	if (!acer)
		return NULL;

	acer->parent = parent;
	mutex_init(&acer->lock);
	mutex_init(&acer->event_lock);
	INIT_LIST_HEAD(&acer->node);
	INIT_LIST_HEAD(&acer->wdev_list);
	INIT_DELAYED_WORK(&acer->rfkill_work, acer_rfkill_update);
	list_add_tail(&acer->node, &acer_wmi_instances);

	return acer;
}

/*
 * All WMI devices of a WMI bus device are registered before any of them is
 * bound to a driver (the WMI core adds a device link for this), but they are
 * probed one after another. The interface type and the available features
 * can only be detected from the complete set of WMI devices, so an instance
 * is set up once all matching WMI devices have probed. The WMI core names
 * WMI devices after their GUID, which allows to count the matching devices
 * below the WMI bus device.
 */
static int acer_wmi_count_matching_wdevs(struct device *dev, void *data)
{
	unsigned int *count = data;
	const struct wmi_device_id *id;

	for (id = acer_wmi_id_table; id->guid_string[0]; id++) {
		if (!strncasecmp(dev_name(dev), id->guid_string,
				 strlen(id->guid_string))) {
			(*count)++;
			break;
		}
	}

	return 0;
}

/*
 * Remove a per WMI bus device driver state from the registry and tear down
 * the platform-facing interfaces. The memory itself is owned by the WMI bus
 * device and released by devres, so it must not be freed here.
 */
static void acer_wmi_instance_teardown(struct acer_wmi *acer)
{
	lockdep_assert_held(&acer_wmi_instances_lock);

	if (acer->pdev) {
		platform_device_unregister(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
	}

	list_del_init(&acer->node);
}

static int acer_wmi_wdev_probe(struct wmi_device *wdev, const void *context)
{
	enum acer_wmi_guid guid = (enum acer_wmi_guid)(uintptr_t)context;
	struct acer_wmi_wdev *wdev_data;
	struct acer_wmi *acer;
	unsigned int present = 0;
	int err;

	mutex_lock(&acer_wmi_instances_lock);

	if (acer_wmi_shutting_down) {
		err = -ENODEV;
		goto out_unlock;
	}

	acer = acer_wmi_instance_find(wdev->dev.parent);
	if (!acer) {
		acer = acer_wmi_instance_create(wdev->dev.parent);
		if (!acer) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}

	/*
	 * setup_failed records a failed attempt, not a permanent instance
	 * state. A later WMI probe may complete the set and retry setup.
	 */
	wdev_data = devm_kzalloc(&wdev->dev, sizeof(*wdev_data), GFP_KERNEL);
	if (!wdev_data) {
		err = -ENOMEM;
		goto out_maybe_destroy;
	}

	wdev_data->acer = acer;
	wdev_data->wdev = wdev;
	wdev_data->guid = guid;
	INIT_LIST_HEAD(&wdev_data->node);
	dev_set_drvdata(&wdev->dev, wdev_data);

	/* A GUID identifies a WMI device type, not a unique device. */
	list_add_tail(&wdev_data->node, &acer->wdev_list);
	if (!acer->wdevs[guid])
		acer->wdevs[guid] = wdev;
	else
		dev_warn(&wdev->dev, "Multiple WMI devices share this GUID\n");

	acer->wdev_count++;
	acer->setup_failed = false;

	if (acer->setup_done) {
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	device_for_each_child(acer->parent, &present,
			      acer_wmi_count_matching_wdevs);

	/* Wait for the remaining WMI devices of this instance to probe. */
	if (acer->wdev_count < present) {
		mutex_unlock(&acer_wmi_instances_lock);
		return 0;
	}

	err = acer_wmi_instance_setup(acer);
	if (err) {
		/*
		 * This WMI device must remain unbound, but keep the instance
		 * alive while already-bound siblings are still present. A later
		 * probe can retry setup after completing the device set.
		 */
		acer->setup_failed = true;
		list_del_init(&wdev_data->node);
		acer->wdev_count--;
		if (acer->wdevs[guid] == wdev) {
			struct acer_wmi_wdev *replacement;

			acer->wdevs[guid] = NULL;
			list_for_each_entry(replacement, &acer->wdev_list,
					    node) {
				if (replacement->guid == guid) {
					acer->wdevs[guid] = replacement->wdev;
					break;
				}
			}
		}
		dev_set_drvdata(&wdev->dev, NULL);
		goto out_maybe_destroy;
	}

	acer->setup_done = true;
	mutex_unlock(&acer_wmi_instances_lock);
	return 0;

out_maybe_destroy:
	/* Drop an instance which never got any WMI device */
	if (!acer->wdev_count && !acer->pdev)
		acer_wmi_instance_teardown(acer);
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
	 * Serialize with the notify path and the rfkill polling work, which
	 * access the sibling WMI devices through acer->wdevs[].
	 */
	mutex_lock(&acer->event_lock);
	list_del_init(&wdev_data->node);
	if (acer->wdevs[wdev_data->guid] == wdev) {
		struct acer_wmi_wdev *replacement;

		acer->wdevs[wdev_data->guid] = NULL;
		list_for_each_entry(replacement, &acer->wdev_list, node) {
			if (replacement->guid == wdev_data->guid) {
				acer->wdevs[wdev_data->guid] =
					replacement->wdev;
				break;
			}
		}
	}
	mutex_unlock(&acer->event_lock);

	if (acer->wdev_count)
		acer->wdev_count--;

	/* Tear down the instance once its last WMI device is gone. */
	if (!acer->wdev_count)
		acer_wmi_instance_teardown(acer);

	mutex_unlock(&acer_wmi_instances_lock);
}

static struct wmi_driver acer_wmi_driver = {
	.driver = {
		.name = "acer-wmi",
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

	/*
	 * The AMW0_GUID1 wmi is not only found on Acer family but also other
	 * machines like Lenovo, Fujitsu and Medion. In the past days,
	 * acer-wmi driver handled those non-Acer machines by quirks list.
	 * But actually acer-wmi driver was loaded on any machines that have
	 * AMW0_GUID1. This behavior is strange because those machines should
	 * be supported by appropriate wmi drivers. e.g. fujitsu-laptop,
	 * ideapad-laptop. So, here checks the machine that has AMW0_GUID1
	 * should be in Acer/Gateway/Packard Bell white list, or it's already
	 * in the past quirk list.
	 */
	if (acer->wdevs[ACER_WMI_GUID_AMW0] &&
	    !dmi_check_system(amw0_whitelist) && quirks == &quirk_unknown) {
		pr_debug(
			"Unsupported machine has AMW0_GUID1, unable to load\n");
		return -ENODEV;
	}

	/*
	 * Detect which ACPI-WMI interface we're using.
	 */
	if (acer->wdevs[ACER_WMI_GUID_AMW0] && acer->wdevs[ACER_WMI_GUID_WMID])
		acer->type = ACER_AMW0_V2;

	if (!acer->wdevs[ACER_WMI_GUID_AMW0] && acer->wdevs[ACER_WMI_GUID_WMID])
		acer->type = ACER_WMID;

	if (acer->wdevs[ACER_WMI_GUID_WMID_APGE])
		acer->type = ACER_WMID_v2;

	if (acer->type)
		dmi_walk(type_aa_dmi_decode, acer);

	if (acer->wdevs[ACER_WMI_GUID_WMID_DATA] && acer->type) {
		if (!has_type_aa && WMID_set_capabilities(acer)) {
			pr_err("Unable to detect available WMID devices\n");
			return -ENODEV;
		}
		/* WMID always provides brightness methods */
		acer->capability |= ACER_CAP_BRIGHTNESS;
	} else if (!acer->wdevs[ACER_WMI_GUID_WMID_DATA] && acer->type &&
		   !has_type_aa && force_caps == -1) {
		pr_err("No WMID device detection method found\n");
		return -ENODEV;
	}

	if (acer->wdevs[ACER_WMI_GUID_AMW0] &&
	    !acer->wdevs[ACER_WMI_GUID_WMID]) {
		acer->type = ACER_AMW0;

		err = AMW0_set_capabilities(acer);
		if (err) {
			pr_err("Unable to detect available AMW0 devices\n");
			return err;
		}
	}

	if (acer->wdevs[ACER_WMI_GUID_AMW0])
		AMW0_find_mailled(acer);

	if (!acer->type) {
		pr_err("No or unsupported WMI interface, unable to load\n");
		return -ENODEV;
	}

	set_quirks(acer);

	if (acpi_video_get_backlight_type() != acpi_backlight_vendor)
		acer->capability &= ~ACER_CAP_BRIGHTNESS;

	if (acer->wdevs[ACER_WMI_GUID_WMID_APGE])
		acer->capability |= ACER_CAP_SET_FUNCTION_MODE;

	if (force_caps != -1)
		acer->capability = force_caps;

	if (acer->wdevs[ACER_WMI_GUID_WMID_APGE] &&
	    (acer->capability & ACER_CAP_SET_FUNCTION_MODE)) {
		if (acer_wmi_enable_rf_button(acer))
			pr_warn("Cannot enable RF Button Driver\n");

		if (ec_raw_mode) {
			if (acer_wmi_enable_ec_raw(acer)) {
				pr_err("Cannot enable EC raw mode\n");
				return -ENODEV;
			}
		} else if (acer_wmi_enable_lm(acer)) {
			pr_err("Cannot enable Launch Manager mode\n");
			return -ENODEV;
		}
	} else if (ec_raw_mode) {
		pr_info("No WMID EC raw mode enable method\n");
	}

	/* Keep the well-known platform device name for the only instance */
	list_for_each_entry(other, &acer_wmi_instances, node) {
		if (other != acer && other->pdev) {
			id = PLATFORM_DEVID_AUTO;
			break;
		}
	}

	acer->pdev = platform_device_alloc("acer-wmi", id);
	if (!acer->pdev)
		return -ENOMEM;

	platform_set_drvdata(acer->pdev, acer);
	acer->dev = &acer->pdev->dev;

	err = platform_device_add(acer->pdev);
	if (err) {
		platform_device_put(acer->pdev);
		acer->pdev = NULL;
		acer->dev = NULL;
		return err;
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
	 * bound, so that the state is saved before the WMI devices are gone.
	 */
	list_for_each_entry_safe(acer, tmp, &acer_wmi_instances, node)
		acer_wmi_instance_teardown(acer);
	mutex_unlock(&acer_wmi_instances_lock);

	platform_driver_unregister(&acer_platform_driver);
	wmi_driver_unregister(&acer_wmi_driver);

	/*
	 * Drop any leftover registry entry, e.g. for an instance whose WMI
	 * devices never bound to the driver. The memory is released by devres
	 * once the WMI bus device is gone.
	 */
	mutex_lock(&acer_wmi_instances_lock);
	list_for_each_entry_safe(acer, tmp, &acer_wmi_instances, node)
		acer_wmi_instance_teardown(acer);
	mutex_unlock(&acer_wmi_instances_lock);
}

static int __init acer_wmi_init(void)
{
	int err;

	pr_info("Acer Laptop ACPI-WMI Extras\n");

	if (dmi_check_system(acer_blacklist)) {
		pr_info("Blacklisted hardware detected - not loading\n");
		return -ENODEV;
	}

	find_quirks();

	mutex_lock(&acer_wmi_instances_lock);
	acer_wmi_shutting_down = false;
	mutex_unlock(&acer_wmi_instances_lock);

	err = platform_driver_register(&acer_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		return err;
	}

	err = wmi_driver_register(&acer_wmi_driver);
	if (err) {
		pr_err("Unable to register WMI driver\n");
		platform_driver_unregister(&acer_platform_driver);
		return err;
	}

	/*
	 * The instances are created and set up from the WMI device probe
	 * callbacks, so no WMI device has to be present at this point. The
	 * driver core also probes matching WMI devices which appear later.
	 */
	return 0;
}

static void __exit acer_wmi_exit(void)
{
	acer_wmi_teardown();

	pr_info("Acer Laptop WMI Extras unloaded\n");
}

module_init(acer_wmi_init);
module_exit(acer_wmi_exit);

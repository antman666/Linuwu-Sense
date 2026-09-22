# Unofficial Linux Kernel Module for Acer Gaming RGB Keyboard Backlight and Turbo Mode (Acer Predator, Nitro)
The code base is still in its early stages, as I’ve just started working on developing this kernel module. It's a bit messy at the moment, but I’m hopeful that, with your help, we can collaborate to improve its structure and make it more organized over time.

Inspired by [acer-predator-turbo](https://github.com/JafarAkhondali/acer-predator-turbo-and-rgb-keyboard-linux-module), which has a similar goal, this project was born out of my own challenges. I faced issues detecting the Turbo key and ended up using [acer_wmi](https://github.com/torvalds/linux/blob/master/drivers/platform/x86/acer-wmi.c), but it lacked key features like RGB , custom fan support, battery limiter, and more. As a result, I decided to implement these missing features in my own project. This driver is not a generic Acer driver and not an acer_wmi fork: it only binds to the Acer Predator/Nitro models listed below and refuses to load on any other machine.

## 🚀 Installation
To begin, identify your current kernel version:
```bash
uname -r
```

Install the appropriate Linux headers based on your kernel version. This module has been tested with kernel version (6.12,6.13 ([previous code base](https://github.com/0x7375646F/Linuwu-Sense/tree/v6.13)),6.14) zen. 
For Arch Linux:
```bash
sudo pacman -S linux-headers
```
Next, clone the repository and build the module:
```bash
git clone https://github.com/0x7375646F/Linuwu-Sense.git
cd Linuwu-Sense
make
```
If your kernel was built with clang, select the matching toolchain (`make LLVM=1`). Install the module with the kernel's standard external module target and load it:
```bash
sudo make modules_install
sudo modprobe linuwu_sense
```
The module advertises the WMI GUIDs it supports through `MODULE_DEVICE_TABLE(wmi)`, so udev also loads it from the WMI device modalias on the next boot. No `/etc/modules-load.d` entry is needed. DKMS builds and installs the module with the same kbuild interface (a `dkms.conf` is not shipped in this repository).

### Driver ownership and module loading

Linuwu-Sense binds the Acer WMI devices it needs through the Linux WMI bus and the driver core. The kernel decides device ownership: a WMI device is bound to at most one driver, and this driver never unloads, blacklists or replaces another driver. The upstream `acer_wmi` module uses the legacy ACPI-WMI interface and does not register a WMI device driver, so it does not take the WMI devices away from Linuwu-Sense. It still talks to the same firmware through its own interface, so running both drivers at the same time is not supported. If `acer_wmi` is active on your system and you do not want that, change your own module configuration; this project does not modify your system's module policy.

To uninstall:
```bash
sudo rmmod linuwu_sense
sudo rm -f /lib/modules/$(uname -r)/updates/linuwu_sense.ko*
sudo depmod -a
```
(The installed module may be compressed, for example `linuwu_sense.ko.zst`. When DKMS or another module manager owns the installation, remove the module with that manager instead.)

The sysfs controls are owned by root and writable by root only by default. If you want to give a group write access (for example for a GUI frontend), configure that yourself with a systemd-tmpfiles or udev rule for the paths below.

> **⚠️ Warning!**
> ## Use at your own risk! This driver is independently developed through reverse engineering the official PredatorSense app, without any involvement from Acer. It interacts with low-level WMI methods, which may not be tested across all models.

## ✅ Supported devices
Linuwu-Sense only targets Acer Predator/Nitro laptops. It is not a generic Acer driver: the module refuses to load unless the machine matches one of the exact DMI entries in the driver. There is no `force_*`, `predator_v4` or `nitro_v4` module parameter to bypass this check.

**Predator:** PH315-53, PHN16-71, PHN16-72, PH16-71, PH18-71, PTX17-71

**Nitro:** AN16-41, AN16-42, AN16-43, AN515-58, AN515-55, ANV16-41, ANV15-41, ANV15-51

Only Predator PHN16-71 is fully supported. The other models are matched by the code, but the controls listed below are only available when the model supports them.

## 🛠️ Usage
# Example Usage and Configuration

Thermal profiles can be switched through `/sys/firmware/acpi/platform_profile`. The available choices are read from the firmware and mapped to the matching Predator/Nitro thermal profile. On Predator models the driver also follows the current power source and the Turbo hotkey cycles through the available profiles. ⚡💻 Customize it to fit your preferences! 🌟

---

For **Predator** laptops, the following path is used: `/sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense`

For **Nitro** laptops, the following path is used: `/sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/nitro_sense`

predator_sense – The Predator controls provided by this driver: LCD override, fan speed, turbo mode, battery limiter, battery calibration, USB charging, backlight timeout and boot animation sound. Only the controls supported by the model are shown.

nitro_sense – The equivalent controls on Nitro laptops. Which files are available depends on the model.

four_zoned_kb – If your keyboard is four-zoned, this directory provides support for it. Unfortunately, there is no support for per-key RGB keyboards.
Here is how to interact with the Virtual Filesystems (VFS) mounted in this path:

### **0. Thermal Profiles (Nitro users especially who don't have switch key) 🚀**

Some acer nitro laptops don't come up with the thermal profile switch button in this case we manually need to set it:

To probe the current thermal profile:

`cat /sys/firmware/acpi/platform_profile`

To check the supported thermal profile:

`cat /sys/firmware/acpi/platform_profile_choices`

To switch the platform profile:

`echo balanced | sudo tee /sys/firmware/acpi/platform_profile`

Replace the balanced with the supported profile you have.

#### **1. Backlight Timeout ⏰**

This feature turns off the keyboard RGB after 30 seconds of idle mode.

- **0** – Disabled
- **1** – Enabled

To check the current status, use:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/backlight_timeout`

To change the state, use:

`echo 1 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/backlight_timeout`

---

#### **2. Battery Calibration 🔋**

This function calibrates your battery to provide a more accurate percentage reading. It involves charging the battery to 100%, draining it to 0%, and recharging it back to 100%. **Do not unplug the laptop from AC power during calibration.**

- **1** – Start calibration
- **0** – Stop calibration

To check the current status:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/battery_calibration`

To change the state:

`echo 1 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/battery_calibration`

---

#### **3. Battery Limiter ⚡**

Limits battery charging to 80%, preserving battery health for laptops primarily used while plugged into AC power.

- **1** – Enabled
- **0** – Disabled

To check the current status:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/battery_limiter`

To change the state:

`echo 1 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/battery_limiter`

---

#### **4. Boot Animation Sound 🎶**

Enables or disables custom boot animation and sound.

- **1** – Enabled
- **0** – Disabled

To check the current status:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/boot_animation_sound`

To change the state:

`echo 0 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/boot_animation_sound`

---

#### **5. Fan Speed  🌬️**

Controls the CPU and GPU fan speeds.

- **0** – Auto
- **1** – Minimum fan speed (not recommended)
- **100** – Maximum fan speed
- Other values like **50, 55, 70** can be set according to your preference.

Example (set CPU to 50 and GPU to 70):

`echo 50,70 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/fan_speed`

---

#### **6. LCD Override 🖥️**

Reduces LCD latency and minimizes ghosting.

- **1** – Enabled
- **0** – Disabled

To check the current status:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/lcd_override`

To change the state:

`echo 1 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/lcd_override`

---

#### **7. USB Charging ⚡**

Allows the USB charging port to provide power even when the laptop is off.

- **0** – Disabled
- **10** – Provides power until battery reaches 10%
- **20** – Provides power until battery reaches 20%
- **30** – Provides power until battery reaches 30%

To check the current status:

`cat /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/usb_charging`

To change the state:

`echo 20 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/predator_sense/usb_charging`

---
## 💻 Keyboard Configuration 
### **Directory: `four_zoned_kb`**

The `four_zoned_kb` directory contains two Virtual File Systems (VFS) that control the RGB backlight behavior of the four-zone keyboard:

1. **`four_zone_mode`**
2. **`per_zone_mode`**

#### **1. Per-Zone Mode (`per_zone_mode`) 🎨**

This mode allows you to set a specific RGB color for each of the four keyboard zones individually. Each zone is represented by an RGB value in hexadecimal format (e.g., `4287f5` where `42` is Red, `87` is Green, and `f5` is Blue).

- **Parameters:**
    
    - The `per_zone_mode` file accepts four parameters, one for each zone, separated by commas.
    - The `per_zone_mode` also accepts brightness value.
    - Each parameter represents the RGB value for a specific zone in the format `RRGGBB`.
- **Example:**

To set all four zones to the same color (`4287f5`) and brightness to full:

`echo 4287f5,4287f5,4287f5,4287f5,100 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/four_zoned_kb/per_zone_mode`

To set each zone with unique colors:

`echo 4287f5,ff5733,33ff57,ff33a6,100 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/four_zoned_kb/per_zone_mode`

When reading (`cat`) the `per_zone_mode` file, the current color values for each zone are displayed in the format:

`4287f5,4287f5,4287f5,4287f5,100`

This indicates the current RGB color for each of the four zones.

### **Four-Zone Mode (`four_zone_mode`) ✨**

The `four_zone_mode` controls advanced RGB effects for your keyboard, requiring seven parameters:

- **Parameters:**
    
    - **Mode (0-7):** Lighting effect type (e.g., static, breathing, wave).
    - **Speed (0-9):** Speed of the effect (if applicable).
    - **Brightness (0-100):** Intensity of the lighting effect.
    - **Direction (1-2):** Direction of the effect (1 = right to left, 2 = left to right).
    - **Red (0-255), Green (0-255), Blue (0-255):** RGB color values.
- **Modes:**
    
    - **0:** Static Mode – Fixed color, no animation.
    - **1:** Breathing Mode – Color fades in and out.
    - **2:** Neon Mode – Neon glow effect, fixed color (black), direction ignored.
    - **3:** Wave Mode – Wave-like effect, color transitions across the keyboard.
    - **4:** Shifting Mode – Shifting light effect, full control over speed, direction, and color.
    - **5:** Zoom Mode – Zoom effect, direction ignored.
    - **6:** Meteor Mode – Meteor-like effect, direction ignored.
    - **7:** Twinkling Mode – Twinkling light effect, direction ignored.
- **Example Command:**
    
    Set to **Neon Mode** with speed 1, full brightness, and top-to-bottom direction:
    
    `echo 3,1,100,2,0,0,0 | sudo tee /sys/module/linuwu_sense/drivers/platform:linuwu-sense/linuwu-sense/four_zoned_kb/four_zone_mode`
    
    **Explanation:**
    
    - `3`: Neon Mode
    - `1`: Speed (1)
    - `100`: Full brightness
    - `2`: Direction (top to bottom)
    - `0`: Red (black for Neon)
    - `0`: Green (black for Neon)
    - `0`: Blue (black for Neon)
 
The four-zone keyboard state is saved when the module is unloaded and restored on the next load (it is stored in `/etc/four_zone_kb_state`). The provided `linuwu_sense.service` unloads the module at shutdown, so the keyboard state survives a reboot. It is a userspace systemd unit and is not installed or enabled by the build system; install it yourself (copy it to `/etc/systemd/system/` and enable it) if you want this behavior.

## GUI (third-party)
The following projects are separate third-party frontends that talk to this module:
- [Div Acer Manager Max By PXDiv](https://github.com/PXDiv/Div-Acer-Manager-Max)
- [GUI LinuwuSense By KumarVivek](https://github.com/kumarvivek1752/Linuwu-Sense-GUI/tree/main)

## License
GNU General Public License v3

### 💖 Donations
Donations are completely optional but show your love for open-source development and motivate me to add more features to this project!
USDT (BEP20 - BNB Smart Chain): 0xDA7aa42B9Fc3041F20f4Ec828A70E9bDD54A6822

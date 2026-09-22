# Linuwu-Sense external kernel module
#
# This file only provides the kbuild interface for building and installing
# the module:
#
#   make                    build linuwu_sense.ko
#   make clean              remove the build output
#   sudo make modules_install
#                           install the module using the kernel's external
#                           module rules (depmod and, when available, the
#                           kernel signing key are handled by kbuild)
#
# The module installation and loading lifecycle belongs to DKMS and to the
# system's own module tooling. This file neither unloads nor blacklists other
# modules and does not manage services or userspace permissions.
#
# If the kernel was built with clang, the matching toolchain has to be
# selected as well, for example: make LLVM=1

obj-m := linuwu_sense.o
linuwu_sense-y := \
	src/linuwu_sense.o \
	src/linuwu_sense_battery.o \
	src/linuwu_sense_event.o \
	src/linuwu_sense_fan.o \
	src/linuwu_sense_gaming.o \
	src/linuwu_sense_hwmon.o \
	src/linuwu_sense_input.o \
	src/linuwu_sense_keyboard.o \
	src/linuwu_sense_profile.o \
	src/linuwu_sense_quirks.o

KVER ?= $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

	# --- auto sign block ---
	# Sign the built module with the user's MOK key when it exists, so that
	# it can be loaded on kernels with module signature enforcement.
	@if [ -f "$(HOME)/module-signing/MOK.priv" ] && [ -f "$(HOME)/module-signing/MOK.der" ]; then \
		if [ -x "$(KDIR)/scripts/sign-file" ]; then \
			SIGN_TOOL="$(KDIR)/scripts/sign-file"; \
		elif [ -x "/usr/src/linux-headers-$(KVER)/scripts/sign-file" ]; then \
			SIGN_TOOL="/usr/src/linux-headers-$(KVER)/scripts/sign-file"; \
		else \
			echo "ERROR: sign-file tool not found, but MOK keys exist."; \
			exit 1; \
		fi; \
		echo "Signing module linuwu_sense.ko using $$SIGN_TOOL"; \
		sudo $$SIGN_TOOL sha256 \
			$(HOME)/module-signing/MOK.priv \
			$(HOME)/module-signing/MOK.der \
			$(PWD)/linuwu_sense.ko; \
	else \
		echo "MOK keys not found in ~/module-signing/. Skipping module signing (Common for non-Secure Boot)."; \
	fi
	# --- end auto sign block ---

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  platform_profile adapter for the Acer WMI Laptop Extras driver.
 *
 *  This module maps the Linux platform_profile choices to the Predator
 *  thermal profile commands provided by the gaming backend. It is the only
 *  owner of the selected platform profile and of the power source state it
 *  is derived from.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/platform_profile.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "linuwu_sense.h"
#include "linuwu_sense_gaming.h"
#include "linuwu_sense_profile.h"
#include "linuwu_sense_quirks.h"

/* Driver state private to the platform_profile frontend */
struct linuwu_sense_profile {
	struct device *dev;
	bool supported;
	bool on_ac;
	u8 thermal_profile;
};

static struct linuwu_sense_profile *acer_profile(struct acer_wmi *acer)
{
	return acer->profile;
}

static int acer_power_source_refresh_locked(struct acer_wmi *acer)
{
	struct linuwu_sense_profile *profile = acer_profile(acer);
	bool on_ac;
	int err;

	err = linuwu_sense_gaming_get_power_source(acer, &on_ac);
	if (err)
		return err;

	profile->on_ac = on_ac;
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
	struct linuwu_sense_profile *state = acer_profile(acer);
	int err;

	err = linuwu_sense_gaming_set_thermal_profile(acer, profile);
	if (err)
		return err;

	state->thermal_profile = profile;

	return 0;
}

static int
acer_predator_v4_platform_profile_get(struct device *dev,
				      enum platform_profile_option *profile)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct linuwu_sense_profile *state = acer_profile(acer);
	u8 tp;
	int err;

	mutex_lock(&acer->lock);
	err = linuwu_sense_gaming_get_thermal_profile(acer, &tp);
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
		state->thermal_profile = tp;
	mutex_unlock(&acer->lock);

	return err;
}

static int
acer_predator_v4_platform_profile_set(struct device *dev,
				      enum platform_profile_option profile)
{
	struct acer_wmi *acer = dev_get_drvdata(dev);
	struct linuwu_sense_profile *state = acer_profile(acer);
	bool old_on_ac;
	u8 tp;
	int err;

	mutex_lock(&acer->lock);

	if (!acer->quirks->predator_v4) {
		tp = acer_platform_profile_to_thermal_profile(profile);
		err = acer_apply_thermal_profile_locked(acer, tp);
		goto out;
	}

	old_on_ac = state->on_ac;
	err = acer_power_source_refresh_locked(acer);
	if (err)
		goto out;

	if (old_on_ac != state->on_ac)
		state->thermal_profile =
			acer_thermal_profile_for_power_transition(
				old_on_ac, state->on_ac,
				state->thermal_profile);

	tp = acer_normalize_platform_profile(state->on_ac, profile);
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

	err = linuwu_sense_gaming_get_supported_thermal_profiles(
		drvdata, &supported_profiles);
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
	struct linuwu_sense_profile *state = acer_profile(acer);
	u8 profile;
	int err;

	mutex_lock(&acer->lock);
	err = acer_power_source_refresh_locked(acer);
	if (!err) {
		profile = acer_default_thermal_profile(state->on_ac);
		err = acer_apply_thermal_profile_locked(acer, profile);
	}
	mutex_unlock(&acer->lock);

	return err;
}

static const struct platform_profile_ops acer_predator_v4_platform_profile_ops = {
	.probe = acer_predator_v4_platform_profile_probe,
	.profile_get = acer_predator_v4_platform_profile_get,
	.profile_set = acer_predator_v4_platform_profile_set,
};

static int acer_platform_profile_setup(struct acer_wmi *acer)
{
	struct linuwu_sense_profile *state = acer_profile(acer);
	const int max_retries = 10;
	int delay_ms = 100;

	if (!acer->quirks->predator_v4 && !acer->quirks->nitro_sense &&
	    !acer->quirks->nitro_v4)
		return 0;

	for (int attempt = 1; attempt <= max_retries; attempt++) {
		struct device *profile_dev;

		profile_dev = devm_platform_profile_register(
			acer->dev, "acer-wmi", acer,
			&acer_predator_v4_platform_profile_ops);
		if (!IS_ERR(profile_dev)) {
			state->dev = profile_dev;
			state->supported = true;
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
	state->dev = NULL;
	state->supported = false;

	return 0;
}

int linuwu_sense_profile_init(struct acer_wmi *acer)
{
	struct linuwu_sense_profile *state;
	int err;

	if (!acer->quirks->predator_v4 && !acer->quirks->nitro_sense &&
	    !acer->quirks->nitro_v4)
		return 0;

	state = devm_kzalloc(acer->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	acer->profile = state;

	if (acer->quirks->predator_v4) {
		err = acer_thermal_profile_init(acer);
		if (err) {
			acer->profile = NULL;
			return err;
		}
	}

	acer_platform_profile_setup(acer);

	return 0;
}

int linuwu_sense_profile_cycle(struct acer_wmi *acer)
{
	struct linuwu_sense_profile *state = acer_profile(acer);
	bool old_on_ac;
	u8 next;
	int err;

	if (!state || !acer->quirks->predator_v4)
		return 0;

	mutex_lock(&acer->lock);
	old_on_ac = state->on_ac;
	err = acer_power_source_refresh_locked(acer);
	if (err)
		goto out;

	if (old_on_ac != state->on_ac)
		state->thermal_profile =
			acer_thermal_profile_for_power_transition(
				old_on_ac, state->on_ac,
				state->thermal_profile);

	next = acer_next_thermal_profile(state->on_ac, state->thermal_profile);
	err = acer_apply_thermal_profile_locked(acer, next);
out:
	mutex_unlock(&acer->lock);

	if (!err && state->supported)
		platform_profile_notify(state->dev);

	return err;
}

int linuwu_sense_profile_power_source_changed(struct acer_wmi *acer, bool on_ac)
{
	struct linuwu_sense_profile *state = acer_profile(acer);
	bool old_on_ac;
	u8 target;
	int err = 0;

	if (!state)
		return 0;

	mutex_lock(&acer->lock);
	old_on_ac = state->on_ac;
	if (old_on_ac != on_ac) {
		target = acer_thermal_profile_for_power_transition(
			old_on_ac, on_ac, state->thermal_profile);
		state->on_ac = on_ac;
		err = acer_apply_thermal_profile_locked(acer, target);
	}
	mutex_unlock(&acer->lock);

	if (!err && old_on_ac != on_ac && state->supported)
		platform_profile_notify(state->dev);

	return err;
}

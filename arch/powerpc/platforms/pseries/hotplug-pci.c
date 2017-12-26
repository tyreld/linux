/*
 * pseries PCI Hotplug Infrastructure
 *
 * Copyright (c) 2018 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt)	"pseries-hotplug-pci: " fmt

#include <asm/rtas.h>
#include <linux/device.h>
#include <linux/of.h>

#include "pseries.h"

int dlpar_pci(struct pseries_hp_errorlog *hp_elog)
{
	int rc;

	lock_device_hotplug();

	switch (hp_elog->action) {
	default:
		pr_err("Invalid action (%d) specified\n", hp_elog->action);
		rc = -EINVAL;
	}

	unlock_device_hotplug();
	return rc;
}

/*
 * pSeries PCI hotplug infrastructure.
 *
 * Copyright (C) 2016 Tyrel Datwyler, IBM Corporation
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt)     "pseries-hotplug-pci: " fmt

#include <asm/rtas.h>
#include <linux/of.h>
#include <linux/pci.h>

#include "pseries.h"

#define POWER_OFF 0

static struct device_node *pci_drc_index_to_dn(u32 drc_index)
{
	struct device_node *dn, *phb_dn;
	u32 my_index;
	int rc;

	for_each_node_by_name(phb_dn, "pci") {
		for_each_child_of_node(phb_dn, dn) {
			rc = of_property_read_u32(dn, "my,ibm-drc-index", &my_index);
			if (rc)
				continue;

			if (my_index == drc_index) {
				of_node_put(phb_dn);
				return dn;
			}
		}
	}
		
	return NULL;
}

static int get_pci_power_domain(struct device_node *dn, u32 my_drc_index, u32 *out_power)
{
	u32 count, drc_index;
	int i, rc;

	rc = of_property_read_u32_index(dn->parent, "ibm,drc-indexes", 0, &count);
	if (rc)
		return rc;

	for (i = 1; i <= count; i++) {
		rc = of_property_read_u32_index(dn->parent, "ibm,drc-indexes", i, &drc_index);
		if (rc)
			return rc;

		if (my_drc_index == drc_index)
			break;
	}

	if (i <= count)
		rc = of_property_read_u32_index(dn->parent, "ibm,drc-power-domains", i, out_power);
	
	return rc;
}

static int dlpar_pci_remove(struct device_node *dn, u32 drc_index)
{
	struct pci_dev *dev = PCI_DN(dn)->pcidev;
	u32 power_domain;
	int setlevel, rc;

	rc = get_pci_power_domain(dn, drc_index, &power_domain);
	if (rc) {
		pr_warn("Cannot find power domain for (drc_index %x)\n", drc_index);
		goto out;
	}

	pci_stop_and_remove_bus_device(dev);
	rc = dlpar_detach_node(dn);
	if (rc)
		pr_err("Failed to detach device node %s, device-tree maybe out of sync\n", dn->full_name);

	rc = rtas_set_indicator(ISOLATION_STATE, drc_index, ISOLATE);
	if (rc) {
		pr_err("Failed to isolate (drc_index %x), rc=(%d)\n", drc_index, rc);
		goto out;
	}

	rc = rtas_set_power_level(power_domain, POWER_OFF, &setlevel); 
	if (rc)
		pr_err("Failed to power off device (drc_index %x), rc=(%d)\n", drc_index, rc);

out:
	return rc;
}

static int dlpar_pci_remove_by_index(u32 drc_index)
{
	struct device_node *dn;
	int rc;

	dn = pci_drc_index_to_dn(drc_index);
	if(!dn) {
		pr_warn("Cannot find PCI (drc_index %x) to remove\n", drc_index);
		return -ENODEV;
	}

	rc = dlpar_pci_remove(dn, drc_index);
	of_node_put(dn);
	return rc;
}

int dlpar_pci(struct pseries_hp_errorlog *hp_elog)
{
	u32 drc_index;
	int rc;

	drc_index = hp_elog->_drc_u.drc_index;

	lock_device_hotplug();

	switch(hp_elog->action) {
	case PSERIES_HP_ELOG_ACTION_REMOVE:
		if (hp_elog->id_type == PSERIES_HP_ELOG_ID_DRC_INDEX)
			rc = dlpar_pci_remove_by_index(drc_index);
		else
			rc = -EINVAL;
		break;
	default:
		pr_err("Invalid action (%d) spedified\n", hp_elog->action);
		rc = -EINVAL;
	}

	return rc;
}

int dlpar_phb(struct pseries_hp_errorlog *hp_elog)
{
	int rc;

	rc = 0;

	return rc;
}

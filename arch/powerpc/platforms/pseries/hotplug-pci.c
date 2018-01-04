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

#include <asm/dr.h>
#include <asm/rtas.h>
#include <linux/device.h>
#include <linux/of.h>

#include "pseries.h"

struct drc_info {
	struct device_node *dn;
	struct device_node *phb_dn;
	u32 index;
	u32 power;
	const char *name;
	const char *type;
};

void dealloc_drc_info(struct drc_info *drc)
{
	of_node_put(drc->dn);
	of_node_put(drc->phb_dn);
	kfree(drc);
}

static int pci_check_card_present(struct drc_info *drc, int *pow_state, int *iso_state)
{
	int state, retlevel, rc;

	*pow_state = POWER_OFF;
	*iso_state = ISOLATE;

	rc = rtas_get_sensor(DR_ENTITY_SENSE, drc->index, &state);

	if ((state == DR_EMPTY) || (state == DR_PRESENT))
		return state;

	if ((state == DR_PWR_ONLY) || (state == DR_NEED_POWER)) {
		rc = rtas_set_power_level(drc->power, POWER_ON, &retlevel);

		if (state == DR_NEED_POWER) {
			rc = rtas_set_indicator(ISOLATION_STATE, drc->index, UNISOLATE);
		}

		rc = rtas_get_sensor(DR_ENTITY_SENSE, drc->index, &state);

		if ((state == DR_EMPTY) || (state == DR_PRESENT))
			return state;
	}

	return 0;
}

static int phb_get_drc_props(struct device_node *phb_dn, const char **names,
			     const char **types, const u32 **indexes,
			     const u32 **power)
{
	*names = of_get_property(phb_dn, "ibm,drc-names", NULL);
	*types = of_get_property(phb_dn, "ibm,drc-types", NULL);
	*indexes = of_get_property(phb_dn, "ibm,drc-indexes", NULL);
	*power = of_get_property(phb_dn, "ibm,drc-power-domains", NULL);

	if (!(*names) || !(*types) || !(*indexes) || !(*power))
		return -EINVAL;

	return 0;
}

static struct device_node *pci_dn_from_phb(struct device_node *phb, u32 drc_index)
{
	struct device_node *dn = NULL;
	u32 index;
	int rc;

	for_each_child_of_node(phb, dn) {
		rc = of_property_read_u32(dn, "ibm,my-drc-index", &index);
		if (rc)
			continue;

		if (index == drc_index)
			break;
	}

	return dn;
}

static struct drc_info *pci_drc_info_from_index(u32 drc_index)
{
	struct device_node *phb_dn;
	const u32 *indexes, *power;
	const char *names, *types;
	struct drc_info *drc;
	int i;

	for_each_node_by_name(phb_dn, "pci") {
		if(phb_get_drc_props(phb_dn, &names, &types, &indexes, &power))
			continue;

		/* Skip leading 4 bytes which contain 32bit value count of elements */
		names = &names[4];
		types = &types[4];

		for (i = 1; i <= be32_to_cpu(indexes[0]); i++) {
			if (be32_to_cpu(indexes[i]) == drc_index) {
				drc = kzalloc(sizeof(drc), GFP_KERNEL);
				if (!drc)
					return NULL;

				drc->index = be32_to_cpu(indexes[i]);
				drc->power = be32_to_cpu(power[i]);
				drc->name = names;
				drc->type = types;

				drc->phb_dn = phb_dn;

				/* Child of PHB with matching "ibm,my-drc-index" property.
				   Presence indicates slot is already hotplugged. */
				drc->dn = pci_dn_from_phb(phb_dn, drc_index);

				return drc;
			}

			names += (strlen(names) + 1);
			types += (strlen(types) + 1);
		}
	}

	return NULL;
}

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

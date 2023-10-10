/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ibmvfc-nvme.h -- IBM Power Virtual Fibre Channel NVMeoF HBA driver
 *
 * Written By: Tyrel Datwyler <tyreld@linux.ibm.com>, IBM Corporation
 *
 * Copyright (C) IBM Corporation, 2022
 */

#ifndef _IBMVFC_NVME_H
#define _IBMVFC_NVME_H

#include <uapi/scsi/fc/fc_fs.h>
#include <uapi/scsi/fc/fc_els.h>
#include <linux/nvme-fc-driver.h>

#include "ibmvfc.h"

#define IBMVFC_NVME		0
#define IBMVFC_NVME_HW_QUEUES	8
#define IBMVFC_MAX_NVME_QUEUES	16
#define IBMVFC_NVME_CHANNELS	8

struct ibmvfc_host;
struct ibmvfc_target;

int ibmvfc_nvme_register_remoteport(struct ibmvfc_target *);
void ibmvfc_nvme_unregister_remoteport(struct ibmvfc_target *);
int ibmvfc_nvme_register(struct ibmvfc_host *);
void ibmvfc_nvem_unregister(struct ibmvfc_host *);
#endif

/*
 * IBM PowerPC Dynamic Reconfiguration (DR) Infrastructure
 *
 * Copyright (c) 2018 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _ASM_POWERPC_DR_H
#define _ASM_POWERPC_DR_H
#ifdef __KERNEL__

#define ISOLATION_STATE         9001

#define ISOLATE                 0
#define UNISOLATE               1

#define POWER_OFF               0
#define POWER_ON                100

#define DR_ENTITY_SENSE         9003

/* Sensor values from dr-entity-sense(get-sensor-state) */
#define DR_EMPTY                0       /* allocated to OS, but empty */
#define DR_PRESENT              1       /* allocated to OS, and present */
#define DR_STATE_UNUSABLE       2       /* unavailable to OS, but changing allocation-state
                                           indicator to usable may possibly make entity available */
#define DR_EXCHANGE             3       /* available for exchange in sparing operation */
#define DR_RECOVERY             4       /* can be recovered by platform */

/* Error status from dr-entity-sense(get-sensor-state) */
#define DR_NO_SENSOR            -3      /* not allocated to OS */
#define DR_NEED_POWER           -9000   /* needs to be powered up and unisolated */
#define DR_PWR_ONLY             -9001   /* needs to be powered up, but not unisolated */

/* Error status from set-power-level for DR entity */
#define DR_FREQ_ERROR           -9000   /* power on would cause frequency change on bus */

#endif /* __KERNEL__ */
#endif /* _ASM_POWERPC_DR_H */

/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * config.h --
 *
 *	vmkernel configuration options set from the host.
 */

#ifndef _CONFIG_H
#define _CONFIG_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"
#include "config_dist.h"


#define CONFIG_OPTION(_indx) configOption[CONFIG_##_indx]

typedef VMK_ReturnStatus (*ConfigCallback)(Bool write, Bool valueChanged, int indx);
extern void Config_Init(void);
void Config_RegisterCallback(uint32 index, ConfigCallback callback);
extern unsigned configOption[];

#endif


/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * nmi_ext.h -- 
 *
 *	Externally-usable non-maskable interrupt definitions.
 */

#ifndef	_NMI_EXT_H
#define	_NMI_EXT_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL

#include "includeCheck.h"
typedef enum {
   NMI_OFF = 0,
   NMI_SETUP_SAMPLER,
   NMI_USING_SAMPLER,
   NMI_DISABLING_SAMPLER,
   NMI_SETUP_WATCHDOG,
   NMI_USING_WATCHDOG
} NMIConfigState;

#endif

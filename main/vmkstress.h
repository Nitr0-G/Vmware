/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkstress.h --
 *
 *	vmkernel stress options set from the host.
 */

#ifndef _VMKSTRESS_H_
#define _VMKSTRESS_H_

#include "vmkstress_dist.h"

void VmkStress_Init(void);


#ifdef  VMK_STRESS_DEBUG
#define VMK_STRESS_DEBUG_ONLY(x) x
#else
#define VMK_STRESS_DEBUG_ONLY(x)
#endif

#endif // _VMKSTRESS_H_

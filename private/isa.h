/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * isa.h --
 *
 *	ISA functions.
 */

#ifndef _ISA_H_
#define _ISA_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "vmnix_if.h"

EXTERN void ISA_Init(VMnix_ConfigOptions *vmnixOptions);
EXTERN uint32 ISA_GetDeviceVector(IRQ isaIRQ);


#endif // ifdef _ISA_H_

/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * ioapic.h --
 *
 *	This is the header file for ioapic module.
 */


#ifndef _IOAPIC_H
#define _IOAPIC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "chipset_int.h"

EXTERN Chipset_ICFunctions IOAPIC_Functions;
EXTERN Chipset_ICFunctions_Internal IOAPIC_Functions_Internal;

EXTERN void IOAPIC_ResetPins(Bool levelOnly);

#endif

/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pic.h --
 *
 *	This is the header file for pic module.
 */


#ifndef _PIC_H
#define _PIC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "chipset_int.h"

EXTERN Chipset_ICFunctions PIC_Functions;
EXTERN Chipset_ICFunctions_Internal PIC_Functions_Internal;

#endif

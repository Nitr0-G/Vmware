/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * idt_dist.h --
 *
 *	interrupt descriptor table
 */

#ifndef _IDT_DIST_H
#define _IDT_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#define IDT_NUM_VECTORS			256
#define IDT_FIRST_EXTERNAL_VECTOR	32

// Users of a vector (bitmask)
#define IDT_HOST 0x01
#define IDT_VMK  0x02


typedef void (*IDT_Handler)(void *clientData, uint32 vector);

extern Bool IDT_VectorAddHandler(uint32 vector, IDT_Handler h, void *data,
		                 Bool sharable, const char *name, uint32 flags);
extern void IDT_VectorRemoveHandler(uint32 vector, void *data);
extern void IDT_VectorEnable(uint32 vector, uint8 user);
extern void IDT_VectorDisable(uint32 vector, uint8 user);
extern void IDT_VectorSync(uint32 vector);
extern void IDT_VectorSyncAll(void);


#endif


/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userCopy.h
 *
 *	Prototypes for userCopy.S assembly routines.
 */

#ifndef VMKERNEL_USER_USERCOPY_H
#define VMKERNEL_USER_USERCOPY_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"

extern void UserDoCopyIn(Reg32 seg, void* dest, VA src, int len);
extern void UserDoCopyInDone(void);
extern void UserDoCopyInString(Reg32 seg, void* dest, VA src, int* maxLen);
extern void UserDoCopyInStringDone(void);
extern void UserDoCopyOut(Reg32 seg, VA dest, const void* src, int len);
extern void UserDoCopyOutDone(void);

#endif /* VMKERNEL_USER_USERCOPY_H */

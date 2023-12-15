/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * uwvmkAPI.h --
 *
 *	Definitions that applications will need when interfacing with
 *	UserWorlds.
 *
 */

#ifndef UWVMKAPI_H
#define UWVMKAPI_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Define the VMkernel Unix-domain socket protocol family.
 *
 * Looking at the list of protocol families in /usr/include/bits/socket.h, it
 * seems that 26 is not taken.
 */
#define PF_VMKUNIX	26
#define AF_VMKUNIX	PF_VMKUNIX

#endif

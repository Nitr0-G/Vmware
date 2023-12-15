/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * serial_ext.h --
 *
 *	External definitions for the serial port module.
 */


#ifndef _SERIAL_EXT_H
#define _SERIAL_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#define SERIAL_FORCE_BREAKPOINT		3
#define SERIAL_FORCE_DUMP		4
#define SERIAL_FORCE_DUMP_AND_BREAK	5

#define SERIAL_WANT_SERIAL		6

#endif

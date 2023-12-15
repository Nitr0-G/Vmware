/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMKERNEL_VSITREEINT_H_
#define _VMKERNEL_VSITREEINT_H_

/*
 * Only the sysinfo infrastructure should be including this file.  
 * Instead, include the necessary sub #include file from below.
 */

#if !defined(VSITREE_ALLOW_INCLUDE) && !defined(VSIDEF_PARSER)
#error "Improper inclusion of vsiTree_int.h"
#endif

#include "sysinfostub.h"
#include "config_vsi.h"
#include "world_vsi.h"
#include "reliability_vsi.h"

#endif //_VMKERNEL_VSITREEINT_H_

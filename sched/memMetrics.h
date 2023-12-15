/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memMetrics.h --
 *
 *	Load metrics for memory resources.
 */

#ifndef _MEMMETRICS_H
#define _MEMMETRICS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "proc.h"

/*
 * Operations
 */

void MemMetrics_Init(Proc_Entry *dir);

#endif

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * acpi.h --
 *
 */
#ifndef _ACPI_H
#define _ACPI_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

Bool ACPI_ParseChipset(VMnix_AcpiInfo *acpiInfo, Chipset_SysInfo *chipsetInfo);
#endif

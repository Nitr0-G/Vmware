/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_cotable.h
 *      Vmkernel load-time config options table.
 *
 * Header to be included as required with VMKCFGOPT appropriately defined.  
 *
 * VMKCFGOPT(optionVar, type, confFileValueHandler, defaultValueString, description)
 *
 * All special confFileValueHandlers defined in vmkloader.c.
 */


VMKCFGOPT(maxPCPUs, unsigned, atoi, "0", "Number of physical CPUs vmkernel should use.")
#ifdef VMX86_DEBUG
VMKCFGOPT(serialPort, uint8, atoi, "1", "0 = disable, 1 = COM1, 2 = COM2.")
#else
VMKCFGOPT(serialPort, uint8, atoi, "0", "0 = disable, 1 = COM1, 2 = COM2.")
#endif
VMKCFGOPT(baudRate, int32, atoi, "115200", "Baud rate to run the serial port at.")
VMKCFGOPT(checksumMPS, Bool, TrueFalse, "TRUE", "Checksum MP config block.")
#ifdef VMX86_DEBUG
VMKCFGOPT(executePOST, Bool, TrueFalse, "TRUE", "Run POST tests.")
#else
VMKCFGOPT(executePOST, Bool, TrueFalse, "FALSE", "Run POST tests.")
#endif
VMKCFGOPT(resetTSC, Bool, TrueFalse, "TRUE", "Reset the TSCs on the CPUs.")
VMKCFGOPT(pageSharing, Bool, TrueFalse, "TRUE", "Enable page sharing.")
VMKCFGOPT(memCheckEveryWord, Bool, TrueFalse, "FALSE", "Check every single word when checking mem.")
VMKCFGOPT(hyperthreading, Bool, TrueFalse, "TRUE", "Enable hyperthreading if available.")
VMKCFGOPT(logicalApicId, Bool, TrueFalse, "FALSE", "Use logical not physical APIC IDs.")
VMKCFGOPT(ignoreNUMA, Bool, TrueFalse, "FALSE", "Pretend it's not NUMA.")
VMKCFGOPT(dumpDiag, Bool, TrueFalse, "FALSE", "Dump diagnostics information.")
VMKCFGOPT(fakeNUMAnodes, uint8, atoi, "0", "Fake # NUMA nodes on UMA systems.")
VMKCFGOPT(realNMI, Bool, TrueFalse, "FALSE", "Use real NMI for LINT1.")
VMKCFGOPT(cpuCellSize, uint8, atoi, "0", "Requested cpu scheduler cell size.")
VMKCFGOPT(acpiIntRouting, Bool, TrueFalse, "TRUE", "Enable int routing using the ACPI info.")
#ifdef VMX86_DEVEL
VMKCFGOPT(screenUse, VMnix_ScreenUse, ScreenUse, "1", "Choose what to display on screen.")
#else
VMKCFGOPT(screenUse, VMnix_ScreenUse, ScreenUse, "0", "Choose what to display on screen.")
#endif


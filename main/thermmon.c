/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * thermmon.c --
 *
 *	Provide proc nodes to manipulate Pentium 4 thermal
 *      monitoring facilities.
 */

/*
 * Includes
 */
#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "splock.h"
#include "timer.h"
#include "cpusched.h"
#include "parse.h"

#include "proc.h"
#include "thermmon.h"

#define LOGLEVEL_MODULE ThermMon
#include "log.h"

#define IA32_THERM_CONTROL            0x19a
#define IA32_THERM_INTERRUPT          0x19b
#define IA32_THERM_STATUS             0x19c
#define IA32_MISC_ENABLE              0x1a0

#define THERMAL_MONITOR_ENABLE_BIT    (1 << 3)
#define THERMAL_STATUS_BIT            (1 << 0)
#define THERMAL_LOG_BIT               (1 << 1)
#define THERMAL_MODULATION_BIT        (1 << 4)

static Proc_Entry thermMonProcEnt;
static uint32 thermMonStatus[MAX_PCPUS];
static uint32 miscEnableMSR[MAX_PCPUS];


static void
ThermMonRunAllPcpus(Timer_Callback cb, void* data)
{
   PCPU i;
   for (i=0; i < numPCPUs; i++) {
      Timer_Add(i, cb, 1, TIMER_ONE_SHOT, (void*)data);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonReadCallback --
 *     Callback that runs on each cpu to read processor-specific thermal MSR info.
 *     Called via the "read" command on the proc node. To view the results, cat
 *     the proc node after issuing the "read" command.
 *
 * Results:
 *     Void.
 *
 * Side effects:
 *     Stores updated thermal MSR info into thermMonStatus and miscEnableMSR arrays.
 *
 *-----------------------------------------------------------------------------
 */
static void
ThermMonReadCallback(UNUSED_PARAM(void* data),
                     UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 fakevar, thermStatusReg, miscEnableReg;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   RDMSR(IA32_THERM_STATUS, thermStatusReg, fakevar);
   thermMonStatus[MY_PCPU] = thermStatusReg;


   RDMSR(IA32_MISC_ENABLE, miscEnableReg, fakevar);
   miscEnableMSR[MY_PCPU] = miscEnableReg;   
}

/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonModulateCallback --
 *
 *    Callback for ThermMonModulate. "Data" should specify the four non-reserved
 *     bits for the thermal monitoring control register.
 *
 * Results:
 *     Void
 *
 * Side effects:
 *     Changes thermal MSRs. Slows down your system dramatically until you undo it
 *     by issuing the "fullspeed" command.
 *
 *-----------------------------------------------------------------------------
 */
static void
ThermMonModulateCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 fakevar, control;
   uint32 newControlBits = (uint32) data;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);
   
   RDMSR(IA32_THERM_CONTROL, control, fakevar);

   // clear the four non-reserved bits (1-4), leave the others alone
   control &= (!(0xf << 1));

   // set the new bits to enable modulation
   control |= newControlBits;

   Log("Writing 0x%x to thermal control register", control);

   WRMSR(IA32_THERM_CONTROL, control, fakevar);   
}



/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonClockModulate --
 *
 *     Slows down processor to "speedEights" eighths of its full speed. Can 
 *     take any value from one to seven, inclusive. The parameter uses increments
 *     in eighths because that's what the cpu understands.
 *
 * Results:
 *     Void.
 *
 * Side effects:
 *	Your machine will now run at a different clock speed, but the
 *      TSC will continue advancing at its same old rate.
 *
 *
 *-----------------------------------------------------------------------------
 */
static void
ThermMonClockModulate(Bool enable, int speedEighths)
{
   uint32 control = 0;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   if ((speedEighths < 1 || speedEighths > 7) && enable) {
      LOG(0, "Invalid speed (%d) for ClockModulate", speedEighths);
      return;
   }

   if (enable) {
      control |= (speedEighths & 0x7) << 1;
      control |= THERMAL_MODULATION_BIT;
   } 

   ThermMonRunAllPcpus(ThermMonModulateCallback, (void*) control);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonResetFlags --
 *
 *     Clears the "log" bit on the thermal MSRs. "data" is ignored.
 *
 * Results:
 *     Void.
 *
 * Side effects:
 *     Changes MSRs.
 *
 *-----------------------------------------------------------------------------
 */

static void
ThermMonResetFlagsCallback(UNUSED_PARAM(void* data),
                           UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 fakevar, thermStatusReg, newStatusReg;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   RDMSR(IA32_THERM_STATUS, thermStatusReg, fakevar);
   newStatusReg = thermStatusReg & (!THERMAL_LOG_BIT);
   WRMSR(IA32_THERM_STATUS, newStatusReg, fakevar);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonSetEnabledCallback --
 *
 *     Turns on or off thermal monitoring (on the chip itself), depending on the
 *     value of "data" as a boolean
 *
 * Results:
 *     Void.
 *
 * Side effects:
 *     Changes thermal MSRs.
 *
 *-----------------------------------------------------------------------------
 */
static void 
ThermMonSetEnabledCallback(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 miscEnableReg, fakevar;
   uint32 newEnableReg;
   Bool enable = (Bool)(int32) data; // gcc doesn't like void*->bool cast

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   RDMSR(IA32_MISC_ENABLE, miscEnableReg, fakevar);

   if (enable) {
      newEnableReg = miscEnableReg | THERMAL_MONITOR_ENABLE_BIT;
      LOG(0, "Enabling thermal monitoring on cpu %d", MY_PCPU);
   } else {
      newEnableReg = miscEnableReg & (!THERMAL_MONITOR_ENABLE_BIT);
      LOG(0, "Disabling thermal monitoring on cpu %d", MY_PCPU);
   }

   fakevar = 0;
   WRMSR(IA32_MISC_ENABLE, newEnableReg, fakevar);
}
/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonProcWrite --
 *
 *     Basic write handler to parse commands to the "thermmon" proc node.
 *     Supports the following commands: enable, disable, modulate, fullspeed,
 *     and reset.
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     Carries out the specified command, possibly changing thermal MSRs.
 *
 *-----------------------------------------------------------------------------
 */
static int
ThermMonProcWrite(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{ 
   char* argv[1];
   int argc;

   argc = Parse_Args(buffer, argv, 1);
   
   if (strcmp(argv[0], "read") == 0) {
      // on a write, launch a timer to check each pcpu
      ThermMonRunAllPcpus(ThermMonReadCallback, NULL);
   } else if (strcmp(argv[0],"enable") == 0) {
      ThermMonRunAllPcpus(ThermMonSetEnabledCallback, (void*) TRUE);
   } else if (strcmp(argv[0],"disable") == 0) {
      ThermMonRunAllPcpus(ThermMonSetEnabledCallback, (void*) FALSE);
   } else if (strcmp(argv[0],"modulate") == 0) {
      ThermMonClockModulate(TRUE, 4); // modulate to half speed
   } else if (strcmp(argv[0],"fullspeed") == 0) {
      ThermMonClockModulate(FALSE, 0);
   } else if (strcmp(argv[0],"reset") == 0) {
      ThermMonRunAllPcpus(ThermMonResetFlagsCallback, NULL);
   } else {
      LOG(0, "ThermMon command not understood: %s", argv[0]); 
   } 

   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ThermMonProcRead --
 *
 *     Basic read handler to display the thermal monitoring status from the cpus.
 *     You MUST echo "read" into the proc node before you can read from it, 
 *     because we use timers on remote CPUs to query their thermal monitoring 
 *     registers, and proc handlers aren't allowed to sleep to wait for the response.
 *
 * Results:
 *     Returns VMK_OK. Displays thermal monitoring info.
 *
 * Side effects:
 *     Displays thermal monitoring info.
 *
 *-----------------------------------------------------------------------------
 */
static int
ThermMonProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   PCPU i;
   *len = 0;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);
   Proc_Printf( buffer, len, "      \t%9s\t%9s\t%9s\n", "Current", "Past", "Enabled");

   for (i=0; i < numPCPUs; i++) {
      Proc_Printf( buffer, len, "PCPU %d:\t%9s\t%9s\t%9s\n", 
                   i, 
                   (thermMonStatus[i] & THERMAL_STATUS_BIT) ? "overheat" : "ok", 
                   (thermMonStatus[i] & THERMAL_LOG_BIT) ? "overheat" : "ok",
                   (miscEnableMSR[i] & THERMAL_MONITOR_ENABLE_BIT) ? "on" : "off");
   }

   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ThermMon_ProcSetup --
 *
 *     Adds a proc node that allows the user to query or set thermal monitoring
 *     features. Note that ThermMon only works on Pentium IV processors.
 *
 * Results:
 *     Void.
 *
 * Side effects:
 *     Adds a proc node.
 *
 *-----------------------------------------------------------------------------
 */
void
ThermMon_Init(void)
{
   Proc_InitEntry(&thermMonProcEnt);

   thermMonProcEnt.parent = NULL;
   thermMonProcEnt.read = ThermMonProcRead;
   thermMonProcEnt.write = ThermMonProcWrite;
   
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      memset(thermMonStatus, 0, sizeof(thermMonStatus));
      memset(miscEnableMSR, 0, sizeof(miscEnableMSR));
      
      Proc_Register(&thermMonProcEnt, "thermmon", FALSE);
      
      LOG(1, "Registered ThermMon proc nodes");
   } else {
      LOG(0,"Processor type does not support thermal monitoring");
   }
}

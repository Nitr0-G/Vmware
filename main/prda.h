/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * prda.h --
 *
 *	This file defines the contents of the per-physcial-cpu
 *      private data area.
 */

#ifndef _PRDA_H
#define _PRDA_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "nmi_ext.h"
#include "x86perfctr.h"
#include "splock.h"
#include "vmkernel_ext.h"

#define PCPU_BSP   1
#define PCPU_AP    2
#define PCPU_DEAD  3

struct World_Handle;
struct IM_Payload;
struct TSEntry;

struct SP_SpinCommon; // avoid including splock.h

typedef struct SwitchStats {
   TSCCycles    switchBegin;
   TSCCycles    vmmToVMM, vmmToNVMM, nvmmToVMM, nvmmToNVMM;
   uint64       vmmToVMMCnt, vmmToNVMMCnt, nvmmToVMMCnt, nvmmToNVMMCnt;
} SwitchStats;

typedef struct PRDA {
   PCPU    	pcpuNum;
   int32    	pcpuState;

   /*
    * CPU scheduling
    */
   struct World_Handle	*runningWorld;    
   Bool			reschedule;
   Bool		        idle;		
   Bool		        halted;		

   /*
    * Bottom-half info.
    */
   Bool			bhInProgress;
   Atomic_uint32	bhPending;

   /*
    * network device bottom half stuff.
    */
   struct Net_EtherDev  *netDevQueue;

   /*
    * World being serviced by vmkernel during current
    * interrupt handler or bottom-half handler.
    */
   struct World_Handle	*vmkServiceWorld;
   uint32		vmkServiceVector;
   TSCCycles		vmkServiceStart;
   uint32		vmkServiceRandom;
   int16		vmkServiceShift;

   /*
    * Linux driver bottom half stuff.
    */
   struct LinuxBHData *linuxBHList;

   /*
    * Linux softirq stuff.
    */
   uint32 softirq_pending;

   /*
    * Data grabbed from NMIs
    */
   int    	perfCounterInts;
   int	  	currentTicks;
   int	  	previousTicks;
   int	  	hungCount;
   Reg32  	lastEIP;
   Reg32  	lastESP;
   Reg32  	lastEBP;
   NMIConfigState   configNMI;

   /*
    * Stack used during post-NMI clts code. Per PCPU since multiple
    * PCPUs could be executing this code at the same time.
    */
#define NMI_PATCH_STACK_SIZE 5
   uint32       nmiPatchStack[NMI_PATCH_STACK_SIZE];

   /*
    * NMI VMKStats state
    */
   uint8	vmkstatsConfig;
   uint32	vmkstatsPerfCtrValue;
   uint32	vmkstatsPerfCtrReset;
   uint32	vmkstatsPerfCtrEvent;
   uint64       vmkstatsMissedEvents;
   uint64       vmkstatsMissingEvents;
   Bool         vmkstatsClearStats;
   PerfCtr_Counter samplerCounter;
   Bool         nmisEnabled;
   

   /*
    *  Misc.
    */
   Bool         stopAP;
   Bool		wantDump;
   uint32       ksegActiveMaps; // number of active kseg maps
   uint64       cpuHzEstimate;
   uint64       busHzEstimate;
   uint32       randSeed;
   uint32       clockMultiplierX2;
   RateConv_Params tscToPseudoTSC;
   RateConv_Params tscToTC;
   /* 
    * Debugging information.
    */
   Bool		inPanic;
   Bool		inNMI;
   Bool		inWatchpoint;
   Bool         inInterruptHandler;

   struct World_Handle   *worldInPanic;

   void         *lastClrIntr;

   SP_Stack     spStack[SP_STACK_NUM_STACKS];

#ifdef VMX86_STATS
   SwitchStats  switchStats;
#endif
   uint64       bhCheck;
   uint64       bhCheckResched;
} PRDA;

/*
 * macros
 */

#define myPRDA	(*(volatile PRDA *)(VMK_FIRST_PRDA_ADDR))
#define MY_PCPU                         (myPRDA.pcpuNum)
#define MY_RUNNING_WORLD                (myPRDA.runningWorld)
#define MY_VMM_GROUP_LEADER             (World_GetVmmLeader(MY_RUNNING_WORLD))


extern uint32 numPCPUs;

extern PRDA **prdas;
extern MPN *prdaMPNs;
extern MPN prdaPTableMPNs[MAX_PCPUS][VMK_NUM_PRDA_PDES];

extern void PRDA_Init(VMnix_Init *vmnixInit);

extern Bool PRDA_IsInitialized(void);
extern uint32 PRDA_GetPCPUNumSafe(void);
extern struct World_Handle* PRDA_GetRunningWorldSafe(void);
extern World_ID PRDA_GetRunningWorldIDSafe(void);
extern const char* PRDA_GetRunningWorldNameSafe(void);
extern VMK_ReturnStatus PRDA_MapRegion(PCPU pcpu, MA pageRoot);


#endif

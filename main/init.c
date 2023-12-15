/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * init.c: vmkernel initialization functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "buildNumber.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "idt.h"
#include "host.h"
#include "timer.h"
#include "serial.h"
#include "debug.h"
#include "apic.h"
#include "rpc.h"
#include "prda.h"
#include "util.h"
#include "chipset.h"
#include "net.h"
#include "pci.h"
#include "it.h"
#include "alloc.h"
#include "vmk_scsi.h"
#include "smp.h"
#include "memalloc.h"
#include "fsDeviceSwitch.h"
#include "fsSwitch.h"
#include "kseg.h"
#include "bh.h"
#include "tlb.h"
#include "config.h"
#include "nmi.h"
#include "vmkstats.h"
#include "mod_loader.h"
#include "bluescreen.h"
#include "proc.h"
#include "timer.h"
#include "helper.h"
#include "post.h"
#include "dump.h"
#include "pshare.h"
#include "swap.h"
#include "mce.h"
#include "watchpoint.h"
#include "migrateBridge.h"
#include "vmkperf.h"
#include "event.h"
#include "thermmon.h"
#include "testworlds.h"
#include "numa.h"
#include "vmkevent.h"
#include "vmkstress.h"
#include "mtrr.h"
#include "cpuid_info.h"
#include "xmap.h"
#include "buddy.h"
#include "isa.h"
#include "vmklinux_dist.h"
#include "eventhisto.h"
#include "hardware.h"
#include "trace.h"
#include "vmktag_dist.h"
#include "conduit_bridge.h"
#include "user.h"
#include "vmk_nfs.h"
#include "vmkcalls_vmcore.h"
#include "vmkcalls_public.h"
#include "log_int.h"
#include "vga.h"
#include "keyboard.h"
#include "term.h"
#include "logterm.h"
#include "acpi_public.h"
#include "statusterm.h"
#include "heap_int.h"
#include "vscsi.h"
#include "heapMgr.h"
#include "reliability.h"
#include "debugterm.h"

#define LOGLEVEL_MODULE Init
#include "log.h"


#define EFLAGS_PRESERVED_ON_VMKCALL (EFLAGS_PRIV & ~(EFLAGS_VIF | EFLAGS_VIP))


/*
 *  globals
 */

uint32 numPCPUs;
PRDA **prdas;
CPUType cpuType;
/* seconds since 1970 according to the console OS 
 * Used to set file modification times */
uint32 consoleOSTime;

CPUIDSummary cpuids[MAX_PCPUS];

Identity cosIdentity;

static uint32 vmnixmodBuildNumber;
static uint32 vmnixmodInterfaceNumber;
static uint32 vmnixKernelVersion;
static Proc_Entry versionInfoProc;

/*
 *  locals
 */

static VMK_ReturnStatus Idle_Init(void);
static CPUType CPUCheckType(void);

Bool guestIdle;
static VMnix_Init vmnixInit;
static VMnix_Info vmnixInfo;
static VMnix_ConfigOptions vmnixOptions;
VMnix_SharedData sharedData;


Bool vmkernelLoaded = FALSE;
Bool vmkernelInEarlyInit = TRUE;
/* Unique ID to distinguish this vmkernel (physical host) from other hosts */
static int vmkernelID;

#ifdef VMX86_DEBUG
/*
 * Our device drivers can't access the prda
 * directly, so use this struct to cache some pointers
 * into the prda.
 */
vmkDebugInfo vmkDebug;
#endif 
static void InitDebugArea(void);
static int InitVersionInfoProcReadHandler(UNUSED_PARAM(Proc_Entry *entry),
                                          char        *page,
                                          int         *lenp);

static TSCCycles tscStartInit;
static VMnix_StartupArgs startupArgs;
static VMnix_InitArgs initArgs;

/*
 *  this function needs error codes
 */
int32
InitEarlyVMKernel(VMnix_StartupArgs *hostStartupArgs)
{
   int res;
   uint32 eflags;

   tscStartInit = RDTSC();

   CopyFromHost(&startupArgs, hostStartupArgs, sizeof(startupArgs));
   CopyFromHost(&vmnixInit, startupArgs.initBlock, sizeof(VMnix_Init));
   CopyFromHost(&vmnixInfo, startupArgs.vmnixInfo, sizeof(VMnix_Info));
   CopyFromHost(&vmnixOptions, startupArgs.configOptions, sizeof(VMnix_ConfigOptions));
   Serial_EarlyInit(&vmnixOptions);  // Should be as early as possible so we can log panics to serial

   //check to make sure that we agree with the vmnix module about the number
   //of system calls. (quick & dirty version checking.)

   if (startupArgs.numVMKSyscalls != _SYSCALL_NUM_SYSCALLS)  {
      Warning("Mismatched syscall numbers: vmnixmod = %d, vmkernel = %d",
              startupArgs.numVMKSyscalls, _SYSCALL_NUM_SYSCALLS);
      return VMK_VERSION_MISMATCH_MAJOR;
   }

   vmnixmodBuildNumber = startupArgs.vmnixBuildNumber;
   vmnixmodInterfaceNumber = startupArgs.vmnixInterfaceNumber;
   vmnixKernelVersion = startupArgs.vmnixKernelVersion;
   if (startupArgs.vmnixBuildNumber != BUILD_NUMBER_NUMERIC) {
      Log("vmnix / vmkernel build numbers differ: %d != %d",
          startupArgs.vmnixBuildNumber, BUILD_NUMBER_NUMERIC);
      // Only do strict build number matching on beta builds.  (can't do
      // it on release builds because it makes patching hard).
      if (vmx86_debug && !vmx86_devel) {
         return VMK_VERSION_MISMATCH_MAJOR;
      }
   }

   if (DEFAULT_TSS_DESC >= MON_VMK_FIRST_COMMON_SEL) {
      return VMK_SEGMENT_OVERLAP;
   }

   Timer_HzEstimateInit();
   cpuHzEstimate = Timer_CPUHzEstimate();
   cpuKhzEstimate = (cpuHzEstimate + 500) / 1000;

   Log_EarlyInit(&vmnixOptions, &sharedData, startupArgs.sharedData);
   Log("cpu 0: early measured cpu speed is %Ld Hz", cpuHzEstimate);

   cpuType = CPUCheckType();
   if (cpuType == CPUTYPE_UNSUPPORTED) {
      return VMK_UNSUPPORTED_CPU;
   }
      
   Log("vmkernelID not yet set.")

   /*
    * Setup to shootdown TLB entries.
    */
   TLB_EarlyInit(&vmnixInit);

   Mem_EarlyInit(&vmnixInit);

   Buddy_Init();
   Heap_Init(); // before Mem_Init, and any other heap creations

   KVMap_Init(VMK_KVMAP_BASE, VMK_KVMAP_LENGTH);

   VGA_Init(&vmnixInfo, &sharedData); // depends on kvmap
   Keyboard_EarlyInit();
   Term_Init(&sharedData); // depends on VGA_Init and Keyboard_EarlyInit
   BlueScreen_Init(); // depends on Term_Init
   DebugTerm_Init();
   LogTerm_Init();
   StatusTerm_Init(vmnixOptions.screenUse);
   StatusTerm_Printf("Starting vmkernel initialization:\n");

   PShare_EarlyInit(vmnixOptions.pageSharing);

   NUMA_Init(&vmnixInit, vmnixOptions.ignoreNUMA,
	     vmnixOptions.fakeNUMAnodes);

   res = MTRR_Init(HOST_PCPU); // before memmap_init
   if (res != VMK_OK) {
      return res;
   }

   res = MemMap_EarlyInit(&vmnixInit, vmnixOptions.memCheckEveryWord);
   if (res != VMK_OK) {
      SysAlert("Memory manager could not start (%d)", res);
      return res;
   }
   MemSched_EarlyInit();

   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();

   /*
    * Host_EarlyInit makes calls to MemRO_ChangeProtection, so 
    * MemRO_EarlyInit must be called to initialize the checksum.
    */
   MemRO_EarlyInit();

   /*
    * Host_EarlyInit needs interrupts disabled and it depends on
    * MemMap_EarlyInit.  Till this point we run with the COS's pagetable.
    * Host_EarlyInit will construct the host world's vmkernel
    * pagetable/segments and switch to it.
    */
   Host_EarlyInit(&vmnixInfo, &sharedData, &startupArgs);

   Mem_Init();
   MemRO_Init(&startupArgs);

   EventHisto_Init(); // should come before IDT_Init
   IDT_Init(&sharedData);   //depends on MemRO_Init
   vmkernelInEarlyInit = FALSE;
   Log("Done");
   return VMK_OK;
}

VMK_ReturnStatus
InitVMKernel(VMnix_InitArgs *args)
{
   VMK_ReturnStatus status;
   TSCRelCycles tscOffset = 0;
   VMnix_AcpiInfo *vmnixAcpiInfo;
   VMnix_AcpiInfo *vmkAcpiInfo = NULL;

   // declare abort as a static variable, otherwise what seems to be a
   // gcc 2.7.2.3 bug will cause Athlons to reset when loading the vmkernel
   static Bool abort = FALSE;

   Log("Continuing init");

   StatusTerm_Printf("Initializing memory ...\n");

   XMap_Init(); // depends on Host_EarlyInit (vmkernel pagetable)

   MemMap_Init(); // depends on XMap_Init
   
   XMap_LateInit(); // depends on MemMap_Init

   Net_EarlyInit();

   SP_EarlyInit();
   BH_Init();

   Debug_Init();   //Debug_Break doesn't work until host_earlyInit

   Proc_Init(&sharedData);

   VmkTag_Init();


   CopyFromHost(&initArgs, args, sizeof(initArgs));
   vmnixAcpiInfo = initArgs.acpiInfo;

   StatusTerm_Printf("Initializing chipset ...\n");

   // Allocate space for the vmkernel acpi info and
   // populate it with the data sent over from the vmnix
   ACPI_CopyAcpiInfo(&vmkAcpiInfo, vmnixAcpiInfo);

   status = SMP_Init(&vmnixInit, &vmnixOptions, vmkAcpiInfo);
   if (status != VMK_OK) {
      Host_RestoreIDT();
      return status;
   }

   Hardware_Init(&vmnixInit);
   /*
    * Find out the type of System Interrupt Controller we are using.
    * (NOTE: This could become a system identification function.)
    * No system specific code (Chipset_XXX) should be run before.
    */
   status = Chipset_Init(&vmnixInit, &vmnixInfo, &vmnixOptions, 
                         &sharedData, vmkAcpiInfo);
   if (status != VMK_OK) {
      Chipset_RestoreHostSetup();
      Host_RestoreIDT();
      return status;
   }

   // Cleanup the space used by vmkernel acpiInfo 
   ACPI_DestroyAcpiInfo(vmkAcpiInfo);

   StatusTerm_Printf("Initializing timing...\n");

   APIC_HzEstimate(&cpuHzEstimate, &busHzEstimate);
   cpuKhzEstimate = (cpuHzEstimate + 500) / 1000;
   busKhzEstimate = (busHzEstimate + 500) / 1000;
   Log("cpu 0: measured cpu speed is %Ld Hz", cpuHzEstimate);
   Log("cpu 0: measured bus speed is %Ld Hz", busHzEstimate);

   status = NUMA_LateInit();
   if (status != VMK_OK) {
      Chipset_RestoreHostSetup();
      Host_RestoreIDT();
      return status;
   }
   
   NUMA_LocalInit(0);
   Timer_InitCycles();

   PRDA_Init(&vmnixInit);
   myPRDA.cpuHzEstimate = cpuHzEstimate;
   myPRDA.busHzEstimate = busHzEstimate;

   /*
    * Initialize the kseg area which is used to obtain a dereferenceable
    * pointer to any machine page.
    */
   Kseg_Init();
   SP_Init();
   Util_Init();     // depends on SP_Init(); should be as early as possible
   MemMap_LateInit(); // depends on kseg_lateinit

   PShare_LateInit();
   Buddy_LateInit();
   
   /* 
    * HeapMgr_Init must occur before any use of standard dynamic heaps; it
    * should probably also occur after Buddy_LateInit and MemMap_LateInit...
    * Sched_Init will be using dynamic heaps.
    */
   HeapMgr_Init(); 

   Dump_Init();

   Event_Init();

   Timer_Init();
   Vmkperf_Init(); // must come before Sched_Init
   Trace_Init();
   
   StatusTerm_Printf("Initializing scheduler ...\n");

   Sched_Init(vmnixOptions.cpuCellSize); // depends on Timer_Init

   World_Init(&vmnixInit);  // depends on Sched_Init for initializing host world

   tscOffset = SMP_BootAPs(&vmnixInit);  // depends on World_Init for initializing idle world

   Alloc_Init();
   RPC_Init(&sharedData);
   Net_Init(&sharedData);
   SCSI_Init(&sharedData);
  

   StatusTerm_Printf("Initializing device support ...\n");

   Host_LateInit(); // needed for Host_SetPendingIRQ to work
   ISA_Init(&vmnixOptions);
   PCI_Init(&vmnixInfo);

   Mod_Init();
   Log_Init();
   Config_Init(); // depends on log_init
   VmkStress_Init(); // depends on log_init

   // MemMap_LogFreeList();

   status = TLB_LateInit();
   if (status != VMK_OK) {
      SysAlert("TLB_Init failed");
      // not safe to call Host_Unload here, defer to later (see below)
      abort = TRUE;
   }

   StatusTerm_Printf("Initializing processors ...\n");

   // performance counter and vmkstats procfs initializations
   VMKStats_Init();
   NMI_Init();

   Watchpoint_Init();
   Watchpoint_Enable(FALSE);
   MCE_Init();

   SMP_StartAPs();  // release APs from initial barrier

   Timer_InitPseudoTSC();  // depends on SMP_StartAPs

   // This needs the true number of cpus, so it must come after
   // SMP_BootAPs.
   // Also, now uses the proc module so must come after Proc_Init
   SP_LateInit();

   status = Idle_Init();
   if (status != VMK_OK) {
      // not safe to call Host_Unload here, defer to later (see below)
      abort = TRUE;
   }

   // Initialize the storage stack starting from file system 
   // device switch and moving above into file system and related
   // components.
   FDS_Init();
   
   //Initialize the VSCSI switch
   VSCSI_Init();

   Action_Init();

   // Start helper world after all processors have been booted, so
   // that helper world can be placed on a different processor
   // from the host.
   Helper_Init(&sharedData);
   SCSI_ResetInit();

   StatusTerm_Printf("Initializing interrupts ...\n");

   // this must come after SMP_BootAPs, it unmasks the interrupts in the IC
   Chipset_LateInit();
   IDT_LateInit();
   IT_Init();
   Host_InitInterrupts(&vmnixInfo);	// after IDT_LateInit
   Serial_LateInit(&vmnixOptions);	// after Host_InitInterrupts
   Keyboard_Init();			// after Host_InitInterrupts

   TLB_LocalInit();

   Timer_LateInit();

   // swap device initialization
   Swap_Init();

   // memsched daemon world initialization
   MemSched_SchedWorldInit(); 

   Heap_LateInit(); // must be after Proc_Init 
   EventHisto_LateInit();
   
   SHARED_DATA_ADD(sharedData.cpuKhzEstimate, uint32 *, &cpuKhzEstimate);
   SHARED_DATA_ADD(sharedData.consoleOSTime, uint32 *, &consoleOSTime);
   SHARED_DATA_ADD(sharedData.numCPUsUsed, uint32 *, &numPCPUs);
   SHARED_DATA_ADD(sharedData.logicalPerPackage, uint8 *, &hyperthreading.logicalPerPackage);
   SHARED_DATA_ADD(sharedData.cpuids, CPUIDSummary *, cpuids);
   SHARED_DATA_ADD(sharedData.cosIdentity, Identity *, &cosIdentity);

   ThermMon_Init();
   TestWorlds_Init();
   User_Init();
   VMKStats_LateInit();
   Reliability_Init(); 
   Host_TimerInit(tscStartInit, tscOffset);

   Term_LateInit();
   StatusTerm_Printf("Enabling interrupts ...\n");

   ENABLE_INTERRUPTS();
   LogTerm_LateInit(); // After enabling interrupts

   InitDebugArea();

   CopyToHost(startupArgs.sharedData, &sharedData, sizeof(sharedData));

   if (abort) {
      Host_Unload(TRUE);
      return VMK_NOT_SUPPORTED;
   }

   // Log checksum of entire vmkernel code region
   Log("checksum 0x%Lx, vmkernel build Number = %d, vmnixmod build number = %d ", 
       MemRO_GetChecksum(), BUILD_NUMBER_NUMERIC, startupArgs.vmnixBuildNumber);

   Proc_InitEntry(&versionInfoProc);
   versionInfoProc.read = InitVersionInfoProcReadHandler;
   Proc_Register(&versionInfoProc, "version", FALSE);

   Log("Vmkernel initialization done.  Returning to console.");
   StatusTerm_Printf("Vmkernel has been loaded succesfully.\n\n");

   // Setting vmkernelLoaded should be the last things in Init
   vmkernelLoaded = TRUE;

   /*
    * VMKCall tables from monitor and vmkernel are partitioned using
    * the lowest indices for monitor and upper for vmkernel devices.
    * Both must agree on the partition point.  If this fails you must
    * update vmkcalls_public.h VMK_EXT_MIN_FUNCTION_ID.
    *
    * props to petr for the ... clever ... use of asm below.
    * (The following won't work
    *     #if VMK_EXT_MIN_FUNCTION_ID != VMK_VMM_MAX_FUNCTION_ID 
    *        #error "blah"
    *     #endif
    *  because VMK_EXT_MIN_FUNCTION_ID is a constant and VMK_VMM_MAX_FUNCTION_ID
    *  is an enum.)
    */

   if (VMK_EXT_MIN_FUNCTION_ID != VMK_VMM_MAX_FUNCTION_ID) {
      asm volatile (
         ".print \"VMK_EXT_MIN_FUNCTION_ID (%c0) != VMK_VMM_MAX_FUNCTION_ID (%c1)\";" \
         ".abort" : : "i"(VMK_EXT_MIN_FUNCTION_ID), "i"(VMK_VMM_MAX_FUNCTION_ID) );
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Init_PostBootMemory
 *
 * 	Invoked on the behalf of a memory controller which allows
 * 	new physical memory to be provided after power-on.
 *
 * Results:
 *      New memory is made available through the memmap facility.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Init_PostBootMemory(VMnix_HotAddMemory *data)
{
   VMnix_HotAddMemory args;

   // copyin args
   CopyFromHost(&args, data, sizeof(VMnix_HotAddMemory));

   return MemMap_HotAdd(
	args.start, args.size, 
	vmnixOptions.memCheckEveryWord,
	args.attrib, &vmnixInit);
}

/*
 *----------------------------------------------------------------------
 *
 * Init_NOPCall --
 *
 *      NOP vmkcall.  useful for vmkcall timing tests.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static VMKERNEL_ENTRY
Init_NOPCall(DECLARE_ARGS(VMK_NOP))
{
   PROCESS_0_ARGS(VMK_NOP);
   return VMK_OK;
}


static VMKERNEL_ENTRY
StopNMIs(DECLARE_0_ARGS(VMK_STOP_NMIS))
{
   PROCESS_0_ARGS(VMK_STOP_NMIS);

   MY_RUNNING_WORLD->nmisInMonitor = FALSE;
   return VMK_OK;
}

static VMKERNEL_ENTRY
StartNMIs(DECLARE_0_ARGS(VMK_START_NMIS))
{
   PROCESS_0_ARGS(VMK_START_NMIS);

   MY_RUNNING_WORLD->nmisInMonitor = TRUE;
   return VMK_OK;
}

static VMKERNEL_ENTRY
GetPerfCtrConfig(DECLARE_ARGS(VMK_GET_PERFCTR_CONFIG))
{
   PROCESS_1_ARG(VMK_GET_PERFCTR_CONFIG, PerfCtr_Config *, ctr);

   NMI_GetPerfCtrConfig(ctr);
   return VMK_OK;
}

static VMKERNEL_ENTRY
VMKInit(DECLARE_ARGS(VMK_INIT))
{
   PROCESS_1_ARG(VMK_INIT, uint32, zero);
   Log("Received INIT from world %d", MY_RUNNING_WORLD->worldID);
   if (zero != 0) {
      World_Panic(MY_RUNNING_WORLD, "vmm->vmk version mismatch. Are you "
                 "running an opt/obj vmm on a beta/release vmkernel?  If so, "
                 "undefine VMM_VMK_ARG_CHECKING in vmm/private/vmk_if.h\n\n"
                 "Got 0x%x, expected 0x0\n", zero);
   }
   Watchpoint_Disable(FALSE);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DoRPCCall --
 *
 *      Handles all usercalls (ie vmm->vmx) calls.  All the real
 *      data is transferred via the shared area, so we just
 *      pass in dummy values to RPC_Call.
 *
 * Results:
 *      a VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */

static VMKERNEL_ENTRY
DoRPCCall(DECLARE_ARGS(VMK_RPC_CALL))
{
   PROCESS_2_ARGS(VMK_RPC_CALL, RPC_Connection, cnx, int32, rpcFunction);
   uint32 dummy = 0;
   uint32 length = sizeof(dummy);   

   return RPC_Call(cnx, rpcFunction, World_VMM(MY_RUNNING_WORLD)->vmxThreadID,
                   (char *)&dummy, sizeof dummy, 
                   (char *)&dummy, &length);
}


/*
 *----------------------------------------------------------------------
 *
 * DoSemaphoreWait --
 *
 *      Used by vmm's locking code.  
 *
 * Results:
 *      a VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static VMKERNEL_ENTRY
DoSemaphoreWait(DECLARE_ARGS(VMK_SEMAPHORE_WAIT))
{
   PROCESS_3_ARGS(VMK_SEMAPHORE_WAIT, RPC_Connection, cnx,
                  uint32, timeout, uint32, actionMask);
   World_VmmInfo *vmmInfo = World_VMM(MY_RUNNING_WORLD);
   uint32 flags = RPC_CAN_BLOCK;
   VMK_ReturnStatus status;
   RPC_MsgInfo msgInfo;
   uint32 dummy;

   // set action wakeup mask
   vmmInfo->semaActionMask = actionMask;

   // invoke rpc primitive to implement semaphore wait
   msgInfo.data = &dummy;
   msgInfo.dataLength = sizeof(dummy);
   status = RPC_GetMsgInterruptible(cnx, flags, &msgInfo,
                                    timeout, UTIL_VMKERNEL_BUFFER,
                                    INVALID_WORLD_ID);

   // clear action wakeup mask
   vmmInfo->semaActionMask = 0;

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * DoSemaphoreSignal --
 *
 *      Used by vmm's locking code.  
 *
 * Results:
 *      a VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static VMKERNEL_ENTRY
DoSemaphoreSignal(DECLARE_ARGS(VMK_SEMAPHORE_SIGNAL))
{
   RPC_Token token;
   int dummy;
   PROCESS_1_ARG(VMK_SEMAPHORE_SIGNAL, RPC_Connection, cnx);

   return RPC_Send(cnx, 0, 0, (char  *)&dummy, sizeof dummy, UTIL_VMKERNEL_BUFFER, &token);
}

/* 
 *----------------------------------------------------------------------
 *
 * VMKExit
 *
 *      Handle a request from the vmm for the current world 
 *      to exit.  
 *
 * Results:
 *      Does not return.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */

VMKERNEL_ENTRY
VMKExit(DECLARE_1_ARG(VMK_EXIT, uint32, status))
{
   PROCESS_1_ARG(VMK_EXIT, uint32, status);

   World_Exit(status);
   return VMK_OK;
}



/* 
 *----------------------------------------------------------------------
 *
 * VMKCheckVersion
 *
 *      Check that the vmm and vmkernel agree on the interface version
 *
 * Results:
 *      return the result of the version check
 *
 * Side effects:
 *      Warnings if mismatch
 *----------------------------------------------------------------------
 */
static VMKERNEL_ENTRY
VMKCheckVersion(DECLARE_2_ARGS(VMK_CHECK_VERSION, uint32, vmmVersion,
                               uint32, vmmMaxFuncID))
{
   VMK_ReturnStatus status;
   PROCESS_2_ARGS(VMK_CHECK_VERSION, uint32, vmmVersion, uint32, vmmMaxFuncID);

   if (VMMVMK_VERSION_MAJOR(vmmVersion) !=
       VMMVMK_VERSION_MAJOR(VMMVMK_VERSION)) {
      Warning("Version mismatch vmkernel(0x%x) vmm(0x%x)",
              VMMVMK_VERSION, vmmVersion);
      status = VMK_NOT_SUPPORTED;
   } else if (VMMVMK_VERSION_MINOR(vmmVersion) !=
       VMMVMK_VERSION_MINOR(VMMVMK_VERSION)) {
      Warning("Minor version mismatch vmkernel(0x%x) vmm(0x%x)",
              VMMVMK_VERSION, vmmVersion);
      status = VMK_VERSION_MISMATCH_MINOR;
   } else if (VMK_EXT_MIN_FUNCTION_ID != vmmMaxFuncID) {
      Warning("VMKCall Table mismatch %d %d.", VMK_VMM_MAX_FUNCTION_ID,
              VMK_EXT_MIN_FUNCTION_ID);
      status = VMK_NOT_SUPPORTED;
   } else {
      status = VMK_OK;
   }
   return status;
}

typedef VMK_ReturnStatus (*VMKFunction)(uint32 function, va_list args);

/*
 * The stubs for these system calls, VMK_*, are in vmk_if.h, and
 * the system call numbers are defined in vmk_stubs.h
 */
static VMKFunction vmkFuncTable[] = {
#define VMKCALL(num, intState, fnName, group, expl) fnName,
#include "vmkcallTable_vmcore.h"
#include "vmkcallTable_public.h"
#undef VMKCALL
};

/*
 *----------------------------------------------------------------------
 *
 * VMKCall --
 *
 *      Processes a VMKCall from the monitor
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Always returns with interrupts disabled.
 *
 *----------------------------------------------------------------------
 */
void
VMKCall(uint32 function, va_list args, VMK_ReturnStatus *status)
{
   World_Handle *curWorld;
   Bool preemptible;
   Bool cltsNeeded;

   DEBUG_ONLY(Reg32 eflagsBefore; SAVE_FLAGS(eflagsBefore));

   curWorld = MY_RUNNING_WORLD;

   asm("cld");

   ASSERT(VMK_IsVMKStack((VA)&function));
   ASSERT(!myPRDA.inInterruptHandler);
   SP_AssertNoLocksHeld();
   ASSERT(CURRENT_CPL() == 0);

   preemptible = CpuSched_DisablePreemption();
   ASSERT(preemptible);

   cltsNeeded = FALSE;
   if (myPRDA.configNMI && !MY_RUNNING_WORLD->nmisInMonitor) {
      // save CR0 TS bit state
      uint32 cr0;
      GET_CR0(cr0);
      if ((cr0 & CR0_TS) == 0) {
         cltsNeeded = TRUE;
      }
      NMI_Enable();
   }

   Watchpoint_Enable(TRUE);

   if (UNLIKELY(curWorld->deathPending)) {
      World_Exit(VMK_OK);
   }
   Trace_EventLocal(TRACE_VMM_VMKCALL, function, function);
   if (LIKELY(function > VMK_NULL && function < VMK_MAX_FUNCTION_ID)) {
      /* Make the function call.*/
      ASSERT(*vmkFuncTable[function]);
      *status = (*vmkFuncTable[function])(function, args);
   } else {
      World_Panic(curWorld, "VMKCall: Invalid function %d\n", function);
   }
   // indicate that the vmkcall has ended with a null event
   Trace_EventLocal(TRACE_VMM_VMKCALL, 0, 0);
   
   BH_Check(TRUE);

   if (UNLIKELY(curWorld->deathPending)) {
      World_Exit(VMK_OK);
   }
   Watchpoint_Disable(TRUE);

   DEBUG_ONLY({
      Reg32 eflagsAfter;
      SAVE_FLAGS(eflagsAfter);
      /* The monitor copes with VIF & VIP changing, and it does change. */
      ASSERT((eflagsBefore & EFLAGS_PRESERVED_ON_VMKCALL) ==
             (eflagsAfter &  EFLAGS_PRESERVED_ON_VMKCALL));
   });

   CLEAR_INTERRUPTS();

   if (myPRDA.configNMI) {
      if (!MY_RUNNING_WORLD->nmisInMonitor) {
         NMI_Disable();
      } else {
         MY_RUNNING_WORLD->vmkSharedData->htThreadNum = 
            SMP_GetHTThreadNum(myPRDA.pcpuNum);
      }
   }

   // restore CR0 TS bit state
   if (cltsNeeded) {
      __asm__ __volatile__ ("clts");
   }

   ASSERT(!myPRDA.inInterruptHandler);
   SP_AssertNoLocksHeld();

   ASSERT(!CpuSched_IsPreemptible());
   CpuSched_RestorePreemption(preemptible);
}

static void
HostPCPUIdle(UNUSED_PARAM(void *data))
{
   ASSERT_HAS_INTERRUPTS();
   CpuSched_EnablePreemption();
   CpuSched_IdleLoop();
   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * Idle_Init
 *
 * 	Create the idle world to run on CPU 0
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
Idle_Init(void)
{
   World_Handle *idle;
   VMK_ReturnStatus status;

   status = World_NewIdleWorld(HOST_PCPU, &idle);
   if (status != VMK_OK) {
      return status;
   }

   Sched_Add(idle, HostPCPUIdle, NULL);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * CPUCheckType
 *
 *      Determine the type of CPU we are running on 
 *
 * Results:
 *      oneof enum type CPUType
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */

static CPUType
CPUCheckTypeAMD(uint32 version)
{
   CPUType type = CPUTYPE_UNSUPPORTED;
   char name[50];
   uint32 *namePtr;
   uint32 regs[4];
   uint32 reg;

   /*
    * Check number of extended functions
    */
   ASM("cpuid" :
       "=a" (reg) :
       "a" (0x80000000) :
       "ebx", "ecx", "edx");
   if (reg < 0x80000006) {
      Warning("Unsupported AMD - 0x%x max extended functions", reg);
      return CPUTYPE_UNSUPPORTED;
   }

   /*
    * Check AMD processor model
    */
   namePtr = (uint32*)name;
   name[48] = '\0';

   ASM("cpuid" 
       : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3]) 
       : "a" (0x80000002)
       );
   namePtr[0] = regs[0];
   namePtr[1] = regs[1];
   namePtr[2] = regs[2];
   namePtr[3] = regs[3];
   ASM("cpuid" 
       : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3]) 
       : "a" (0x80000003)
       );
   namePtr[4] = regs[0];
   namePtr[5] = regs[1];
   namePtr[6] = regs[2];
   namePtr[7] = regs[3];
   ASM("cpuid" 
       : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3]) 
       : "a" (0x80000004)
       );
   namePtr[8] = regs[0];
   namePtr[9] = regs[1];
   namePtr[10] = regs[2];
   namePtr[11] = regs[3];

   Log("CPU is %s Model %d", name, CPUID_MODEL(version));

#define ATHLON "AMD Athlon"
#define DURON "AMD Duron"

   if (strncmp(name, ATHLON, strlen(ATHLON))==0) {
      if (CPUID_MODEL(version) < 2) {
	 Warning("Unsupported CPU model %d", CPUID_MODEL(version));
	 return CPUTYPE_UNSUPPORTED;
      } else {
	 type = CPUTYPE_AMD_ATHLON;
      }
   } else if (strncmp(name, DURON, strlen(DURON))==0) {
      type = CPUTYPE_AMD_DURON;
   } else {
      Warning("Sorry, %s is an unsupported CPU", name);
      return CPUTYPE_UNSUPPORTED;
   }

   return type;
}

static CPUType
CPUCheckType(void)
{
   char name[16];
   uint32 *namePtr;
   uint32 regs[4];
   uint32 version, features;

   /*
    *  Check cpu family using cpuid instruction
    */
   ASM("cpuid" :
       "=a" (version),
       "=d" (features) :
       "a" (1) :
       "ebx", "ecx");

   /*
    * Check Processor ID string
    */
   ASM("cpuid" 
       : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3]) 
       : "a" (0)
       );

   namePtr = (uint32*)name;
   namePtr[0] = regs[1];
   namePtr[1] = regs[3];
   namePtr[2] = regs[2];
   namePtr[3] = 0;

   if (strcmp(name,"GenuineIntel")==0) {
      if (CPUID_FAMILY_IS_P6(version)) {
	 return CPUTYPE_INTEL_P6;
      }
      if (CPUID_FAMILY_IS_PENTIUM4(version)) {
	 return CPUTYPE_INTEL_PENTIUM4;
      }
      Warning("Unsupported CPU - not P6 class and above, "
	      "version = %d, features = %d", version, features);
      return CPUTYPE_UNSUPPORTED;
   } else if (strcmp(name, "AuthenticAMD")==0) {
      if (CPUID_FAMILY_IS_OPTERON(version)) {
         /* Opteron in legacy mode looks exactly like an Athlon to vmkernel */
         return CPUTYPE_AMD_ATHLON;
      } else {
         return CPUCheckTypeAMD(version);
      }
   } else {
      Warning("Unsupported CPU - not Intel or AMD");
      return CPUTYPE_UNSUPPORTED;
   }
}


Bool VMK_IsValidMPN(MPN mpn)
{
   int i;

   for (i = 0; i < MAX_VMNIX_MEM_RANGES; i++) {
      if (vmnixInit.vmkMem[i].startMPN == 0) {
         break;
      }
      if ((mpn >= vmnixInit.vmkMem[i].startMPN) &&
          (mpn <= vmnixInit.vmkMem[i].endMPN)) {
         return TRUE;
      }
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * InitDebugArea
 *
 *      Some of our linux drivers need to access fields in the prda.
 *      Because the prda isn't visible to gpl modules, a small struct
 *      with the necessary pointers into the prda is used instead.
 *      This function intializes said struct.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static void 
InitDebugArea(void)
{
#ifdef VMX86_DEBUG
   vmkDebug.lastClrIntrRA = &(myPRDA.lastClrIntr);
   vmkDebug.inIntHandler = &(myPRDA.inInterruptHandler);
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * VMK_CheckAssertStress
 *
 *      Check if the current ASSERT check is the nth one, and if so,
 *      return TRUE. Else increment the stress counter
 *
 * Results:
 *      TRUE if this is the nth ASSERT check, FALSE otherwise
 *
 * Side effects:
 *      Increment stress counter
 *
 *----------------------------------------------------------------------
 */
Bool
VMK_CheckAssertStress(void)
{
   if (!VMK_STRESS_DEBUG_OPTION(ASSERT_STRESS)) {
      Panic("VMK_CheckAssertStress called when stress option disabled\n");
   }

   // no failure untill coredump partition is setup
   if (!Dump_IsEnabled()) {
      return FALSE;
   }

#if 0
   if (!World_IsVMMWorld(MY_RUNNING_WORLD)) {
      return FALSE;
   }
#endif
#if 0
   if (!myPRDA.inInterruptHandler) {
      return FALSE;
   }
#endif

   return VMK_STRESS_DEBUG_COUNTER(ASSERT_STRESS);
}

/*
 *----------------------------------------------------------------------
 *
 * InitVersionInfoProcReadHandler --
 *
 *      Prints out the version numbers of various components.
 *
 * Results: 
 *      VMK_OK
 *
 * Side effects:
 *      *page and *lenp are updated.
 *
 *----------------------------------------------------------------------
 */

static int 
InitVersionInfoProcReadHandler(UNUSED_PARAM(Proc_Entry *entry),
                               char        *page,
                               int         *lenp)
{
   *lenp = 0;

   Proc_Printf(page, lenp, PRODUCT_NAME " " PRODUCT_VERSION_NUMBER 
               " [" BUILD_VERSION "], built on " __DATE__ "\n");

   /*
    * The vmkheartbeat script parses this line.  Make sure to update
    * install/server/vmkheartbeat.sh (and vmkuptime.pl) if you muck
    * with this.
    */
   Proc_Printf(page, lenp, "vmkernel build: %d, vmkcall: %d.%d "
               "vmnix interface: %d.%d driver interface: %d.%d kernel: %d.%d\n", BUILD_NUMBER_NUMERIC, 
               VERSION_MAJOR(VMMVMK_VERSION), VERSION_MINOR(VMMVMK_VERSION),
               VERSION_MAJOR(VMX_VMNIX_VERSION), VERSION_MINOR(VMX_VMNIX_VERSION),
               VERSION_MAJOR(VMKDRIVER_VERSION), VERSION_MINOR(VMKDRIVER_VERSION),
               VERSION_MAJOR(VMNIX_VERSION), VERSION_MINOR(VMNIX_VERSION));
   Proc_Printf(page, lenp, "vmnixmod build: %d, interface: %d.%d\n", vmnixmodBuildNumber,
               VERSION_MAJOR(vmnixmodInterfaceNumber), VERSION_MINOR(vmnixmodInterfaceNumber));
   Proc_Printf(page, lenp, "vmnix kernel interface: %d.%d\n",
               VERSION_MAJOR(vmnixKernelVersion), VERSION_MINOR(vmnixKernelVersion));
   Proc_Printf(page, lenp, "Loadable module version info:\n");
   Mod_ProcPrintVersionInfo(page, lenp);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VMK_VA2MA --
 *
 *      Convert a virtual address to machine address
 *
 * Results:
 *      machine address corresponding to parameter address
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
MA
VMK_VA2MA(VA vaddr)
{
   MPN mpn = INVALID_MPN;
   MA maddr;

   ASSERT(vaddr < VMK_VA_END);
   if ((vaddr >= VMK_HOST_STACK_BASE) && (vaddr < VMK_HOST_STACK_TOP)) {
      // `address' on host stack.
      ASSERT(CpuSched_IsHostWorld());
      mpn = Host_StackVA2MPN(vaddr);
   } else if ((vaddr >= VMK_FIRST_ADDR) && (vaddr < VMK_KVMAP_BASE)) {
      // `address' references Mem_Alloc memory 
      mpn = Mem_VA2MPN(vaddr);
   } else if ((vaddr >= VMK_KVMAP_BASE) && (vaddr < VMK_KVMAP_BASE + VMK_KVMAP_LENGTH)) {
      // `address' represents virtual address in the KVMap segment.
      mpn = KVMap_VA2MPN(vaddr);
   } else if ((vaddr >= VMK_FIRST_XMAP_ADDR) && 
              (vaddr < VMK_FIRST_XMAP_ADDR + VMK_XMAP_LENGTH)) {
      // `address' represents virtual address in the XMap segment.
      mpn = XMap_VA2MPN(vaddr);
   } else {
      int i;
      VPN stackBaseVPN = VA_2_VPN(World_GetVMKStackBase(MY_RUNNING_WORLD));
      FOR_ALL_VMK_STACK_MPNS(MY_RUNNING_WORLD, i) {
         if (VA_2_VPN(vaddr) == stackBaseVPN + i) {
            LOG(2, "translating stack address %#x", vaddr);
            ASSERT(!CpuSched_IsHostWorld());
            mpn = MY_RUNNING_WORLD->vmkStackMPNs[i];
            break;
         }
      }
   }
   ASSERT(mpn != INVALID_MPN);
 
   maddr = MPN_2_MA(mpn) + (vaddr & PAGE_MASK);
#ifdef HOSTSCSI_DEBUG
   Warning("mpn: %d maddr: 0x%Lx vaddr: 0x%x off: %d",
            mpn, maddr, vaddr, (vaddr & PAGE_MASK));
#endif
   return maddr;
}

/*
 *----------------------------------------------------------------------
 *
 * VMK_MA2VA --
 *
 *      Convert a virtual address to machine address (For use only in linux
 *      driver code! (our implementation of phys_to_virt).
 *
 *	Note that this does not work on XMap address space. An assert failure should
 *	occur if Mem_MA2VPN returns an unexpected value.
 *
 * Results:
 *      a virtual address corresponding to parameter address
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
VA
VMK_MA2VA(MA maddr)
{
   int i;
   VPN vpn;

   if ((vpn = Host_StackMA2VPN(maddr)) != INVALID_VPN) {
      // 'maddr' on host stack.
      ASSERT(CpuSched_IsHostWorld());
      return VPN_2_VA(vpn) + ((VA)maddr & PAGE_MASK);
   }

   FOR_ALL_VMK_STACK_MPNS(MY_RUNNING_WORLD, i) {
      if (MA_2_MPN(maddr) == MY_RUNNING_WORLD->vmkStackMPNs[i]) {
         /*
          * `maddr' references world stack memory.
          *
          * ioctl handlers in the linux drivers seem to do this a whole lot.
          */
         LOG(2, "translating stack page maddr 0x%Lx", maddr);
         return World_GetVMKStackBase(MY_RUNNING_WORLD) + i * PAGE_SIZE +
            ((VA)maddr & PAGE_MASK);
      }
   }
   
   /*
    * `maddr' references Mem_Alloc memory.
    */
   vpn = Mem_MA2VPN(maddr);

   /* this assert should catch machine addresses that don't belong to mainHeap */
   ASSERT(vpn != INVALID_VPN);

   return VPN_2_VA(vpn) + ((VA)(maddr) & PAGE_MASK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Init_VMKernelIDCallback --
 *
 *      Config option callback for vmkernelID. 
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Sets vmkernelID. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus 
Init_VMKernelIDCallback(Bool write, Bool changed, int idx)
{
   if (write && changed) {
      vmkernelID = Config_GetOption(CONFIG_IPADDRESS);
      Log("vmkernelID = %d.", vmkernelID)
   }
   if ((vmkernelID == 0) || ((vmkernelID & 127) == 127)) {
      SysAlert("Invalid vmkernel id: %d. Distributed vmfs locking may not work.", vmkernelID);
   }
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMK_Get/ValidVMKernelID --
 *
 *      Get/check vmkernelID. 
 *
 * Results:
 *      vmkernelID.  
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

int
VMK_GetVMKernelID(void)
{
   return vmkernelID;
}

Bool
VMK_CheckVMKernelID(void)
{
   if ((vmkernelID == 0) || ((vmkernelID & 127) == 127)) {
      return FALSE;
   }
   return TRUE;
}

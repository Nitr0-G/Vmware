/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * host.c - host related functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "memmap.h"
#include "world.h"
#include "sharedArea.h"
#include "idt.h"
#include "host.h"
#include "rpc.h"
#include "vmkemit.h"
#include "vmk_stubs.h"
#include "timer.h"
#include "debug.h"
#include "util.h"
#include "net.h"
#include "socket_dist.h"
#include "action.h"
#include "mod_loader.h"
#include "pci.h"
#include "apic.h"
#include "chipset.h"
#include "smp.h"
#include "alloc.h"
#include "vmk_scsi.h"
#include "volumeCache.h"
#include "memalloc.h"
#include "bh.h"
#include "kseg.h"
#include "libc.h"
#include "config.h"
#include "proc.h"
#include "serial.h"
#include "nmi.h"
#include "mce.h"
#include "kvmap.h"
#include "helper.h"
#include "post.h"
#include "bluescreen.h"
#include "dump.h"
#include "user.h"
#include "pagetable.h"
#include "watchpoint.h"
#include "migrateBridge.h"
#include "vmkevent.h"
#include "fsSwitch.h"
#include "vmksysinfo.h"
#include "conduit_bridge.h"
#include "cosdump.h"
#include "log_int.h"
#include "term.h"
#include "cow.h"
#include "vscsi.h"
#include "fsDeviceSwitch.h"
#include "fsNameSpace.h"
#include "fsClientLib.h"
#include "trace.h"

#define LOGLEVEL_MODULE Host
#include "log.h"

static Task hostVMKTask, hostDFTask, hostNmiTask;
static Task *hostTaskAddr;
static uint8 hostDFStack[PAGE_SIZE];
static uint8 hostNmiStack[PAGE_SIZE];

static Descriptor *hostGDT;

// only here to help debugging
static void *hostIDTHandlers;

static MA hostInVmkernelCR3;

static Bool hostShouldIdle = TRUE;

static int vmkernelBroken;
unsigned debugRegs[NUM_DEBUG_REGS];
unsigned statCounters[VMNIX_STAT_NUM];

/* 
 * Used by the network and scsi modules to provide device information to the
 * host vmnix module. vmkDevLock protects vmkDev.qTail.
 */
VMnix_VMKDevShared         vmkDev;
SP_SpinLockIRQ             vmkDevLock;

HostTime hostTime;
Atomic_uint32 interruptCause;

// store the BIOS info on IDE disks -- needed by the ide driver
unsigned char drive_info[MAX_BIOS_IDE_DRIVES*DRIVE_INFO_SIZE];

typedef struct HostExcFrame {
   uint16    es, __esPad;
   uint16    ds, __dsPad;
   uint16    gs, __gsPad;
   uint16    fs, __fsPad;
   uint32    eax;
   uint32    ecx;
   uint32    edx;
   uint32    ebx;
   uint32    ebp;
   uint32    esi;
   uint32    edi;
} HostExcFrame;

/*
 * Local typedef used to collect ConduitEnable variables for call to 
 * IPC variant of ConduitEnable.  IPC variant is used because the call 
 * to Conduit_Enable may block and the VMX client cannot tolerate this.
 */
typedef struct Fn_ConduitEnableArgs {       
   Conduit_HandleEnableArgs     args;
   World_ID                     worldID;
   Conduit_HandleID             handleID; 
} Fn_ConduitEnableArgs;



typedef int (*SyscallHandler)(uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5);

extern void HostEntry(void);
extern void CommonNmiHandler(void);
extern void HostAsmVMKEntry(void);

// functions to be called back in COS context.
extern void HostAsmRetHidden(void);
extern void HostAsmRetGenTrap(void);
extern void HostAsmRetGenIntr(void);
extern void HostAsmRetGenTrapErr(void);
extern void HostAsmRetGenIntrErr(void);

static void HostDoubleFaultHandler(void);

static void *HostDefineGate(Gate *hostIDT, int gateNum,
                            void (*handler)(VMKExcFrame *regs),
                            Bool hasErrorCode, int dpl, void *codeStart);

static void HostHandleException(VMKExcFrame *regs);
static void HostHandleInterrupt(VMKExcFrame *regs);
static void HostSyscall(VMKExcFrame *regs);
static void HostReturnCheckIntr(VMKExcFrame *regs, Bool unloaded,
                                Bool interruptOK);
static void HostReturnGenerateInt(VMKExcFrame *regs,
                                  int gateNum, Bool hasErrorCode);
static void HostReturnHidden(VMKExcFrame *regs);

static void HostWorldInitContext(void);

static VMK_ReturnStatus HostMakeSyncCall(void *hostArgs, int argSize, 
                                         HelperRequestSyncFn *fn,
		                         Helper_RequestHandle *hostHelperHandle);

static VMK_ReturnStatus HostMakeSyncCallWithResult(void *hostArgs, int argSize, 
                                                   void *hostResult, int resSize,
                                                   HelperRequestSyncFn *fn,
                                                   Helper_RequestHandle *hostHelperHandle);

static VMK_ReturnStatus Host_Idle(void);
static World_ID Host_CreateWorld(VMnix_CreateWorldArgs *hostArgs);
static int Host_BindWorld(World_ID worldID, uint32 vcpuid, World_ID *hostWorldID);
static int Host_DestroyWorld(World_ID worldID);
static VMK_ReturnStatus Host_RunWorld(VMnix_RunWorldArgs *hostArgs);
static VMK_ReturnStatus Host_ReadRegs(World_ID worldID, char *buffer, unsigned long bufferLength);
static VMK_ReturnStatus Host_RPCGetMsg(RPC_Connection cnxID, RPC_MsgInfo *outMsgInfo);
static VMK_ReturnStatus Host_RPCSendMsg(RPC_Connection cnxID, int32 function,
				  char *data, uint32 dataLength);
static VMK_ReturnStatus Host_RPCPostReply(RPC_Connection cnxID, RPC_Token token,
                                          void *buf, uint32 bufLen);
static VMK_ReturnStatus Host_RPCRegister(char *inName, int nameLength, 
                                   RPC_Connection *resultCnxID);
static VMK_ReturnStatus Host_RPCUnregister(RPC_Connection cnxID,
                                           Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostNetPortEnable(VMnix_NetPortEnableArgs *hostArgs,
                                          Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostNetPortDisable(VMnix_NetPortDisableArgs *hostArgs,
                                           Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostOpenSCSIDevice(VMnix_OpenSCSIDevArgs *hostArgs,
					   VMnix_OpenSCSIDevIntResult *hostResult,
					   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCloseSCSIDevice(World_ID worldID,
					    SCSI_HandleID *handleID,
				Helper_RequestHandle *helperHandle);
static VMK_ReturnStatus HostFileGetPhysLayout(VMnix_FileGetPhysLayoutArgs *hostArgs,
					      VMnix_FileGetPhysLayoutResult *hostResult,
					      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostSCSIGetCapacity(SCSI_HandleID *handleID, 
				VMnix_GetCapacityResult *result);
static VMK_ReturnStatus HostSCSIGetGeometry(SCSI_HandleID *handleID, 
				VMnix_GetCapacityResult *result,
					   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostSCSIAdapterList(VMnix_AdapterListArgs *hostArgs, 
                                            VMnix_AdapterListResult *hostResult,
                                            Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostGetLUNList(VMnix_LUNListArgs *hostArgs, 
                                       VMnix_LUNListResult *hostResult,
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostSetDumpPartition(VMnix_SetDumpArgs *hostArgs,
                                             Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSCreate(VMnix_FSCreateArgs *hostArgs,
				   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSToVMFS2(VMnix_ConvertToFS2Args *hostArgs,
                                      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSExtend(VMnix_FSExtendArgs *hostArgs,
                                     Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSGetAttr(VMnix_FSGetAttrArgs *hostArgs,
				      VMnix_PartitionListResult *hostResult,
				      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSSetAttr(VMnix_FSSetAttrArgs *hostArgs,
				      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSDump(VMnix_FSDumpArgs *hostArgs,
				   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFSReaddir(VMnix_ReaddirArgs *hostArgs,
                                      VMnix_ReaddirResult *hostResult,
                                      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileCreate(VMnix_FileCreateArgs *hostArgs,
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCOWOpenHierarchy(VMnix_COWOpenHierarchyArgs *hostArgs, 
                                             VMnix_COWOpenHierarchyResult *hostResult,
                                             Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCOWCombine(VMnix_COWCombineArgs *hostArgs, 
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCOWCloseHierarchy(COW_HandleID *chi, 
					      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostVSCSICreateDev(VMnix_VSCSICreateDevArgs *hostArgs, 
                                           VMnix_VSCSICreateDevResult *hostResult,
                                           Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostVSCSIDestroyDev(VMnix_VSCSIDestroyDevArgs *hostArgs, 
                                           Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCOWGetBlockNumberAndFID(VMnix_COWGetFIDAndLBNArgs *hostArgs, 
						    VMnix_COWGetFIDAndLBNResult *hostResult,
                                                    Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostMapRawDisk(VMnix_MapRawDiskArgs *hostArgs,
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostQueryRawDisk(VMnix_QueryRawDiskArgs *hostArgs,
                                         VMnix_QueryRawDiskResult *hostResult,
                                         Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileLookup(VMnix_FileLookupArgs *hostArgs,
                                       VMnix_FileLookupResult *hostResult,
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileOpen(VMnix_FileOpenArgs *hostArgs, 
				     VMnix_FileOpenResult *hostResult,
				     Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileAttr(VMnix_FileAttrArgs *hostArgs,
				     VMnix_FileAttrResult *hostResult,
				     Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileSetAttr(VMnix_FileSetAttrArgs *hostArgs,
				   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileIO(VMnix_FileIOArgs *hostArgs,
				   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileClose(FS_FileHandleID *hostHandleID,
				      Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileRemove(VMnix_FileRemoveArgs *hostArgs,
				       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFileRename(VMnix_FileRenameArgs *hostArgs,
				       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFilePhysMemIO(VMnix_FilePhysMemIOArgs *hostArgs,
				   Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostLUNReserve(VMnix_LUNReserveArgs *hostArgs,
				Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostNetInfo(uint32 cmd, VMnix_NetInfoArgs *hostArgs);
static VMK_ReturnStatus HostTargetInfo(VMnix_TargetInfoArgs *hostArgs,
                                       VMnix_TargetInfo *hostResult,
                                       Helper_RequestHandle *hostHelperHandle);
static void HostTimerCallback(void *ignore, Timer_AbsCycles timestamp);

static VMK_ReturnStatus HostSCSICommand(SCSI_HandleID *hostHandleID,
                                        HostSCSI_Command *command);
static VMK_ReturnStatus HostBlockCommand(SCSI_HandleID *hostHandleID,
                                         HostSCSI_Command *command);


static VMK_ReturnStatus HostSCSICmdComplete(SCSI_HandleID *hostHandleID,
                                            SCSI_Result *outResult,
                                            Bool *more);

static VMK_ReturnStatus HostHelperRequestStatus(Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostHelperRequestCancel(Helper_RequestHandle *hostHelperHandle,
                                                Bool force);

static VMK_ReturnStatus HostMarkCheckpoint(VMnix_MarkCheckpointArgs *args,
				Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostCheckpointCleanup(World_ID *data);
static VMK_ReturnStatus HostSaveMemory(void *wPtr);
static VMK_ReturnStatus HostMigrateWriteCptData(VMnix_MigCptDataArgs *hostArgs,
                                                void *hostBuf);
static VMK_ReturnStatus HostMigrateToBegin(World_ID *wPtr,
                                           VMnix_MigrateProgressResult
                                           *hostProgress);
static VMK_ReturnStatus HostMigrateReadCptData(VMnix_MigCptDataArgs *hostArgs,
                                               void *hostData,
                                               uint32 hostDataLength);
static VMK_ReturnStatus HostMigrateSetParameters(VMnix_MigrationArgs
                                                 *hostArgs);
static VMK_ReturnStatus HostCheckActions(World_ID *data);

static void HostWarning(const char *string, int length);
static void HostDisableInterrupt(IRQ irq);
static void HostEnableInterrupt(IRQ irq);
static VMK_ReturnStatus HostFindAdapName(uint32 bus, uint32 devfn, char* name);
static VMK_ReturnStatus HostAllocVMKMem(uint32 *hostSize, void **hostResult);
static VMK_ReturnStatus HostFreeVMKMem(void **hostAddr);

static VMK_ReturnStatus HostSaveBIOSInfoIDE(char *info);

static VMK_ReturnStatus HostActivateSwapFile(VMnix_ActivateSwapFileArgs *hostArgs,
					     Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostDeactivateSwapFile(uint32 *fileNum,
                                               Helper_RequestHandle *hostHelperHandle);

static VMK_ReturnStatus HostMemMap(VMnix_DoMemMapArgs *hostArgs);
static VMK_ReturnStatus HostSetMemMapLast(VMnix_SetMMapLastArgs *hostArgs);

static VMK_ReturnStatus Host_GetAPICBase(MA *ma);

static VMK_ReturnStatus HostSCSIAdapProcInfo(VMnix_ProcArgs *args,
                                             VMnix_ProcResult *result,
                                             Helper_RequestHandle *helperHandle); 
static VMK_ReturnStatus HostGetNICStats(VMnix_NICStatsArgs *args,
                                        void *result,
                                        Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostSCSIDevIoctl(VMnix_SCSIDevIoctlArgs *args,
                                         VMnix_SCSIDevIoctlResult *hostResult,
                                         Helper_RequestHandle *helperHandle); 
static VMK_ReturnStatus HostCharDevIoctl(VMnix_CharDevIoctlArgs *args,
                                         VMnix_CharDevIoctlResult *hostResult,
                                         Helper_RequestHandle *helperHandle); 
static VMK_ReturnStatus HostNetDevIoctl(VMnix_NetDevIoctlArgs *args,
                                        VMnix_NetDevIoctlResult *hostResult,
                                        Helper_RequestHandle *helperHandle); 
static VMK_ReturnStatus HostDelaySCSICmds(World_ID worldID, uint32 delay);
static VMK_ReturnStatus HostScanAdapter(VMnix_ScanAdapterArgs *hostArgs,
					Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostModUnload(VMnix_ModUnloadArgs *hostArgs,
				      Helper_RequestHandle *hostHelperHandle);
VMK_ReturnStatus Init_PostBootMemory(VMnix_HotAddMemory *data);
static VMK_ReturnStatus HostCreateConduitAdapter(VMnix_CreateConduitAdapArgs *hostOpenArgs,
                                                 VMnix_CreateConduitAdapResult *result);
static VMK_ReturnStatus HostConduitEnable(Conduit_HandleID handleID, 
                                          World_ID worldID,
                                          Conduit_HandleEnableArgs *args,
                                          Helper_RequestHandle *helperHandle);
static VMK_ReturnStatus HostConduitDisable(Conduit_HandleID *handlePtr);
static VMK_ReturnStatus HostGetConduitVersion(Conduit_HandleID *hostHandleID);
static VMK_ReturnStatus HostRemoveConduitAdapter(VMnix_RemoveConduitAdapArgs *hostArgs);
static VMK_ReturnStatus HostConduitVDevInfo(VMnix_ConduitDevInfoArgs *args,
                                            CnDev_Record *rec);
static VMK_ReturnStatus HostConduitNewPipe(VMnix_ConduitNewPipeArgs *hostOpenArgs,
                                           Conduit_OpenPipeArgs *result);
static VMK_ReturnStatus HostConduitTransmit(Conduit_HandleID *hostHandleID);
static VMK_ReturnStatus HostConduitDeviceMemory(VMnix_ConduitDeviceMemoryArgs *hostArgs,
                                                Conduit_DeviceMemoryCmd *result);
static VMK_ReturnStatus HostConduitGetBackingStore(Conduit_HandleID handleID,
                                                   uint32 pgNum, MPN *allocMPN);
static VMK_ReturnStatus HostConduitConfigDevForWorld(VMnix_ConduitConfigDevForWorldArgs	*args);
static VMK_ReturnStatus HostConduitRemovePipe(VMnix_ConduitRemovePipeArgs *hostArgs);
static VMK_ReturnStatus HostConduitLockPage(VMnix_ConduitLockPageArgs *hostArgs
                                            , uint32 *result);
static uint32 HostConduitSend(World_ID worldID, 
                              Conduit_HandleID *hostHandleID);

static VMK_ReturnStatus HostReadVMKCoreMem(VA vaddr, uint32 len, char *buffer);
static void Host_SetActiveIoctlHandle(Helper_RequestHandle handle);
static void HostCopyToCOS(char *cosAddr, const char *vmkAddr, uint32 len);
static void HostCopyFromCOS(char *vmkAddr, const char *cosAddr, uint32 len);
static void HostGetNicState(char *inBuf, VMnix_CosVmnicInfo *vmnicInfo);
static VMK_ReturnStatus HostGetTimeOfDay(int64 *tod);
static VMK_ReturnStatus HostSetTimeOfDay(const int64 *tod);
static VMK_ReturnStatus HostSetCOSContext(VMnix_SetCOSContextArgs *arg);
static Bool HostGetNextAnonPage(World_ID world, MPN inMPN,
				VMnix_GetNextAnonPageResult *out);
static VMK_ReturnStatus HostReadPage(World_ID worldID, VPN vpn, void *data);
static VMK_ReturnStatus HostReadVMKStack(World_ID worldID, int pageNum,
					 void *data, VA *vaddr);
static MPN HostLookupMPN(World_ID worldID, VPN userVPN);

static VMK_ReturnStatus Host_UserAddArg(VMnix_SetWorldArgArgs *hostArgs);
static VMK_ReturnStatus Host_UserSetBreak(VMnix_SetBreakArgs* hostArgs);
static VMK_ReturnStatus Host_UserSetLoaderInfo(VMnix_SetLoaderArgs* hostArgs);
static VMK_ReturnStatus Host_UserMapFile(VMnix_UserMapFileArgs* hostArgs);
static VMK_ReturnStatus Host_UserMapSection(VMnix_UserMapSectionArgs* hostArgs);
static VMK_ReturnStatus Host_UserSetWorldWD(VMnix_SetWorldWDArgs* hostArgs);
static VMK_ReturnStatus Host_UserForwardSignal(VMnix_ForwardSignalArgs* hostArgs);
static VMK_ReturnStatus Host_UserProxyObjReady(VMnix_ProxyObjReadyArgs*
					       hostArgs,Helper_RequestHandle*
					       helperHandle);
static VMK_ReturnStatus Host_UserSetIdentity(VMnix_SetWorldIdentityArgs *hostArgs);
static VMK_ReturnStatus Host_UserSetDumpFlag(VMnix_SetWorldDumpArgs *hostArgs);
static VMK_ReturnStatus Host_UserSetMaxEnvVars(VMnix_SetMaxEnvVarsArgs *hostArgs);
static VMK_ReturnStatus Host_UserAddEnvVar(VMnix_AddEnvVarArgs *hostArgs);
static VMK_ReturnStatus Host_UserCreateSpecialFds(VMnix_CreateSpecialFdsArgs *hostArgs);

static VMK_ReturnStatus HostAllocLowVMKPages(MPN *mpn, uint32 pages);
static VMK_ReturnStatus HostFreeLowVMKPages(MPN mpn);
static VMK_ReturnStatus HostCosPanic(VMnix_CosPanicArgs *hostArgs);

static VMK_ReturnStatus HostFDSMakeDev(VMnix_FDS_MakeDevArgs *hostArgs, 
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFDSMakeDevFn(void *data, void **result);
static VMK_ReturnStatus HostUserSetExecName(VMnix_SetExecNameArgs *hostArgs);
static VMK_ReturnStatus HostFDSOpenDev(VMnix_FDS_OpenDevArgs *args, 
                                       VMnix_FDS_OpenDevResult *result, 
                                       Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFDSCloseDev(VMnix_FDS_CloseDevArgs *args, 
                                        Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFDSIO(VMnix_FDS_IOArgs *hostArgs,
                                  Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostFDSIoctl(VMnix_FDS_IoctlArgs *hostArgs,
                                     Helper_RequestHandle *hostHelperHandle);
static VMK_ReturnStatus HostModAlloc(VMnix_ModAllocArgs *hostArgs,
                                     VMnix_ModAllocResult *hostResult);
static VMK_ReturnStatus HostModPutPage(int moduleID, void *addr,void *hostData);
static VMK_ReturnStatus HostModLoadDone(VMnix_ModLoadDoneArgs *hostArgs);
static VMK_ReturnStatus HostModList(int maxModules,
                                    VMnix_ModListResult *hostList);
static VMK_ReturnStatus HostModAddSym(VMnix_SymArgs *hostArgs);
static VMK_ReturnStatus HostModGetSym(VMnix_SymArgs *hostArgs);

static VMK_ReturnStatus HostUserSetCosPid(VMnix_SetCosPidArgs *hostArgs);

/*
 * Generate system call table.  See vmk_sctableh for details.
 */
#define VMNIX_VMK_CALL(dummy, handler, _ignore...)\
        (SyscallHandler)handler,
#define VMX_VMK_PASSTHROUGH(dummy, handler)\
        (SyscallHandler)handler,
static SyscallHandler syscallTable[] = {
#include "vmk_sctable.h"
};

#define NUM_SYSCALLS (sizeof(syscallTable) / sizeof(SyscallHandler))

static Gate origHostIDTCopy[IDT_NUM_VECTORS];
static Gate *origHostIDT;
static int32 origHostIDTLength;
static HostIC hostIC;
static SP_SpinLockIRQ hostICPendingLock;
static Bool hostInited = FALSE;

uint32	        hostCR0 = 0;
uint32		hostCR4 = 0;
World_Handle    *hostWorld = NULL;


#define HOST_IDLE_WAIT_EVENT     1

static const uint8 zeroPage[PAGE_SIZE];


/*
 *-----------------------------------------------------------------------------
 *
 * Host_Stack { MA2VPN | VA2MPN } 
 *
 *      Convert address on host world stack. Used in vmkernel implementation of
 *      virt_to_phys/phys_to_virt for drivers.
 *
 * Results:
 *      Returns the MPN/VPN of the address on the host world's stack. 
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VPN
Host_StackMA2VPN(MA maddr) 
{
   int i;

   FOR_ALL_VMK_STACK_MPNS(hostWorld, i) {
      if (MA_2_MPN(maddr) == hostWorld->vmkStackMPNs[i]) {
         return (VA_2_VPN(VMK_HOST_STACK_BASE) + i);
      }
   }
   return INVALID_VPN;
}

MPN 
Host_StackVA2MPN(VA vaddr)
{
   int i;

   ASSERT((vaddr >= VMK_HOST_STACK_BASE) && (vaddr < VMK_HOST_STACK_TOP));
   i = VA_2_VPN(vaddr) - VA_2_VPN(VMK_HOST_STACK_BASE);
   return hostWorld->vmkStackMPNs[i];
}


/*
 *-----------------------------------------------------------------------------
 *
 *  HostCopyTo/FromCOS --
 *
 *      Non-inline versions of Copy*Host.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

static void 
HostCopyToCOS(char *cosAddr, 
              const char *vmkAddr, 
              uint32 len)
{
   CopyToHost(cosAddr, vmkAddr, len);
}

static void 
HostCopyFromCOS(char *vmkAddr, 
                const char *cosAddr, 
                uint32 len)
{
   CopyFromHost(vmkAddr, cosAddr, len);
}

/*
 *----------------------------------------------------------------------------
 * 
 * HostRequestCancelFn --
 *
 *       Generic cleanup function to be invoked when a request is
 *       cancelled.
 *
 * Results:
 *       none.
 *
 * Side effects:
 *       none.
 *
 *----------------------------------------------------------------------------
 */
static void
HostRequestCancelFn(void *data)
{
   ASSERT(data);
   Mem_Free(data);
}

/*
 *----------------------------------------------------------------------------
 * 
 * HostIssueSyncCall --
 *
 *    Implement a system call by using the helper world associated
 *    with the the specified qType, to call function 'fn'.
 *
 *    The function 'fn' must free the argument structure 'args' that is
 *    allocated here and passed to it as 'data'.
 *
 * Results:
 *      VMK_STATUS_PENDING if the request was issued, error code otherwise
 *
 * Side effects:
 *      Helper request made. 
 *
 *----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostIssueSyncCall(void *hostArgs, int argSize, Helper_QueueType qType,
                  HelperRequestSyncFn *fn, 
                  Helper_RequestHandle *hostHelperHandle)
{
   void *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   ASSERT(hostArgs);
   args = Mem_Alloc(argSize);
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, argSize);

   helperHandle = Helper_RequestSync(qType, fn, args, HostRequestCancelFn,
                                     0, NULL);
   if (helperHandle == HELPER_INVALID_HANDLE) {
      Mem_Free(args);
      return VMK_NO_FREE_HANDLES;
   }
   status = VMK_STATUS_PENDING;

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus
HostMakeSyncCall(void *hostArgs, int argSize, HelperRequestSyncFn *fn,
		 Helper_RequestHandle *hostHelperHandle)
{
   return HostIssueSyncCall(hostArgs, argSize, HELPER_MISC_QUEUE,
                            fn, hostHelperHandle);
}


/*
 *----------------------------------------------------------------------------
 * 
 * HostConduitDeviceMemory --
 *
 *
 *	Populates or depopulates a region of the device memory 
 *	associated with the targeted Conduit adapter
 *
 *
 * Results: 
 *	None.
 *	
 *
 * Side effects:
 *      Device range is backed or freed by MPNs (populate/depopulate)
 *      Device range is tagged or untagged (tag option)
 *      Device range tag is returned (another tag option)
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitDeviceMemory(VMnix_ConduitDeviceMemoryArgs *hostArgs,
                        Conduit_DeviceMemoryCmd *result)
{
   VMK_ReturnStatus status;
   VMnix_ConduitDeviceMemoryArgs args;
   Conduit_DeviceMemoryCmd *conduitArgs;

   CopyFromHost(&args, hostArgs, sizeof(args));
   conduitArgs = &(args.cmd);

   status = Conduit_DeviceMemory(args.handleID, conduitArgs);

   CopyToHost(result, conduitArgs, sizeof(Conduit_DeviceMemoryCmd));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostConduitGetBackingStore --
 *
 *      Front end for vmkernel lock page handler.  Dereferences
 *	range of pgnum to find the proper MPN.
 *
 * Results: 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitGetBackingStore(Conduit_HandleID handleID, 
                           uint32 pgNum, MPN *allocMPN)
{
   VMK_ReturnStatus status;
   MPN		mpn;
   
   status = Conduit_GetBackingStore(hostWorld, pgNum, &mpn);
   CopyToHost(allocMPN, &mpn, sizeof(mpn));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostCreateConduitAdapter --
 *
 *      Open an instance of a conduit adapter.  The "adapter" is an
 *      encapsulating mechanismism which allows for better scaling
 *      and a place for per-world administration state to be kept.
 *
 * Results: 
 *	On success, a handle id is returned that can be used for future
 *	operations on this device instance.
 *
 * Side effects:
 *	A new handle is allocated. Fields in the in/out args 
 *	structure may be updated i.e. name.
 *
 *----------------------------------------------------------------------
 */



static VMK_ReturnStatus
HostCreateConduitAdapter(VMnix_CreateConduitAdapArgs *hostOpenArgs,
                         VMnix_CreateConduitAdapResult *result)
{
   Conduit_ClientType clientType;
   VMK_ReturnStatus status;
   VMnix_CreateConduitAdapResult resArgs;
   VMnix_CreateConduitAdapArgs openArgs;

   CopyFromHost(&openArgs, hostOpenArgs, sizeof(openArgs));

   if (openArgs.worldID == INVALID_WORLD_ID) {
      openArgs.worldID = hostWorld->worldID;
      clientType = CONDUIT_HANDLE_HOST;
   } else {
      clientType = CONDUIT_HANDLE_VMM;
   }
   status = Conduit_CreateAdapter(&openArgs, 
                                  clientType, &(resArgs.handleID));
   if (status == VMK_OK) {
      CopyToHost(result, &resArgs, sizeof(resArgs));
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostConduitEnableFn --
 *
 *      Helper function for HostGetConduitEnableFn(). Call Conduit_Enable
 *      and return status.
 *
 * Results:
 *      Status of conduit module call. 
 *      
 * Side effects:
 *      If call was successful, targeted adapter or pipe is enabled.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitEnableFn(void* data,
                    void** result)
{
   VMK_ReturnStatus status;
   Fn_ConduitEnableArgs *fnArgs = (Fn_ConduitEnableArgs *) data;
   Conduit_HandleEnableArgs args;

   args = fnArgs->args;


   status =
      Conduit_Enable(fnArgs->handleID, fnArgs->worldID, &args);
   *result = NULL;
   Mem_Free(fnArgs);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostConduitEnable --
 *
 *	Host entry point for the interface to recommision a disabled
 *	conduit adapter or pipe.  The adapter is the holder of pipes for
 *	a particular world and the focal point for conduit/pipe actions.
 *
 * Results: 
 *	Returns status pending after calling async helper routine if
 *	everything is ok, returns VMK_NO_MEMORY if unable to allocate
 *      for call to async routine.
 *
 * Side effects:
 *	An adapter or pipe handle is recommissioned.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitEnable(Conduit_HandleID handleID,
                  World_ID worldID,
                  Conduit_HandleEnableArgs *devArgs,
                  Helper_RequestHandle *hostHelperHandle)
{
   Conduit_HandleEnableArgs args;
   Helper_RequestHandle helperHandle;
   Fn_ConduitEnableArgs *fnArgs;

   CopyFromHost(&args, devArgs, sizeof(Conduit_HandleEnableArgs));
   fnArgs = Mem_Alloc(sizeof(Fn_ConduitEnableArgs));
   if (fnArgs == NULL) {
      return VMK_NO_MEMORY;
   }
   if (worldID == INVALID_WORLD_ID) {
      fnArgs->worldID = hostWorld->worldID;
   } else {
      fnArgs->worldID = worldID;
   }
   fnArgs->args = args;
   fnArgs->handleID = handleID;
   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, 
                                     HostConduitEnableFn, fnArgs, 
                                     HostRequestCancelFn,
                                     0, NULL);
   CopyToHost(hostHelperHandle, 
              &helperHandle, sizeof(Helper_RequestHandle));

   return VMK_STATUS_PENDING;
}


/*
 *----------------------------------------------------------------------
 *
 * HostConduitDisable --
 *
 *	Host entry point for the interface to decommission or disable a
 *	conduit adapter or pipe.  The adapter is the holder of pipes for
 *	a particular world and the focal point for conduit/pipe actions.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	An adapter or pipe handle is decommissioned.
 *	Guest conduit memory is unmapped and the device callback table in 
 *	the conduit is cleared.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitDisable(Conduit_HandleID *handlePtr)
{
   Conduit_HandleID handle;
   CopyFromHost(&handle, handlePtr, sizeof(Conduit_HandleID));
   return Conduit_VMXDisable(handle);
}


/*
 *----------------------------------------------------------------------
 *
 * HostGetConduitVersion --
 *
 *      Return the conduit version to a host caller.
 *
 * Results: 
 *	CONDUIT_MAGIC
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostGetConduitVersion(Conduit_HandleID *hostHandleID)
{
   return Conduit_HostGetConduitVersion(hostHandleID);
}


/*----------------------------------------------------------------------
 *
 * HostConduitConfigDevForWorld --
 *
 *	Authorize a device back-end for a particular world.  This makes
 *	the device visible and attachable in the target world.  For
 *	security reasons this call is not made availble to guest
 *	level conduit interfaces.
 *
 * Results: 
 *	Device record contains an element for target world authorizing
 *	the world to see the device on queries and to attach to the 
 *	device on demand.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */


static VMK_ReturnStatus 
HostConduitConfigDevForWorld(VMnix_ConduitConfigDevForWorldArgs	*args)
{
   VMnix_ConduitConfigDevForWorldArgs devArgs;
   CnDev_Numerics *nBuf;
   CnDev_Strings *sBuf;
   VMK_ReturnStatus status;

   CopyFromHost(&devArgs, args, sizeof(VMnix_ConduitConfigDevForWorldArgs));

   nBuf = (CnDev_Numerics *)Mem_Alloc(sizeof(CnDev_Numerics));
   sBuf = (CnDev_Strings *)Mem_Alloc(CN_DEV_VMX_CONFIG_STRING_BUF_SIZE);

   CopyFromHost(nBuf, devArgs.nBuf, 
			(devArgs.numNumerics)*sizeof(unsigned long));
   if(devArgs.numStrings != 0) {
      CopyFromHost(sBuf, devArgs.sBuf, CN_DEV_VMX_CONFIG_STRING_BUF_SIZE);
   }

   status = Conduit_CnDevConfigDeviceForWorld(&devArgs, nBuf, sBuf);

   CopyToHost(args, &devArgs, sizeof(VMnix_ConduitConfigDevForWorldArgs));
   if(devArgs.flags & CN_DEV_VMX_REQUEST) {
      CopyToHost(devArgs.nBuf, nBuf, 
		(devArgs.numNumerics) * sizeof(unsigned long));
      if(devArgs.numStrings != 0) {
         CopyToHost(devArgs.sBuf, sBuf, CN_DEV_VMX_CONFIG_STRING_BUF_SIZE);
      }
   }
   Mem_Free(sBuf);
   Mem_Free(nBuf);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Conduit_HostNewPipe --
 *
 *	Host entry point for the interface to create a new pipe.
 *	Establishes a new conduit for the client on a targeted device.
 *      If the targeted device exists a new conduit is associated with it 
 *	and the handle to the conduit is returned.  If the device does not
 *	exist, a default shared memory  device of the appropriate size
 *	and configuration is created and the new handle is associated
 *	with the new device. 
 *
 *	This call maps the shared memory provided by the guest into 
 *	the vmkernel.  It's *	actions also result in the calling of the 
 *	device specific initialization.  Connection is thus established 
 *	between the vmkernel and the guest layers.  Further, the specific
 *	driver will set up the necessary state for the new client
 *	instantiation, allowing conduit to vdev callbacks on behalf
 *	of the client.
 *	
 *	This routine combines much of the traditional open and 
 *	enable function seen in an adapter with the added device
 *	connection function.
 *
 *
 * Results: 
 *	A new conduit is created and assocaited with the targeted device
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitNewPipe(VMnix_ConduitNewPipeArgs *hostOpenArgs,
                   Conduit_OpenPipeArgs *result)
{
   Conduit_ClientType clientType;
   VMK_ReturnStatus status;
   VMnix_ConduitNewPipeArgs openArgs;
   Conduit_OpenPipeArgs *conduitArgs;

   CopyFromHost(&openArgs, hostOpenArgs, sizeof(openArgs));
   conduitArgs = &(openArgs.args);

   if (openArgs.args.worldID == INVALID_WORLD_ID) {
      openArgs.args.worldID = hostWorld->worldID;
      clientType = CONDUIT_HANDLE_HOST;
   } else {
      clientType = CONDUIT_HANDLE_VMM;
   }
  
   status = Conduit_HostNewPipe(openArgs.handleID, clientType, conduitArgs);

   CopyToHost(result, conduitArgs, sizeof(Conduit_OpenPipeArgs));

   return status;

}

/*
 *----------------------------------------------------------------------
 *
 * HostConduitTransmit --
 *
 *	Conduit_Transmit reads the adapter send buffer to discover the 
 *	particular pipes which are signalling send. HostConduitTransmit
 *	then acquires the pipe structure and does a call back on the 
 *	attached device.
 *
 * Results: 
 *	The Transmit routine is called and targeted pipe back-ends
 *	are signaled.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */


static VMK_ReturnStatus
HostConduitTransmit(Conduit_HandleID *hostHandleID)
{
   Conduit_HandleID handleID;
   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));
   return Conduit_Transmit(handleID, hostWorld);
}

/*
 *----------------------------------------------------------------------
 *
 * HostConduitVDevInfo
 *
 *	HostConduitVDevInfo serves a double purpose.  If the caller
 *	indicates CN_DEV_RECORD_DEVICE_QUERY, this routine will call
 *	CnDev_Table with the correct dev id to query the specific
 *	device back-end.  If the caller does not indicate 
 *	CN_DEV_RECORD_DEVICE_QUERY, the registered device backend
 *	table will be searched for entries which match the callers
 *	search criteria.  Searches include, deviceID scan where the
 *	next valid ID is returned, Vendor, device name query where
 *	the next matching valid ID is retruned and query by name
 *	where an arbitrary name decided upon by the two endpoints is
 *	searched for.
 *
 *
 * Results: 
 *	In the case of CN_DEV_RECORD_DEVICE_QUERY the device specific
 *	action is carried by invoking the specific device back-end.  If
 *	the caller does not direct CN_DEV_RECORD_DEVICE_QUERY the
 *	conduit device table will be searched and if an entry matching
 *	the search criteria is found, its record will be returned.
 *	
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */


static VMK_ReturnStatus
HostConduitVDevInfo(VMnix_ConduitDevInfoArgs *args,
                    CnDev_Record *rec)
{
   VMnix_ConduitDevInfoArgs devArgs;
   VMK_ReturnStatus status;


   CopyFromHost(&devArgs, args, sizeof(VMnix_ConduitDevInfoArgs));

   if (devArgs.worldID == INVALID_WORLD_ID) {
      devArgs.worldID = hostWorld->worldID;
   }
   status = Conduit_DevInfo(
		devArgs.handleID, devArgs.worldID, &(devArgs.rec));
   CopyToHost(rec, &(devArgs.rec), sizeof(CnDev_Record));
   return status;

}

/*
 *----------------------------------------------------------------------
 *
 * HostConduitRemovePipe --
 *
 *	COS entry point for the interface to teardown a conduit client.
 *
 *
 * Results: 
 *	Conduit connection is severed and all of the associated 
 *	resources are freed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostConduitRemovePipe(VMnix_ConduitRemovePipeArgs *hostArgs)
{
   VMK_ReturnStatus		status;
   VMnix_ConduitRemovePipeArgs	args;
 
   CopyFromHost(&args, hostArgs, sizeof(args));

   if (args.worldID == INVALID_WORLD_ID) {
      args.worldID = hostWorld->worldID;
   }

   status = Conduit_HostRemovePipe(args.handleID,
                                   args.worldID, args.pipeID);

   return status;

}


/*
 *-----------------------------------------------------------------------------
 *
 * HostConduitSend --
 *
 *      HostConduitSend pings the kernel based conduit service for
 *      the specified adapter, starting transfers on any pending signals.
 *
 * Results:
 *
 *      None.
 *
 * Side Effects:
 *
 *      Any pending sends are delivered.
 *
 *-----------------------------------------------------------------------------
 */

static uint32 
HostConduitSend(World_ID worldID, Conduit_HandleID *handleID)
{
   VMK_ReturnStatus status;
   World_Handle		*world;

   world = World_Find(worldID);
   status = Conduit_Transmit(*handleID, world);
   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Conduit_LockPage --
 *
 *      Conduit vmkernel lock page handler.  Dereferences
 *      offset of pgnum to find the proper backing object and calls
 *      Conduit_GetBackingStore with the proper world.
 *      At this point we respond to LockPage by returning a page but
 *      take no direct action to change the state of the page in the
 *      conduit backing memory.  This is done explicitly with the use of
 *      the get and set tags variants on the DeviceMemory call.  This is
 *      because the reliance on mapping and unmapping of a range of
 *      memory in the monitor in order to do a flush requires the
 *      "tag" or memory object handle.  If the monitor ever moves
 *      to a clean page flush, we can streamline the code, removing
 *      the need for the get and set tag code. We can rely on page-unlock
 *      delivery out of a flush routine to signal that a page is
 *      free to be reclaimed or swapped within the conduit backing
 *      store. 
 *
 * Results:
 *      Returns the page backing the conduit physical memory at the
 *      proscribed offset.  The default "nonsense" page is returned
 *      if there is no backing page at the given offset.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
*/

static VMK_ReturnStatus 
HostConduitLockPage(VMnix_ConduitLockPageArgs *hostArgs, uint32 *result)
{
   World_Handle			*world;
   VMnix_ConduitLockPageArgs	args;
   VMK_ReturnStatus		status;

   CopyFromHost(&args, hostArgs, sizeof(args));

   world = World_Find(args.worldID);
   status = VMK_BAD_PARAM;
   if (args.flags & CONDUIT_LOCK_PAGE) {
      status = Conduit_GetBackingStore(world, args.p, &args.mpn);
      CopyToHost(result, &(args.mpn), sizeof(uint32));
   } else if (args.flags & CONDUIT_UNLOCK_PAGE) {
      status = VMK_OK;
   }
   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostRemoveConduitAdapter --
 *
 *      Close the targeted Adapter
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	All of the associated pipes are dropped
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostRemoveConduitAdapter(VMnix_RemoveConduitAdapArgs *hostArgs)
{
   VMnix_RemoveConduitAdapArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));

   if (args.worldID == INVALID_WORLD_ID) {
      args.worldID = hostWorld->worldID;
   }
   Conduit_RemoveAdapter(args.worldID, args.handleID);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostMakeSyncCallWithResult --
 *
 *      Standard procedure for making a helper sync request. Essentially the
 *      template most host syscall functions use if they don't intervene
 *      with special actions.  
 *      
 * Results:
 *      VMK_NO_MEMORY
 *      VMK_STATUS_PENDING - helper request made, awaiting execution.
 *
 * Side effects:
 *      Helper request made. 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostMakeSyncCallWithResult(void *hostArgs, 
                           int argSize, 
                           void *hostResult, 
                           int resultSize, 
                           HelperRequestSyncFn *fn,
		           Helper_RequestHandle *hostHelperHandle)
{
   void *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = Mem_Alloc(argSize);
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, argSize);

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, fn, args, 
                                     HostRequestCancelFn,
                                     resultSize, hostResult);
   if (helperHandle == HELPER_INVALID_HANDLE) {
      Mem_Free(args);
      return VMK_NO_FREE_HANDLES;
   }
   status = VMK_STATUS_PENDING;

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  Helper_Get/SetActiveIoctlHandle --
 *
 *      Get/set the helper request handle performing current ioctl. 
 *
 * Results:
 *      Helper_RequestHandle. 
 *
 * Side effects:
 *      vmnixmod sets activeIoctlHandle through shared area. 
 *
 *-----------------------------------------------------------------------------
 */

volatile Helper_RequestHandle activeIoctlHandle = HELPER_INVALID_HANDLE;

Helper_RequestHandle
Host_GetActiveIoctlHandle(void)
{
   return activeIoctlHandle;
}

void
Host_SetActiveIoctlHandle(Helper_RequestHandle handle)
{
   ASSERT(activeIoctlHandle == HELPER_INVALID_HANDLE);
   activeIoctlHandle = handle;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostGetNICStatsFn
 *
 *      Helper function for HostGetNICStats(). Call Net_GetNICStats() and copy
 *      results.
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      Status of net module call. 
 *      
 * Side effects:
 *      If call was successful, result is returned to the helper module.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostGetNICStatsFn(void* data,
                  void** result)
{
   VMK_ReturnStatus status;
   VMnix_NICStatsArgs *args = (VMnix_NICStatsArgs *) data;
   void* stats = Mem_Alloc(args->resultLen); 

   if (stats == NULL) {
      status = VMK_NO_MEMORY;
   } else {
      status = Net_HostGetNICStats(args->devName, stats);
      if (status == VMK_OK) {
         *result = stats;
      } else {
         Mem_Free(stats);
      }
   }
   Mem_Free(args);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostGetNICStats
 *
 *      Forward a get_stats() call done by the host for /proc/net/dev to the
 *      vmkernel's NIC driver.
 *
 * Results:
 *      VMK_NO_MEMORY
 *      VMK_STATUS_PENDING - helper request made, awaiting execution.
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostGetNICStats(VMnix_NICStatsArgs *hostArgs,
                void* result,
                Helper_RequestHandle *hostHelperHandle)
{
   VMnix_NICStatsArgs *args;  
   Helper_RequestHandle handle;

   args = Mem_Alloc(sizeof(VMnix_NICStatsArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(VMnix_NICStatsArgs));
   LOG(2, "called on %s", args->devName);

   handle = Helper_RequestSync(HELPER_MISC_QUEUE, HostGetNICStatsFn, args, 
                               HostRequestCancelFn, 
                               args->resultLen, result);
   CopyToHost(hostHelperHandle, &handle, sizeof(handle));
   return VMK_STATUS_PENDING;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostNetPortEnable --
 *
 *      Enable a Net port by queueing the request to a helper world.
 *      This prevents our COS world from having to wait in the vmkernel.
 *      
 *
 * Results:
 *      VMK_NO_MEMORY
 *      VMK_STATUS_PENDING - helper request made, awaiting execution.
 *      
 * Side effects:
 *      Helper request to enable handle is made
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostNetPortEnable(VMnix_NetPortEnableArgs *hostArgs,
                  Helper_RequestHandle *hostHelperHandle)
{
   
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_NetPortEnableArgs),
                           Net_HostPortEnable, hostHelperHandle); 
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostNetPortDisable --
 *
 *      Disable a Net port by queueing the request to a helper world.
 *      This prevents our COS world from having to wait in the vmkernel.
 *
 * Results:
 *      VMK_NO_MEMORY
 *      VMK_STATUS_PENDING - helper request made, awaiting execution.
 *      
 * Side effects:
 *      Helper request to disable handle is made
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostNetPortDisable(VMnix_NetPortDisableArgs *hostArgs,
                   Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_NetPortDisableArgs),
                           Net_HostPortDisable, hostHelperHandle); 
}


static VMK_ReturnStatus
HostOpenSCSIDeviceFn(void *data, void **resultp)
{
   int flags;
   VMnix_OpenSCSIDevArgs *args = (VMnix_OpenSCSIDevArgs *)data;
   VMK_ReturnStatus status;
   SCSI_HandleID handleID;
   VMnix_OpenSCSIDevIntResult *result;

   result = (VMnix_OpenSCSIDevIntResult *)Mem_Alloc(sizeof(VMnix_OpenSCSIDevIntResult));
   if (result == NULL) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   } 

   VMLOG(1, args->worldID, "%s:%d:%d:%d", args->name, args->targetID,
         args->lun, args->partition);

   flags = args->flags;
   if (args->worldID == INVALID_WORLD_ID) {
      args->worldID = hostWorld->worldID;
      flags |= SCSI_OPEN_HOST;
   }
   status = SCSI_OpenDevice(args->worldID, args->name, args->targetID,
                            args->lun, args->partition,
                            flags, &handleID);
   LOG(1, "status=%d, handle 0x%x", status, handleID);
   if (status != VMK_OK) {
      Mem_Free(result);
      result = NULL;
   } else {
      ASSERT(handleID != -1);
      result->handleID = handleID;
      result->cmplMapIndex = SCSI_GetCmplMapIndex(handleID);

      if (args->shares) {
         Warning("Not setting the shares value (awaiting fix for bug #49838.");
      }

#if 0
      // set the shares value
      if (args->shares) {
	 
         status = SCSI_SetDiskShares(handleID, args->worldID, args->shares);
      } else {
         status = SCSI_SetDiskShares(handleID, args->worldID, SCHED_CONFIG_NONE);
      }
      if (status != VMK_OK) {
         Mem_Free(result);
         result = NULL;
      }
#endif
   }
   *resultp = result;
   Mem_Free(args);
   return status;
}

/*
 * Open the specified SCSI device, returning a handle and a pointer to
 * the interruptPendingMask.
 */
static VMK_ReturnStatus
HostOpenSCSIDevice(VMnix_OpenSCSIDevArgs *hostArgs,
		   VMnix_OpenSCSIDevIntResult *hostResult,
                   Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_OpenSCSIDevArgs),
                                     hostResult, sizeof(VMnix_OpenSCSIDevIntResult),
                                     HostOpenSCSIDeviceFn, hostHelperHandle);
}

typedef struct {
   World_ID worldID;
   SCSI_HandleID handleID;
} HostSCSICloseArgs;

static VMK_ReturnStatus
HostCloseSCSIDeviceFn(void *data, void **result)
{
   HostSCSICloseArgs *args = (HostSCSICloseArgs *)data;
   VMK_ReturnStatus status;
  
   status = SCSI_CloseDevice(args->worldID, args->handleID);
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostCloseSCSIDevice(World_ID worldID, SCSI_HandleID *hostHandleID,
		    Helper_RequestHandle *hostHelperHandle)
{
   SCSI_HandleID handleID;
   Helper_RequestHandle helperHandle;
   HostSCSICloseArgs *args;

   args = Mem_Alloc(sizeof(*args));
   if (!args) {
      return VMK_NO_MEMORY;
   }

   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));
   if (worldID == INVALID_WORLD_ID) {
      worldID = hostWorld->worldID;
   }

   args->worldID = worldID;
   args->handleID = handleID;
   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostCloseSCSIDeviceFn, args, 
                                     HostRequestCancelFn,
                                     0, NULL);

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return VMK_STATUS_PENDING;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostSCSIAdapProcInfoFn
 *
 *      Helper function for HostSCSIAdapProcInfo(). Call into the SCSI module.
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      Status of SCSI module call. 
 *      
 * Side effects:
 *      If call was successful, # of bytes read/written is returned to
 *	the helper. 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostSCSIAdapProcInfoFn(void* data,     // IN:
                       void **result)  // IN/OUT: 
{
   VMnix_ProcArgs *args = (VMnix_ProcArgs *) data;
   VMnix_ProcResult *procResult; 
   uint32 nbytes;
   VMK_ReturnStatus status;
   
   LOG(2, "adap=%s count=%d", args->adapName, args->count); 
   status = SCSI_AdapProcInfo(args->adapName, args->vmkBuf, args->offset, 
                              args->count, &nbytes, args->isWrite);

   if (status == VMK_OK) {
      procResult = Mem_Alloc(sizeof(VMnix_ProcResult));
      if (procResult == NULL) {
         status = VMK_NO_MEMORY;
      } else {
         procResult->nbytes = nbytes;
         *result = procResult;
      }
   } 
   
   Mem_Free(args);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostSCSIAdapProcInfo
 *
 *      Forward a read/write on /proc/scsi/<driver>/<adap#> to the
 *	vmkernel driver.
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      VMK_STATUS_PENDING - Helper request made, awaiting execution. 
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostSCSIAdapProcInfo(VMnix_ProcArgs* procArgs,               // IN:
                     VMnix_ProcResult* procResult,           // OUT:
                     Helper_RequestHandle* hostHelperHandle) // IN:
{
   return HostMakeSyncCallWithResult(procArgs, sizeof(VMnix_ProcArgs), 
                                     procResult, sizeof(VMnix_ProcResult),
                                     HostSCSIAdapProcInfoFn, hostHelperHandle);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostSCSIDevIoctl(Fn) --
 *
 *      Forward an ioctl to a {scsi, block} device from the host. 
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      VMK_STATUS_PENDING - Helper request made, awaiting execution. 
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostSCSIDevIoctlFn(void *args,
                   void **result)
{
   VMK_ReturnStatus status;
   VMnix_SCSIDevIoctlArgs *ioctlArgs = (VMnix_SCSIDevIoctlArgs *) args;
   VMnix_SCSIDevIoctlResult *ioctlRes = Mem_Alloc(sizeof(VMnix_SCSIDevIoctlResult));
   Helper_RequestHandle rh;

   if (ioctlRes == NULL) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   memset(ioctlRes, 0, sizeof(VMnix_SCSIDevIoctlResult));
  
   rh = Helper_GetActiveRequestHandle();
   ASSERT(rh != HELPER_INVALID_HANDLE);
   Host_SetActiveIoctlHandle(rh);

   status = SCSI_HostIoctl(ioctlArgs->handleID,
                           ioctlArgs->hostFileFlags,
                           ioctlArgs->cmd, 
                           ioctlArgs->userArgsPtr, 
                           &ioctlRes->drvErr);

   if (status != VMK_OK) {
      Mem_Free(ioctlRes);
      ioctlRes = NULL;
   }
   *result = ioctlRes;
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus 
HostSCSIDevIoctl(VMnix_SCSIDevIoctlArgs *hostArgs,
                 VMnix_SCSIDevIoctlResult *hostResult,
                 Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_SCSIDevIoctlArgs), 
                                     hostResult, sizeof(VMnix_SCSIDevIoctlResult),
                                     HostSCSIDevIoctlFn, hostHelperHandle);
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostCharDevIoctl(Fn) --
 *
 *      Forward an ioctl on a char device (registered by some vmkernel driver
 *      as a mgmt mechanism) from the host. 
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      VMK_STATUS_PENDING - Helper request made, awaiting execution. 
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostCharDevIoctlFn(void *args,
                   void **result)
{
   VMK_ReturnStatus status;
   VMnix_CharDevIoctlArgs *ioctlArgs = (VMnix_CharDevIoctlArgs *) args;
   VMnix_CharDevIoctlResult *ioctlRes = Mem_Alloc(sizeof(VMnix_CharDevIoctlResult));
   Helper_RequestHandle rh;

   if (ioctlRes == NULL) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   memset(ioctlRes, 0, sizeof(VMnix_CharDevIoctlResult));

   rh = Helper_GetActiveRequestHandle();
   ASSERT(rh != HELPER_INVALID_HANDLE);
   Host_SetActiveIoctlHandle(rh);

   status = SCSI_HostCharDevIoctl(ioctlArgs->major,
                                  ioctlArgs->minor,
                                  ioctlArgs->hostFileFlags,
                                  ioctlArgs->cmd, 
                                  ioctlArgs->userArgsPtr, 
                                  &ioctlRes->drvErr);
   if (status != VMK_OK) {
      Mem_Free(ioctlRes);
      ioctlRes = NULL;
   }
   *result = ioctlRes;
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus 
HostCharDevIoctl(VMnix_CharDevIoctlArgs *hostArgs,
                 VMnix_CharDevIoctlResult *hostResult,
                 Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_CharDevIoctlArgs), 
                                     hostResult, sizeof(VMnix_CharDevIoctlResult),
                                     HostCharDevIoctlFn, hostHelperHandle);
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostNetDevIoctl(Fn) --
 *
 *      Forward an ioctl on a vmkernel network device from the host. 
 *
 * Results:
 *      VMK_NO_MEMORY,
 *      VMK_STATUS_PENDING - Helper request made, awaiting execution. 
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostNetDevIoctlFn(void *args,
                  void **result)
{
   VMK_ReturnStatus status;
   VMnix_NetDevIoctlArgs *ioctlArgs = (VMnix_NetDevIoctlArgs *) args;
   VMnix_NetDevIoctlResult *ioctlRes = Mem_Alloc(sizeof(VMnix_NetDevIoctlResult));
   Helper_RequestHandle rh;

   if (ioctlRes == NULL) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   memset(ioctlRes, 0, sizeof(VMnix_NetDevIoctlResult));

   rh = Helper_GetActiveRequestHandle();
   ASSERT(rh != HELPER_INVALID_HANDLE);
   Host_SetActiveIoctlHandle(rh);
   
   status = Net_HostIoctl(ioctlArgs->devName,
                          ioctlArgs->cmd,
                          ioctlArgs->vmkBuf,
                          &ioctlRes->drvErr);
   if (status != VMK_OK) {
      Mem_Free(ioctlRes);
      ioctlRes = NULL;
   }
   *result = ioctlRes;
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus 
HostNetDevIoctl(VMnix_NetDevIoctlArgs *hostArgs,
                VMnix_NetDevIoctlResult *hostResult,
                Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_NetDevIoctlArgs), 
                                     hostResult, sizeof(VMnix_NetDevIoctlResult),
                                     HostNetDevIoctlFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostSetDumpPartitionFn(void *data, void **result)
{
   VMnix_SetDumpArgs *args = (VMnix_SetDumpArgs *)data;
   VMK_ReturnStatus status = Dump_SetPartition(args->adapName,
                                               args->targetID,
                                               args->lun,
                                               args->partition);
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostSetDumpPartition(VMnix_SetDumpArgs *hostArgs,
                     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_SetDumpArgs),
                           HostSetDumpPartitionFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFSCreateFn(void *data, void **result)
{
   VMnix_FSCreateArgs *args = (VMnix_FSCreateArgs *)data;
   VMK_ReturnStatus status = FSS_Create(args->fsType, args->deviceName,
                                        args->fileBlockSize, args->numFiles);
   Mem_Free(args);
   return status;
}

/*
 * Create a new VMFS file system.
 */
static VMK_ReturnStatus
HostFSCreate(VMnix_FSCreateArgs *hostArgs,
	     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FSCreateArgs),
			   HostFSCreateFn, hostHelperHandle);
}


static VMK_ReturnStatus
HostFSToVMFS2Fn(void *data, void **result)
{
   VMnix_ConvertToFS2Args *args = (VMnix_ConvertToFS2Args *)data;
   VMK_ReturnStatus status;

   status = FSS_UpgradeVolume(args->volumeName);
   Mem_Free(args);
   return status;
}

/*
 * Convert a given VMFS-1 volume to a VMFS-2 volume.
 */
static VMK_ReturnStatus
HostFSToVMFS2(VMnix_ConvertToFS2Args *hostArgs,
              Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_ConvertToFS2Args),
			   HostFSToVMFS2Fn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFSExtendFn(void *data, void **result)
{
   VMnix_FSExtendArgs *args = (VMnix_FSExtendArgs *)data;
   VMK_ReturnStatus status = FSS_Extend(args->volumeName,
					args->extVolumeName,
					args->numFiles);
   Mem_Free(args);
   return status;
}

/*
 * Extend a VMFS-2 with another physical extent.
 */
static VMK_ReturnStatus
HostFSExtend(VMnix_FSExtendArgs *hostArgs,
	     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FSExtendArgs),
			   HostFSExtendFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFSGetAttrFn(void *data, void **resultp)
{
   VMK_ReturnStatus status;
   VMnix_FSGetAttrArgs *args = (VMnix_FSGetAttrArgs *) data;
   VMnix_PartitionListResult *result = NULL;

   result = (VMnix_PartitionListResult *)
      Mem_Alloc(VMNIX_PARTITION_ARR_SIZE(args->maxPartitions));
   if (result == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   status = FSS_GetAttributes(&args->oid, args->maxPartitions, result);
   *resultp = result;

 done:
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFSGetAttr(VMnix_FSGetAttrArgs *hostArgs, VMnix_PartitionListResult *hostResult,
	      Helper_RequestHandle *hostHelperHandle)
{
   VMnix_FSGetAttrArgs *args;
   int resultSize;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = Mem_Alloc(sizeof(VMnix_FSGetAttrArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(VMnix_FSGetAttrArgs));

   resultSize = VMNIX_PARTITION_ARR_SIZE(args->maxPartitions);

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostFSGetAttrFn, 
                                     args, HostRequestCancelFn,
                                     resultSize, hostResult);
   status = VMK_STATUS_PENDING;

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus
HostFSSetAttrFn(void *data, void **result)
{
   VMnix_FSSetAttrArgs *args = (VMnix_FSSetAttrArgs *)data;
   VMK_ReturnStatus status = VMK_NOT_IMPLEMENTED;

#if 0
   status = FSS_SetAttributes(&args->oidargs->volumeName, args->flags,
                              args->fsName, args->mode);
#endif

   Mem_Free(args);
   return status;
}

/*
 * Set the name of an VMFS file system.
 */
static VMK_ReturnStatus
HostFSSetAttr(VMnix_FSSetAttrArgs *hostArgs,
	      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FSSetAttrArgs),
			   HostFSSetAttrFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFSDumpFn(void *data, void **result)
{
   VMnix_FSDumpArgs *args = (VMnix_FSDumpArgs *)data;
   VMK_ReturnStatus status = FSS_DumpPath(args->path, args->verbose);
   Mem_Free(args);
   return status;
}

/*
 * Dump info on the VMFS file system to the log.
 */
static VMK_ReturnStatus
HostFSDump(VMnix_FSDumpArgs *hostArgs, Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FSDumpArgs),
			   HostFSDumpFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFSReaddirFn(void *data, void **resultp)
{
   VMnix_ReaddirArgs *args = (VMnix_ReaddirArgs *)data;
   VMK_ReturnStatus status;
   VMnix_ReaddirResult *result = NULL;

   if (args->maxDirEntries == 0) {
      status = VMK_BAD_PARAM;
      goto errorExit;
   }
   result = (VMnix_ReaddirResult *)
      Mem_Alloc(VMNIX_READDIR_RESULT_SIZE(args->maxDirEntries));
   if (result == NULL) {
      status = VMK_NO_MEMORY;
      goto errorExit;
   }
   *resultp = result;

   status = FSS_Readdir(&args->dirOID, args->maxDirEntries, result);
errorExit:
   Mem_Free(args);
   // don't need to free result because helper code handles that
   return status;
}

/*
 * Return info on the files in the VMFS.
 */
static VMK_ReturnStatus
HostFSReaddir(VMnix_ReaddirArgs *hostArgs, VMnix_ReaddirResult *hostResult,
              Helper_RequestHandle *hostHelperHandle)
{
   VMnix_ReaddirArgs *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = (VMnix_ReaddirArgs *)Mem_Alloc(sizeof(VMnix_ReaddirArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(*args));

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostFSReaddirFn, args, 
                                     HostRequestCancelFn, 
                                     VMNIX_READDIR_RESULT_SIZE(args->maxDirEntries),
                                     hostResult);
   status = VMK_STATUS_PENDING;

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus
HostFileCreateFn(void *data, void **result)
{
   VMnix_FileCreateArgs *args = (VMnix_FileCreateArgs *)data;
   VMK_ReturnStatus status;
   FSS_ObjectID oid;

   status = FSS_CreateFile(&args->dirOID, args->fileName,
                           args->createFlags, NULL, &oid);
   if (status == VMK_OK) {
      FS_FileAttributes attrs;
      uint16 opFlags = FILEATTR_SET_PERMISSIONS;

      if ((args->createFlags & FS_CREATE_DIR) == 0) {
	 // Don't try to set length for directories.
	 opFlags |= FILEATTR_SET_LENGTH;
      }
      attrs.uid = args->uid;
      attrs.gid = args->gid;
      attrs.mode = args->mode;
      attrs.length = args->length;

      status = FSS_SetFileAttributes(&oid, opFlags, &attrs);

      if (status !=  VMK_OK) {
	 FSS_RemoveFile(&args->dirOID, args->fileName);
      }
   }

   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostCOWOpenHierarchyFn(void *data, void **result)
{
   VMnix_COWOpenHierarchyArgs *args = (VMnix_COWOpenHierarchyArgs *)data;
   VMK_ReturnStatus status;

   *result = Mem_Alloc(sizeof(VMnix_COWOpenHierarchyResult));

   if (!*result) {
      return VMK_NO_MEMORY;
   }

   status = COW_OpenHierarchy(args->fids, args->numFids, *result);
   Mem_Free(args);

   return status;
}


static VMK_ReturnStatus
HostCOWCloseHierarchyFn(void *data, void **result)
{
   COW_HandleID *chi = (COW_HandleID *)chi;
   return COW_CloseHierarchy(*chi);
}


static VMK_ReturnStatus
HostCOWCombineFn(void *data, void **result)
{
   VMnix_COWCombineArgs *args = (VMnix_COWCombineArgs *)data;
   VMK_ReturnStatus status;

   status = COW_Combine(&args->cowHandleID, args->linkOffsetFromBottom);
   Mem_Free(args);

   return status;
}

static VMK_ReturnStatus
HostVSCSICreateDevFn(void *data, void **result)
{
   VMnix_VSCSICreateDevArgs *args = (VMnix_VSCSICreateDevArgs *)data;
   VMK_ReturnStatus status;

   *result = Mem_Alloc(sizeof(VMnix_VSCSICreateDevResult));

   if (!*result) {
      return VMK_NO_MEMORY;
   }

   status = VSCSI_CreateDevice(args->wid, &args->desc, *result);
   Mem_Free(args);

   return status;
}

static VMK_ReturnStatus
HostVSCSIDestroyDevFn(void *data, void **result)
{
   VMnix_VSCSIDestroyDevArgs *args = (VMnix_VSCSIDestroyDevArgs *)data;
   VMK_ReturnStatus status;

   status = VSCSI_DestroyDevice(args->wid, args->vscsiID);
   Mem_Free(args);

   return status;
}

static VMK_ReturnStatus 
HostCOWGetBlockNumberFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_COWGetFIDAndLBNArgs *cowArgs = (VMnix_COWGetFIDAndLBNArgs *) data;
   VMnix_COWGetFIDAndLBNResult *cowResult = NULL;
   *result = NULL;

   cowResult = *result = Mem_Alloc(sizeof(VMnix_COWGetFIDAndLBNResult));
   if(cowResult == NULL) {
      Mem_Free(cowArgs);
      return VMK_NO_MEMORY;
   }
   status = COW_GetBlockOffsetAndFileHandle(cowArgs->cowHandle,
            cowArgs->blockOffset, &cowResult->fileHandle,  
            &cowResult->actualBlockNumber, &cowResult->length);
   Mem_Free(cowArgs);
   return status;
}


/*
 * Create a new VMFS file.
 */
static VMK_ReturnStatus
HostFileCreate(VMnix_FileCreateArgs *hostArgs,
	       Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FileCreateArgs),
			   HostFileCreateFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostCOWOpenHierarchy(VMnix_COWOpenHierarchyArgs *hostArgs, 
                     VMnix_COWOpenHierarchyResult *hostResult,
                     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof *hostArgs,
                                     hostResult, sizeof *hostResult,
                                     HostCOWOpenHierarchyFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostCOWCombine(VMnix_COWCombineArgs *hostArgs, 
               Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof *hostArgs,
                           HostCOWCombineFn, hostHelperHandle);
}


static VMK_ReturnStatus
HostCOWCloseHierarchy(COW_HandleID *chi, 
		      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(chi, sizeof(*chi),
                           HostCOWCloseHierarchyFn, hostHelperHandle);
}



static VMK_ReturnStatus 
HostVSCSICreateDev(VMnix_VSCSICreateDevArgs *hostArgs, 
                   VMnix_VSCSICreateDevResult *hostResult,
                   Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof *hostArgs,
                                     hostResult, sizeof *hostResult,
                                     HostVSCSICreateDevFn, hostHelperHandle);
}

static VMK_ReturnStatus 
HostVSCSIDestroyDev(VMnix_VSCSIDestroyDevArgs *hostArgs, 
                   Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof *hostArgs,
                           HostVSCSIDestroyDevFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostCOWGetBlockNumberAndFID(VMnix_COWGetFIDAndLBNArgs *hostArgs, 
			    VMnix_COWGetFIDAndLBNResult *hostResult, 
                            Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(*hostArgs),
				     hostResult, sizeof(*hostResult),
				     HostCOWGetBlockNumberFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostMapRawDiskFn(void *data, void **result)
{
   VMnix_MapRawDiskArgs *args = (VMnix_MapRawDiskArgs *)data;
   VMK_ReturnStatus status = VMK_NOT_IMPLEMENTED;
//   FS_FileHandleID fileHandleID = FS_INVALID_FILE_HANDLE;

#if 0
   status = FSS_CreateFile(args->filePath,
                           FS_CREATE_RAWDISK_MAPPING,
                           args, &fileHandleID);
   if (status == VMK_OK) {
      FS_FileAttributes attrs;

      attrs.uid = args->uid;
      attrs.gid = args->gid;
      attrs.mode = args->mode;
      status = FSClient_SetFileAttributes(fileHandleID, FILEATTR_SET_PERMISSIONS,
					  &attrs);
   }
   if (status == VMK_OK) {
      status = FSS_CloseFile(fileHandleID);
   } else if (fileHandleID != FS_INVALID_FILE_HANDLE) {
      FSS_RemoveOpenFile(fileHandleID);
      FSS_CloseFile(fileHandleID);
   }
#endif
   Mem_Free(args);
   return status;
}

/*
 * Map a RAW disk onto a VMFS-2 file
 */
static VMK_ReturnStatus
HostMapRawDisk(VMnix_MapRawDiskArgs *hostArgs,
               Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_MapRawDiskArgs),
			   HostMapRawDiskFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostQueryRawDiskFn(void *args, void **resultp)
{
   VMK_ReturnStatus status;

   *resultp = (VMnix_QueryRawDiskResult *)
      Mem_Alloc(sizeof(VMnix_QueryRawDiskResult));
   if (*resultp == NULL) {
      status = VMK_NO_MEMORY;
      goto exit;
   }
   memset(*resultp, 0, sizeof(VMnix_QueryRawDiskResult));
   status = FSS_QueryRawDisk((VMnix_QueryRawDiskArgs *)args,
                             (VMnix_QueryRawDiskResult *)(*resultp));
exit:
   Mem_Free(args);
   return status;
}

/*
 * Return the vmhba name for a raw disk mapping.
 */
static VMK_ReturnStatus
HostQueryRawDisk(VMnix_QueryRawDiskArgs *hostArgs,
                 VMnix_QueryRawDiskResult *hostResult,
                 Helper_RequestHandle *hostHelperHandle)
{ 
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_QueryRawDiskArgs), 
                                     hostResult,
                                     sizeof(VMnix_QueryRawDiskResult),
                                     HostQueryRawDiskFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFileOpenFn(void *data, void **resultp)
{
   VMnix_FileOpenArgs *args = (VMnix_FileOpenArgs *)data;
   VMK_ReturnStatus status;
   FS_FileHandleID fileHandle;
   VMnix_FileOpenResult *result;

   status = FSS_OpenFile(&args->oid, args->flags, &fileHandle);
   if (status != VMK_OK) {
      return status; 
   }
   
   result = Mem_Alloc(sizeof(VMnix_FileOpenResult));
   if (result == NULL) {
      status = VMK_NO_MEMORY;
      FSS_CloseFile(fileHandle);
   } else {
      result->handleID = fileHandle;
      *resultp = result;
   }
   Mem_Free(args);
   LOG(2, "status = %#x", status);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostFileOpen --
 *
 *	Open a file on the file system of the specified SCSI disk with the
 *	indicated mode, creating a REDO log on the same file system as
 *	necessary.
 *
 * Results:
 *      Handle to the file (or the REDO log).
 *
 * Side effects:
 *      Opens the specified file.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostFileOpen(VMnix_FileOpenArgs *hostArgs,
	     VMnix_FileOpenResult *hostResult,
	     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_FileOpenArgs), 
                                     hostResult, sizeof(VMnix_FileOpenResult),
                                     HostFileOpenFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFileLookupFn(void *data, void **resultp)
{
   VMnix_FileLookupArgs *args = (VMnix_FileLookupArgs *)data;
   VMK_ReturnStatus status;
   VMnix_FileLookupResult *result;

   result = (VMnix_FileLookupResult *)Mem_Alloc(sizeof(*result));
   if (result == NULL) {
      status = VMK_NO_MEMORY;
      goto onError;
   }

   status = FSS_Lookup(&args->dirOID, args->fileName, &result->oid);
   if (status == VMK_OK) {
      status = FSS_GetFileAttributes(&result->oid, &result->attrs);
      if (status == VMK_OK) {
         *resultp = result;
      }
   }
onError:
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFileLookup(VMnix_FileLookupArgs *hostArgs,
               VMnix_FileLookupResult *hostResult,
               Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_FileLookupArgs), 
                                     hostResult, sizeof(VMnix_FileLookupResult),
                                     HostFileLookupFn, hostHelperHandle);
}


static VMK_ReturnStatus
HostFileGetPhysLayoutFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_FileGetPhysLayoutArgs *args = (VMnix_FileGetPhysLayoutArgs *)data;
   VMnix_FileGetPhysLayoutResult *retval;

   retval = Mem_Alloc(sizeof(*retval));
   if (NULL == retval) {
      return VMK_NO_MEMORY;
   }
   status = FSS_FileGetPhysLayout(args->fileHandleID, args->offset, 
				  retval);
   Mem_Free(args);
   *result = retval;
   return status;
}


static VMK_ReturnStatus
HostFileGetPhysLayout(VMnix_FileGetPhysLayoutArgs *hostArgs,
		      VMnix_FileGetPhysLayoutResult *hostResult,
		      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(*hostArgs),
				     hostResult,
				     sizeof(*hostResult),
				     HostFileGetPhysLayoutFn,
				     hostHelperHandle);
}


static VMK_ReturnStatus
HostFileAttrFn(void *data, void **resultp)
{
   VMnix_FileAttrArgs *args = (VMnix_FileAttrArgs *)data;
   VMK_ReturnStatus status;
   FS_FileAttributes attrs;
   VMnix_FileAttrResult *result;


   status = FSS_GetFileAttributes(&args->oid, &attrs);
   if (status == VMK_OK) {
      result = Mem_Alloc(sizeof(VMnix_FileAttrResult));
      if (result == NULL) {
	 status = VMK_NO_MEMORY;
      } else {
	 result->length = attrs.length;
	 result->fsBlockSize = attrs.fsBlockSize;
	 result->numBlocks = attrs.numBlocks;
	 result->flags = attrs.flags;
	 result->descNum = attrs.descNum;
	 result->mtime = attrs.mtime;
	 result->ctime = attrs.ctime;
	 result->atime = attrs.atime;
         result->uid = attrs.uid;
         result->gid = attrs.gid;
         result->mode = attrs.mode;
	 *resultp = result;
      }
      *resultp = result;
   }
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFileSetAttrFn(void *data, void **result)
{
   VMnix_FileSetAttrArgs *args = (VMnix_FileSetAttrArgs *)data;
   VMK_ReturnStatus status;
   FS_FileAttributes attrs;

   attrs.generation = args->generation;
   attrs.length = args->length;
   attrs.uid = args->uid;
   attrs.gid = args->gid;
   attrs.mode = args->mode;
   attrs.toolsVersion = args->toolsVersion;
   attrs.virtualHWVersion = args->virtualHWVersion;

   ASSERT(!(args->cowFile && args->swapFile));
   attrs.flags = (args->cowFile) ? FS_COW_FILE : 0;
   attrs.flags = (args->swapFile) ? FS_SWAP_FILE : 0;

   status = FSS_SetFileAttributes(&args->oid, args->opFlags, &attrs);
   Mem_Free(args);
   return status;
}

/*
 * Set the attributes of the specified VMFS file.
 */
static VMK_ReturnStatus
HostFileSetAttr(VMnix_FileSetAttrArgs *hostArgs,
		Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FileSetAttrArgs),
			   HostFileSetAttrFn, hostHelperHandle);
}

/*
 * Return the attributes of the specified VMFS file.
 */
static VMK_ReturnStatus
HostFileAttr(VMnix_FileAttrArgs *hostArgs, 
             VMnix_FileAttrResult *hostResult,
	     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_FileAttrArgs), 
                                     hostResult, sizeof(VMnix_FileAttrResult),
                                     HostFileAttrFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostActivateSwapFileFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_ActivateSwapFileArgs *args = (VMnix_ActivateSwapFileArgs *) data;

   status = Swap_ActivateFile(args->filePath);

   Mem_Free(data);
   return status;
}

/*
 * Activate the specified swap file.
 */
static VMK_ReturnStatus
HostActivateSwapFile(VMnix_ActivateSwapFileArgs *hostArgs,
		     Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_ActivateSwapFileArgs),
			   HostActivateSwapFileFn, hostHelperHandle);
}

/*
 * Deactivate the specified swap file.
 */
static VMK_ReturnStatus
HostDeactivateSwapFileFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   uint32 fileNum = *(uint32 *)data;
   status = Swap_DeactivateFile(fileNum);
   Mem_Free(data);
   return status;
}

/*
 * Deactivate/Close all swap files
 */
static VMK_ReturnStatus
HostDeactivateSwapFile(uint32 *fileNum,
                       Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(fileNum, sizeof(uint32), HostDeactivateSwapFileFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFileIOFn(void *data, void **result)
{
   VMnix_FileIOArgs *args = (VMnix_FileIOArgs *)data;
   VMK_ReturnStatus status;
   uint32 bytesTransferred;
   IO_Flags ioFlags;

   ioFlags = (args->isRead) ? FS_READ_OP : FS_WRITE_OP;

   status = FSS_BufferCacheIO(&args->oid, args->offset, args->buf,
                              args->length, ioFlags, SG_MACH_ADDR,
                              &bytesTransferred);

   Mem_Free(args);
   return status;
}

/*
 * Read/write to an open VMFS file.
 */
static VMK_ReturnStatus
HostFileIO(VMnix_FileIOArgs *hostArgs, Helper_RequestHandle *hostHelperHandle)
{
   VMnix_FileIOArgs *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = Mem_Alloc(sizeof(VMnix_FileIOArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(VMnix_FileIOArgs));

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostFileIOFn, args, 
                                     HostRequestCancelFn, 
                                     args->length, NULL);
   status = VMK_STATUS_PENDING;

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus
HostFileCloseSyncFn(void *data, void **result)
{
   FS_FileHandleID fileHandle = *(FS_FileHandleID *)data;
   Mem_Free(data);
   return FSS_CloseFile(fileHandle);
}

/*
 * Close the specified VMFS file.
 */
static VMK_ReturnStatus
HostFileClose(FS_FileHandleID *hostHandleID, 
              Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostHandleID, sizeof(FS_FileHandleID),
                             HostFileCloseSyncFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFileRemoveFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_FileRemoveArgs *args = (VMnix_FileRemoveArgs *)data;

   status = FSS_RemoveFile(&args->dirOID, args->fileName);
   Mem_Free(args);
   return status;
}

/*
 * Remove the specified VMFS file.
 */
static VMK_ReturnStatus
HostFileRemove(VMnix_FileRemoveArgs *hostArgs,
	       Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FileRemoveArgs),
			   HostFileRemoveFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFileRenameFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_FileRenameArgs *args = (VMnix_FileRenameArgs *)data;

   status = FSS_RenameFile(&args->oldDirOID, args->oldFileName,
                           &args->newDirOID, args->newFileName);
   Mem_Free(args);
   return status;
}

/*
 * Rename the specified VMFS file.
 */
static VMK_ReturnStatus
HostFileRename(VMnix_FileRenameArgs *hostArgs,
	       Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FileRenameArgs),
			   HostFileRenameFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostFilePhysMemIOFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_FilePhysMemIOArgs *args = (VMnix_FilePhysMemIOArgs *)data;

   status = Alloc_PhysMemIO(args);
   Mem_Free(args);
   return status;
}

/*
 * Read/write the physical memory of a world to the specified VMFS file.
 */
static VMK_ReturnStatus
HostFilePhysMemIO(VMnix_FilePhysMemIOArgs *hostArgs,
		  Helper_RequestHandle *hostHelperHandle)
{
   return HostIssueSyncCall(hostArgs, sizeof(VMnix_FilePhysMemIOArgs),
			    HELPER_SUSPEND_RESUME_QUEUE, 
                            HostFilePhysMemIOFn, hostHelperHandle);
}

static VMK_ReturnStatus 
HostSCSIGetCapacity(SCSI_HandleID *hostHandleID, VMnix_GetCapacityResult *result)
{
   VMK_ReturnStatus status;
   VMnix_GetCapacityResult r;
   SCSI_HandleID handleID;

   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));

   status = SCSI_GetCapacity(handleID, &r);

   CopyToHost(result, &r, sizeof(r));

   return status;
}


static VMK_ReturnStatus 
HostSCSIGetGeometryFn(void *args, void **result)
{
   SCSI_HandleID hostHandleID = *(SCSI_HandleID*)args;
   VMnix_GetCapacityResult *r;
   VMK_ReturnStatus status;

   r = (VMnix_GetCapacityResult *) Mem_Alloc(sizeof(VMnix_GetCapacityResult));
   if (r == NULL) {
      Mem_Free(args);
      return (VMK_NO_MEMORY);
   }
   status = SCSI_GetGeometry(hostHandleID, r);
   if (status != VMK_OK) {
      Mem_Free(r);
      r = NULL;
   }
   *result = (void *)r;
   Mem_Free(args);
   return (status);
}


static VMK_ReturnStatus 
HostSCSIGetGeometry(SCSI_HandleID *hostHandleID, 
                    VMnix_GetCapacityResult *hostResult,
                    Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostHandleID, sizeof(SCSI_HandleID), 
                                     hostResult, sizeof(VMnix_GetCapacityResult),
                                     HostSCSIGetGeometryFn, hostHelperHandle);
}

static VMK_ReturnStatus
HostSCSIAdapterListFn(void *data,
                      void **resultp)
{
   VMnix_AdapterListArgs *args = (VMnix_AdapterListArgs *) data; 
   VMnix_AdapterListResult *result; 
   VMK_ReturnStatus status;

   result = (VMnix_AdapterListResult *) 
             Mem_Alloc(VMNIX_SCSIADAPTERLIST_RESULT_SIZE(args->maxEntries));
   if (!result) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   status = SCSI_AdapterList(args, result);
   if (status != VMK_OK) {
      Mem_Free(result);
      result = NULL;
   }
   *resultp = result;
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus 
HostSCSIAdapterList(VMnix_AdapterListArgs *hostArgs, 
                    VMnix_AdapterListResult *hostResult,
                    Helper_RequestHandle *hostHelperHandle)
{
   VMnix_AdapterListArgs *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = Mem_Alloc(sizeof(VMnix_AdapterListArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(VMnix_AdapterListArgs));

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostSCSIAdapterListFn, 
                                     args, HostRequestCancelFn, 
                                     VMNIX_SCSIADAPTERLIST_RESULT_SIZE(args->maxEntries), 
                                     hostResult);
   if (helperHandle == HELPER_INVALID_HANDLE) {
      Mem_Free(args);
      return VMK_NO_FREE_HANDLES;
   }

   status = VMK_STATUS_PENDING;
   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus
HostGetLUNListFn(void *data,
                 void **resultp)
{
   VMnix_LUNListArgs *args = (VMnix_LUNListArgs *) data; 
   VMnix_LUNListResult *result; 
   VMK_ReturnStatus status;

   result = (VMnix_LUNListResult *) Mem_Alloc(VMNIX_LUNLIST_RESULT_SIZE(args->maxEntries));
   if (!result) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   status = SCSI_GetLUNList(args, result);
   if (status != VMK_OK) {
      Mem_Free(result);
      result = NULL;
   }
   *resultp = result;
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus 
HostGetLUNList(VMnix_LUNListArgs *hostArgs, 
               VMnix_LUNListResult *hostResult,
               Helper_RequestHandle *hostHelperHandle)
{
   VMnix_LUNListArgs *args;
   Helper_RequestHandle helperHandle;
   VMK_ReturnStatus status;

   args = Mem_Alloc(sizeof(VMnix_LUNListArgs));
   if (args == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(args, hostArgs, sizeof(VMnix_LUNListArgs));

   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, HostGetLUNListFn, 
                                     args, HostRequestCancelFn, 
                                     VMNIX_LUNLIST_RESULT_SIZE(args->maxEntries), 
                                     hostResult);
   if (helperHandle == HELPER_INVALID_HANDLE) {
      Mem_Free(args);
      return VMK_NO_FREE_HANDLES;
   }

   status = VMK_STATUS_PENDING;
   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}

static VMK_ReturnStatus 
HostNetInfo(uint32 cmd, VMnix_NetInfoArgs *hostArgs)
{
   NOT_IMPLEMENTED();
   return VMK_OK;
}

static VMK_ReturnStatus 
HostFindAdapName(uint32 bus, uint32 devfn, char* name)
{
   VMK_ReturnStatus status;
   char *adapName;

   status = Scsi_FindAdapName(bus, devfn, &adapName);
   if (status == VMK_OK) {
      CopyToHost(name, adapName, strlen(adapName)+1);
   }
   return(status);
}

static VMK_ReturnStatus
HostTargetInfoFn(void *args, void **resultp) 
{
   VMK_ReturnStatus status;
   VMnix_TargetInfoArgs *tiArgs = (VMnix_TargetInfoArgs *)args;

   *resultp = (VMnix_TargetInfo *) Mem_Alloc(sizeof(VMnix_TargetInfo));
   if (*resultp == NULL) {
      status = VMK_NO_MEMORY;
      goto exit;
   }
   memset(*resultp, 0, sizeof(VMnix_TargetInfo));
   status = SCSI_GetTargetInfo(tiArgs->diskName, tiArgs->targetID,
                               tiArgs->lun, (VMnix_TargetInfo *)(*resultp));

exit:
   Mem_Free(args);
   return status;
}

/*
 *    Given a vmhba name, return the target info.
 */
static VMK_ReturnStatus 
HostTargetInfo(VMnix_TargetInfoArgs *hostArgs,
                 VMnix_TargetInfo *hostResult,
                 Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_TargetInfoArgs),
                                     hostResult, sizeof(VMnix_TargetInfo),
                                     HostTargetInfoFn, hostHelperHandle);
}


/*
 *----------------------------------------------------------------------
 *
 * HostBlockCommand --
 *
 *      Queue a block command to the hardware adapter.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
HostBlockCommand(SCSI_HandleID *hostHandleID, HostSCSI_Command *command)
{
   SCSI_HandleID handleID;
   HostSCSI_Command *scsiCmd;
   VMK_ReturnStatus status;

   scsiCmd = (HostSCSI_Command *)Mem_Alloc(sizeof(HostSCSI_Command));
   if (scsiCmd == NULL) {
      status = VMK_NO_MEMORY;
      return(status);
   }

   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));
   CopyFromHost(scsiCmd, command, sizeof(*scsiCmd));

   SCSI_ExecuteHostCommand(handleID, &scsiCmd->command, &status);
   Mem_Free(scsiCmd);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostSCSICommand --
 *
 *      Queue a SCSI command to the hardware adapter.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
HostSCSICommand(SCSI_HandleID *hostHandleID,
                HostSCSI_Command *command)
{
   HostSCSI_Command *scsiCmd;
   SCSI_HandleID handleID;
   VMK_ReturnStatus status;

   scsiCmd = (HostSCSI_Command *)Mem_Alloc(sizeof(HostSCSI_Command));
   if (scsiCmd == NULL) {
      status = VMK_NO_MEMORY;
      return(status);
   }

   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));
   CopyFromHost(scsiCmd, command, sizeof(*scsiCmd));

   SCSI_ExecuteHostCommand(handleID, &scsiCmd->command, &status);
   Mem_Free(scsiCmd);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostSCSICmdComplete --
 *
 *      Return completed scsi command information.
 *
 * Results:
 *      Success or failure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
HostSCSICmdComplete(SCSI_HandleID *hostHandleID,
                    SCSI_Result *result,
                    Bool *more)
{
   VMK_ReturnStatus retval = VMK_OK;
   Bool moreCmds = TRUE;
   SCSI_Result outResult;
   SCSI_HandleID handleID;

   CopyFromHost(&handleID, hostHandleID, sizeof(handleID));

   retval = SCSI_CmdCompleteInt(handleID, &outResult, &moreCmds);
   if (retval == VMK_OK) {

#ifdef HOST_SCSI
      LOG(0, "Complete: %d", outResult.serialNumber);
#endif

      CopyToHost(result, &outResult, sizeof(outResult));
      CopyToHost(more, &moreCmds, sizeof(moreCmds));
   }

   return retval;
}

/*
 *----------------------------------------------------------------------
 *
 * HostHelperRequestStatus --
 *
 *      Check the status of a helper request
 *
 * Results:
 *      VMK_OK or VMK_STATUS_PENDING
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostHelperRequestStatus(Helper_RequestHandle *hostHelperHandle)
{
   Helper_RequestHandle helperHandle;

   CopyFromHost(&helperHandle, hostHelperHandle, sizeof(Helper_RequestHandle));
   if (helperHandle == -1) {
      return VMK_OK;
   }
   return Helper_RequestStatus(helperHandle);
}


/*
 *----------------------------------------------------------------------
 *
 * HostHelperRequestCancel --
 *
 *      Cancel a helper request
 *
 * Results:
 *      VMK_OK or VMK_STATUS_PENDING
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostHelperRequestCancel(Helper_RequestHandle *hostHelperHandle,
                        Bool force)
{
   Helper_RequestHandle helperHandle;

   CopyFromHost(&helperHandle, hostHelperHandle, sizeof(Helper_RequestHandle));
   if (helperHandle == -1) {
      return VMK_INVALID_HANDLE;
   }
   return Helper_RequestCancel(helperHandle, force);
}


static VMK_ReturnStatus
HostMarkCheckpointFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_MarkCheckpointArgs *args = (VMnix_MarkCheckpointArgs *)data;

   status = Alloc_MarkCheckpoint(args->worldID, args->wakeup, args->start);
   Mem_Free(args);
   return status;
}   

/*
 * Must do MarkCheckpoint in a helper world, since it may block trying to
 * get memory for the checkpoint buffers.
 */
static VMK_ReturnStatus
HostMarkCheckpoint(VMnix_MarkCheckpointArgs *hostArgs,
		   Helper_RequestHandle *hostHelperHandle)
{
   VMnix_MarkCheckpointArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));
   Migrate_MarkCheckpoint(&args);

   return HostIssueSyncCall(hostArgs, sizeof(VMnix_MarkCheckpointArgs),
			    HELPER_SUSPEND_RESUME_QUEUE, 
                            HostMarkCheckpointFn, hostHelperHandle);
}

/*
 *----------------------------------------------------------------------- 
 *
 * HostCheckpointCleanup --
 *
 *     Inform vmkernel that checkpoint has aborted
 *
 * Results:
 *      VMK_OK on success, VMK_NOT_FOUND on failure.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
HostCheckpointCleanup(World_ID *data)
{
   World_ID worldID;
   World_Handle *world;
   
   CopyFromHost(&worldID, data, sizeof(worldID));
   world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }
   Alloc_CheckpointCleanup(world);
   World_Release(world);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HostSaveMemory --
 *
 *      Saves a reference to the world so that its memory won't get
 *      cleaned up until the destination has paged in all changed pages.
 *      (or a timeout / error occurs).
 *
 * Results: 
 *	VMK_NOT_FOUND if the world doesn't exist.
 *	VMK_OK otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
HostSaveMemory(void *wPtr)
{
   World_ID worldID;

   CopyFromHost(&worldID, wPtr, sizeof(World_ID));
   return Migrate_SaveMemory(worldID);
}


/*
 *----------------------------------------------------------------------
 *
 * HostMigrateWriteCptData --
 *
 *	Write to the migrate data file.
 *
 * Results: 
 *	VMK_OK or error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
HostMigrateWriteCptData(VMnix_MigCptDataArgs *hostArgs, void *hostBuf)
{
   VMnix_MigCptDataArgs args;

   CopyFromHost(&args, hostArgs, sizeof(VMnix_MigCptDataArgs));
   args.data = hostBuf;
   return Migrate_WriteCptData(&args, UTIL_HOST_BUFFER);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostMigrateToBegin --
 *
 *      Begin migration to this machine.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      Returns progress in *hostProgress.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostMigrateToBegin(World_ID *wPtr, VMnix_MigrateProgressResult *hostProgress)
{
   World_ID toWorldID;
   VMK_ReturnStatus status;
   VMnix_MigrateProgressResult progress;

   CopyFromHost(&toWorldID, wPtr, sizeof(toWorldID));
   status = Migrate_ToBegin(toWorldID, &progress);
   CopyToHost(hostProgress, &progress, sizeof(progress));
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HostMigrateReadCptData --
 *
 *      Read data from the locally saved checkpoint state.
 *
 * Results: 
 *	VMK_OK or error.
 *
 * Side effects:
 *	If VMK_OK is returned, hostArgs->size is set to the number of
 *	bytes read.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
HostMigrateReadCptData(VMnix_MigCptDataArgs *hostArgs,
                       void *hostData, uint32 hostDataLength)
{
   VMnix_MigCptDataArgs args;
   VMK_ReturnStatus status;

   CopyFromHost(&args, hostArgs, sizeof(*hostArgs));
   args.data = hostData;
   args.size = hostDataLength;
   status = Migrate_ReadCptData(&args, UTIL_HOST_BUFFER);
   CopyToHost(&hostArgs->size, &args.size, sizeof(args.size));

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HostMigrateSetParameters --
 *
 *      Called by userlevel prior to a migration (either to or from
 *      this machine).  If migrating to, the destination ip address
 *      will be zero.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostMigrateSetParameters(VMnix_MigrationArgs *hostArgs)
{
   VMnix_MigrationArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));
   return Migrate_SetParameters(&args);
}


/*
 *----------------------------------------------------------------------- 
 *
 * HostCheckActions --
 *
 *     Call the strangely named CpuSched_AsyncCheckActionsByID, to
 *     wakeup / interrupt a world.
 *
 * Results:
 *      Result of functions called.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
HostCheckActions(World_ID *data)
{
   World_ID worldID;
   
   CopyFromHost(&worldID, data, sizeof(worldID));
   return CpuSched_AsyncCheckActionsByID(worldID);
}


/*
 *----------------------------------------------------------------------
 *
 * HostWarning --
 *
 *      Print message from the vmnix module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
HostWarning(const char *string, int length)
{
   char msg[256];
   int len = MIN(sizeof(msg), length);
   int msgLen;

   CopyFromHost(msg, string, len);

   msg[len - 1] = '\0';

   msgLen = strlen(msg);

   /*
    * nuke trailing newline so that vmkernel
    * logging code knows to prepend a timestamp
    */
   if (msgLen > 1 && msg[msgLen -1] == '\n') {
      msg[msgLen -1] = '\0';
      if (vmx86_debug && strncmp("sysalert", msg, 7) == 0) {
         if (strncmp("sysalerttest", msg, 11) == 0) {
            int i;
            /* Test overflowing sysalert buffer (not that this actually 
             * causes an overflow, because another processor will usually 
             * handles the alert quickly enough).
             */
            for (i = 0; i < 15; i++) {
               SysAlert("%d) test:%s", i, msg);
            }
         } else {
            SysAlert("%s", msg);
         }
      } else {
         _Log("VMNIX: %s\n", msg);
      }
   } else {
      _Log("%s", msg);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * ReadOrigHostIDT --
 *
 *      Return the desired entry from the host's original version of the 
 *	IDT before we changed it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      
 *
 *----------------------------------------------------------------------
 */
static void
ReadOrigHostIDT(int32 vector, Gate *gate)
{
   // CopyFromHost(gate, &origHostIDT[vector], sizeof(*gate));
   *gate = origHostIDTCopy[vector];
}

/*
 *----------------------------------------------------------------------
 *
 * Host_EarlyInit --
 *
 *      Initialize the host module. This module handles switching
 *      between the host and the vmkernel.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Host_EarlyInit(VMnix_Info *vmnixInfo, VMnix_SharedData *sharedData,
               VMnix_StartupArgs *startupArgs)
{
   DTR32 dtr;
   Gate *hostIDT;
   MPN hostIDTmpn;
   int i;
   Selector cosTaskSel;
   Descriptor cosTaskDesc;
   void *codeAddr;

   /*
    * this assert is here to make sure that COS doesn't use global pages for
    * addresses that are in the vmkenel linear address space.  If this
    * ever changes, we'll have to flush these when entering/leaving vmkernel.
    */
   ASSERT(VMK_VA_END - VMM_FIRST_LINEAR_ADDR < VMNIX_KVA_START);

   SP_InitLockIRQ("HostICLck", &hostICPendingLock, SP_RANK_HOSTIC_LOCK);

   hostIC.type = vmnixInfo->ICType;
   hostIC.numirqs = vmnixInfo->numirqs;
   hostIC.numirqslices = NR_SLICES_NEEDED(hostIC.numirqs);
   Log("host is using %s with %d irqs",
       hostIC.type == ICTYPE_PIC ? "PIC" : "IOAPIC",
       hostIC.numirqs);

   for (i = 0; i < NR_IRQSLICES; i++) {
      hostIC.pending[i] = 0;
   }
   hostIC.inService = 0;

   for (i = 0; i < NR_IRQS; i++) {
      hostIC.cosVector[i] = vmnixInfo->irq[i].vector;
      /*
       * NOTE: It's possible for an irq to appear used even when it
       * was not possible to assign a vector (see PR 26263 and PR 38318).
       * So vmnixInfo->irq[].used should not be used to determine irq
       * presence. Only vmnixInfo->irq[].vector is appropriate.
       */
      if (hostIC.cosVector[i]) {
	  if (vmnixInfo->irq[i].pin != -1) {
	     Log("vector 0x%02x for irq %3d on %02d-%02d",
			hostIC.cosVector[i], i,
			vmnixInfo->irq[i].ic, vmnixInfo->irq[i].pin);
	  } else {
	     Log("vector 0x%02x for irq %3d on %02d NO PIN",
			hostIC.cosVector[i], i,
			vmnixInfo->irq[i].ic);
	  }
	  hostIC.flags[i] = IRQ_PRESENT;
      } else {
          hostIC.flags[i] = 0;
      }
      hostIC.vmkVector[i] = 0;
   }
   
   SHARED_DATA_ADD(sharedData->hostIC, HostIC *, &hostIC);
   SHARED_DATA_ADD(sharedData->debugRegs, unsigned *, &debugRegs[0]);
   SHARED_DATA_ADD(sharedData->statCounters, unsigned *, &statCounters[0]);
   SHARED_DATA_ADD(sharedData->configOption, unsigned *, &configOption[0]);
   SHARED_DATA_ADD(sharedData->vmkernelBroken, int *, &vmkernelBroken);
   SHARED_DATA_ADD(sharedData->hostTime, HostTime *, &hostTime);

   Atomic_Write(&interruptCause, 0);
   SHARED_DATA_ADD(sharedData->interruptCause, Atomic_uint32 *,
		   &interruptCause);

   // Rank > driver lock. 
   SP_InitLockIRQ("vmkDevLock", &vmkDevLock, SP_RANK_IRQ_PROC);
   vmkDev.qHead = 0;
   vmkDev.qTail = 0;
   SHARED_DATA_ADD(sharedData->vmkDev, VMnix_VMKDevShared*, &vmkDev); 
   SHARED_DATA_ADD(sharedData->activeIoctlHandle, Helper_RequestHandle*,
                   &activeIoctlHandle); 

   /*
    * Save a pointer to the hosts IDT and GDT
    */

   GET_IDT(dtr);
   origHostIDT = (Gate *)dtr.offset;
   origHostIDTLength = (dtr.limit + 1) / sizeof(Gate);
   CopyFromHost(origHostIDTCopy, origHostIDT, origHostIDTLength * sizeof(Gate));
   ASSERT(origHostIDTLength >= IDT_NUM_VECTORS);

   GET_GDT(dtr);
   hostGDT = (Descriptor *)dtr.offset;
   ASSERT((dtr.limit + 1) / sizeof(Gate) >= DEFAULT_NUM_ENTRIES);

   /*
    * Fill in our IDT.
    *
    * We use a task switch so that we can save+swap registers, selectors,
    * stack, IDT, and CR3 all in a single instruction, so the CPU state is
    * always valid to take NMIs or other interrupts/exceptions.  However,
    * task switches don't quite do exactly what you need (they don't save
    * current cr3 and they always set the CR0 TS bit) so first all entry
    * points go through an interrupt gate that saves cr3 and cr0 and then
    * we switch the task.
    */
   hostIDTmpn = MemMap_AllocAnyKernelPage();
   ASSERT_NOT_IMPLEMENTED(hostIDTmpn);
   hostIDT = KVMap_MapMPN(hostIDTmpn, TLB_LOCALONLY);

   ASSERT(IDT_NUM_VECTORS * sizeof(Gate) <= PAGE_SIZE);
   memset(hostIDT, 0, IDT_NUM_VECTORS * sizeof (Gate));

   // the code for exception handlers is allocated after the read only
   // section of the binary, but give it 32 bytes of space 
   hostIDTHandlers = (void*)ALIGN_UP(startupArgs->endReadOnly + 32, 32);
   codeAddr = hostIDTHandlers;
   MemRO_ChangeProtection(MEMRO_WRITABLE);
   for (i=0; i<IDT_NUM_VECTORS; i++) {
      ASSERT(origHostIDTCopy[i].present);
      if (IDT_VectorIsException(i)) {
         /* intel defined exceptions */
         codeAddr = HostDefineGate(hostIDT, i, HostHandleException,
                                   idtExcHasErrorCode[i],
                                   origHostIDTCopy[i].DPL, codeAddr);

      } else if (i == IDT_LINUXSYSCALL_VECTOR) {
         /* special case linux syscall handler; we let COS to handle its own
          * system calls called by int 0x80. By doing this we eliminate
          * superfluous context switches between COS and vmkernel. System
          * calls from COS applications end up directly in COS kernel
          * and not in vmkernel.
          */
         hostIDT[i] = origHostIDTCopy[i];

      } else if (i == IDT_VMKSYSCALL_VECTOR) {
         /* vmkernel syscall vector */
         codeAddr = HostDefineGate(hostIDT, i, HostSyscall, FALSE, 0, codeAddr);

      } else {
         codeAddr = HostDefineGate(hostIDT, i, HostHandleInterrupt, FALSE,
                                   origHostIDTCopy[i].DPL, codeAddr);
      }
   }
   MemRO_ChangeProtection(MEMRO_READONLY);
   LOG(0, "exception handlers from %p to %p", hostIDTHandlers, codeAddr);
   startupArgs->endReadOnly = (VA)codeAddr;

   /*
    * Set up the NMI handler. This should never run in the COS when
    * we are using the COS task. But we need it for when we take an
    * NMI in the COS when we are using the VMkernel task.
    */
   IDT_DefaultTaskInit(&hostNmiTask, (uint32)CommonNmiHandler,
                       (uint32)(hostNmiStack + PAGE_SIZE - 4), 0);
                       
   Host_SetGDTEntry(VMNIX_VMK_NMI_TSS_DESC,
                    (uint32)&hostNmiTask + VMNIX_VMM_FIRST_LINEAR_ADDR,
                    sizeof(hostNmiTask) - 1,
                    TASK_DESC,
                    0, 0, 1, 1, 0);

   /*
    * Setup the double fault handler.
    */
   IDT_DefaultTaskInit(&hostDFTask, (uint32)HostDoubleFaultHandler,
                       (uint32)(hostDFStack + PAGE_SIZE - 4), 0);

   Host_SetGDTEntry(VMNIX_VMK_DF_TSS_DESC,
                    VMKVA_2_HOSTVA((VA)&hostDFTask),
                    sizeof(hostDFTask) - 1,
                    TASK_DESC,
                    0, 0, 1, 1, 0);    // S, DPL, present, DB, gran

   hostIDT[EXC_DF].segment = MAKE_SELECTOR(VMNIX_VMK_DF_TSS_DESC, 0, 0);
   hostIDT[EXC_DF].type = TASK_GATE;
   hostIDT[EXC_DF].present = 1;

   KVMap_FreePages(hostIDT);

   /*
    * Setup the task used for vmkernel side of the host world.
    */
   memcpy(&hostVMKTask, &hostDFTask, sizeof hostVMKTask);
   hostVMKTask.esp = VMK_HOST_STACK_TOP - 16;
   hostVMKTask.esp0 = hostVMKTask.esp1 = hostVMKTask.esp2 = hostVMKTask.esp;
   hostVMKTask.eip = (uint32)HostAsmVMKEntry;

   Host_SetGDTEntry(VMNIX_VMK_TSS_DESC,
                    VMKVA_2_HOSTVA((VA)&hostVMKTask),
                    sizeof(hostVMKTask) - 1,
                    TASK_DESC,
                    0, 0, 1, 1, 0);    // S, DPL, present, DB, gran

   /* 
    * Create code and data gdt entries for when we switch from the host.
    */
   Host_SetGDTEntry(DEFAULT_CS_DESC,
                    VMM_FIRST_LINEAR_ADDR, VMM_NUM_PAGES + VMK_NUM_CODE_PAGES - 1,
                    CODE_DESC,         // type
                    1, 0, 1, 1, 1);    // S, DPL, present, DB, gran

   Host_SetGDTEntry(DEFAULT_DS_DESC,
                    VMM_FIRST_LINEAR_ADDR, VMM_VMK_PAGES - 1,
                    DATA_DESC,         // type
                    1, 0, 1, 1, 1);    // S, DPL, present, DB, gran

   GET_TR(cosTaskSel);
   CopyFromHost(&cosTaskDesc, &hostGDT[cosTaskSel>>3], sizeof cosTaskDesc);
   hostTaskAddr = (Task *)Desc_GetBase(&cosTaskDesc);
   
   /*
    * Load our IDT.  First map the idt mpn into the host world pagetable,
    * then set the idt register.
    */

   Host_SetIDT(hostIDTmpn, FALSE);

   dtr.offset = HOST_IDT_LINEAR_ADDR;
   dtr.limit = sizeof(Gate) * IDT_NUM_VECTORS - 1;
   SET_IDT(dtr);
   LOG(1, "idt.offset = %#x, %#x", dtr.offset, dtr.limit);

   HostWorldInitContext();
}

/*
 *----------------------------------------------------------------------
 *
 * Host_SetIDT --
 *      
 *      Map the given page that contains the IDT at the linear address
 *      where we keep the host world's IDT.  If newPTable is specified, a
 *      new pagtable page is allocated, otherwise the existing pagetable
 *      page is modified.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Host_SetIDT(MPN idtMPN, Bool newPTable)
{
   MA cr3;
   VMK_PTE *pTable;
   int i;

   ASSERT((hostWorld == NULL) || (hostWorld == MY_RUNNING_WORLD));

   GET_CR3(cr3);

   Log("cr3=%Lx mpn=%x newPT=%d", cr3, idtMPN, newPTable);

   if (newPTable) {
      pTable = PT_AllocPageTable(hostInVmkernelCR3, HOST_IDT_LINEAR_ADDR, PTE_KERNEL, NULL, NULL);
   } else {
      pTable = PT_GetPageTable(cr3, HOST_IDT_LINEAR_ADDR, NULL);
   }
   for (i = 0; i < VMK_PTES_PER_PDE; i++) {
      PT_INVAL(&pTable[i]);
   }
   PT_SET(&pTable[ADDR_PTE_BITS(HOST_IDT_LINEAR_ADDR)],
          VMK_MAKE_PTE(idtMPN, 0, PTE_KERNEL));
   PT_ReleasePageTable(pTable, NULL);

   TLB_Flush(TLB_LOCALONLY);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_VMnixVMKDev --
 *      
 *      Add a request for action on a vmkernel device's visibility in the 
 *      host's tables. The request is put in a queue to be processed later 
 *      by the vmnixmod module. 
 *      To be generic and extensible, this interface should take an array of
 *      strings and an array of ints. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Request added to shared queue. Returns with a warning if queue is full.
 *
 *----------------------------------------------------------------------
 */

void
Host_VMnixVMKDev(VMnix_VMKDevType type,  // IN:
                 const char *vmkName,    // IN: device's vmkernel name 
                 const char *drvName,    // IN: drivername 
                 const char *majorName,  // IN: major name (blk devs only) 
                 uint64 data,            // IN: device info 
                 Bool reg)               // IN: register/unregister
{
   VMnix_VMKDevInfo *devInfo;
   SP_IRQL prev;

   prev = SP_LockIRQ(&vmkDevLock, SP_IRQL_KERNEL);
   // Check for "full" queue; wastes one entry, but it simplifies things.
   if (((vmkDev.qTail + 1) % VMNIX_VMKDEV_MAXREQ) == vmkDev.qHead) {
      SP_UnlockIRQ(&vmkDevLock, prev);
      Warning("vmkDev queue full.");
      return;
   }

   // Add a request at qTail.
   devInfo = &vmkDev.queue[vmkDev.qTail];
   ASSERT(devInfo->action == VMNIX_VMKDEV_ACTION_NONE);
   memset(devInfo, 0, sizeof(VMnix_VMKDevInfo));
   devInfo->type = type;
   if (vmkName) {
      memcpy(devInfo->vmkName, vmkName, VMNIX_DEVICE_NAME_LENGTH);
   }
   if (drvName) {
      memcpy(devInfo->name.drv, drvName,
             MAX(VMNIX_MODULE_NAME_LENGTH,VMNIX_DEVICE_NAME_LENGTH));
   }
   if (majorName) {
      memcpy(devInfo->majorName, majorName,
             MAX(VMNIX_MODULE_NAME_LENGTH,VMNIX_DEVICE_NAME_LENGTH));
   }
   devInfo->action = reg ? VMNIX_VMKDEV_ACTION_REGISTER 
                        : VMNIX_VMKDEV_ACTION_UNREGISTER;
   devInfo->data = data;
   
   vmkDev.qTail = (vmkDev.qTail + 1) % VMNIX_VMKDEV_MAXREQ;
   SP_UnlockIRQ(&vmkDevLock, prev);

   Host_InterruptVMnix(VMNIX_MKDEV_EVENT);
}

/*
 *----------------------------------------------------------------------
 *
 *  HostIRQIsValid --
 *
 *      determine if irq is real in hardware
 *
 * Results:
 *      TRUE if irq is OK, FALSE otherwise
 *
 * Side effects:
 *      None 
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
HostIRQIsValid(IRQ irq)
{
   return (irq < hostIC.numirqs) &&
	  (hostIC.flags[irq] & IRQ_PRESENT) &&
	  (irq != VMNIX_IRQ) &&
	  (irq != TIMER_IRQ);
}



/*
 *----------------------------------------------------------------------
 *
 *  Host_SetupIRQ --
 *
 *      Keep track of interrupt equivalence between COS and vmkernel
 *
 * Results:
 *      None
 *
 * Side effects:
 *      hostIC.flags and hostIC.vmkVector are updated for irq
 *
 *----------------------------------------------------------------------
 */
void
Host_SetupIRQ(IRQ irq, uint32 vector, Bool isa, Bool edge)
{
   Bool ok;


   if (!HostIRQIsValid(irq)) {
      Warning("irq %d is not valid", irq);
      return;
   }

   ASSERT(isa || !edge);
   ok = IDT_VectorSetHostIRQ(vector, irq, (isa?IDT_ISA:0) | (edge?IDT_EDGE:0));
   if (!ok) {
      Warning("couldn't set up irq forwarding for %d", irq);
      return;
   }

   if (hostIC.flags[irq] & IRQ_SETUP) {
      // ISA cannot be set several times
      ASSERT(!isa);
      ASSERT(!(hostIC.flags[irq] & IRQ_ISA));
      // vector must stay the same
      ASSERT(hostIC.vmkVector[irq] == vector);
   } else {
      hostIC.flags[irq] |= IRQ_SETUP | (isa?IRQ_ISA:0);
      hostIC.vmkVector[irq] = vector;
   }

   return;
}


/*
 *----------------------------------------------------------------------
 *
 * HostDisableInterrupt --
 *
 *      Disable forwarding of an irq
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The forwarding is disabled.
 *
 *----------------------------------------------------------------------
 */
static void
HostDisableInterrupt(IRQ irq)
{
   uint32 vector;
   SP_IRQL prev;


   /*
    * Since this is a system call, we need to make sure we are not getting
    * a bad parameter.
    */
   if (!HostIRQIsValid(irq)) {
      Warning("Bogus irq %d", irq);
      return;
   }

   /*
    * A device probing for its irq (see parport_pc) may try any kind of irqs.
    */
   if (!(hostIC.flags[irq] & IRQ_SETUP)) {
      Warning("irq not set up %d", irq);
      return;
   }

   /*
    * We should no longer forward this irq. Disable the vector for COS.
    */
   vector = hostIC.vmkVector[irq];
   Log("irq %d vector 0x%x (host 0x%x)", irq, vector, hostIC.cosVector[irq]);
   IDT_VectorDisable(vector, IDT_HOST);

   /*
    * Even when COS calls this with interrupts disabled, we reenable
    * interrupts when we enter the vmkernel, so the interrupt for this irq
    * may have happened and the irq may have been set pending by now.
    */
   prev = SP_LockIRQ(&hostICPendingLock, SP_IRQL_KERNEL);
   if (hostIC.pending[irq / IRQS_PER_SLICE] & (1 << (irq % IRQS_PER_SLICE))) {
      Log("irq %d happened while being masked", irq);
      hostIC.pending[irq / IRQS_PER_SLICE] &= ~(1 << (irq % IRQS_PER_SLICE));
   }
   SP_UnlockIRQ(&hostICPendingLock, prev);
}


/*
 *----------------------------------------------------------------------
 *
 * HostEnableInterrupt --
 *
 *      Enable forwarding of an irq
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The forwarding is enabled.
 *
 *----------------------------------------------------------------------
 */
static void
HostEnableInterrupt(IRQ irq)
{
   uint32 vector;


   /*
    * Since this is a system call, we need to make sure we are not getting
    * a bad parameter.
    */
   if (!HostIRQIsValid(irq)) {
      Warning("Bogus irq %d", irq);
      return;
   }

   /*
    * A device probing for its irq (see parport_pc) may try any kind of irqs.
    */
   if (!(hostIC.flags[irq] & IRQ_SETUP)) {
      Warning("irq not set up %d", irq);
      return;
   }


   /*
    * We should forward this irq. Enable the vector for COS.
    */
   vector = hostIC.vmkVector[irq];
   Log("irq %d vector 0x%x (host 0x%x)", irq, vector, hostIC.cosVector[irq]);
   IDT_VectorEnable(vector, IDT_HOST);
}

/*
 *----------------------------------------------------------------------
 *
 * HostDoublefaultHandler --
 *
 *      Handle a double fault exception.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
HostDoubleFaultHandler(void)
{
   Descriptor trDesc;
   VMKFullExcFrame fullFrame;
   LA base;
   Task task;

   CpuSched_DisablePreemption();
   Panic_MarkCPUInPanic(); //should be done before any log/warning/sysalert
   SysAlert("BEGIN");

   CopyFromHost(&trDesc, &hostGDT[SELECTOR_INDEX(hostDFTask.prevTask)], sizeof(trDesc));
   base = Desc_GetBase(&trDesc);

   CopyFromHost(&task, (void *)base, sizeof(Task));

   Warning("eip=0x%x ebp=0x%x esp=%x", task.eip, task.ebp, task.esp);

   fullFrame.frame.u.in.gateNum = EXC_DF;
   fullFrame.frame.eip = task.eip;
   fullFrame.frame.cs = task.cs;
   fullFrame.frame.eflags = task.eflags;
   fullFrame.regs.es = task.es;
   fullFrame.regs.ds = task.ds;
   fullFrame.regs.fs = task.fs;
   fullFrame.regs.gs = task.gs;
   fullFrame.regs.eax = task.eax;
   fullFrame.regs.ebx = task.ebx;
   fullFrame.regs.ecx = task.ecx;
   fullFrame.regs.edx = task.edx;
   fullFrame.regs.ebp = task.ebp;
   fullFrame.regs.esi = task.esi;
   fullFrame.regs.edi = task.edi;

   BlueScreen_PostException(&fullFrame);
   Debug_Break();
}

/*
 *----------------------------------------------------------------------
 *
 * HostWorldInitContext --
 *
 *      Initialize the host world's pagetable, segment descriptors, and
 *      double fault handler.  When this function is called we're still
 *      running the COS's pagetable.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Set up enough to begin booting the APs.
 *
 *----------------------------------------------------------------------
 */
void
HostWorldInitContext(void)
{
   int i;
   VMK_PDPTE *pageRoot;
   VMK_PDE *hostInVmkernelPDirLow, *hostInVmkernelPDirHigh, *hostPDirHigh;
   MA cr3;

   GET_CR3(cr3);
   pageRoot = PT_AllocPageRoot(&hostInVmkernelCR3, INVALID_MPN);
   ASSERT_NOT_IMPLEMENTED(pageRoot != NULL);
   TLB_SetVMKernelPDir(VMK_PTE_2_MPN(pageRoot[0]));
   PT_ReleasePageRoot(pageRoot);

   /*
    * copy the COS kernel part (as opposed to user application address
    * space) of the host pagetable.  The kernel part includes the vmkernel
    * mapped in at high addresses, all of this resides in the last
    * (high) page directory.
    */
   ASSERT(ADDR_PDPTE_BITS(VMNIX_KVA_START) == 
          ADDR_PDPTE_BITS(VMNIX_VMK_MAP_LINEAR_ADDR));
   
   hostPDirHigh = PT_GetPageDir(cr3, VMNIX_KVA_START, NULL);
   ASSERT_NOT_IMPLEMENTED(hostPDirHigh != NULL); 

   hostInVmkernelPDirHigh = PT_GetPageDir(hostInVmkernelCR3, VMNIX_KVA_START, NULL);
   ASSERT_NOT_IMPLEMENTED(hostInVmkernelPDirHigh != NULL);

   for (i = 0; i < VMK_PDES_PER_PDPTE; i++) {
      PT_SET(&hostInVmkernelPDirHigh[i], hostPDirHigh[i]);
   }

   PT_ReleasePageDir(hostPDirHigh, NULL);

   /* 
    * now copy vmkernel part of the pagetable to low linear addresses.
    * (vmkernel resides at high addresses in COS context, but low addresses
    * in vmkernel context).
    */
   hostInVmkernelPDirLow = PT_GetPageDir(hostInVmkernelCR3, 0, NULL);
   ASSERT_NOT_IMPLEMENTED(hostInVmkernelPDirLow != NULL);

   for (i = 0; i < VMK_NUM_HOST_PDES; i++) {
      ASSERT(ADDR_PDE_BITS(VMNIX_VMK_FIRST_LINEAR_ADDR) + i < VMK_PDES_PER_PDPTE);
      PT_SET(&hostInVmkernelPDirLow[i], 
             hostInVmkernelPDirHigh[ADDR_PDE_BITS(VMNIX_VMK_FIRST_LINEAR_ADDR) + i]);
   }

   PT_ReleasePageDir(hostInVmkernelPDirLow, NULL);
   PT_ReleasePageDir(hostInVmkernelPDirHigh, NULL);

   // switch both vmkernel task and double fault task to use the new pagetable
   hostDFTask.cr3 = hostInVmkernelCR3;
   hostNmiTask.cr3 = hostInVmkernelCR3;
   hostVMKTask.cr3 = hostInVmkernelCR3;

   /*
    * set global host specific data
    */
   GET_CR0(hostCR0);
   GET_CR4(hostCR4);
}


/*
 *----------------------------------------------------------------------
 *
 * Host_LateInit --
 *
 *      Perform late initialization of the host module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Host_LateInit(void)
{
   /*
    * set global host specific data
    */
   hostWorld = MY_RUNNING_WORLD;
   hostInited = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_InitInterrupts --
 *
 * 	Reenable all the interrupts that COS had enabled before we loaded
 * 	the vmkernel
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Interrupts are enabled for host devices.
 *
 *----------------------------------------------------------------------
 */

void
Host_InitInterrupts(VMnix_Info *vmnixInfo)
{
   IRQ irq;
   VMnix_irq *vmnixirq = vmnixInfo->irq;


   /*
    * Enable interrupts for enabled irqs.
    */
   Log("Enabling irqs");
   for (irq = 0; irq < NR_IRQS; irq++) {

      /*
       * We need to filter VMNIX_IRQ and TIMER_IRQ out because they are not
       * real in hardware, they are emulated by vmkernel.
       */
      if ((irq == VMNIX_IRQ) || (irq == TIMER_IRQ)) {
	 continue;
      }

      if ((vmnixirq[irq].used &
			(IRQ_COS_USED|IRQ_COS_DISABLED)) == IRQ_COS_USED) {
         HostEnableInterrupt(irq);
      }
   }

   Host_DumpIntrInfo();
}


/*
 *----------------------------------------------------------------------
 *
 * HostDefineGate --
 *
 *      Setup a gate in the host worlds idt.  codeStart argument indicates
 *      where the exception handler emitting code should be written, and
 *      the function returns the updated value.
 *
 * Results:
 *      The virtual address after the end of the emitted code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
HostDefineGate(Gate *hostIDT,
               int    gateNum,
               void (*handler)(VMKExcFrame *regs),
               Bool   hasErrorCode,
               int    dpl,
               void *codeStart)
{
   EmitPtr memptr = (EmitPtr)codeStart;
   uint32 cs = GET_CS();
   Gate *g;

   if (!hasErrorCode) {
      EMIT32_PUSH_IMM8(0);
   }

   EMIT_PUSH_IMM(gateNum);
   EMIT_PUSH_IMM(handler);
   EMIT32_JUMP_IMM(HostEntry);

   g = &hostIDT[gateNum];
   g->segment = cs;
   g->offset_lo = (uint32)(codeStart) & 0xffff;
   g->offset_hi = (uint32)(codeStart) >> 16;
   g->type = INTER_GATE;
   g->DPL = dpl;
   g->present = 1;

   return memptr;
}

/*
 *----------------------------------------------------------------------
 *
 * HostVMKEntry
 *
 *      The main entry point for calls from the COS to the vmkernel.  This
 *      function basically creates a VMKExcFrame structure from the COS
 *      task structure and stack, then dispatches to either syscall,
 *      exception, or interrupt handlers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
HostVMKEntry(void)
{
   VMKFullExcFrame fullFrame;
   char *hostESP;
   uint32 hostCR3;
   typedef void (*HostCallHandler)(VMKExcFrame *regs);
   HostCallHandler handler;
   Bool preemptible;
   
   ASSERT_NO_INTERRUPTS();
   preemptible = CpuSched_DisablePreemption();
   if (hostWorld == NULL) {
      /*
       * For the first call (init_vmkernel), hostworld == NULL and we're
       * not preemptible, but we want to leave this call with preemptible
       * true, so force it to be true.
       */
      preemptible = TRUE;
   }
   ASSERT(preemptible);

   if (vmx86_debug) {
      Descriptor trDesc;
      CopyFromHost(&trDesc, &hostGDT[hostVMKTask.prevTask >> 3], sizeof(trDesc));
      ASSERT(Desc_GetBase(&trDesc) == (LA)hostTaskAddr);
   }
   CopyFromHost(&hostESP, &hostTaskAddr->esp, sizeof (uint32));

   /*
    * Here's what the host stack looks like at this point:
    *     Offset:    Value
    *          0     saved cr0
    *          4     saved cr3
    *          8     saved ebx
    *         12     saved eax
    *         16     handler
    *         20     gateNum
    *         24     errorCode
    *         28     eip
    *         32     cs
    *         36     eflags
    */

#define HOST_STACK_CR3_OFFSET        4
#define HOST_STACK_EBX_OFFSET        8
#define HOST_STACK_EAX_OFFSET       12
#define HOST_STACK_EXCFRAME_OFFSET  16
#define HOST_STACK_HANDLER_OFFSET   16
#define HOST_STACK_GATENUM_OFFSET   20
#define HOST_STACK_ERRORCODE_OFFSET 24
#define HOST_STACK_EIP_OFFSET       28
#define HOST_STACK_CS_OFFSET        32
#define HOST_STACK_EFLAGS_OFFSET    36
#define HOST_STACK_FRAME_SIZE       40

   // copy cr3 from stack to task
   CopyFromHost(&hostCR3, hostESP + HOST_STACK_CR3_OFFSET, sizeof hostCR3);
   CopyToHost(&(hostTaskAddr->cr3), &hostCR3, sizeof hostCR3);

   // fill out the VMKExcFrame part of the fullFrame by copying from host stack
   memset(&fullFrame, 0, sizeof fullFrame);
   CopyFromHost(&fullFrame.frame,
                hostESP + HOST_STACK_EXCFRAME_OFFSET,
                sizeof (VMKExcFrame));
   fullFrame.frame.hostESP = (uint32)hostESP;

   // call the handler (syscall, exception, or interrupt)
   handler = (HostCallHandler)fullFrame.frame.u.in.handler; 
   handler(&fullFrame.frame);
   if (vmx86_debug) {
      /*
       * The handler above is supposed to set up the function to call in COS
       * context after the task return in eax by calling HostReturn*
       */
      void *hostHandler;
      CopyFromHost(&hostHandler, &hostTaskAddr->eax, sizeof (uint32));
      ASSERT((hostHandler == &HostAsmRetHidden) ||
             (hostHandler == &HostAsmRetGenTrap) ||
             (hostHandler == &HostAsmRetGenIntr) ||
             (hostHandler == &HostAsmRetGenTrapErr) ||
             (hostHandler == &HostAsmRetGenIntrErr));
   }

   ASSERT_NO_INTERRUPTS();
   CpuSched_RestorePreemption(preemptible);
}

/*
 *----------------------------------------------------------------------
 *
 * HostHandleException --
 *
 *      Handle an exception that happened while in the host world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostHandleException(VMKExcFrame *regs)
{
   /* the COS handles its own syscall trap so we should never get to
      this point with an int 0x80 call */
   ASSERT(regs->u.in.gateNum != IDT_LINUXSYSCALL_VECTOR);

   if (regs->u.in.gateNum == EXC_NMI) {
      NMIContext nmiContext;
      uint32 ebp;

      myPRDA.inNMI = TRUE;
      asm("cld");

      CopyFromHost(&ebp, &hostTaskAddr->ebp, sizeof ebp);

      /*
       * Setup nmiContext similar to what IDT_NmiHandler does.
       */

      nmiContext.ebp = ebp;
      nmiContext.esp = regs->hostESP + HOST_STACK_EFLAGS_OFFSET + 4;
      nmiContext.ss = 0;   //unknown
      nmiContext.cs = regs->cs;
      nmiContext.eip = regs->eip;
      nmiContext.eflags = regs->eflags;
      if (User_SegInUsermode(regs->cs)) {
         nmiContext.source = NMI_FROM_COS_USER;
      } else {
         nmiContext.source = NMI_FROM_COS;
      }

      NMI_Interrupt(&nmiContext);
      myPRDA.inNMI = FALSE;

      if (!myPRDA.configNMI) {
         /*
          * Some hosts run a 'health agent' that checks for problems with
          * hardware and depends on NMIs to detect some errors. Since the
          * default NMI host handler simply prints a harmless message,
          * it is better to always forward NMIs to the host.
          */
         NMI_Pending = FALSE;
         HostReturnGenerateInt(regs, EXC_NMI, IDT_ExcHasErrorCode(EXC_NMI));
      } else {
         HostReturnHidden(regs);
      }
      return;
   } else if (regs->u.in.gateNum == EXC_DB && 
              Watchpoint_Check(regs) == WATCHPOINT_ACTION_CONTINUE) {
      HostReturnHidden(regs);      
      return;
   } else if (regs->u.in.gateNum == EXC_MC) {
      /*
       * Machine Check Exception, hide it from the host.
       * Although the host (RH72) can handle it, it is better we deal with
       * it here since it concerns the health of the whole machine.
       */
      MCE_Handle_Exception();
      HostReturnHidden(regs);
      return;
   }

   STAT_INC(VMNIX_STAT_HANDLEEXC);

   if (regs->cs == VMNIX_VMK_CS) {
      Bool dismissed = FALSE;
      Bool dismissFS = FALSE;
      /*
       * exception in vmkernel code, can happen normal execution due to bad
       * fs or gs in COS task (see below).  Or due to errors in early
       * initialization before we switch to task gate.
       */
      if (regs->eip == (uint32)&HostEntryTaskReturn) {
	 /* If the EIP is the first
          * instruction in the COS task after returning from vmkernel,
          * then it could be caused by bad fs or gs.
          * Linux leaves fs and gs pointing to bad values for short
          * durations when fs and gs are not being used, so we just
          * replace fs or gs with 0 and ignore the exception.
          */
         uint32 fs, gs;
#ifdef VMX86_LOG
         uint32 hostEIP = 0;

         if (LOGLEVEL() >= 1) {
            char *hostESP;
            CopyFromHost(&hostESP, &hostTaskAddr->esp, sizeof (uint32));
            /*
             * We've entered the vmkernel twice at this point [kindof].  Once for an 
             * interrupt, and then again for this exception that hit when we 
             * tried to return to the host from the first entry.    See comment in 
             * HostVMKEntry for the stack layout.
             *
             * Eventually, we should further limit the fs/gs zeroing to a set
             * of known hostEIPs.  (see bug 44169)
             */
            CopyFromHost(&hostEIP, hostESP + HOST_STACK_EIP_OFFSET + HOST_STACK_FRAME_SIZE, 
                         sizeof (uint32));
         }
#endif

         CopyFromHost(&fs, &hostTaskAddr->fs, sizeof (uint32));
         CopyFromHost(&gs, &hostTaskAddr->gs, sizeof (uint32));

         if ((regs->u.in.gateNum == EXC_TS) ||
             (regs->u.in.gateNum == EXC_NP)) {
	    /*
	     * A selector is bad (most likely pointing beyond the end of the
	     * GDT or LDT segment limit).
	     */
	    if (SELECTOR_CLEAR_RPL(fs) == SELECTOR_CLEAR_RPL(regs->errorCode)) {
               dismissed = TRUE;
	       dismissFS = TRUE;
               LOG(1, "dismissed fs=0x%x for #TS @eip=%#x", fs, hostEIP);
            } else if (SELECTOR_CLEAR_RPL(gs) == SELECTOR_CLEAR_RPL(regs->errorCode)) {
               dismissed = TRUE;
	       dismissFS = FALSE;
               LOG(1, "dismissed gs=0x%x for #TS @eip=%#x", gs, hostEIP);
	    }

         } else if (regs->u.in.gateNum == EXC_PF) {
	    /*
	     * A selector may point to a descriptor that cannot be retrieved
	     * because the memory backing it is not mapped by COS.
	     *
	     * NOTE: GDT should always be mapped, so we check on LDT only.
	     */
	    uint16 ldt;
	    Descriptor ldtDesc;
	    uint32 ldtBase;
	    uint32 cr2;

	    GET_CR2(cr2);
	    CopyFromHost(&ldt, &hostTaskAddr->ldt, sizeof ldt);
            ASSERT(SELECTOR_TABLE(ldt) == SELECTOR_GDT); // LDT described in GDT
	    CopyFromHost(&ldtDesc,&hostGDT[SELECTOR_INDEX(ldt)],sizeof ldtDesc);
	    ldtBase = Desc_GetBase(&ldtDesc);

	    if ((SELECTOR_TABLE(fs) == SELECTOR_LDT) &&
		((ldtBase + (fs & SELECTOR_INDEX_MASK)) == cr2)) {
	       dismissed = TRUE;
	       dismissFS = TRUE;
	       LOG(1, "dismissed fs=0x%x for #PF, ldtBase 0x%08x, cr2 0x%08x @eip=%#x",
			       fs, ldtBase, cr2, hostEIP);
	    } else if ((SELECTOR_TABLE(gs) == SELECTOR_LDT) &&
		       ((ldtBase + (gs & SELECTOR_INDEX_MASK)) == cr2)) {
	       dismissed = TRUE;
	       dismissFS = FALSE;
	       LOG(1, "dismissed gs=0x%x for #PF, ldtBase 0x%08x, cr2 0x%08x @eip=%#x",
			       gs, ldtBase, cr2, hostEIP);
	    }

	 }
      }
      if (dismissed) {
	 uint32 nullSel = 0;
	 CopyToHost(dismissFS ? &hostTaskAddr->fs : &hostTaskAddr->gs,
			 &nullSel, sizeof (uint32));
	 HostReturnHidden(regs);
      } else {
         Task task;
         CopyFromHost(&task, hostTaskAddr, sizeof task);
         Panic("exception %d from eip 0x%x in host context (task=%p)\n",
               regs->u.in.gateNum, regs->eip, &task);
      }
   } else {
      STAT_INC(VMNIX_STAT_RETURNEXC + regs->u.in.gateNum);
      HostReturnGenerateInt(regs, regs->u.in.gateNum, 
                            IDT_ExcHasErrorCode(regs->u.in.gateNum));
   }
}


/*
 *----------------------------------------------------------------------
 *
 * HostHandleInterrupt --
 *
 *      Handle an interrupt that happened while in the host world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostHandleInterrupt(VMKExcFrame *regs)
{
   STAT_INC(VMNIX_STAT_HANDLEINTR);

   Watchpoint_ForceEnable();

   IDT_HandleInterrupt(regs);

   HostReturnCheckIntr(regs, FALSE, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * HostSyscall --
 *
 *      Dispatch a system call.
 *
 * Results:
 *      Errno.
 *
 * Side effects:
 *      Interrupts are enabled for all system calls except the one
 *      used to initialize vmkernel.
 *
 *----------------------------------------------------------------------
 */

void
HostSyscall(VMKExcFrame *regs)
{
   Task task;
   unsigned syscallNum;
   uint32 ret = -1;
   Bool unloading;

   STAT_INC(VMNIX_STAT_VMKERNELCALL);

   Watchpoint_ForceEnable();

   CopyFromHost(&task, hostTaskAddr, sizeof task);

   syscallNum = task.eax;

   unloading = (syscallNum == _SYSCALL_Unload);

   if (syscallNum != _SYSCALL_InitVMKernel && MY_RUNNING_WORLD != NULL) {
      // Set the identity of the COS world during the sys call to the
      // identity of the calling process in the COS world.
      Identity_Copy(&(MY_RUNNING_WORLD->ident), &cosIdentity);
   }

   /*
    * Enable interrupts before executing the system call.
    */
   if (syscallNum != _SYSCALL_InitVMKernel) {
      ENABLE_INTERRUPTS();
   }

   /*
    * Copy the initial -1 to the hosts eax so that we will return an error
    * if we do a long jump out of the syscall handler.
    */
   CopyToHost((void*)(regs->hostESP + HOST_STACK_EAX_OFFSET),
              &ret, sizeof(uint32));

   if ((syscallNum < NUM_SYSCALLS) && (syscallTable[syscallNum])) {
      ret = syscallTable[syscallNum](task.ebx, task.ecx, task.edx, task.esi, task.edi);
   }

   CopyToHost((void*)(regs->hostESP + HOST_STACK_EAX_OFFSET),
              &ret, sizeof(uint32));

   // if InitVMKernel call fails, just return the error to vmnix module
   if ((syscallNum == _SYSCALL_InitVMKernel) && (ret != VMK_OK)) {
      HostReturnHidden(regs);
      return;
   }

   /*
    * We must disable interrupts before HostReturnCheckIntr because it
    * might schedule an APIC self interrupt to be triggered when COS
    * enables its interrupts.
    */
   CLEAR_INTERRUPTS();
   HostReturnCheckIntr(regs, unloading, 
                       (regs->eflags & EFLAGS_IF) ? TRUE : FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * HostReturnCheckIntr --
 *
 *      Return back to the host checking for pending interrupts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Never returns.
 *
 *----------------------------------------------------------------------
 */

void
HostReturnCheckIntr(VMKExcFrame *regs, Bool unloaded, 
                    Bool interruptOK)
{
   ASSERT(!CpuSched_IsPreemptible());
   ASSERT_NO_INTERRUPTS();
 
   if (!unloaded) {
      BH_Check(TRUE);
   }

   if (hostTime.currentTime > hostTime.lastTime) {
      Host_SetPendingIRQ(TIMER_IRQ);
   }

   if (!myPRDA.configNMI) {
      /*
       * Some hosts run a 'health agent' that checks for problems with
       * hardware and depends on NMIs to detect some errors. Since the
       * default NMI host handler simply prints a harmless message,
       * it is better to always forward NMIs to the host.
       */
      if (NMI_Pending) {
         NMI_Pending = FALSE;
         HostReturnGenerateInt(regs, 2, IDT_ExcHasErrorCode(2));
         return;
      }
   }
   if (!unloaded && !hostIC.inService) {
      /*
       * Check for pending interrupts. 
       *
       * Don't check for pending interrupts if we are unloading the vmkernel 
       * because the host disables interrupts before it calls us so it 
       * does not expect an interrupt.  This will work fine on a UP since we 
       * can't have any pending interrupts anyway since interrupts are
       * disabled.
       * This also works on an MP if the host runs on processor 0 and all
       * interrupts go to processor 0.  What about other cases?
       * Will the host lose interrupts after the vmkernel is unloaded?
       */
      // We should report the irq corresponding to the highest vector
      // to respect the priorities. However the vectors are allocated
      // by the COS without care for priorities so that does not matter
      // much for now. XXX
      /*
       * To avoid starvation, i.e. an irq never being reported because other
       * irqs are always found before it, we start scanning the pending list
       * from after the last forwarded irq.
       * Such starvation happens for instance because of the hardware bug
       * described in PR 41300 that leaves an irq always pending.
       * The impact on COS should be minimal since as explained in the
       * previous comment, COS does not care about the priorities.
       */
      static IRQ irqLastForwarded = 0;
      IRQ irqToForward = -1;
      int numirqs;
      IRQ irq;
      int i;
      int slice = 0;
      int element = 0;
      SP_IRQL prev = SP_LockIRQ(&hostICPendingLock, SP_IRQL_KERNEL);

      numirqs = hostIC.numirqslices * IRQS_PER_SLICE; // max number to scan
      irq = irqLastForwarded;

      for (i = 0; i < numirqs; i++) {
	 irq = (irq + 1) % numirqs;
	 slice = irq / IRQS_PER_SLICE;
	 element = irq % IRQS_PER_SLICE;
	 if (hostIC.pending[slice] & (1 << element)) {
	    irqToForward = irq;
	    break;
	 }
      }

      if (irqToForward != -1) {
	 if (!interruptOK) {
            /*
             *  We can't simulate an interrupt now because we were
             *  called with interrupts disabled.  Set up an APIC self
             *  interrupt on this CPU so we will get an interrupt as
             *  soon as they are enabled.
             */
            APIC_SelfInterrupt(IDT_NOOP_VECTOR);
	 } else {
            hostIC.inService = 1;
            hostIC.pending[slice] &= ~(1 << element);
	    irqLastForwarded = irqToForward;
            SP_UnlockIRQ(&hostICPendingLock, prev);

            STAT_INC(VMNIX_STAT_RETURNINTR + irqToForward);
            Trace_EventLocal(TRACE_HOST_INTR, irqToForward, hostIC.cosVector[irqToForward]);
            HostReturnGenerateInt(regs, hostIC.cosVector[irqToForward], FALSE);
            return;
         }
      }

      SP_UnlockIRQ(&hostICPendingLock, prev);        
   }

   STAT_INC(VMNIX_STAT_RETURNHIDDEN);
   HostReturnHidden(regs);
}


/*
 *----------------------------------------------------------------------
 *
 * HostSetReturnFn --
 *
 *      Set the function to call after switching back to the COS task.  The
 *      eax register in the task structure is used to tell HostEntry where
 *      to jump to.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
HostSetReturnFn(void *handler)
{
   CopyToHost(&hostTaskAddr->eax, &handler, sizeof (uint32));
}

/*
 *----------------------------------------------------------------------
 *
 * HostReturnHidden --
 *
 *      Return back to the host like nothing happend
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
HostReturnHidden(VMKExcFrame *regs)
{
   HostSetReturnFn(&HostAsmRetHidden);
}

/*
 *----------------------------------------------------------------------
 *
 * HostReturnGenerateInt --
 *
 *      Return back to the host causing an exception or interrupt to happen.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostReturnGenerateInt(VMKExcFrame  *regs,
                      int           gateNum,
                      Bool          hasErrorCode)
{
   Gate g;
   uint32 handler;
   uint32 cs;

   ReadOrigHostIDT(gateNum, &g);
   handler = (uint32)((g.offset_hi << 16) | g.offset_lo);
   cs = g.segment;

   if (!hasErrorCode) {
      /*
       * set up the CS:EIP on host stack to jump to desired location
       * since the host IDT handler is not expecting an errorcode,
       * overwrite gate/errorcode part of stack.
       */
      CopyToHost((void*)(regs->hostESP + HOST_STACK_GATENUM_OFFSET),
                 &handler, sizeof (uint32));
      CopyToHost((void*)(regs->hostESP + HOST_STACK_ERRORCODE_OFFSET),
                 &cs, sizeof (uint32));
   
      if (g.type == TRAP_GATE) {
         HostSetReturnFn(&HostAsmRetGenTrap);
      } else {
         ASSERT(g.type == INTER_GATE);
         HostSetReturnFn(&HostAsmRetGenIntr);
      }
   } else {
      /*
       * set up the CS:EIP on host stack to jump to desired location
       * since the host IDT handler is expecting an errorcode, leave that
       * there, but overwrite handler/gate part of stack.
       */
      CopyToHost((void*)(regs->hostESP + HOST_STACK_HANDLER_OFFSET),
                 &handler, sizeof (uint32));
      CopyToHost((void*)(regs->hostESP + HOST_STACK_GATENUM_OFFSET),
                 &cs, sizeof (uint32));

      if (g.type == TRAP_GATE) {
         HostSetReturnFn(&HostAsmRetGenTrapErr);
      } else {
         ASSERT(g.type == INTER_GATE);
         HostSetReturnFn(&HostAsmRetGenIntrErr);
      }                     
   }
}



/*
 *----------------------------------------------------------------------
 *
 * Host_SetPendingIRQ --
 *
 *      Set an interrupt to happen on the host. The interrupt will
 *      happen some time in the future.  Try to get it to happen
 *      as quickly as possible
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Host_SetPendingIRQ(IRQ irq)
{
   SP_IRQL prev;

   prev = SP_LockIRQ(&hostICPendingLock, SP_IRQL_KERNEL);

   ASSERT((irq >= 0) && (irq < hostIC.numirqs));
   hostIC.pending[irq / IRQS_PER_SLICE] |= (1 << (irq % IRQS_PER_SLICE));

   SP_UnlockIRQ(&hostICPendingLock, prev);

   /* inform scheduler to reduce host scheduling latency */
   CpuSched_HostInterrupt();

   if (!CpuSched_Wakeup(HOST_IDLE_WAIT_EVENT)) {
      /*
       * The host was not idle and we want it to run.  We set hostShouldIdle
       * to FALSE so that the next time that the host enters the idle loop
       * it won't actually wait or yield.  This handles the case where the host
       * is in the middle of calling idle when it gets interrupted and checks
       * for pending interrupts.  After handling the interrupt the host will
       * continue with the idle call and end up yielding or waiting when maybe 
       * it shouldn't.
       */
      hostShouldIdle = FALSE;
      if (World_CpuSchedRunState(hostWorld) != CPUSCHED_RUN) {
	 // host is not running, try to get it to run
	 CpuSched_MarkReschedule(HOST_PCPU);
      } else {
	 if (myPRDA.pcpuNum != HOST_PCPU) {
	    APIC_SendIPI(HOST_PCPU, IDT_NOOP_VECTOR);
	 }
      }
   }
}



/*
 *----------------------------------------------------------------------
 *
 * HostPendingInterrupt --
 *
 *      Query if the host has a pending interrupt to handle
 *
 * Results:
 *      TRUE if host has a pending interrupt, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
HostInterruptPending(void)
{
   int i;

   for (i = 0; i < hostIC.numirqslices; i++) {
      if (hostIC.pending[i]) {
	 return TRUE;
      }
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * HostTimerCallback --
 *
 *      Do periodic host related stuff. Responsible for generating timer
 *      interrupts to the console OS.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void 
HostTimerCallback(UNUSED_PARAM(void *ignore),
                  UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   hostTime.currentTime = Timer_SysUptime() / 10;
   Host_SetPendingIRQ(TIMER_IRQ);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_TimerInit --
 *
 *      Set up HostTimerCallback to provide pseudo timer interrupts to
 *      the console OS.
 *
 * Algorithm:
 *    The vmkernel periodically sets hostTime.currentTime to its own
 *    uptime in 10ms jiffies, then sets a pseudo timer interrupt
 *    pending in the COS.  On each pseudo timer interrupt, the COS
 *    updates its jiffies counter by adding the number of jiffies that
 *    have passed since the last one, i.e.:
 *
 *    jiffies += hostTime.currentTime - hostTime.lastTime;
 *    hostTime.lastTime = hostTime.currentTime;
 *
 *    On the very first pseudo timer interrupt, this jiffies update
 *    needs to include the time since the COS shut off real timer
 *    interrupts to begin loading the vmkernel.  We measure this lost
 *    time using the TSC and subtract it from the initial value of
 *    hostTime.lastTime below.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets a callback.
 *
 *----------------------------------------------------------------------
 */
void 
Host_TimerInit(uint64 tscStart, uint64 tscOffset)
{
   uint32 lostJiffies = Timer_TSCToMS(RDTSC() + tscOffset - tscStart + 5) / 10;
   hostTime.currentTime = Timer_SysUptime() / 10;
   hostTime.lastTime = hostTime.currentTime - lostJiffies;
   Host_SetPendingIRQ(TIMER_IRQ);
   Timer_Add(HOST_PCPU, HostTimerCallback, 10, TIMER_PERIODIC, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * HostGetTimeOfDay --
 *
 *       Syscall access to Timer_GetTimeOfDay
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HostGetTimeOfDay(int64 *tod) // OUT
{
   int64 tmp = Timer_GetTimeOfDay();
   CopyToHost(tod, &tmp, sizeof(tmp));
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HostSetTimeOfDay --
 *
 *       Syscall access to Timer_SetTimeOfDay
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HostSetTimeOfDay(const int64 *tod) // IN
{
   int64 tmp;
   CopyFromHost(&tmp, tod, sizeof(tmp));
   Timer_SetTimeOfDay(tmp);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_InterruptVMnix --
 *
 *      Interrupt VMnix.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The appropriate bit in the interrupt mask is turned on and
 *      an interrupt is posted to VMnix.
 *
 *----------------------------------------------------------------------
 */
void
Host_InterruptVMnix(VMnix_InterruptCause cause)
{
   Atomic_Or(&interruptCause, (1 << cause));
   if (hostInited) {
      Host_SetPendingIRQ(VMNIX_IRQ);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Host_Idle --
 *
 *      Idle the host.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Host_Idle(void)
{
   STAT_INC(VMNIX_STAT_IDLE);

   if (hostShouldIdle) {
      SP_IRQL prevIRQL;
   
      prevIRQL = SP_LockIRQ(&hostICPendingLock, SP_IRQL_KERNEL);
      if (!HostInterruptPending()) {
         CpuSched_WaitIRQ(HOST_IDLE_WAIT_EVENT,
                          CPUSCHED_WAIT_IDLE,
                          &hostICPendingLock,
                          prevIRQL);
      } else {
         SP_UnlockIRQ(&hostICPendingLock, prevIRQL);
      }
   } else {
      hostShouldIdle = TRUE;   
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_CreateWorld --
 *
 *      Create a new world and return its world id.
 *
 * Results:
 *      World ID of new world.
 *      INVALID_WORLD_ID on error (the only error should be that there
 *      are already too many worlds)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
World_ID
Host_CreateWorld(VMnix_CreateWorldArgs *hostArgs)
{
   World_Handle *newWorld = NULL;
   int status;
   VMnix_CreateWorldArgs vmnixArgs;
   World_InitArgs args;
   SharedAreaDesc *sharedAreaDescs = NULL;

   CopyFromHost(&vmnixArgs, hostArgs, sizeof(vmnixArgs));

   if ((vmnixArgs.flags & VMNIX_GROUP_LEADER) != 0) {
      vmnixArgs.groupLeader = WORLD_GROUP_DEFAULT;
   }

   if ((vmnixArgs.flags & VMNIX_USER_WORLD) == 0) {
      sharedAreaDescs = vmnixArgs.sharedAreaArgs.descs;
      ASSERT(vmnixArgs.sharedAreaArgs.numDescs != 0);
      vmnixArgs.sharedAreaArgs.descs = Mem_Alloc(sizeof(SharedAreaDesc) * 
                                                 vmnixArgs.sharedAreaArgs.numDescs);
      if (vmnixArgs.sharedAreaArgs.descs == NULL) {
         return VMK_NO_MEMORY;
      }
      CopyFromHost(vmnixArgs.sharedAreaArgs.descs, sharedAreaDescs, 
                   sizeof(SharedAreaDesc) * 
                   vmnixArgs.sharedAreaArgs.numDescs);

      World_ConfigVMMArgs(&args, &vmnixArgs);
      status = World_New(&args, &newWorld);

      Mem_Free(vmnixArgs.sharedAreaArgs.descs);
   } else {
      World_ConfigUSERArgs(&args, &vmnixArgs);
      status = World_New(&args, &newWorld);
   }
   if (status == VMK_OK) {
      ASSERT(newWorld != NULL);
      World_Bind(newWorld->worldID);
      return newWorld->worldID;
   }

   ASSERT(status == VMK_LIMIT_EXCEEDED);

   return INVALID_WORLD_ID;
}



/*
 *----------------------------------------------------------------------
 *
 * Host_BindWorld --
 *
 *      Bind to a world based on its groupleader and vcpuid.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Host_BindWorld(World_ID groupLeaderID, uint32 vcpuid, World_ID *hostWorldID)
{
   VMK_ReturnStatus status;
   World_Handle *leader = World_Find(groupLeaderID);
   if (leader) {
      if (vcpuid < World_VMMGroup(leader)->memberCount) {
         CopyToHost(hostWorldID, &World_VMMGroup(leader)->members[vcpuid], 
                    sizeof(*hostWorldID));
         status = World_Bind(World_VMMGroup(leader)->members[vcpuid]);
      } else {
         VmWarn(groupLeaderID, "bad vcpuid: %d\n", vcpuid);
         status = VMK_BAD_PARAM;
      }
      World_Release(leader);
   } else {
      WarnVmNotFound(groupLeaderID);
      status = VMK_NOT_FOUND;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_DestroyWorld --
 *
 *      Destroy a world based on its world id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Host_DestroyWorld(World_ID worldID)
{
   VmLog(worldID, "destroying world from host");
   return !(World_Destroy(worldID, FALSE) == VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RunWorld --
 *
 * 	If given world is the vmm leader, updates the sched group that
 * 	was setup for the VM. Adds the given world to the VM's sched
 * 	group and also puts the given world on the run queue.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_RunWorld(VMnix_RunWorldArgs *hostArgs)
{
   VMnix_RunWorldArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));

   // start world running
   return World_MakeRunnable(args.worldID, args.start);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_ReadRegs --
 *
 *      Read the registers from the given world.   This doesn't check
 *	to make sure that the requested world isn't running.
 *
 * Results:
 *      1 if the wrong size argument, 0 if success.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_ReadRegs(World_ID worldID, char *buffer, unsigned long bufferLength)
{
   if (bufferLength != sizeof(VMnix_ReadRegsResult)) {
      return 1;
   } else {
      VMnix_ReadRegsResult result;
      VMK_ReturnStatus status = World_ReadRegs(worldID, &result);
      CopyToHost(buffer, &result, sizeof(result));

      return status;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Host_Unload --
 *
 *      Unload the vmkernel.
 *
 * Results:
 *      VMK_OK on success, non-zero on failure.
 *
 * Side effects:
 *      The vmkernel is unloaded.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_Unload(int force)
{
   extern Bool vmkernelLoaded;

   if (World_Cleanup((Bool)force) != VMK_OK) {
      return VMK_FAILURE;
   }

   // check for corruption in main vmkernel code region;
   //   compute checksum and compare with expected value
   if (MemRO_GetChecksum() != 0) {
      if (!MemRO_IsWritable()) {
	 uint64 checksum = MemRO_CalcChecksum();
	 ASSERT(checksum == MemRO_GetChecksum());
	 if (checksum != MemRO_GetChecksum()) {
	    SysAlert("Unloading VMKernel: checksum BAD: 0x%Lx 0x%Lx",
		    checksum, MemRO_GetChecksum());
	 }
      }
   }

   /*
    * Need to change this to FALSE here because unloading SCSI drivers
    * may require that world's be waited for as long as the vmkernel is
    * loaded.
    */
   vmkernelLoaded = FALSE;

   /*
    * Net_Cleanup and SCSI_Cleanup were moved up before clear_interrupts
    * because the IBM RAID driver flushes the cache during cleanup, and
    * it needs to handle interrupts while flushing the cache.
    * Following is an OLD comment
    ** Net_Cleanup, SCSI_Cleanup, and Mod_Cleanup should be moved after
    ** Chipset_Disable but if I do this it spews "APIC id = 0xf" messages.
    ** What I really need is to mask all interrupts in the IOAPIC before I
    ** call these cleanup functions so a Chipset_MaskAll would work as well.
    ** Chipset_MaskAll();
    */

   LOG(0, "Shutting down scsi devices");
   SCSI_Cleanup();

   LOG(0, "Shutting down network devices");
   Net_Cleanup();

   /*
    * Kill off idle worlds after drivers are unloaded
    */
   World_LateCleanup();

   /*
    * Now, disable all interrupts.
    */
   CLEAR_INTERRUPTS();

   LOG(0, "Shutting down APs");
   SMP_StopAPs();

   /*
    * I turned off module cleanup because Net_Cleanup and SCSI_Cleanup
    * do any necessary cleanup.  Calling Mod_Cleanup can force some
    * things to happen twice.
    */
#ifdef notdef
   LOG(0, "Cleaning up device modules");
   Mod_Cleanup();
#endif

   LOG(0, "Restoring host interrupt handling");
   Chipset_RestoreHostSetup();

   LOG(0, "Restoring host idt");
   Host_RestoreIDT();

   Term_Display(TERM_COS);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_RestoreIDT --
 *
 *      Restore the hosts IDT.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The IDT is modified.
 *
 *----------------------------------------------------------------------
 */
void 
Host_RestoreIDT(void)
{
   DTR32 dtr;

   dtr.offset = (uint32)origHostIDT;
   dtr.limit = origHostIDTLength * sizeof(Gate) - 1;

   SET_IDT(dtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RPCRegister --
 *
 *      Handle a host call to register on a connection.
 *
 * Results:
 *      VMK_NAME_TOO_LONG if the name is too long.  Otherwise the status
 *	from RPC_Register is returned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_RPCRegister(char *inName, int nameLength, RPC_Connection *resultCnxID)
{
   char name[RPC_CNX_NAME_LENGTH];
   RPC_Connection cnx;
   VMK_ReturnStatus status;
   int numBuffers;
   uint32 bufferLength;
   Bool isSemaphore = FALSE;

   if (nameLength >= RPC_CNX_NAME_LENGTH) {
      return VMK_NAME_TOO_LONG;
   }

   CopyFromHost(name, inName, nameLength);   

   // XXX hack until we modify the userlevel+interface to pass this info
   // XXX also these go away with userworlds
   if (strncmp(name, "sema.", 5) == 0) {
      // mutex
      numBuffers = 1;
      bufferLength = sizeof(uint32);
      isSemaphore = TRUE;
   } else if (strncmp(name, "userVCPU.", 8) == 0) {
      // userRPC to vcpu thread
      numBuffers = 1;
      bufferLength = sizeof(uint32);
   } else if (strncmp(name, "vmxApp.", 6) == 0) {
      // cross userRPC to vmx thread
      numBuffers = MAX_VCPUS;
      bufferLength = sizeof(uint32);
   } else if (strncmp(name, "vmkevent.", 9) == 0) {
      // vmkevent for vmx and host agent/serverd
      numBuffers = 10;
      bufferLength = 512;
   } else {
      numBuffers = 40;
      bufferLength = RPC_MAX_MSG_LENGTH;
   }

   status = RPC_Register(name, isSemaphore, TRUE, hostWorld->worldID, 
                         numBuffers, bufferLength, mainHeap, &cnx);

   CopyToHost(resultCnxID, &cnx, sizeof(cnx));

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RPCUnRegister --
 *
 *      Handle a host call to unregister on a connection.  Since
 *      unregistering is a blocking operation issue a synchronous helper
 *      request.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostRPCUnregisterFn(void *data, UNUSED_PARAM(void **result))
{
   RPC_Connection cnxID = (RPC_Connection)data;
   return RPC_Unregister(cnxID);
}

VMK_ReturnStatus
Host_RPCUnregister(RPC_Connection cnxID, Helper_RequestHandle *hostHelperHandle)
{
   Helper_RequestHandle helperHandle = 
      Helper_RequestSync(HELPER_MISC_QUEUE, HostRPCUnregisterFn, (void*)cnxID,
                         NULL, 0, NULL);
   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   if (helperHandle == HELPER_INVALID_HANDLE) {
      return VMK_NO_FREE_HANDLES;
   } else {
      return VMK_STATUS_PENDING;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RPCGetMsg --
 *
 *      Handle a host call to get a message.
 *
 * Results:
 *      Whatever status is returned from RPC_GetMsg.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_RPCGetMsg(RPC_Connection cnxID, RPC_MsgInfo *hostMsgInfo)
{
   return RPC_GetMsg(cnxID, 0, hostMsgInfo, 0, UTIL_HOST_BUFFER, INVALID_WORLD_ID);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RPCSendMsg --
 *
 *      Handle a host call to send a message.
 *
 * Results:
 *      Whatever status is returned from RPC_SendMsg.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_RPCSendMsg(RPC_Connection cnxID, int32 function,
	        char *data, uint32 dataLength)
{
   RPC_Token token;

   return RPC_Send(cnxID, function, 0, data, dataLength, UTIL_HOST_BUFFER, 
                   &token);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_RPCPostReply --
 *
 *      Handle a host reply to a message.
 *
 * Results:
 *      Returns status from RPC_PostReply.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_RPCPostReply(RPC_Connection cnxID, RPC_Token token,
                  void *buf, uint32 bufLen)
{
   return RPC_PostReply(cnxID, token, buf, bufLen, UTIL_HOST_BUFFER);
}

extern void BackToHost(void);

/*
 *----------------------------------------------------------------------
 *
 * Host_Broken --
 *
 *      The vmkernel is broken and needs to be forceably unloaded by the 
 *	host.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vmkernel is unloaded.
 *
 *----------------------------------------------------------------------
 */
void
Host_Broken(void)
{
   vmkernelBroken = TRUE;

   if (CpuSched_IsHostWorld()) {
      HostReturnHidden(NULL);
      BackToHost();
   } else {
      while (1) {
         /* OK - things have already gone bad */
	 CpuSched_YieldToHost();
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Host_DumpIntrInfo --
 *
 *      Print out the interrupt information in the IO-APIC and 
 *      the software host PIC.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void 
Host_DumpIntrInfo(void)
{
   int i;

   Chipset_Dump();
   Log("HOST SW IC: numslices %d inService=%d",
		   hostIC.numirqslices, hostIC.inService);
   for (i = 0; i < hostIC.numirqslices; i++) {
      Log("HOST SW IC: for slice %d pending=0x%x",
		   i, hostIC.pending[i]);
   }
}

static VMK_ReturnStatus
HostLUNReserveFn(void *data, void **result)
{
   VMK_ReturnStatus status;
   VMnix_LUNReserveArgs *args = (VMnix_LUNReserveArgs *)data;
   SCSI_HandleID handleID;
   int flags = SCSI_OPEN_HOST;

   if (args->reset) {
      // Allow a "lazy" open of the SCSI device, so we can then reset
      // a device that is reserved by another host.
      flags |= SCSI_OPEN_PHYSICAL_RESERVE;
   }
   status = SCSI_OpenDevice(hostWorld->worldID, args->diskName,
			    args->targetID, args->lun, args->partition,
			    flags, &handleID);
   if (status == VMK_OK) {
      if (args->reset) {
         status = SCSI_ResetPhysBus(handleID, args->lunreset);
      }
      else {
         status = SCSI_ReservePhysTarget(handleID, args->reserve);
      }
      SCSI_CloseDevice(hostWorld->worldID, handleID);
   }
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostLUNReserve(VMnix_LUNReserveArgs *hostArgs,
	      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_LUNReserveArgs),
			   HostLUNReserveFn, hostHelperHandle);
}

/*
 *----------------------------------------------------------------------
 *
 * HostAllocVMKMem --
 *
 *      Allocate vmkernel memory and return the vmkernel virtual address. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostAllocVMKMem(uint32 *hostSize, void **hostResult)
{
   uint32 size;
   void *r;

   CopyFromHost(&size, hostSize, sizeof(size));
   r = Mem_Alloc(size);
   if (r == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyToHost(hostResult, &r, sizeof(r));
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostFreeVMKMem --
 *
 *      Free vmkernel memory previously allocated by HostAllocVMKMem
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostFreeVMKMem(void **hostAddr)
{
   void *addr;

   CopyFromHost(&addr, hostAddr, sizeof(addr));            
   Mem_Free(addr);
   return VMK_OK;
}

VMK_ReturnStatus
HostSaveBIOSInfoIDE(char *hostInfo)
{
   LOG_ONLY(unsigned char *BIOS = drive_info;
	    int unit;)

   CopyFromHost(drive_info, hostInfo, sizeof(drive_info));

   LOG_ONLY(for (unit = 0; unit < MAX_BIOS_IDE_DRIVES; ++unit) {
      unsigned short cyl = *(unsigned short *)BIOS;
      unsigned char head = *(BIOS+2);
      unsigned char sect = *(BIOS+14);
      
      LOG(1, "BIOS drive_info hd%d: C/H/S=%d/%d/%d",
             unit, cyl, head, sect);
      BIOS += DRIVE_INFO_SIZE;
   })

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostMemMap --
 *
 *	Specify memory admission control parameters for "worldID"
 *	in "admitArg", and map memory starting at "startVA". 
 *
 * Results:
 *      Result of function called.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostMemMap(VMnix_DoMemMapArgs *hostArgs)
{
   VMnix_DoMemMapArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));

   return Alloc_OverheadMemMap(args.worldID, args.startUserVA);
}

/*
 *----------------------------------------------------------------------
 *
 * HostSetMemMapLast --
 *
 *      Set the last address that is being used in the mmap region.
 *      
 * Results:
 *      Result of function called.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostSetMemMapLast(VMnix_SetMMapLastArgs *hostArgs)
{
   VMnix_SetMMapLastArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));

   return Alloc_SetMMapLast(args.worldID, args.endMapOffset);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_GetAPICBase
 *
 *      Return the base machine address for the apic.  MA is passed in
 *      as an argument because it is a 64bit value and currently vmkernel
 *      syscalls only handle 32bit return values.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_GetAPICBase(MA *hostMA)
{
   MA ma = APIC_GetBaseMA();
   CopyToHost(hostMA, &ma, sizeof(MA));

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostDelaySCSICmds --
 *
 *      Set the delaySCSICmdsCycles of the world to delay scsi commands
 *      that come back too fast.  See the comment in vmk_scsi.c for
 *      details.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HostDelaySCSICmds(World_ID worldID, uint32 delay)
{
   World_Handle *world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   World_VMMGroup(world)->delaySCSICmdsUsec = delay;

   World_Release(world);

   return VMK_OK;
}

static VMK_ReturnStatus
HostScanAdapterFn(void *data, void **result)
{
   VMK_ReturnStatus status = VMK_OK;
   VMnix_ScanAdapterArgs *args = (VMnix_ScanAdapterArgs *)data;

   if (args->VMFSScanOnly) {
      status = VC_RescanVolumes(NULL, NULL);
   } else {
      status = VC_RescanVolumes(SCSI_DISK_DRIVER_STRING, args->adapterName);
   }
   Mem_Free(args);
   return status;
}

/*
 * Force a rescan of a particular adapter, or just a rescan for VMFSes.
 */
static VMK_ReturnStatus
HostScanAdapter(VMnix_ScanAdapterArgs *hostArgs,
	      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_ScanAdapterArgs),
			   HostScanAdapterFn, hostHelperHandle);
}

/*
 *----------------------------------------------------------------------
 *
 * HostReadVMKCoreMem --
 *
 *      Read upto a page from the given virtual address and write it to the
 *      given host buffer.  Reads spanning multiple pages are not
 *      allowed. This is used for /proc/vmware/vmkcore.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HostReadVMKCoreMem(VA vaddr, uint32 len, char *buffer)
{
   ASSERT(vaddr >= VMK_FIRST_ADDR);
   ASSERT(vaddr < VMK_VA_END);
   ASSERT(len <= PAGE_SIZE);
   ASSERT(VA_2_VPN(vaddr) == VA_2_VPN(vaddr + len - 1));

   if (Util_VerifyVPN(VA_2_VPN(vaddr), FALSE)) {
      CopyToHost(buffer, (void*)vaddr, len);
   } else {
      CopyToHost(buffer, zeroPage, len);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_SetGDTEntry --
 *
 *      Setup the given description index in the host GDT.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Host_SetGDTEntry(int index, LA base, VA limit, uint32 type, 
                 uint32 S, uint32 DPL, uint32 present, uint32 DB, 
                 uint32 gran)
{
   Descriptor desc;

   ASSERT(index < VMNIX_VMK_LAST_DESC);

   LOG(1, "index=%d, base=%x, limit=%x, type=%x", index, base, limit, type);

   CopyFromHost(&desc, &hostGDT[index], sizeof desc);
   if ((desc.present) &&
       (Desc_GetLimit(&desc) != limit)) {
      /*
       * This check makes sure we're not overwriting any GDT entries that
       * someone else set up in the COS GDT.
       */
      Panic("entry %d already present (base=0x%x, limit=0x%x)",
            index, Desc_GetBase(&desc), Desc_GetLimit(&desc));
   }
   Desc_SetDescriptor(&desc, base, limit, type, S, DPL, present, DB, gran);
   CopyToHost(&hostGDT[index], &desc, sizeof desc);
}

/*
 *----------------------------------------------------------------------
 *
 * Host_GetVMKTask
 *
 *      Return pointer to task structure used when the hostworld is running
 *      in the vmkernel context.
 *
 * Results:
 *      pointer to task structure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Task *
Host_GetVMKTask(void)
{
   return &hostVMKTask;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_GetVMKPageRoot
 *
 *      Return page table root for the hostworld when running in vmkernel
 *      context. This is the pagetable for the vmkernel's double fault
 *      task.
 *
 * Results:
 *      machine address for page table root
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MA
Host_GetVMKPageRoot(void)
{
   ASSERT(!vmkernelLoaded);  //should only be called during initialization
   ASSERT(hostInVmkernelCR3 != 0);
   return hostInVmkernelCR3;
}

static VMK_ReturnStatus
HostModUnloadFn(void *data, void **result)
{
   VMnix_ModUnloadArgs *args = (VMnix_ModUnloadArgs *)data;
   VMK_ReturnStatus status = Mod_Unload(args->moduleID);
   Mem_Free(args);
   return status;
}

/*
 * Unload the specified module.
 */
static VMK_ReturnStatus
HostModUnload(VMnix_ModUnloadArgs *hostArgs,
	      Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_ModUnloadArgs),
			   HostModUnloadFn, hostHelperHandle);
}

static void
HostGetNicState(char *inBuf, VMnix_CosVmnicInfo *vmnicInfo)
{
   char nicName[VMNIX_DEVICE_NAME_LENGTH];
   VMnix_CosVmnicInfo tmpInfo;
   ASSERT(inBuf);
   ASSERT(vmnicInfo);
   CopyFromHost(nicName, inBuf, sizeof nicName);
   Net_HostGetNicState(nicName, &tmpInfo);
   CopyToHost(vmnicInfo, &tmpInfo, sizeof tmpInfo);
}

/*
 *----------------------------------------------------------------------
 *
 * HostSetCOSContext
 *
 *      Associates a given helper request with a COS context and makes
 *      sure that the COS context gets an interrupt when the request
 *      finishes.
 *
 *      It just forwards things to the correspoding function in helper.c
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Helper_SetCOSContext
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostSetCOSContext(VMnix_SetCOSContextArgs *args)
{
   VMnix_SetCOSContextArgs hostArgs;

   CopyFromHost(&hostArgs, args, sizeof(hostArgs));
   return Helper_SetCOSContext(&hostArgs);
}


/*
 *----------------------------------------------------------------------
 * HostCosPanic
 *      
 *      Handles a panic / oops from the COS.  Dumps the printk log
 *      buffer to the vmkernel log file, and then coredumps the 
 *      cos and PSODs if the PSOD_ON_COS_PANIC config option is set. 
 *
 *      This function must be called from the context of the host
 *      world.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      Boom!
 *
 *----------------------------------------------------------------------
 */
#define MAX_PRINTK_DUMP (VMK_LOG_BUFFER_SIZE / 2)
static VMK_ReturnStatus
HostCosPanic(VMnix_CosPanicArgs *hostArgs)
{
   static VMnix_CosPanicArgs args;
   MA hostTaskCr3 = 0;
   ASSERT(World_IsHOSTWorld(MY_RUNNING_WORLD));

   CopyFromHost(&args, hostArgs, sizeof args);
   args.hostMsg[(sizeof args.hostMsg) - 1] = '\0';
   SysAlert("COS Error: %s", args.hostMsg);

   CopyFromHost(&hostTaskCr3, &hostTaskAddr->cr3, sizeof(hostTaskAddr->cr3));
   LOG(0, "cr3 = %#Lx", hostTaskCr3);

   /*
    * Dump the log buffer to serial just in case
    */
   CosDump_LogBuffer(args.hostLogBuf, args.logEnd, args.logBufLen, 
                     MAX_PRINTK_DUMP, hostTaskCr3);
   if (CONFIG_OPTION(PSOD_ON_COS_PANIC)) {
      IDT_UnshareInterrupts();

      CosDump_Core(hostTaskCr3, args.hdr);
      Debug_AddCOSPanicBacktrace(&args.excFrame);
      CLEAR_INTERRUPTS();
      NMI_Disable();
      BlueScreen_Post(args.hostMsg, &args.excFrame);
      CosDump_BacktraceToPSOD(args.hostLogBuf, args.logEnd, args.logBufLen,
                              MAX_PRINTK_DUMP, hostTaskCr3);
      Debug_Break();
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HostGetNextAnonPage
 * 
 *	Returns the next anonymous page.
 *
 * Results:
 *	TRUE on success, FALSE on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
HostGetNextAnonPage(World_ID worldID, MPN inMPN,
		    VMnix_GetNextAnonPageResult *out)
{
   VMnix_GetNextAnonPageResult result;
   VMK_ReturnStatus status;

   status = Alloc_GetNextAnonPage(worldID, inMPN, &result.mpn);
   CopyToHost(out, &result, sizeof result);

   return (status == VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * HostReadPage
 * 
 *	Returns the data in the vpn of the given world.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostReadPage(World_ID worldID, VPN vpn, void *data)
{
   VMK_ReturnStatus status;
   World_Handle *world;
   MPN mpn;

   world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   status = World_VPN2MPN(world, vpn, &mpn);
   if (status == VMK_OK) {
      if (mpn == INVALID_MPN) {
	 CopyToHost(data, zeroPage, PAGE_SIZE);
      } else {
	 void* mappedData = KVMap_MapMPN(mpn, TLB_LOCALONLY);
	 ASSERT(mappedData != NULL);

	 CopyToHost(data, mappedData, PAGE_SIZE);

	 KVMap_FreePages(mappedData);
      }
   }

   World_Release(world);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HostReadVMKStack
 * 
 *	Returns the data in the given page number of the given world's stack.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostReadVMKStack(World_ID worldID, int pageNum, void *data, VA *vaddr)
{
   VMK_ReturnStatus status;
   World_Handle *world;
   VA va;
   
   world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   status = World_GetVMKStackPage(world, pageNum, &va);
   if (status == VMK_OK) {
      CopyToHost(data, (void*)va, PAGE_SIZE);
      CopyToHost(vaddr, &va, sizeof *vaddr);
   }

   World_Release(world);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostLookupMPN
 * 
 *	Returns the MPN associated with the given VPN or INVALID_MPN if
 *	there isn't one.
 *
 * Results:
 *	The MPN or INVALID_MPN.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static MPN
HostLookupMPN(World_ID worldID, VPN userVPN)
{
   VMK_ReturnStatus status;
   MPN mpn;

   status = Alloc_LookupMPN(worldID, userVPN, &mpn);
   if (status != VMK_OK) {
      LOG(1, "Alloc_LookupMPN(%d, %#x) failed: %s",
	    worldID, userVPN, VMK_ReturnStatusToString(status));
      return INVALID_MPN;
   }

   ASSERT(mpn != INVALID_MPN);
   return mpn;
}


/*
 *----------------------------------------------------------------------
 *
 * HostFindUserWorld --
 *
 *	Returns the UserWorld associated with the world id.
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if this world doesn't exist or isn't a UserWorld.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
HostFindUserWorld(World_ID worldID, World_Handle **world)
{
   World_Handle *w;

   w = World_Find(worldID);
   if (w == NULL) {
      LOG(0, "World %d not found.  Caller: %p", worldID,
	  __builtin_return_address(0));
      return VMK_NOT_FOUND;
   }

   if (!World_IsUSERWorld(w)) {
      LOG(0, "World %d not a UserWorld.  Caller: %p", worldID,
	  __builtin_return_address(0));
      World_Release(w);
      return VMK_NOT_FOUND;
   }

   *world = w;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserAddArg --
 *
 *      Add an argument to the worlds argument list.  These arguments
 *	will show up in argv when the world starts running.
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if this world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_UserAddArg(VMnix_SetWorldArgArgs  *hostArgs)
{
   VMnix_SetWorldArgArgs inArgs;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&inArgs, hostArgs, sizeof(inArgs));

   status = HostFindUserWorld(inArgs.worldID, &world);
   if (status == VMK_OK) {
      /* Make sure arg is null terminated. */
      inArgs.arg[sizeof(inArgs.arg) - 1] = '\0';

      status = UserInit_AddArg(world, inArgs.arg);

      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetBreak --
 *
 *      Calls UserMem_SetDataEnd with the UserMem of the specified world.
 *
 * Results:
 *      VMK_OK if brk is set, VMK_BAD_PARAM or VMK_LIMIT_EXCEEDED if
 *      brk goes below or above hard limits, respectively.
 *
 * Side effects:
 *	Whatever happens in UserMem_SetDataEnd.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserSetBreak(VMnix_SetBreakArgs* hostArgs)
{
   VMnix_SetBreakArgs args;
   VMK_ReturnStatus status;
   World_Handle* world;
   
   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);
   
   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_SetBreak(world, args.brk);
      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetLoaderInfo --
 *
 *	Saves the given User_LoaderInfo in the initArgs struct.
 *
 * Results:
 *	VMK_OK on success, appropriate error code on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserSetLoaderInfo(VMnix_SetLoaderArgs* hostArgs)
{
   VMK_ReturnStatus status = VMK_OK;
   VMnix_SetLoaderArgs args;
   World_Handle* world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_SetLoaderInfo(world, args.phdr, args.phent,
				      args.phnum, args.base, args.entry);
      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserMapFile --
 *
 *	Stores the userland fd to filename mapping for later use when mapped
 *	sections are actually mapped in.
 *
 * Results:
 *	VMK_OK on success, appropriate error code on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserMapFile(VMnix_UserMapFileArgs* hostArgs)
{
   VMK_ReturnStatus status;
   VMnix_UserMapFileArgs args;
   World_Handle* world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      /* Ensure string is null terminated */
      args.name[sizeof(args.name) - 1] = '\0';
      status = UserInit_AddMapFile(world, args.id, args.name);
      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserMapSection --
 *
 *      Saves the map information to be used later to actually mmap the
 *      region in.
 *
 * Results:
 *      VMK_OK on success, appropriate error code on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserMapSection(VMnix_UserMapSectionArgs* hostArgs)
{
   VMK_ReturnStatus status;
   VMnix_UserMapSectionArgs args;
   World_Handle* world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_AddMapSection(world, args.addr, args.length, args.prot,
				      args.flags, args.id, args.offset,
				      args.zeroAddr);
      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetWorldWD --
 *
 *      Store the name of the working directory for this world.  It
 *	will be set in User_WorldStart.
 *
 * Results:
 *	VMK_NOT_FOUND if given world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserSetWorldWD(VMnix_SetWorldWDArgs* hostArgs)
{
   VMK_ReturnStatus status;
   VMnix_SetWorldWDArgs args;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      /* Ensure string is null terminated. */
      args.arg[sizeof(args.arg) - 1] = '\0';
      status = UserInit_SetWorldWD(world, args.arg);
      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserForwardSignal --
 *
 *	Forward the given signal to the given cartel.  If the cartel
 *	is newborn it cannot get any signals, so just destroy it
 *	directly.
 * 
 * Results:
 *	VMK_OK if cartel is signalled, or an appropriate error code.
 *
 * Side effects:
 *	Changes saved shutdown state for given cartel.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserForwardSignal(VMnix_ForwardSignalArgs* hostArgs)
{
   VMnix_ForwardSignalArgs args;
   VMK_ReturnStatus status;
   World_Handle* world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = LinuxSignal_Forward(world, args.sig);
      World_Release(world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserProxyObjReady --
 *
 *	Wakes up any worlds waiting for the given proxy fd.
 * 
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	Worlds will be woken.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HostUserProxyObjReadyFn(VMnix_ProxyObjReadyArgs* args)
{
   VMK_ReturnStatus status;
   World_Handle* world;

   status = HostFindUserWorld(args->cartelID, &world);
   if (status == VMK_OK) {
      status = UserProxy_ObjReady(world, args->fileHandle, &args->pcUpdate);
      World_Release(world);
   }

   Mem_Free(args);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserProxyObjReady --
 *
 *	Start a helper world runing HostUserProxyObjReadyFn.
 * 
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserProxyObjReady(VMnix_ProxyObjReadyArgs* hostArgs,
		       Helper_RequestHandle* helperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof *hostArgs,
   			   (HelperRequestSyncFn*)HostUserProxyObjReadyFn,
			   helperHandle);
}


/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetIdentity --
 *
 *      Set the uids and gids of a userworld.
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if this world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_UserSetIdentity(VMnix_SetWorldIdentityArgs *hostArgs)
{
   VMnix_SetWorldIdentityArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof(args));

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_SetIdentity(world, args.umask,
				    args.ruid, args.euid, args.suid,
				    args.rgid, args.egid, args.sgid,
				    MIN(args.ngids, sizeof args.gids),
				    args.gids);
      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetDumpFlag --
 *
 *      Set the 'coreDumpEnabled' flag on the given world's cartel.
 *      World must not be started yet. 
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if the world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_UserSetDumpFlag(VMnix_SetWorldDumpArgs *hostArgs)
{
   VMnix_SetWorldDumpArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof(args));

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_SetDumpFlag(world, args.enabled);
      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_UserSetMaxEnvVars --
 *
 *	Limits the total number of environment variables for this
 *	UserWorld.
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if the world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Host_UserSetMaxEnvVars(VMnix_SetMaxEnvVarsArgs *hostArgs)
{
   VMnix_SetMaxEnvVarsArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserInit_SetMaxEnvVars(world, args.maxEnvVars);
      World_Release(world);
   }

   return status;

}

/*
 *----------------------------------------------------------------------
 *
 * Host_UserAddEnvVar --
 *
 *	Add an environment variable to this UserWorld's environment.
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if the world doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_UserAddEnvVar(VMnix_AddEnvVarArgs *hostArgs)
{
   VMnix_AddEnvVarArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      char *envVar;

      envVar = Mem_Alloc(args.length);
      if (envVar == NULL) {
         status = VMK_NO_MEMORY;
      } else {
         CopyFromHost(envVar, args.envVar, args.length);
	 envVar[args.length - 1] = '\0';

         status = UserInit_AddEnvVar(world, envVar, args.length);

	 Mem_Free(envVar);
      }

      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Host_UserCreateSpecialFds --
 *
 *	Uses the given type information to create the special fds (stdin,
 *	stdout, stderr).
 *
 * Results:
 *      VMK_OK upon success
 *	VMK_NOT_FOUND if the world doesn't exist.
 *	VMK_FAILURE if the special fds cannot be created.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Host_UserCreateSpecialFds(VMnix_CreateSpecialFdsArgs *hostArgs)
{
   VMnix_CreateSpecialFdsArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   ASSERT(MY_RUNNING_WORLD == hostWorld);
   CopyFromHost(&args, hostArgs, sizeof(args));

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = args.vmkTerminal ?
	      UserTerm_CreateSpecialFds(world) :
	      UserProxy_CreateSpecialFds(world, args.inType, args.outType,
					  args.errType);
      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostAllocLowVMKPages --
 *
 *      Allocates VMKernel low pages.
 *
 * Results:
 *      VMK_OK upon success
 *      VMK_NO_MEMORY if we can't get memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostAllocLowVMKPages(MPN *mpn, uint32 pages)
{
   MPN tmpMpn;

   /* we need to DMA from and to those pages so we allocate MM_TYPE_LOW
    * memory */
   tmpMpn = MemMap_AllocKernelPages(pages, MM_NODE_ANY, MM_COLOR_ANY, MM_TYPE_LOW);
   /* get memory failed */
   if (tmpMpn == INVALID_MPN) {
      return VMK_NO_MEMORY;
   }

   CopyToHost(mpn, &tmpMpn, sizeof(MPN));
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostFreeLowVMKPages --
 *
 *      Frees VMKernel low pages.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostFreeLowVMKPages(MPN mpn)
{

   MemMap_FreeKernelPages(mpn);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostMemMakeDevFn --
 *
 *      Makes a Memory Device.
 *
 * Results:
 *      VMK_OK if everything is fine.
 *      VMK_NOT_FOUND if no such driver type is found
 *      VMK_NOT_IMPLEMENTED if driver MakeDev function isn't implemented
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostFDSMakeDevFn(void* args, void **result)
{  
   VMK_ReturnStatus status = VMK_OK;
   VMnix_FDS_MakeDevArgs* hostArgs = (VMnix_FDS_MakeDevArgs *) args;
   status = FDS_MakeDev((VMnix_FDS_MakeDevArgs *) hostArgs);
   
   Mem_Free(hostArgs);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostMemMakeDev --
 *
 *      Makes a Memory Device.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostFDSMakeDev(VMnix_FDS_MakeDevArgs *hostArgs, 
                  Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FDS_MakeDevArgs),
			   HostFDSMakeDevFn, hostHelperHandle);
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostFDSOpenDev --
 *
 *      Open a device in the fs device switch.
 *
 * Results:
 *      VMK_ReturnStatus. 
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostFDSOpenDevFn(void* args, 
                 void **result)
{  
   VMK_ReturnStatus status = VMK_OK;
   VMnix_FDS_OpenDevArgs *hostArgs = (VMnix_FDS_OpenDevArgs *) args;
   VMnix_FDS_OpenDevResult *r;
   FDS_Handle *fdsHandle = Mem_Alloc(sizeof(FDS_Handle));

   if (!fdsHandle) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   *result = NULL;
   status = FDS_OpenDevice(hostWorld->worldID, hostArgs->devName, SCSI_OPEN_HOST, 
                           &fdsHandle->hid, &fdsHandle->devOps);
   if (status == VMK_OK) {
      r = Mem_Alloc(sizeof(VMnix_FDS_OpenDevResult));
      if (!r) {
         Mem_Free(fdsHandle);
         status = VMK_NO_MEMORY;
      } else {
         LOG(0, "fdsHandle=%p hid=0x%Lx", fdsHandle, fdsHandle->hid);
         r->cookie = fdsHandle;
         *result = (void *) r;
      }
   }
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFDSOpenDev(VMnix_FDS_OpenDevArgs *args, 
               VMnix_FDS_OpenDevResult *result, 
               Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCallWithResult(args, sizeof(VMnix_FDS_OpenDevArgs),
                                     result, sizeof(VMnix_FDS_OpenDevResult), 
			             HostFDSOpenDevFn, hostHelperHandle);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostFDSCloseDev --
 *
 *      Close device in the fs device switch. 
 *
 * Results:
 *      VMK_ReturnStatus. 
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostFDSCloseDevFn(void* args, 
                  void **result)
{  
   VMK_ReturnStatus status = VMK_OK;
   VMnix_FDS_CloseDevArgs *hostArgs = (VMnix_FDS_CloseDevArgs *) args;
   FDS_Handle *fdsHandle = (FDS_Handle *) hostArgs->cookie;

   LOG(0, "fdsHandle=%p hid=0x%Lx", fdsHandle, fdsHandle->hid);
   status = fdsHandle->devOps->FDS_CloseDevice(hostWorld->worldID, fdsHandle->hid);
   ASSERT(status == VMK_OK);

   Mem_Free(fdsHandle);
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFDSCloseDev(VMnix_FDS_CloseDevArgs *args, 
                Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(args, sizeof(VMnix_FDS_CloseDevArgs), 
			   HostFDSCloseDevFn, hostHelperHandle);
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostFDSIO --
 *
 *      
 *
 * Results:
 *      
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostFDSIOFn(void *args,
            void **result)
{
   VMnix_FDS_IOArgs *hostArgs = (VMnix_FDS_IOArgs *) args;
   FDS_Handle *fdsHandle = (FDS_Handle *) hostArgs->cookie;
   VMK_ReturnStatus status = VMK_OK;
   SG_Array sgArray;

   LOG(3, "fdsHandle=%p hid=0x%Lx", fdsHandle, fdsHandle->hid); 
   *result = NULL;
   sgArray.addrType = SG_MACH_ADDR;
   sgArray.length = 1;
   sgArray.sg[0].offset = hostArgs->offset;
   sgArray.sg[0].addr = hostArgs->cosBufMA;
   sgArray.sg[0].length = hostArgs->length;

   status = fdsHandle->devOps->FDS_SyncIO(fdsHandle->hid, &sgArray, hostArgs->isRead);
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFDSIO(VMnix_FDS_IOArgs *hostArgs,
          Helper_RequestHandle *hostHelperHandle)
{
   return HostMakeSyncCall(hostArgs, sizeof(VMnix_FDS_IOArgs), 
                           HostFDSIOFn, hostHelperHandle);
}



/*
 *-----------------------------------------------------------------------------
 *
 * HostFDSIoctl --
 *
 *      
 *
 * Results:
 *      
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HostFDSIoctlFn(void *args,
               void **result)
{
   VMnix_FDS_IoctlArgs *hostArgs = (VMnix_FDS_IoctlArgs *) args;
   FDS_Handle *fdsHandle = (FDS_Handle *) hostArgs->cookie;
   VMK_ReturnStatus status = VMK_OK;
   void *r;

   LOG(3, "fdsHandle=%p hid=0x%Lx cmd=0x%x", fdsHandle, fdsHandle->hid,
       hostArgs->cmd); 
   *result = NULL;
   r = Mem_Alloc(hostArgs->resultSize);
   if (!r) {
      Mem_Free(args);
      return VMK_NO_MEMORY;
   }
   status = fdsHandle->devOps->FDS_Ioctl(fdsHandle->hid, hostArgs->cmd, r);
   if (status == VMK_OK) {
      *result = r;
   } else {
      Mem_Free(r);
   }
   Mem_Free(args);
   return status;
}

static VMK_ReturnStatus
HostFDSIoctl(VMnix_FDS_IoctlArgs *hostArgs,
             Helper_RequestHandle *hostHelperHandle)
{
   VMnix_FDS_IoctlArgs args;
   CopyFromHost(&args, hostArgs, sizeof(VMnix_FDS_IoctlArgs));
   return HostMakeSyncCallWithResult(hostArgs, sizeof(VMnix_FDS_IoctlArgs), 
                                     args.result, args.resultSize,
                                     HostFDSIoctlFn, hostHelperHandle);
}

/*
 *----------------------------------------------------------------------
 *
 * HostUserSetExecName --
 *
 *	Sets the executable name for this userworld.
 *
 * Results:
 *      VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostUserSetExecName(VMnix_SetExecNameArgs *hostArgs)
{
   VMnix_SetExecNameArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserDump_SetExecName(world, args.execName);
      World_Release(world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostModAlloc --
 *
 *      Entry point for MOD_ALLOC host->vmk syscall
 *
 * Results:
 *      Mod_Alloc
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModAlloc(VMnix_ModAllocArgs *hostArgs, VMnix_ModAllocResult *hostResult)
{
   VMnix_ModAllocArgs args;
   VMnix_ModAllocResult result;
   VMK_ReturnStatus status;

   CopyFromHost(&args, hostArgs, sizeof(args));
   status = Mod_Alloc(&args, &result);
   if (status == VMK_OK) {
      CopyToHost(hostResult, &result, sizeof(result));
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostModPutPage --
 *
 *      Entry point for MOD_PUT_PAGE host->vmk syscall
 *
 * Results:
 *      Mod_PutPage
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModPutPage(int moduleID, void *addr, void *hostData)
{
   VMK_ReturnStatus status;
   void *data;

   data = Mem_Alloc(PAGE_SIZE);
   if (data == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(data, hostData, PAGE_SIZE);
   status = Mod_PutPage(moduleID, addr, data);
   Mem_Free(data);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostModLoadDone --
 *
 *      Entry point for MOD_LOAD_DONE host->vmk syscall
 *
 * Results:
 *      Mod_LoadDone
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModLoadDone(VMnix_ModLoadDoneArgs *hostArgs)
{
   VMnix_ModLoadDoneArgs args;

   CopyFromHost(&args, hostArgs, sizeof(args));
   return Mod_LoadDone(&args);
}

/*
 *----------------------------------------------------------------------
 *
 * HostModList --
 *
 *      Entry point for MOD_LIST host->vmk syscall
 *
 * Results:
 *      Mod_List
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModList(int maxModules, VMnix_ModListResult *hostList)
{
   VMnix_ModListResult *list;

   list = Mem_Alloc(sizeof(VMnix_ModListResult) +
                    (maxModules - 1) * sizeof(VMnix_ModDesc));
   if (list == NULL) {
      return VMK_NO_MEMORY;
   }

   Mod_List(maxModules, list);

   CopyToHost(hostList, list, sizeof(VMnix_ModListResult) +
                              (list->numModules - 1) * sizeof(VMnix_ModDesc));
   Mem_Free(list);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HostModAddSym --
 *
 *      Entry point for MOD_ADD_SYMBOL host->vmk syscall
 *
 * Results:
 *      Mod_AddSym
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModAddSym(VMnix_SymArgs *hostArgs)
{
   VMnix_SymArgs args;
   char *name;
   VMK_ReturnStatus status;

   CopyFromHost(&args, hostArgs, sizeof(args));

   name = Mem_Alloc(args.nameLength);
   if (name == NULL) {
      return VMK_NO_MEMORY;
   }
   CopyFromHost(name, args.name, args.nameLength);
   args.name = name;

   status = Mod_AddSym(&args);

   Mem_Free(name);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostModGetSym --
 *
 *      Entry point for MOD_GET_SYMBOL host->vmk syscall
 *
 * Results:
 *      Mod_GetSym
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostModGetSym(VMnix_SymArgs *hostArgs)
{
   VMnix_SymArgs args;
   VMK_ReturnStatus status;
   char *hostName, *name;

   CopyFromHost(&args, hostArgs, sizeof(args));

   name = Mem_Alloc(args.nameLength);
   if (name == NULL) {
      return VMK_NO_MEMORY;
   }
   hostName = args.name;
   args.name = name;

   status = Mod_GetSym(&args);
   args.name = hostName;
   if (status == VMK_OK) {
      CopyToHost(hostName, name, args.nameLength);
      CopyToHost(hostArgs, &args, sizeof(*hostArgs));
   }
   Mem_Free(name);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HostUserSetCosPid --
 *
 *	Sets the cos pid for this cartel (the cos pid is the pid of
 *	the proxy).
 *
 * Results:
 *      VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HostUserSetCosPid(VMnix_SetCosPidArgs *hostArgs)
{
   VMnix_SetCosPidArgs args;
   VMK_ReturnStatus status;
   World_Handle *world;

   CopyFromHost(&args, hostArgs, sizeof args);

   status = HostFindUserWorld(args.worldID, &world);
   if (status == VMK_OK) {
      status = UserProxy_SetCosProxyPid(world, args.cosPid);
      World_Release(world);
   }

   return status;
}

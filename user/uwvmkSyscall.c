/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * uwvmkSyscall.c --
 *
 *	UWVMK syscall support.  (That's UserWorld to VMKernel SYSCALLs).
 */

#include "user_int.h"
#include "uwvmkDispatch.h" /* Automatically generated from uwvmkSyscall.list */

#define LOGLEVEL_MODULE UWVMKSyscall
#include "userLog.h"
#include "timer.h"         /* for RateConv_Params */
#include "cpuid_info.h"    /* for CPUIDSummary */
#include "sharedArea.h"    /* for SharedArea_InitUser(), SharedArea_Alloc() */
#include "net.h"           /* for Net_DoOpenDevice(), etc */
#include "socket_dist.h"   /* for Net_TcpipStackLoaded */
#include "vmnix_syscall.h"
#include "kvmap.h"
#include "world.h"
#include "vmk_scsi.h"
#include "smp.h"           /* for hyperthreading, logical number of CPUs */
#include "fsSwitch.h"
#include "userFile.h"
#include "helper.h"
#include "vmkevent.h"
#include "migrateBridge.h"
#include "vscsi.h"
#include "cow.h"

extern CPUIDSummary cpuids[];

/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_Undefined
 *
 *      The undefined system call for the UWVMK entrypoints.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets frame->regs.eax to linux equivalent of
 *      VMK_UNDEFINED_SYSCALL.
 *
 *----------------------------------------------------------------------
 */
void
UWVMKSyscall_Undefined(VMKFullUserExcFrame* frame)
{
   UWWarn("Undefined UWVMK syscall");
   frame->regs.eax = VMK_UNDEFINED_SYSCALL;
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_GetSyscallVersion --
 *
 *	Return the kernel syscall version checksum in the given output
 *	parameter.
 *
 * Results:
 *	VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UWVMKSyscall_GetSyscallVersion(uint32* version)
{
   ASSERT(version);

   UWLOG_SyscallEnter("(...) -> %#x",
                      UWVMKSYSCALL_CHECKSUM);

   /* constant generated in uwvmkDispatch.h */
   *version = UWVMKSYSCALL_CHECKSUM;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_LockPage --
 *
 *	Lock the MPN for the given userVPN.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	*outMPN is set the MPN.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_LockPage(VPN userVPN,
                      MPN* outMPN)
{
   return User_GetPageMPN(MY_RUNNING_WORLD, userVPN, USER_PAGE_PINNED, outMPN);
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_ProbeMPN --
 *
 *	Translates the given VPN to a MPN.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	*outMPN is set the MPN.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ProbeMPN(VPN userVPN,
                      MPN* outMPN)
{
   return UserMem_Probe(MY_RUNNING_WORLD, userVPN, outMPN);
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_GetNextAnonPage --
 *
 *	Returns the next anonymous page.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	Sets *outMPN to a valid MPN if there is one, or INVALID_MPN if not.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetNextAnonPage(World_ID worldID,
                             MPN inMPN,
                             MPN* outMPN)
{
   return Alloc_GetNextAnonPage(worldID, inMPN, outMPN);
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_GetMPNContents --
 *
 *	Get a page of machine memory.
 *
 * Results:
 *	VMK_OK, or User_CopyOut error or VMK_BAD_PARAM if mpn is illegal.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetMPNContents(MPN mpn,
                            UserVA /* uint8* */ buf)
{
   VMK_ReturnStatus status;

   UWLOG(1, "(mpn=%#x, buf=%#x)", mpn, buf);

   if (VMK_IsValidMPN(mpn)) {
      uint8* kbuf;

      kbuf = KVMap_MapMPN(mpn, TLB_LOCALONLY);
      ASSERT(kbuf != NULL);

      status = User_CopyOut(buf, kbuf, PAGE_SIZE);

      KVMap_FreePages(kbuf);
   } else {
      UWLOG(0, "Invalid MPN %#x", mpn);
      status = VMK_BAD_PARAM;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_SetMPNContents --
 *
 *	Overwrite an aribtrary page of machine memory.
 *
 * Results:
 *	VMK_OK, or User_CopyOut or VMK_BAD_PARAM if mpn is illegal.
 *
 * Side effects:
 *	Memory is changed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_SetMPNContents(MPN mpn,
                            UserVA /* uint8* */ buf)
{
#ifdef DEBUG_STUB
   VMK_ReturnStatus status;

   UWLOG(0, "(mpn=%#x, buf=%#x)", mpn, buf);

   if (VMK_IsValidMPN(mpn)) {
      uint8* kbuf;

      kbuf = KVMap_MapMPN(mpn, TLB_LOCALONLY);
      ASSERT(kbuf != NULL);

      status = User_CopyIn(kbuf, buf, PAGE_SIZE);

      KVMap_FreePages(kbuf);
   } else {
      UWLOG(0, "Invalid MPN %#x", mpn);
      status = VMK_BAD_PARAM;
   }      

   return status;
#else
   return VMK_NOT_SUPPORTED;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_ReadVMKStack --
 *
 *	Reads the specified page of the vmkernel's stack and returns it
 *	in data.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	data is filled with, guess what, data.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ReadVMKStack(World_ID worldID,
                          int pageNum,
                          UserVA /* uint8* */ data,
			  VA* vaddr)
{
   VMK_ReturnStatus status;
   World_Handle* world;

   world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   status = World_GetVMKStackPage(world, pageNum, vaddr);
   if (status == VMK_OK) {
      status = User_CopyOut(data, (void*)*vaddr, PAGE_SIZE);
   }

   World_Release(world);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_BreakIntoDebugger --
 *
 *	Simply calls UserDebug_InDebuggerCheck.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	May block, may transmit some network traffic.  It pretty much depends on
 *	what happens in userDebug.c.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_BreakIntoDebugger(UserVA /* void* */ userFullFrame)
{
   User_ThreadInfo* uti = MY_RUNNING_WORLD->userThreadInfo;
   VMKFullUserExcFrame kFullFrame;
   VMK_ReturnStatus status;

   /*
    * Normally we shouldn't be copying data to where uti->exceptionFrame points.
    * However, this case warrants it because the current uti->exceptionFrame is
    * not correct.  This syscall only happens when we're in an interrupt context
    * and notice that we need to enter the debugger.  Thus we save the current
    * state and munge the registers and stack to initiate this syscall upon
    * return to userland.  Now we're here, but we need to restore the fullFrame
    * as it was when we originally entered the interrupt handler.  We know that
    * uti->exceptionFrame points to the frame pushed on the stack for the
    * exception, so we simply overwrite it with the frame we know is the correct
    * one.  Kids, don't try this at home.
    */
   ASSERT(uti->exceptionFrame != NULL);
      
   status = User_CopyIn(&kFullFrame, userFullFrame, sizeof kFullFrame);
   if (status == VMK_OK) {
      status = User_CleanFrameCopy(uti->exceptionFrame, &kFullFrame);
   }

   if (status != VMK_OK) {
      /*
       * This is a serious problem.  This should really never happen, but if it
       * does, all we can do is log the error and kill the cartel.
       */
      UWWarn("Debugger support cannot copy in userFullFrame, nuking cartel.");
      User_CartelShutdown(CARTEL_EXIT_SYSERR_BASE, FALSE, uti->exceptionFrame);
   }

   UserDebug_InDebuggerCheck();

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_LiveCoreDump --
 *
 *	Generate a coredump and continue execution
 *
 * Results:
 *	VMK_OK, error on out of memory or other problem.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_LiveCoreDump(UserVA /* char * */ coreFileName,
                          int coreFileNameLen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;

   if (coreFileNameLen < 1) {
      UWLOG(0, "file name length too small.");
      return VMK_BAD_PARAM;
   }

   status = UserDump_CoreDump();
   if (status == VMK_OK) {
      UWLOG(0, "dump file: %s", uci->coreDump.dumpName);
      if (coreFileNameLen > sizeof(uci->coreDump.dumpName)) {
         coreFileNameLen = sizeof(uci->coreDump.dumpName);
      }
      User_CopyOut(coreFileName, uci->coreDump.dumpName, coreFileNameLen);
      UserDump_ReleaseDumper();
   } else {
      UWLOG(0, "NO dump file: %s", UWLOG_ReturnStatusToString(status));
   }

   return status;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_RPCConnect --
 *
 *    Create a new connection under the provided name.
 *
 * Results:
 *    Return status from RPC_Register(), normally VMK_OK.
 *
 * Side effects:
 *    Returns a cnxID, set to -1 if it already exists.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_RPCConnect(const char *cnxName,
			int32 *cnxFD,
			int32 *cnxID)
{
   return UserVMKRpc_Create(MY_USER_CARTEL_INFO,
                            cnxName, cnxFD, cnxID);
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_RPCGetMsg --
 *
 *    Return next available message on the specified connection ID.
 *
 * Results:
 *    Return status from RPC_GetMsg(), normally VMK_OK.
 *    When no message is available, will block for "timeout"
 *    milliseconds (0 == infinite) if isBlocking is TRUE.
 *
 * Side effects:
 *    If available, a message is copied to the output buffer
 *    and the message length and function values are set.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_RPCGetMsg(int32 cnxFD,
                       UserVA /* RPC_MsgInfo * */ userMsgInfo,
                       uint32 timeout,
                       int32 isBlocking,
                       World_ID switchToWorldID)
{
   VMK_ReturnStatus status;
   RPC_Connection cnxID;
   uint32 flags = (isBlocking) ? RPC_CAN_BLOCK : 0;

   status = UserVMKRpc_GetIDForFD(MY_USER_CARTEL_INFO, cnxFD, &cnxID);
   if (status == VMK_OK) {
      status = RPC_GetMsg(cnxID, flags,
                          (RPC_MsgInfo *)userMsgInfo,
                          timeout, UTIL_USERWORLD_BUFFER,
                          switchToWorldID);
   }

   return status;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_RPCSendMsg --
 *
 *    Send a message on the specified connection ID.
 *
 * Results:
 *    Return status from RPC_Send(), normally VMK_OK.
 *
 * Side effects:
 *    A message is queued for the connection.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_RPCSendMsg(int32 cnxFD,
                        int32 rpcFunction,
                        UserVA /* char * */ msgBuf,
                        int32 msgBufLen)
{
   VMK_ReturnStatus status;
   RPC_Token token;
   RPC_Connection cnxID;

   status = UserVMKRpc_GetIDForFD(MY_USER_CARTEL_INFO, cnxFD, &cnxID);
   if (status == VMK_OK) {
      status =  RPC_Send(cnxID, rpcFunction, 0, (char *)msgBuf, msgBufLen,
                         UTIL_USERWORLD_BUFFER, &token);
   }

   return status;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_RPCReply --
 *
 *    Post a reply on the specified connection ID.
 *
 * Results:
 *    Return status from RPC_PostReply(), normally VMK_OK.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_RPCReply(int32 cnxFD,
                      int32 token)
{
   // XXX RPC_PostReply can't handle 0-length buffers.
   static const char msgBuf[sizeof(uint32)] = { 0 };
   VMK_ReturnStatus status;
   RPC_Connection cnxID;

   status = UserVMKRpc_GetIDForFD(MY_USER_CARTEL_INFO, cnxFD, &cnxID);
   if (status != VMK_OK) {
      return status;
   }

   return RPC_PostReply(cnxID, token, msgBuf,
                        sizeof(msgBuf), UTIL_VMKERNEL_BUFFER);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_CreateVCPU --
 *
 *    Create a guest VM.
 *
 * Results:
 *    Return status from World_New(), normally VMK_OK.
 *
 * Side effects:
 *    Creates a new guest VM world.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_CreateVCPU(VMnix_CreateWorldArgs *args,
                        SharedAreaDesc *sharedAreaDescs,
                        World_ID *worldID)
{
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   World_Handle *newWorld = NULL;
   const Bool creatingLeader = (args->flags & VMNIX_GROUP_LEADER);
   World_InitArgs worldArgs;
   
   if (creatingLeader
       && (World_GetVmmLeaderID(MY_RUNNING_WORLD) != INVALID_WORLD_ID)) {
      /*
       * Technically this is racy with other accesses to
       * vmm leader and the actual assignment below, but if we
       * have multiple threads racing to call CreateVM, something else
       * is seriously wrong with the VMX.
       */
      UWWarn("Tried to create leader VCPU, but we already have one (%d).",
             World_GetVmmLeaderID(MY_RUNNING_WORLD));
      return VMK_BUSY;
   }

   if ((!creatingLeader)
       && (World_GetVmmLeaderID(MY_RUNNING_WORLD) == INVALID_WORLD_ID)) {
      UWWarn("Trying to create follower VCPU without pre-exisiting leader.");
      return VMK_BAD_PARAM;
   }

   // setup the VMX overhead memory limit for the VM
   if (creatingLeader) {
      status = MemSched_SetUserOverhead(MY_RUNNING_WORLD, 
                                        args->sched.mem.numOverhead);
      if (status != VMK_OK) {
         return status;
      }
   }

   // override VMM world creation arguments
   args->flags &= ~VMNIX_GROUP_LEADER;
   args->groupLeader = World_GetGroupLeaderID(MY_RUNNING_WORLD);
   args->sharedAreaArgs.descs = sharedAreaDescs;

   World_ConfigVMMArgs(&worldArgs, args);
   status = World_New(&worldArgs, &newWorld);

   if (status == VMK_OK) {
      ASSERT(newWorld != NULL);
      World_Bind(newWorld->worldID);
      *worldID = newWorld->worldID;
      ASSERT(!creatingLeader || World_GetVmmLeaderID(MY_RUNNING_WORLD) == *worldID);
   } else {
      *worldID = INVALID_WORLD_ID;
      return status;
   }

   /*
    * If the config file has specified joint affinity the user worlds
    * are given the same affinity as the vmm worlds. 
    */
   if (creatingLeader) {
      CpuMask affinMask;
      Bool updateAffinity = TRUE;
      int i;

      UWLOG(1, "adding world leader: %u", newWorld->worldID);
      // compute the affinity mask to apply to userworlds
      affinMask = args->sched.cpu.vcpuAffinity[0];
      for (i=0; i < args->sched.cpu.numVcpus; i++) {
         if (args->sched.cpu.vcpuAffinity[0] != affinMask) {
            // only apply the mask if we have joint affinity
            updateAffinity = FALSE;
            break;
         }
      }

      // give userworlds the same affinity as vmm worlds,
      // if the vmm has joint affinity
      if (updateAffinity) {
         SP_Lock(&uci->peers.lock);
         for (i=0; i < ARRAYSIZE(uci->peers.activePeers); i++) {
            World_Handle *w;

            if (uci->peers.activePeers[i] == INVALID_WORLD_ID
                || (w = World_Find(uci->peers.activePeers[i])) == NULL) {
               // ignore empty slots and not-found worlds
               continue;
            }

            // give userworlds the same affinity as vmm worlds,
            // if the vmm has joint affinity
            if (updateAffinity) {
               CpuSched_WorldSetAffinity(uci->peers.activePeers[i], affinMask);
            }

            World_Release(w);
         }
         SP_Unlock(&uci->peers.lock);
      }
   } else {
      LOG(0, "vmmLeader=%u, thisWorld=%u", newWorld->worldID, MY_RUNNING_WORLD->worldID);
   }

   return status;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_BindVCPU --
 *
 *    Bind to given VCPU's VMM.
 *
 * Results:
 *    Return status from World_Bind(), normally VMK_OK.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_BindVCPU(World_ID groupLeaderID,
                      int32 vcpuID,
                      World_ID *worldID)
{
   VMK_ReturnStatus status;
   World_Handle *vmmLeader = User_FindVmmLeader(MY_RUNNING_WORLD);

   /* Make sure VMM has been created. */
   if (vmmLeader == NULL) {
      UWWarn("vmm doesn't exist");
      return VMK_NOT_FOUND;
   }

   /* Just use given groupLeaderID to double-check the "correct" value. */
   if (groupLeaderID != vmmLeader->worldID) {
      UWWarn("Caller passed %d as leader, but should have passed %d.  Ignoring",
             groupLeaderID, vmmLeader->worldID);
      World_Release(vmmLeader);
      return VMK_BAD_PARAM;
   }

   ASSERT(World_IsVMMWorld(vmmLeader));
   if (vcpuID < World_VMMGroup(vmmLeader)->memberCount) {
      *worldID = World_VMMGroup(vmmLeader)->members[vcpuID];
      status = World_Bind(World_VMMGroup(vmmLeader)->members[vcpuID]);
   } else {
      UWWarn("bad vcpuid: %d (leader %d, %d members)\n",
             vcpuID, vmmLeader->worldID, World_VMMGroup(vmmLeader)->memberCount);
      World_Release(vmmLeader);
      status = VMK_BAD_PARAM;
   }

   World_Release(vmmLeader);
   return status;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_ReleaseAllVCPUs --
 *
 *    Release all VCPUs associated with current UserWorld.
 *
 * Results:
 *    VMK_OK if VMM group was destroyed, error if no associated VMM or
 *    it cannot be destroyed.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ReleaseAllVCPUs(void)
{
   VMK_ReturnStatus status = World_DestroyVmms(MY_RUNNING_WORLD, TRUE, FALSE);
   if (status != VMK_OK) {
      UWWarn("vmm doesn't exist %s", UWLOG_ReturnStatusToString(status));
   }
   return status;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_GetCPUkhzEstimate --
 *
 *    Return the estimated CPU speed in kHz.
 *
 * Results:
 *    Always returns VMK_OK.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetCPUkhzEstimate(uint32 *cpukHzEstimate)
{
   *cpukHzEstimate = cpuKhzEstimate;

   return VMK_OK;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_GetNumCPUsUsed --
 *
 *    Return the number of CPUs used by the vmkernel.
 *
 * Results:
 *    Always returns VMK_OK.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetNumCPUsUsed(uint32 *numCPUs)
{
   *numCPUs = numPCPUs;

   return VMK_OK;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_GetNumLogicalCPUsPerPackage --
 *
 *    Return the number of logical CPUs (hyperthreads) per physical
 *    processor package or 1 on a non-hyperthreaded system.
 *
 * Results:
 *    Always returns VMK_OK.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetNumLogicalCPUsPerPackage(uint8 *numLogicalCPUsPerPackage)
{
   *numLogicalCPUsPerPackage = SMP_LogicalCPUPerPackage();

   return VMK_OK;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_GetCPUIDs --
 *
 *    Return CPU ID information for all used physical CPUs.
 *
 * Results:
 *    If bufLen size is correct, returns status from User_CopyOut()
 *    otherwise, returns VMK_BAD_PARAM.
 *
 * Side effects:
 *    none
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_GetCPUIDs(UserVA /* CPUIDSummary * */ outCpuIDs,
                       uint32 bufLen)
{
   if (bufLen != (sizeof(CPUIDSummary) * numPCPUs)) {
      return VMK_BAD_PARAM;
   }

   return User_CopyOut(outCpuIDs, cpuids, bufLen);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_SetVMXInfo --
 *
 *    Cache vmx specific info in the vmkernel for easier debugging.
 *
 * Results:
 *    Return status from World_SetVMXInfoWork(), normally VMK_OK.
 *
 * Side effects:
 *    Userworld vmx info cached in the vmkernel for later debugging.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_SetVMXInfo(const char *cfgPath,
                        const char *uuidString,
                        const char *displayName)
{
   VMK_ReturnStatus status;
   World_Handle *vmmLeader = User_FindVmmLeader(MY_RUNNING_WORLD);
   if (vmmLeader == NULL) {
      UWWarn("vmm doesn't exist");
      return VMK_NOT_FOUND;
   }
   status = World_SetVMXInfoWork(vmmLeader->worldID, -1, cfgPath,
                                 uuidString, displayName);

   World_Release(vmmLeader);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_SetUID --
 *
 *      VMX86_DEVEL hack to avoid true setuid.
 *
 * Results:
 *      TRUE.
 *
 * Side effects:
 *      The process becomes setuid, but currently with effective UID
 *	the same as real UID.  That is, the process's saved uid (suid)
 *      is set to 0, but its other uids are unchanged.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_SetUID()
{
#ifdef VMX86_DEVEL
   MY_RUNNING_WORLD->ident.suid = 0;
   return VMK_OK;
#else
   return VMK_NO_PERMISSION;
#endif
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_MemTestMap --
 *
 *    Map the memtest mmap region.
 *
 * Results:
 *    Return status from UserMem_MemTestMap(),
 *    normally VMK_OK.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_MemTestMap(UserVA /* MPN * */ mpnAddr,
                        UserVA /* uint32 * */ numPagesAddr,
                        UserVA /* void ** */ addr)
{
   return UserMem_MemTestMap(MY_RUNNING_WORLD, mpnAddr, numPagesAddr, addr);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_SetupPhysMemMap --
 *
 *    Map the monitor's physical memory.
 *
 * Results:
 *    Return status from UserMem_SetupPhysMemMap(),
 *    normally VMK_OK.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_SetupPhysMemMap(PPN startPPN,
			     uint32 length,
                             UserVA /* void ** */ addr)
{
   return UserMem_SetupPhysMemMap(MY_RUNNING_WORLD, startPPN, length, addr);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_ReleasePhysMemMap --
 *
 *    Unmap the monitor's physical memory.
 *
 * Results:
 *    Return status from UserMem_ReleasePhysMemMap(),
 *    normally VMK_OK.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ReleasePhysMemMap(UserVA /* void* */ vaddr,
			       uint32 length)
{
   return UserMem_Unmap(MY_RUNNING_WORLD, vaddr, length);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_AsyncCheckActions --
 *
 *    Wakeup/interrupt the world to check for monitor actions.
 *
 * Results:
 *    Return value from CpuSched_AsyncCheckActionsByID().
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_AsyncCheckActions(World_ID worldID)
{
   return CpuSched_AsyncCheckActionsByID(worldID);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_AddPage --
 *
 *	Map the mpn into the given world's page table at vpn.
 *
 * Results:
 *	Return value from World_AddPage(), normally VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_AddPage(int32 vcpuID,
                     VPN vpn,
                     MPN mpn,
                     int32 readOnly)
{
   int32 worldID;
   VMK_ReturnStatus status;
   World_Handle *vmmLeader = User_FindVmmLeader(MY_RUNNING_WORLD);

   if (vmmLeader) {
      if (vcpuID < World_VMMGroup(vmmLeader)->memberCount) {
         worldID = World_VMMGroup(vmmLeader)->members[vcpuID];
         status = World_AddPage(worldID, vpn, mpn, readOnly);
      } else {
         VmWarn(vmmLeader->worldID, "bad vcpuid: %d\n", vcpuID);
         status = VMK_BAD_PARAM;
      }

      World_Release(vmmLeader);
   } else {
      UWWarn("vmm doesn't exist");
      status = VMK_NOT_FOUND;
   }

   return status;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_ReadPage --
 *
 *	Returns the data from the specified page.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ReadPage(World_ID worldID,
		      VPN vpn,
		      UserVA /* uint8* */ data)
{
   VMK_ReturnStatus status;
   World_Handle* world;
   MPN mpn;

   world = World_Find(worldID);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   status = World_VPN2MPN(world, vpn, &mpn);
   if (status == VMK_OK) {
      if (mpn == INVALID_MPN) {
	 status = User_CopyOut(data, zeroPage, PAGE_SIZE);
      } else {
	 void* mappedData = KVMap_MapMPN(mpn, TLB_LOCALONLY);
	 ASSERT(mappedData != NULL);

	 status = User_CopyOut(data, mappedData, PAGE_SIZE);

	 KVMap_FreePages(mappedData);
      }
   }

   World_Release(world);

   return status;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_ReadRegs --
 *
 *	Returns the current register state for the specified world.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_ReadRegs(World_ID worldID,
		      VMnix_ReadRegsResult* result)
{
   return World_ReadRegs(worldID, result);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_RunWorld --
 *
 *	Makes the given VMM world runnable.  This is called by the VMX thread
 *	that corresponds to the particular VCPU that the given vmmWorld is
 *	going to run.  So, we use this information to link up the vmmWorld
 *	to the matching userworld.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_RunWorld(World_ID worldID,
                      VMnix_Entry start)
{
   // local copy of sched
   World_Handle *world;

   world = World_Find(worldID);
   if (world == NULL) { 
      WarnVmNotFound(worldID);
      return (VMK_NOT_FOUND);
   }

   if (!World_IsVMMWorld(world)) {
      World_Release(world);
      UWWarn("%d is not a vmm world", worldID);
      return (VMK_BAD_PARAM);
   }

   World_VMM(world)->vmxThreadID = MY_RUNNING_WORLD->worldID;

   World_MakeRunnable(worldID, start);

   World_Release(world);
   return VMK_OK;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_NetConnect --
 *
 *    Connect to a virtual network.
 *
 * Results:
 *    Return value from Net_Connect(), normally VMK_OK.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_NetConnect(World_ID worldID,
                        const char *name,
                        Net_PortID *portID)
{
   return Net_Connect(worldID, name, portID);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_NetDisconnect --
 *
 *    Disconnect from a virtual network.
 *
 * Results:
 *    Return value from Net_Disconnect(), normally VMK_OK.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_NetDisconnect(World_ID worldID,
                           Net_PortID portID)
{
   return Net_Disconnect(worldID, portID);
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_UsingVMKTcpIpStack --
 *
 *	Returns whether this UserWorld is using the VMkernel TCP/IP
 *	stack.
 *
 * Results:
 *	See UserSocket_UsingVMKTcpIpStack.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_UsingVMKTcpIpStack(void)
{
   return UserSocket_UsingVMKTcpIpStack(MY_USER_CARTEL_INFO);
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_DelaySCSICmds --
 *
 *      Set the mininum delay for SCSI command.  See PR 19244
 *      delay is specified in microseconds
 *
 * Results:
 *      VMK_OK.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_DelaySCSICmds(uint32 delay)
{
   World_Handle *vmmLeader = User_FindVmmLeader(MY_RUNNING_WORLD);
   if (vmmLeader == NULL) {
      UWWarn("vmm doesn't exist");
      return VMK_NOT_FOUND;
   }

   ASSERT(World_IsVMMWorld(vmmLeader));

   World_VMMGroup(vmmLeader)->delaySCSICmdsUsec = delay;

   World_Release(vmmLeader);
   return VMK_OK;
}

/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_PhysMemIO --
 *
 *	Request that the physical memory of the current VM be
 *	read/written to the VMFS file specified by fd at the specified
 *	offset.  More efficient than accessing the data via the VMX's
 *	mapping of the physical memory, since the vmkernel is actually
 *	managing the physical memory.
 *
 * Results:
 *      VMK_OK or error.
 *
 *---------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_PhysMemIO(LinuxFd fd, uint32 offsetHi, uint32 offsetLo,
                       int startPercent, int endPercent, Bool read)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   UserObj* obj;
   VMnix_FilePhysMemIOArgs args;
   uint64 offset = ((uint64) offsetHi << 32) | offsetLo;
   
   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return status;
   }
   if (obj->type != USEROBJ_TYPE_FILE) {
      (void) UserObj_Release(uci, obj);
      return VMK_BAD_PARAM;
   }
   if ((read && !UserObj_IsOpenForRead(obj)) ||
       (!read && !UserObj_IsOpenForWrite(obj))) {
      (void) UserObj_Release(uci, obj);
      return VMK_INVALID_HANDLE;
   }

   Semaphore_Lock(&obj->sema);
   obj->methods->fsync(obj, TRUE);

   args.worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   args.handleID = obj->data.vmfsObject->handle;
   args.offset = offset;
   args.startPercent = startPercent;
   args.endPercent = endPercent;
   args.read = read;
   status = Alloc_PhysMemIO(&args);

   Semaphore_Unlock(&obj->sema);

   (void) UserObj_Release(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UWVMKSyscall_MarkCheckpoint
 *
 *	Mark the start/end of a checkpoint.  If wakeup is TRUE, mark
 *	the very beginning of the checkpoint process, and wake up the
 *	monitor from a memory wait, if necessary.  If start is TRUE,
 *	this is the start of the saving part of the checkpoint, else
 *	this is the end of the checkpoint process.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_MarkCheckpoint(Bool wakeup, Bool start)
{
   VMnix_MarkCheckpointArgs args;
   
   args.worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   args.wakeup = wakeup;
   args.start = start;
   Migrate_MarkCheckpoint(&args);
   return Alloc_MarkCheckpoint(args.worldID, args.wakeup, args.start);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_CheckpointCleanup --
 *
 *      Informs the vmkernel that checkpoint is aborted.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_CheckpointCleanup(void)
{
   World_Handle *vmmLeader = User_FindVmmLeader(MY_RUNNING_WORLD);
   if (vmmLeader == NULL) {
      UWWarn("vmm doesn't exist");
      return VMK_NOT_FOUND;
   }

   Alloc_CheckpointCleanup(vmmLeader);
   World_Release(vmmLeader);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_SaveMemory --
 *
 *      Saves a reference to the world so that its memory won't get
 *      cleaned up until the destination has paged in all changed pages.
 *      (or a timeout / error occurs).
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
UWVMKSyscall_SaveMemory(void)
{
   return Migrate_SaveMemory(World_GetVmmLeaderID(MY_RUNNING_WORLD));
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_MigrateWriteCptData --
 *
 *	Write to the migrate data file.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_MigrateWriteCptData(int offset, UserVAConst data,
                                 int size, Bool completed)
{
   VMnix_MigCptDataArgs args;

   args.offset = offset;
   args.data = (void *) data;
   args.size = size;
   args.completed = completed;
   args.worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);

   return Migrate_WriteCptData(&args, UTIL_USERWORLD_BUFFER);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_MigratePollForData --
 *
 *      Ask the vmkernel if it is ready to handle requests to read checkpoint
 *      data.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      Begin migration to this machine.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_MigratePollForData(UserVAConst data)
{
   VMK_ReturnStatus status, status1;
   VMnix_MigrateProgressResult progress;

   status = Migrate_ToBegin(World_GetVmmLeaderID(MY_RUNNING_WORLD), &progress);
   if (status == VMK_OK || status == VMK_NOT_FOUND ||
       status == VMK_STATUS_PENDING) {
      status1 = User_CopyOut(data, &progress, sizeof(progress));
      if (status1 != VMK_OK) {
	 status = status1;
      }
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_MigrateReadCptData --
 *
 *	Read from the migrate data file.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      If VMK_OK is returned, *sizeOut is set to the number of bytes
 *      read.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_MigrateReadCptData(int offset, UserVA data, int size,
                                int *sizeOut)
{
   VMnix_MigCptDataArgs args;
   VMK_ReturnStatus status;

   args.offset = offset;
   args.data = (void *) data;
   args.size = size;
   args.worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);

   status = Migrate_ReadCptData(&args, UTIL_USERWORLD_BUFFER);
   *sizeOut = args.size;
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_InitMigration --
 *
 *	Called by both source & destination to inform the vmkernel of
 *	migration state.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_InitMigration(uint32 tsHi, uint32 tsLo,
                           uint32 srcIpAddr, uint32 destIpAddr,
                           World_ID destWorldID, Bool grabResources)
{
   VMnix_MigrationArgs args;

   args.ts = ((uint64) tsHi << 32) | tsLo;
   args.srcIpAddr = srcIpAddr;
   args.destIpAddr = destIpAddr;
   args.worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   args.destWorldID = destWorldID;
   args.grabResources = grabResources;

   return Migrate_SetParameters(&args);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_Inb --
 *
 *	Low level get byte from port.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_Inb(uint32 port, uint8 *value)
{
   *value = INB(port);
   
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_Outb --
 *
 *	Low level output byte to port.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_Outb(uint32 port, uint8 value)
{
   OUTB(port, value);

   return VMK_OK;
}


/*
 *---------------------------------------------------------------------
 *
 * UWVMKSyscall_SysAlert --
 *
 *    Generates System Alert message in the vmkernel log.
 *
 * Results:
 *    Always returns VMK_OK, since SysAlert() call in the vmkernel 
 *    returns void.
 *
 * Side effects:
 *    prints the System Alert message in the vmkernel log
 *
 *---------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_SysAlert(const char  *msg)
{
   SysAlert("%s",msg);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_IsCosPidAlive --
 *
 *	Calls the proxy to check if the current cos pid is alive.
 *
 * Results:
 *      VMK_OK if alive, VMK_NOT_FOUND otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_IsCosPidAlive(int cosPid)
{
   ASSERT(MY_RUNNING_WORLD && MY_RUNNING_WORLD->userCartelInfo);
   return UserProxy_IsCosPidAlive(MY_RUNNING_WORLD->userCartelInfo, cosPid);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UWVMKSyscall_GetCosProxyPid --
 *
 *	Returns the pid of the COS proxy for this cartel.
 *
 * Results:
 *	VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
UWVMKSyscall_GetCosProxyPid(int *cosPid)
{
   ASSERT(MY_RUNNING_WORLD && MY_RUNNING_WORLD->userCartelInfo);
   *cosPid = UserProxy_GetCosProxyPid(MY_RUNNING_WORLD->userCartelInfo);
   return VMK_OK;
}

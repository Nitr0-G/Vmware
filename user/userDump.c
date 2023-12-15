/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userDump.c --
 *
 *	Userworld core dumper.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "user_int.h"
#include "dump_ext.h"
#include "util.h"
#include "memmap.h"
#include "userDump.h"
#include "compress.h"
#include "kvmap.h"

#define LOGLEVEL_MODULE		UserDump
#include "userLog.h"

#define USERDUMP_MAX_INDEX	512

/*
 * Structure used for Heap_Dump callback.
 */
typedef struct UserDumpHeapData {
   UserDump_DumpData *dumpData;
   int numHeadersWritten;
   int numRegionsWritten;
} UserDumpHeapData;

/*
 * User coredump file layout:
 *
 *                                          0 +------------+
 *                                  PAGE_SIZE |            | Dump_Info metadata
 *                                            +------------+
 *                                            |            | object types
 *                                            +------------+
 *                                            |            | mmap types
 *                                            +------------+
 *                                            |            | pointer table
 *                                            +------------+
 *       # fd objs * sizeof(UserDump_FdEntry) |            | objects in fd table
 *                                            +------------+
 *     # mmap objs * sizeof(UserDump_FdEntry) |            | mmap-only objects
 *                                            +------------+
 *          # worlds * sizeof(Dump_WorldData) |            | Registers
 *                                            +------------+
 *  # mmap'ed regions * sizeof(Dump_MMapInfo) |            | mmap metadata
 *                                            +------------+
 *     (depends on how much stuff is mmap'ed) |            | mmap'ed regions
 *                                            +------------+
 *  # heap regions*sizeof(UserDump_HeapRange) |            | cartel heap metadata
 *                                            +------------+
 *         (depends on how large the heap is) |            | cartel heap regions
 *                                            +------------+
 *
 * The userworld coredump process is pretty straightforward: each section of the
 * address space is dumped in turn, with its location and length saved in the
 * metadata section.
 *
 * The core dump obviously relies on the address space layout of userworlds.
 * Check out vmkernel/public/user_layout.h for more info.
 */

/*
 *----------------------------------------------------------------------
 *
 * UserDump_CartelInit --
 *
 *	Initialize cartel core dump states.
 *
 * Results:
 *	VMK_OK on success.
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_CartelInit(User_CartelInfo *uci)
{
   uci->coreDump.header = User_HeapAlloc(uci, sizeof *uci->coreDump.header);
   if (uci->coreDump.header == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(uci->coreDump.dumpName, 0, sizeof uci->coreDump.dumpName);
   memset(uci->coreDump.header, 0, sizeof *uci->coreDump.header);
   SP_InitLock("User_DumpLock", &uci->coreDump.dumpLock, UW_SP_RANK_DUMP);
   uci->coreDump.dumperWorld = INVALID_WORLD_ID;
   uci->coreDump.inProgress = FALSE;
   uci->coreDump.enabled = TRUE;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserDump_CartelCleanup --
 *
 *	Cleanup cartel core dump states.
 *
 * Results:
 *	VMK_OK on success.
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_CartelCleanup(User_CartelInfo *uci)
{
   uci->coreDump.enabled = FALSE;
   uci->coreDump.dumperWorld = INVALID_WORLD_ID;
   User_HeapFree(uci, uci->coreDump.header);
   SP_CleanupLock(&uci->coreDump.dumpLock);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_SetExecName --
 *
 *	Sets the name of the executable that's running so that we'll
 *	have it if we need to dump.
 *
 * Results:
 *	VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_SetExecName(World_Handle *world, char *execName)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserDump_Header *header;

   ASSERT(World_IsUSERWorld(world));
   ASSERT(uci != NULL);

   header = uci->coreDump.header;

   if (header->executableName[0] != 0) {
      UWWarn("Executable name already set ('%s').  Replacing with '%s'",
	     header->executableName, execName);
   }

   UWLOG(1, "Setting executable name to '%s'", execName);
   snprintf(header->executableName, sizeof header->executableName,
	    "%s", execName);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpCompressAlloc --
 *
 *      Allocate memory for compression dictionary.  We do this by
 *	calling Mem_Alloc to allocate from the main heap, as we max out
 *	even the growable heap.
 *
 * Results:
 *	A pointer to the memory allocated or NULL if we failed to
 *	allocate memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
UserDumpCompressAlloc(UNUSED_PARAM(void *opaque), uint32 items, uint32 size)
{
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   void *ptr;

   ptr = Mem_Alloc(items * size);
   if (ptr == NULL) {
      SysAlert("out of dictionary memory while dumping cartel %d",
	       uci->cartelID);
   }

   return ptr;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpCompressFree --
 *
 *      Called to free compression dictionary memory.  Just call
 *	Mem_Free.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
UserDumpCompressFree(UNUSED_PARAM(void *opaque), void *ptr)
{
   Mem_Free(ptr);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpCompressOutputFn --
 *
 *      Write the compressed data to the specified object.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpCompressOutputFn(void *arg, Bool partial)
{
   VMK_ReturnStatus status;
   UserDump_DumpData *dumpData = (UserDump_DumpData*)arg;
   UserObj *obj = dumpData->obj;
   uint32 bytesWritten;

   Semaphore_Lock(&obj->sema);
   status = UserObj_WriteMPN(obj, dumpData->mpn, obj->offset, &bytesWritten);
   if (!partial) {
      obj->offset += bytesWritten;
   }
   Semaphore_Unlock(&obj->sema);

   if (status == VMK_OK && !partial && bytesWritten < PAGE_SIZE) {
      UWLOG(0, "Wrote out less bytes (%d) than expected (%d).", bytesWritten,
	    PAGE_SIZE);
      return VMK_WRITE_ERROR;
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_Write --
 *
 *	Writes data on given mpn to the given UserObj.
 *
 * Results:
 *	VMK_OK on success.
 *
 * Side effects:
 *	Bytes are written to *obj, updates obj->offset.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_Write(UserDump_DumpData *dumpData, uint8* buffer, int length)
{
   return Compress_AppendData(&dumpData->compressContext, buffer, length);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_Seek --
 *
 *	Move the file position as specified.
 *
 * Results:
 *	New offset.
 *
 * Side effects:
 *	May change obj->offset.
 *
 *----------------------------------------------------------------------
 */
uint32
UserDump_Seek(UserObj* obj, int32 offset, int whence)
{
   uint32 pos;

   Semaphore_Lock(&obj->sema);

   switch(whence) {
      case USEROBJ_SEEK_SET:
	 obj->offset = offset;
	 break;
      case USEROBJ_SEEK_CUR:
	 obj->offset += offset;
	 break;
      default:
         /*
	  * Since this function is used only by dumper code, it's ok to Panic
	  * here.
	  */
	 Panic("UserDump_Seek: Invalid argument.\n");
   }

   pos = (uint32)obj->offset;
   Semaphore_Unlock(&obj->sema);

   return pos;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_WriteUserRange --
 *
 *	Writes a range of userspace addresses to the given UserObj.
 *
 * Results:
 *	VMK_OK on success, or apprpriate error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_WriteUserRange(World_Handle *world, UserDump_DumpData *dumpData,
			UserVA startVA, UserVA endVA)
{
   VMK_ReturnStatus status;
   UserVA curVA;

   ASSERT(startVA % PAGE_SIZE == 0);

   for (curVA = startVA; curVA < endVA; curVA += PAGE_SIZE) {
      MPN mpn;
      status = UserMem_Probe(world, VA_2_VPN(curVA), &mpn);

      if (mpn != INVALID_MPN) {
         uint8 *page;

         UWLOG(2, "va: %#x  la: %#x  mpn: %#x  offset: %#x", curVA,
	       LPN_2_LA(VMK_USER_VPN_2_LPN(VA_2_VPN(curVA))), mpn,
	       (uint32)dumpData->obj->offset);

	 page = KVMap_MapMPN(mpn, TLB_LOCALONLY);
	 if (page == NULL) {
	    return VMK_NO_MEMORY;
	 }

	 status = UserDump_Write(dumpData, page, PAGE_SIZE);

	 KVMap_FreePages(page);

	 if (status != VMK_OK) {
	    return status;
	 }
      } else {
         UWLOG(2, "va: %#x  la: %#x  mpn: INVALID_MPN", curVA,
	       LPN_2_LA(VMK_USER_VPN_2_LPN(VA_2_VPN(curVA))));

	 status = UserDump_Write(dumpData, zeroPage, PAGE_SIZE);
	 if (status != VMK_OK) {
	    return status;
	 }
      }
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_WaitForDumper --
 *
 *	Deschedules the current world until the dumper finishes.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
UserDump_WaitForDumper(void)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   UWLOG(1, "world waiting for coredump...");
   /*
    * Uninterruptible wait.  Core dump should finish quickly, then this
    * thread will be released.
    */
   while (UserDump_DumpInProgress()) {
      CpuSched_Wait((uint32)&uci->coreDump.inProgress,
                    CPUSCHED_WAIT_UW_DEBUGGER,
		    NULL);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_DumpInProgress --
 *
 *	Returns whether we're dumping or not.
 *
 * Results:
 *	Returns uci->inCoreDump.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserDump_DumpInProgress(void)
{
   return MY_USER_CARTEL_INFO->coreDump.inProgress;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpGetCoreFile --
 *
 *	Tries to find a suitable name for this coredump.  Opens the
 *	file, too.
 *
 * Results:
 *	VMK_OK on success.
 *
 * Side effects:
 *	A new file may be created, *obj and *fileName are updated.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpGetCoreFile(UserObj** obj, char* fileName, int maxNameLen)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   int openErr = 0;
   int nameStart;
   char *pathStart;
   int i;
   UserObj* cwd;

   ASSERT(fileName);

   /*
    * Put cwd into fileName.
    */
   cwd = UserObj_AcquireCwd(uci);
   status = UserObj_GetDirName(uci, cwd, fileName, maxNameLen, &pathStart);
   (void) UserObj_Release(uci, cwd);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to determine current working directory: %s",
            UWLOG_ReturnStatusToString(status));
      UWLOG(0, "Defaulting to / for core dump directory.");
      fileName[0] = '\0'; // will prefix name with '/'
      pathStart = fileName;
   }

   /*
    * Move name in fileName to beginning of fileName buffer.  Do this
    * by hand because the source and destination overlap.
    */
   if (pathStart != fileName) {
      char* dest = fileName;
      while ((*pathStart != '\0')
          && (pathStart < (fileName + maxNameLen))) {
         *dest++ = *pathStart++;
      }
      *dest = '\0';
   }
      
   nameStart = strlen(fileName);

   for (i = 0; i < USERDUMP_MAX_INDEX; i++) {
      int rc;

      // Make a full path to the core dump name
      // If dump name is updated or changed, update apps/scripts/vm-support, too
      rc = snprintf(fileName + nameStart, maxNameLen - nameStart,
		    "/%s-zdump.%d", MY_RUNNING_WORLD->worldName, i);
      if (rc > maxNameLen - nameStart) {
         UWLOG(0, "Overflowed name buffer (needed %d chars).  Cannot name core file.\n",
               nameStart + rc);
         return VMK_NAME_TOO_LONG;
      }

      UWLOG(2, "Trying %s", fileName);

      // Try to create the core dump, leave existing dumps alone
      status = UserObj_Open(uci, fileName, USEROBJ_OPEN_CREATE |
      			    USEROBJ_OPEN_EXCLUSIVE | USEROBJ_OPEN_WRONLY,
			    0400, obj);
      if (status == VMK_EXISTS ||
	  status == VMK_WrapLinuxError(LINUX_EEXIST)) {
         UWLOG(0, "%s already exists.  Trying again.", fileName);
	 continue;
      } else if (status == VMK_LIMIT_EXCEEDED ||
		 status == VMK_WrapLinuxError(LINUX_ENOSPC)) {
         UWLOG(0, "No space left for core file.");
         return status;
      } else if (status != VMK_OK) {
         openErr++;
	 UWLOG(0, "UserObj_Open(%s) returned %s", fileName,
               UWLOG_ReturnStatusToString(status));
         if (openErr > 3) {
            UWLOG(0, "Giving up.");
            return status;
         } else {
            // Hope the problem was temporary or related to the file name
            continue;
         }
      }

      break;
   }

   if (i == USERDUMP_MAX_INDEX) {
      UWLOG(0, "Unable to find available coredump name.");
      return VMK_LIMIT_EXCEEDED;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpGetPeerList --
 *
 *	Returns a list of World_Handles for all threads in the cartel.
 *	Each World_Handle is left with its readerCount up'ed so that
 *	the world won't disappear from under us.
 *
 * Results:
 *	The number of threads in the cartel.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
UserDumpGetPeerList(World_Handle **peerList, int size)
{
   World_ID peerIds[USER_MAX_ACTIVE_PEERS];
   int numPeers;
   int i, j;

   ASSERT(size == USER_MAX_ACTIVE_PEERS);

   numPeers = UserThread_GetPeersDebug(peerIds);
   ASSERT(numPeers > 0);
   ASSERT(numPeers <= USER_MAX_ACTIVE_PEERS);

   for (i = 0, j = 0; i < numPeers; i++) {
      World_Handle *world = World_Find(peerIds[i]);
      if (world == NULL) {
         continue;
      }

      ASSERT(World_IsUSERWorld(world));

      peerList[j++] = world;
      // Keep the readerCount up on the world so it won't go away.
   }

   ASSERT(j > 0);
   return j;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpFreePeerList --
 *
 *	Calls World_Release on all the worlds in the given list.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
UserDumpFreePeerList(World_Handle **peerList, int numPeers)
{
   int i;

   for (i = 0; i < numPeers; i++) {
      World_Release(peerList[i]);
      peerList[i] = NULL;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpPointerTable --
 *
 *	Dumps a table containing important UserWorld pointers.  These
 *	pointers include uci, as well as all uti's for the cartel.
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
UserDumpPointerTable(UserDump_Header *header, UserDump_DumpData *dumpData,
		     World_Handle **peerList, int numPeers)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserDump_PtrTable ptrTable;
   UserDump_Thread threadList[USER_MAX_ACTIVE_PEERS];
   int i;

   ptrTable.userCartelInfo = (uint32)uci;
   ptrTable.worldGroup = (uint32)MY_RUNNING_WORLD->group;
   ptrTable.numThreads = numPeers;

   /*
    * For each world in the cartel, record its world id and userThreadInfo
    * pointer.
    */
   for (i = 0; i < numPeers; i++) {
      threadList[i].worldID = peerList[i]->worldID;
      threadList[i].uti = (uint32)peerList[i]->userThreadInfo;
   }

   status = UserDump_Write(dumpData, (uint8*)&ptrTable, sizeof ptrTable);
   if (status != VMK_OK) {
      return status;
   }

   status = UserDump_Write(dumpData, (uint8*)threadList,
			   ptrTable.numThreads * sizeof *threadList);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpSetRegisterData --
 *
 *	Copies the register data for a given world id to the given
 *	Dump_WorldData struct.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the world isn't found,
 *	or error from UserDump_Write.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpSetRegisterData(UserDump_DumpData *dumpData, World_Handle *world)
{
   User_ThreadInfo *uti = world->userThreadInfo;
   Dump_WorldData worldData;
   VMK_ReturnStatus status;

   memset(&worldData, 0, sizeof worldData);

   /*
    * Copy the registers to the coredump format.
    */
   worldData.id = world->worldID;
   snprintf(worldData.name, DUMP_NAME_LENGTH, "%s", world->worldName);

   if (uti->exceptionFrame != NULL) {
      worldData.signal = uti->exceptionFrame->frame.errorCode;

      worldData.regs.eax = uti->exceptionFrame->regs.eax;
      worldData.regs.ecx = uti->exceptionFrame->regs.ecx;
      worldData.regs.edx = uti->exceptionFrame->regs.edx;
      worldData.regs.ebx = uti->exceptionFrame->regs.ebx;
      worldData.regs.esp = uti->exceptionFrame->frame.esp;
      worldData.regs.ebp = uti->exceptionFrame->regs.ebp;
      worldData.regs.esi = uti->exceptionFrame->regs.esi;
      worldData.regs.edi = uti->exceptionFrame->regs.edi;

      worldData.regs.eip = uti->exceptionFrame->frame.eip;
      worldData.regs.eflags = uti->exceptionFrame->frame.eflags;

      worldData.regs.cs = uti->exceptionFrame->frame.cs;
      worldData.regs.ss = uti->exceptionFrame->frame.ss;
      worldData.regs.ds = uti->exceptionFrame->regs.ds;
      worldData.regs.es = uti->exceptionFrame->regs.es;
      worldData.regs.fs = uti->exceptionFrame->regs.fs;
      worldData.regs.gs = uti->exceptionFrame->regs.gs;
   } else {
      /*
       * Leave as all zeros.
       */
      UWWarn("Null thread-local exceptionFrame.");
   }

   status = UserDump_Write(dumpData, (uint8*)&worldData, sizeof worldData);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn\'t dump registers for world %d, status %s",
	    world->worldID, UWLOG_ReturnStatusToString(status));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpRegisters --
 *
 *	Writes out the register state for each world in the cartel.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	UserObj state is modified.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserDumpRegisters(UserDump_Header* header, UserDump_DumpData *dumpData,
		  World_Handle **peerList, int numPeers)
{
   VMK_ReturnStatus status;
   int i;

   /*
    * First write out the registers of the thread that initiated the dump
    * (ie, this thread).  We do this because the first thread's registers
    * written out is assumed to be the current thread.  Thus if a UserWorld
    * panics and dumps, when you load gdb, the first thread you see will be the
    * one that panicked.
    */
   status = UserDumpSetRegisterData(dumpData, MY_RUNNING_WORLD);
   if (status != VMK_OK) {
      return status;
   }

   /*
    * Now dump the registers for the other threads in the cartel.
    */
   for (i = 0; i < numPeers; i++) {
      if (peerList[i] == MY_RUNNING_WORLD) {
         continue;
      }

      status = UserDumpSetRegisterData(dumpData, peerList[i]);
      if (status != VMK_OK && status != VMK_NOT_FOUND) {
         return status;
      }
   }

   header->regEntries = numPeers;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpHeapRegionHeaders --
 *
 *	Callback for Heap_Dump.  Simply writes start address and length
 *	of this region to the dump file.
 *
 *	All heaps are allocated on with page-aligned addresses.
 *	However, the first bytes of the heap allocation is used for
 *	the Heap struct.  The address that's passed to this function is
 *	the first byte after that heap structure.  However, ELF core
 *	dumps prefer everything to be page-aligned, so we simply round
 *	down the start address to get the very beginning of the heap
 *	allocation.  Similarly, we round up the length to make every
 *	page-aligned.
 *
 * Results:
 *	See UserDump_Write.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpHeapRegionHeaders(void *data, VA start, uint32 len)
{
   UserDumpHeapData *heapData = (UserDumpHeapData*)data;
   UserDump_HeapRange heapRange;
   VMK_ReturnStatus status;

   heapRange.start = ALIGN_DOWN(start, PAGE_SIZE);
   heapRange.length = ALIGN_UP(len, PAGE_SIZE);
   status = UserDump_Write(heapData->dumpData, (uint8*)&heapRange,
			   sizeof heapRange);
   if (status == VMK_OK) {
      heapData->numHeadersWritten++;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpHeapRegionData --
 *
 *	Callback for Heap_Dump.  Writes out VMkernel heap data starting
 *	from address 'start' for 'len' bytes.
 *
 * Results:
 *	See UserDump_Write.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpHeapRegionData(void *data, VA start, uint32 len)
{
   UserDumpHeapData *heapData = (UserDumpHeapData*)data;
   VMK_ReturnStatus status;

   /*
    * If a new region was added since we dumped the headers, don't write its
    * data here.
    */
   if (heapData->numRegionsWritten == heapData->numHeadersWritten) {
      return VMK_OK;
   }
   ASSERT(heapData->numRegionsWritten < heapData->numHeadersWritten);

   status = UserDump_Write(heapData->dumpData,
			   (uint8*)ALIGN_DOWN(start, PAGE_SIZE),
			   ALIGN_UP(len, PAGE_SIZE));
   if (status == VMK_OK) {
      heapData->numRegionsWritten++;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpHeap --
 *
 *	Dumps the VMkernel heaps for the current cartel and world group.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpHeap(UserDump_Header *header, UserDump_DumpData *dumpData)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserDumpHeapData heapData;

   memset(&heapData, 0, sizeof heapData);
   heapData.dumpData = dumpData;

   /*
    * First dump the start address and length for each heap region for both the
    * cartel and world group heaps.
    */
   status = Heap_Dump(uci->heap, UserDumpHeapRegionHeaders, &heapData);
   if (status != VMK_OK) {
      return status;
   }

   status = Heap_Dump(MY_RUNNING_WORLD->group->heap, UserDumpHeapRegionHeaders,
		      &heapData);
   if (status != VMK_OK) {
      return status;
   }

   /*
    * Now dump the data for each region.
    */
   status = Heap_Dump(uci->heap, UserDumpHeapRegionData, &heapData);
   if (status != VMK_OK) {
      return status;
   }

   status = Heap_Dump(MY_RUNNING_WORLD->group->heap, UserDumpHeapRegionData,
		      &heapData);
   if (status != VMK_OK) {
      return status;
   }


   ASSERT(heapData.numHeadersWritten == heapData.numRegionsWritten);
   header->heapRegions = heapData.numRegionsWritten;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpHeader --
 *
 *	Dumps out the metadata header by writing directly into the
 *	buffer used by the compression library then calling the
 *	compression callback.  Note that this is to circumvent the
 *	compression library so that the header is not compressed.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	UserObj state is modified.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDumpHeader(UserDump_Header* header, UserDump_DumpData *dumpData)
{
   VMK_ReturnStatus status;

   ASSERT(sizeof(UserDump_Header) < PAGE_SIZE);

   UserDump_Seek(dumpData->obj, 0, USEROBJ_SEEK_SET);

   Util_ZeroPage(dumpData->buffer);
   memcpy(dumpData->buffer, header, sizeof(UserDump_Header));
   status = UserDumpCompressOutputFn(dumpData, FALSE);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn\'t dump DumpHeader, error %s",
	    UWLOG_ReturnStatusToString(status));
      return status;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDumpZeroHeader --
 *
 *	Zeroes out the header, save for the executable name.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
UserDumpZeroHeader(UserDump_Header *header)
{
   header->version = 0;
   header->startOffset = 0;
   header->regEntries = 0;
   header->mmapElements = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_CoreDump --
 *
 *      Write a core dump to a file in the file system for this world.
 *
 *      If several threads of the same cartel try to coredump at the
 *      same time, only one will become the "dumperWorld" and all others
 *      will wait until the core dump finishes.
 *
 * Results:
 *	VMK_OK if the core dump was written, VMK_BUSY if a dump is in
 *	progress, error code otherwise.
 *
 *      If the function returns VMK_OK, the thread that initiates the
 *      coredump remains the "dumperWorld" of the cartel. The dumperWorld
 *      can release the "dumperWorld" of the cartel by calling 
 *      UserDump_ReleaseDumper().
 *
 * Side effects:
 *      A new file is created and written to in the current working
 *      directory (or /, if cwd has problems).
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_CoreDump(void)
{
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   UserDump_Header *header = uci->coreDump.header;
   UserObj* obj = NULL;
   UserDump_DumpData dumpData;
   World_Handle *peerList[USER_MAX_ACTIVE_PEERS] = { NULL };
   int numPeers = 0;

   SP_Lock(&uci->coreDump.dumpLock);

   if (uci->coreDump.dumperWorld == MY_RUNNING_WORLD->worldID) {
      /*
       * Recursive core dump happened. Done with coredump progress.
       */
      UWWarn("Recursive core dumping ...");
      status = VMK_FAILURE;

      SP_Unlock(&uci->coreDump.dumpLock);
      goto out; 
   }

   /*
    * Make sure only one world dumps core for a given cartel.
    */
   if (uci->coreDump.dumperWorld != INVALID_WORLD_ID) {
      UWWarn("Already someone dumping ...");
      SP_Unlock(&uci->coreDump.dumpLock);
      /*
       * Wait for coredump to finish.
       */
      UserDump_WaitForDumper();
      return VMK_BUSY;
   }

   // Assign the cartel dumper and start the core dump process.
   uci->coreDump.dumperWorld = MY_RUNNING_WORLD->worldID;
   uci->coreDump.inProgress = TRUE;

   SP_Unlock(&uci->coreDump.dumpLock);

   /*
    * Sleep for 100ms to allow all the other worlds in the cartel to realize
    * we're dumping.
    */
   CpuSched_Sleep(100);

   /*
    * Initialize Dump_Info and make room for it at the front of the core.
    */
   UserDumpZeroHeader(header);
   header->version = DUMP_TYPE_USER | DUMP_VERSION_USER;

   /*
    * Allocate space for the compression buffer.
    */
   dumpData.buffer = User_HeapAlign(uci, PAGE_SIZE, PAGE_SIZE);
   if (dumpData.buffer == NULL) {
      ASSERT(FALSE);
      status = VMK_NO_MEMORY;
      goto out;
   }
   dumpData.mpn = MA_2_MPN(VMK_VA2MA((VA)dumpData.buffer));

   /*
    *   Find an unused filename.
    */
   status = UserDumpGetCoreFile(&obj, uci->coreDump.dumpName,
				sizeof uci->coreDump.dumpName);
   if (status != VMK_OK) {
      goto out;
   }
   header->startOffset = UserDump_Seek(obj, PAGE_SIZE, USEROBJ_SEEK_SET);
   ASSERT(header->startOffset == PAGE_SIZE);
   dumpData.obj = obj;

   UWLog("Dumping cartel %d (from world %d) to file %s ...",
         MY_USER_CARTEL_INFO->cartelID, MY_RUNNING_WORLD->worldID, 
         uci->coreDump.dumpName);

   /*
    *   Set up dump compression.
    */
   status = Compress_Start(&dumpData.compressContext, &UserDumpCompressAlloc,
			   &UserDumpCompressFree, dumpData.buffer, PAGE_SIZE,
			   &UserDumpCompressOutputFn, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   /*
    *   Write out the string versions of the UserObj types and UserMemMapInfo
    *   types.
    */
   status = UserObj_DumpObjTypes(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   status = UserMem_DumpMapTypes(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   numPeers = UserDumpGetPeerList(peerList, USER_MAX_ACTIVE_PEERS);

   /*
    *   Write out some important pointer values (for use with the cartel heap).
    */
   status = UserDumpPointerTable(header, &dumpData, peerList, numPeers);
   if (status != VMK_OK) {
      goto out;
   }

   /*
    *   Write out open objects in the fd table as well as those backing
    *   mmap regions (which may not be in the fd table).
    */
   status = UserObj_DumpFdTable(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   status = UserMem_DumpMMapObjects(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   /*
    *   Write out registers for all worlds in cartel.
    */
   status = UserDumpRegisters(header, &dumpData, peerList, numPeers);
   if (status != VMK_OK) {
      goto out;
   }

   /* 
    *   Write out the mmap'ed regions.
    */
   status = UserMem_DumpMMap(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   /*
    *   Write out the cartel and world group heap.
    */
   status = UserDumpHeap(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   /*
    *   Stop and cleanup compression.
    */
   status = Compress_Finish(&dumpData.compressContext, NULL);
   if (status != VMK_OK) {
      UWWarn("Failed to properly cleanup compression.");
      /*
       * XXX: Proceed even though we encountered an error here.  We still want
       * to try and write out the header (which doesn't use compression).
       *
       * XXX: Depending on where Compress_Finish failed, we may not have cleaned
       * up our allocations on the main heap...
       */
   }

   /*
    *   Dump the header.
    */
   status = UserDumpHeader(header, &dumpData);
   if (status != VMK_OK) {
      goto out;
   }

   UWLog("Userworld coredump complete.");

out:
   if (dumpData.buffer != NULL) {
      User_HeapFree(uci, dumpData.buffer);
   }

   if (obj != NULL) {
      (void) UserObj_Release(uci, obj);
   }

   UserDumpFreePeerList(peerList, numPeers);

   SP_Lock(&uci->coreDump.dumpLock);

   /*
    * Release uci core dumper and wake up others threads in the cartel.
    */
   uci->coreDump.inProgress= FALSE;
   CpuSched_Wakeup((uint32)&uci->coreDump.inProgress);

   /*
    * If core dumping failed, release the dumper for the uci.
    */
   if (status != VMK_OK) {
      uci->coreDump.dumperWorld = INVALID_WORLD_ID;
   }

   SP_Unlock(&uci->coreDump.dumpLock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_ReleaseDumper --
 *
 *      Release the dumperWorld of the current cartel.
 *      The caller thread must be the current dumperWorld.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
void UserDump_ReleaseDumper(void)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   SP_Lock(&uci->coreDump.dumpLock);

   ASSERT(uci->coreDump.inProgress == FALSE);
   ASSERT(uci->coreDump.dumperWorld == MY_RUNNING_WORLD->worldID);

   if (uci->coreDump.dumperWorld == MY_RUNNING_WORLD->worldID) {
      uci->coreDump.dumperWorld = INVALID_WORLD_ID;
   }
   memset(uci->coreDump.dumpName, 0, sizeof(uci->coreDump.dumpName));
   UserDumpZeroHeader(uci->coreDump.header);
   SP_Unlock(&uci->coreDump.dumpLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDump_DebugCoreDump --
 *
 *	For internal (i.e., debugging) triggers of a core dump.
 *
 * Results:
 *      VMK_OK if dump suceeded, otherwise if it failed.
 *
 * Side effects:
 *      Generates a core in pwd, 
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDump_DebugCoreDump(void)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci;

   // Do aggressive sanity checks to avoid making problems worse

   if (! World_IsUSERWorld(MY_RUNNING_WORLD)) {
      UWLOG(0, "Cannot dump.  Current World not a UserWorld.");
      return VMK_NOT_FOUND;
   }

   uci = MY_USER_CARTEL_INFO;
   if (uci == NULL) {
      UWLOG(0, "Cannot dump.  Current world has null uci.");
      return VMK_INVALID_HANDLE;
   }

   UWLOG(0, "Starting UW core dump");
   status = UserDump_CoreDump();
   if (status == VMK_OK) {
      UWLOG(0, "dump file: %s", uci->coreDump.dumpName);
      UserDump_ReleaseDumper();
   } else {
      UWLOG(0, "NO dump file: %s", UWLOG_ReturnStatusToString(status));
   }
   return status;
}

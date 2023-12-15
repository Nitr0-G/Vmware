/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * dump.c --
 *
 *	Dump the vmkernel data structures.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmk_scsi.h"
#include "chipset.h"
#include "tlb.h"
#include "dump.h"
#include "bluescreen.h"
#include "memmap.h"
#include "memalloc.h"
#include "world.h"
#include "netDebug.h"
#include "util.h"
#include "compress.h"
#include "xmap.h"
#include "host_dist.h"
#include "hardware.h"
#include "idt.h"
#include "log_int.h"
#include "fsSwitch.h"
#include "serial.h"
#include "host.h"
#include "helper.h"

#define LOGLEVEL_MODULE Dump
#include "log.h"

static SCSI_HandleID dumpHandleID = -1;
static Bool dumpInProgress = FALSE;

#define ERR_BUF_LENGTH	100
static char errBuf[ERR_BUF_LENGTH];

typedef VMK_ReturnStatus (*DumpWriteFunc)(uint32 offset, VA data, 
					  uint32 length, const char *dumpType);
typedef Bool (*DumpPAECapableFunc)(void);

static uint32 dumperSeqNum = 1;
static uint64 dumperTimestamp;
static int32 dumpNetID = -1;
static uint32 dumpBytes = 0;
static uint32 dumpNextMB = 1;
static uint32 dumperIPAddr;
static uint8 dumperMACAddr[6];
static Net_DumperMsgHdr dumperMsgReply;

#define DUMP_RETRY_MS		100
#define DUMP_MAX_PKT_DATA_SIZE	1400

static DumpWriteFunc dumpWriteFunc;
static DumpPAECapableFunc dumpIsPAECapableFunc;

static VMK_ReturnStatus Dump(VMKFullExcFrame *frame);

#define MAX_DUMP_INCR	8 * PAGE_SIZE

static uint8 writeBuffer[PAGE_SIZE];

static CompressContext dumpCompressContext;
static char dumpCompressBuf[PAGE_SIZE];
static uint32 currentDumpOffset;

/*
 * Compression module needs some memory for it's dictionary.  Normally, it
 * would get this from the heap, but we don't want to rely on heap during
 * coredump, so we allocate it statically.  Currently the total amount is
 * 256K+5816 allocated in 5 pieces.
*/
#define DUMP_DICT_SIZE (256*1024+6*1024)
static char dumpDictMem[DUMP_DICT_SIZE];
static uint32 dumpDictMemUsed;

/*
 *----------------------------------------------------------------------
 *
 * Dump_Init --
 *
 *      Initialize the dump module.
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
Dump_Init(void)
{
   // this function used to do useful things, but that functionality
   // was moved elsewhere, so it's empty now.
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_RequestLiveDump --
 *
 *	Schedule a "live" dump.  We will dump on the next interrupt taken.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Dump_RequestLiveDump(void)
{
   Warning("Asking for coredump ra=%p", __builtin_return_address(0));
   myPRDA.wantDump = TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_SetPartition --
 *
 *      Set the partition that we are going to use for vmkernel dumps.
 *
 * Results:
 *      Status of the open.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Dump_SetPartition(const char *adapName, uint32 targetID,
                  uint32 lun, uint32 partition)
{
   if (strncmp(adapName, "none", VMNIX_DEVICE_NAME_LENGTH) == 0 ||
       strncmp(adapName, "None", VMNIX_DEVICE_NAME_LENGTH) == 0) {
      if (dumpHandleID != -1) {
         LOG(0, "Closing handle %#x", dumpHandleID);
         SCSI_CloseDevice(hostWorld->worldID, dumpHandleID);
         dumpHandleID = -1;
      }
   } else {
      VMK_ReturnStatus status;
      SCSI_HandleID tmpHandleID;

      if (partition == 0) {
         return VMK_INVALID_TYPE;
      }
      status = SCSI_OpenDevice(hostWorld->worldID, adapName, targetID,
                               lun, partition, SCSI_OPEN_DUMP, &tmpHandleID);
      if (status != VMK_OK) {
         return status;
      }
      if (dumpHandleID != -1) {
         Log("Disabling active dump handle %#x before "
             "resetting it to %s:%d:%d:%d", dumpHandleID,
             adapName, targetID, lun, partition);
         SCSI_CloseDevice(hostWorld->worldID, dumpHandleID);
      }
      dumpHandleID = tmpHandleID;
      LOG(0, "%s:%d:%d:%d, handle %#x", adapName, targetID,
          lun, partition, dumpHandleID);
   }

   return VMK_OK;
}

/*
 * Core dump format:
 *
 *  Description                  Length (in bytes)
 * -------------                -------------------
 * Dump_Info struct             DUMP_MULTIPLE (the only uncompressed field)
 * Log buffer                   VMK_LOG_BUFFER_SIZE
 * Dump_WorldData structs       DUMP_MULTIPLE * # of worlds
 * VMM code/data/tc             VMM_NUM_PAGES * PAGE_SIZE
 * active world's mappedStack   world->numStackMPNs * PAGE_SIZE
 * active world's mapped2Stack  world->numStack2MPNs * PAGE_SIZE
 * vmkernel code/data/heap      VMK_NUM_CODEHEAP_PDES * PDE_SIZE
 * kvmap                        VMK_NUM_MAP_PDES * PDE_SIZE
 * prda                         VMK_NUM_PRDA_PDES * PDE_SIZE
 * kseg                         VMK_NUM_KSEG_PDES * PDE_SIZE
 */

/*
 *----------------------------------------------------------------------
 *
 * DumpCompressOutputFn --
 *
 *      Write the compressed data to disk or net.  Called when the
 *      compressed data buffer needs to be flushed.
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
DumpCompressOutputFn(void *arg, Bool partial)
{
   VMK_ReturnStatus status;
   Dump_Info *info = (Dump_Info*)arg;
   static uint32 compressedOffset;

   if (compressedOffset == 0) {
      compressedOffset += info->startOffset;
   }

   status = dumpWriteFunc(compressedOffset,
                          (VA)dumpCompressBuf, PAGE_SIZE, "compressed");
   if (!partial) {
      compressedOffset += PAGE_SIZE;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpCompressAlloc --
 *
 *      Allocate memory for compression dictionary.  Since
 *      we statically allocate it, just return an offset into the static
 *      buffer.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
DumpCompressAlloc(UNUSED_PARAM(void *opaque), uint32 items, uint32 size)
{
   uint32 totalSize = items * size;
   uint32 availMem = DUMP_DICT_SIZE - dumpDictMemUsed;
   void *p;

   if (totalSize > availMem) {
      SysAlert("out of dictionary memory req (%d * %d = %d) avail %d",
               items, size, totalSize, availMem);
      if (vmx86_debug) {
         Panic("resize dumpDictMem\n");
      }
      return NULL;
   }

   p = &dumpDictMem[dumpDictMemUsed];
   dumpDictMemUsed += totalSize;

   LOG(1, "allocated %d bytes at %p avail=%d\n",
       totalSize, p, DUMP_DICT_SIZE - dumpDictMemUsed);
   return p;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpCompressFree --
 *
 *      Called to free compression dictionary memory, but since we
 *      statically allocate it (see DumpCompressAlloc), nothing to do here.
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
DumpCompressFree(UNUSED_PARAM(void *opaque), void *ptr)
{
   LOG(1, "freeing ptr %p", ptr);
}

/*
 *----------------------------------------------------------------------
 *
 * DumpCompressFreeAll --
 *
 *      Called to free all the compression dictionary memory
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
DumpCompressFreeAll(void)
{
   dumpDictMemUsed = 0;
}
/*
 *----------------------------------------------------------------------
 *
 * DumpWarning --
 *
 *      Print out a message to the log and the blue screen.
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
DumpWarning(const char *buf)
{
   _Log("%s", buf);
   BlueScreen_Append(buf);
}

/*
 *----------------------------------------------------------------------
 *
 * DumpLogProgress -
 *
 *      Print out dump progress info to log + bluescreen
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
DumpLogProgress(int n)
{
   char buf[2];

   if (n > 9) {
      n = 9;
   }
   buf[0] = '0' + n;
   buf[1] = '\0';
   DumpWarning(buf);
}

/*
 *----------------------------------------------------------------------
 *
 * Dumper_PktFunc --
 *
 *      This function is called whenever a packets is detected on 
 *	the dumpers port.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The message is copied into dumperMsgReply.
 *
 *----------------------------------------------------------------------
 */
void
Dumper_PktFunc(UNUSED_PARAM(struct NetDebug_Cnx* cnx), 
	       UNUSED_PARAM(uint8 *srcMACAddr),
               UNUSED_PARAM(uint32 srcIPAddr),
               uint32 srcUDPPort,
	       void *data,
               uint32 length)
{
   Net_DumperMsgHdr *hdr = (Net_DumperMsgHdr *)data;

   LOG(2, "Got packet from port %d length %d type %d sn %d", 
           srcUDPPort, length, hdr->type, hdr->sequenceNumber);
   if (length < sizeof(Net_DumperMsgHdr)) {
      Warning("Too short");
      return;
   }

   if (hdr->magic != NET_DUMPER_MSG_MAGIC) {
      Warning("Bad magic");
      return;
   }

   switch (hdr->type) {
   case NET_DUMPER_MSG_DUMP:
      Log("Forcing vmkernel dump");
      Dump_RequestLiveDump();
      break;
   case NET_DUMPER_MSG_BREAK:
      Log("Forcing breakpoint");
      IDT_WantBreakpoint();
      break;
   case NET_DUMPER_MSG_DUMP_AND_BREAK:
      Log("Forcing vmkernel dump and breakpoint");
      Dump_RequestLiveDump();
      IDT_WantBreakpoint();
      break;
   default:
      memcpy(&dumperMsgReply, hdr, sizeof(*hdr));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * DumpDoSendMsg --
 *
 *      Send a message on the dumper port.  The message has is guaranteed
 *	to be small enough to fit in single ethernet packet.
 *
 * Results:
 *      VMK_OK if could send the message.
 *      VMK_TIMEOUT if the dumper application doesn't respond.
 *      VMK_FAILURE for other kinds of failures.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DumpDoSendMsg(Net_DumperMsgHdr *msg, void *data, uint32 length, uint32 timeoutMS, 
	      Net_DumperMsgHdr *reply)
{
   int numTries = CEIL(timeoutMS, DUMP_RETRY_MS);

   dumperMsgReply.sequenceNumber = -1;

   LOG(2, "data=0x%x length=%d", (uint32)data, length);

   msg->magic = NET_DUMPER_MSG_MAGIC;
   msg->sequenceNumber = dumperSeqNum++;
   msg->timestamp = dumperTimestamp;
   msg->dumpID = dumpNetID;

   if (dumperMACAddr[0] == 0 && dumperMACAddr[1] == 0 && dumperMACAddr[2] == 0 &&
       dumperMACAddr[3] == 0 && dumperMACAddr[4] == 0 && dumperMACAddr[5] == 0) {
      int i;

      LOG(1, "Looking up MAC address");

#define MAX_ARP_TRIES	10
      for (i = 0; i < MAX_ARP_TRIES; i++) {
	 if (NetDebug_ARP(dumperIPAddr, dumperMACAddr)) {
	    LOG(1, "ARP worked, got %02x:%02x:%02x:%02x:%02x:%02x",
	        dumperMACAddr[0], dumperMACAddr[1], dumperMACAddr[2],
		dumperMACAddr[3], dumperMACAddr[4], dumperMACAddr[5]);
	    break;
	 }

	 Util_Udelay(50000);
	 NetDebug_Poll();
      }

      if (i == MAX_ARP_TRIES) {
	 Warning("ARP timed out");
	 return VMK_TIMEOUT;
      }
   }

   while (numTries > 0) {
      int i;
      if (!NetDebug_Transmit(msg, sizeof(*msg), data, length, NET_DUMPER_PORT,
			     dumperMACAddr, dumperIPAddr, NET_DUMPER_PORT, 0)) {
	 Warning("failed");
	 return VMK_FAILURE;
      }

      for (i = 0; i < DUMP_RETRY_MS; i++) {
	 Util_Udelay(1000);
	 NetDebug_Poll();

	 if (dumperMsgReply.sequenceNumber == msg->sequenceNumber) {
	    LOG(2, "Got reply");
	    *reply = dumperMsgReply;
	    return dumperMsgReply.status;
	 }
      }

      numTries--;
   }

   return VMK_TIMEOUT;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpSendMsg --
 *
 *      Send a message on the dumper port.  The message is broken 
 *	up into ethernet packet size chunks and then sent by calling
 *	DumpDoSendMsg.
 *
 * Results:
 *      VMK_OK if could send the message or whatever 
 *	DumpDoSendMsg returns.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DumpSendMsg(Net_DumperMsgHdr *msg, void *data, uint32 offset, 
	    uint32 length, uint32 timeoutMS, Net_DumperMsgHdr *reply)
{
   int numPackets;
   int bytesLeft;
   int i;

   LOG(2, "data=0x%x offset=%d length=%d", (uint32)data, offset, length);

   if (length == 0) {
      return DumpDoSendMsg(msg, NULL, 0, timeoutMS, reply);
   }

   numPackets = CEIL(length, DUMP_MAX_PKT_DATA_SIZE);
   bytesLeft = length;

   for (i = 0; i < numPackets; i++) {
      VMK_ReturnStatus status;
      int toSend = bytesLeft;
      if (toSend > DUMP_MAX_PKT_DATA_SIZE) {
	 toSend = DUMP_MAX_PKT_DATA_SIZE;
      }

      msg->dataOffset = offset;      
      msg->dataLength = toSend;

      status = DumpDoSendMsg(msg, data, toSend, timeoutMS, reply);
      if (status != VMK_OK) {
	 return status;
      }

      bytesLeft -= toSend;
      offset += toSend;
      data += toSend;
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpNet --
 *
 *      Dump data to the network.
 *
 * Results:
 *      VMK_OK if could send the message, error otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
DumpNet(uint32 offset, VA data, uint32 length, UNUSED_PARAM(const char *dumpType))
{
   VMK_ReturnStatus status;
   Net_DumperMsgHdr msg, reply;

   if (dumpNetID < 0) {
      if (dumperIPAddr == 0) {
	 Warning("No dumper set");
	 return VMK_FAILURE;
      }
      msg.type = NET_DUMPER_MSG_INIT;
      dumperTimestamp = RDTSC();

      LOG(1, "Sending INIT message");

      status = DumpSendMsg(&msg, NULL, 0, 0, 5000, &reply);
      if (status != VMK_OK) {
	 Warning("Couldn't attach to a dumper world @ 0x%x",
	         dumperIPAddr);
	 return status;
      }

      dumpNetID = reply.payload;
      dumpBytes = 0;
      dumpNextMB = 1;

      LOG(1, "Returned id %d", dumpNetID);

      if (dumpNetID < 0) {
	 Warning("Negative dumpNetID from dumper");
	 return VMK_FAILURE;
      }
   }

   if (length == 0) {
      VMK_ReturnStatus status;
      LOG(1, "Sending DONE message");
      msg.type = NET_DUMPER_MSG_DONE;
      status = DumpSendMsg(&msg, NULL, 0, 0, 5000, &reply);
      dumpNetID = -1;
      return status;
   }

   msg.type = NET_DUMPER_MSG_DATA;

   status = DumpSendMsg(&msg, (void *)data, offset, length, 5000, &reply);

   if (status == VMK_OK) {
      dumpBytes += length;
      if (dumpBytes / (1024 * 1024) >= dumpNextMB) {
	 dumpNextMB++;
      }
   } else {
      Warning("failed with status %d", status);
      dumpNetID = -1;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpNetPAECapable --
 *
 *      Stub to say that we are always PAE capable.  Since we always
 *	copy the packet we don't have to worry about PAE.
 *
 * Results:
 *      TRUE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
DumpNetPAECapable(void)
{
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpSCSI --
 *
 *      Dump to the SCSI device.
 *
 * Results:
 *      VMK_OK if could dump to the SCSI device, failure code 
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
DumpSCSI(uint32 offset, VA data, uint32 length, const char *dumpType)
{
   VMK_ReturnStatus status = VMK_OK;
   static int64 dumpSlotOffset = -1;
   
   if (length == 0) {
      // marks the end of coredump
      return VMK_OK;
   }

   /*
    * dumpSlotOffset marks the offset in a shared coredump partition where
    * this kernel is supposed to dump its core.  The offset is determined
    * by (uuid % number of coredump slots).
    */
   if (dumpSlotOffset == -1) {
      VMnix_GetCapacityResult capacity;
      uint64 dumpPartitionSize;
      unsigned numSlots, slotNum;

      status = SCSI_GetCapacity(dumpHandleID, &capacity);
      if (status != VMK_OK) {
         return status;
      }
      dumpPartitionSize = ((uint64)capacity.numDiskBlocks) * capacity.diskBlockSize;
      numSlots = dumpPartitionSize / VMKERNEL_DUMP_SIZE;

      if (numSlots <= 1) {
         slotNum = 0;
         numSlots = 1;
      } else {
         Hardware_DMIUUID uuid;
         status = Hardware_GetUUID(&uuid);
         if (status != VMK_OK) {
            return status;
         }
         slotNum = DumpHashUUID(&uuid) % numSlots;
      }
      /*
       * Use errBuf temporarily to print out message.
       */
      snprintf(errBuf, sizeof errBuf, "using slot %d of %d... ", slotNum + 1,
	       numSlots);
      DumpWarning(errBuf);
      dumpSlotOffset = ((uint64)slotNum) * VMKERNEL_DUMP_SIZE;
   }

   // we can't exceed our coredump into another coredump slot.
   if (offset + length > VMKERNEL_DUMP_SIZE) {
      status = VMK_LIMIT_EXCEEDED;
   }

   if (status == VMK_OK) {
      status = SCSI_Dump(dumpHandleID, dumpSlotOffset + offset, data, length, FALSE);
   }

   /*
    * errBuf is global, but that's okay as all the other CPUs have come to
    * rest already.
    */
   if (status == VMK_TIMEOUT) {
      snprintf(errBuf, sizeof errBuf, "Timeout\n");
   } else if (status == VMK_LIMIT_EXCEEDED) {
      snprintf(errBuf, sizeof errBuf, "Out of space o=0x%x l=0x%x\n", offset, length);
   } else if (status != VMK_OK) {
      snprintf(errBuf, sizeof errBuf, "Couldn't dump %s: status=%d\n", dumpType, status);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpSCSIPAECapable --
 *
 *      Return if the adapter is PAE capable.
 *
 * Results:
 *      Return from SCSIAdapterIsPAECapable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
DumpSCSIPAECapable(void)
{
   return SCSI_IsHandleToPAEAdapter(dumpHandleID);
}

/*
 *----------------------------------------------------------------------
 *
 * DumpRange --
 *
 *      Write the given range of data to the core dump
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Dump_Range(VA vaddr, uint32 size, const char *errorMsg)
{
   VMK_ReturnStatus status = VMK_OK;
   static uint8 buffer[MAX_DUMP_INCR];

   ASSERT(size <= MAX_DUMP_INCR);
   if (vaddr == 0) {
      memset(buffer, 0, size);
      status = Compress_AppendData(&dumpCompressContext, buffer, size);
   } else {
      status = Compress_AppendData(&dumpCompressContext, (void*)vaddr, size);
   }
   if (status != VMK_OK) {
      Warning("failure (%#x) while dumping range '%s'", status, errorMsg);
   }

   currentDumpOffset += size;

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * DumpDump --
 *
 *      Write out the core to the network dumper and/or the 
 *	selected partition.
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
DumpDump(VMKFullExcFrame *frame, Bool liveDump)
{
   uint32 eflags;
   VMK_ReturnStatus status;

   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();

   if (dumpInProgress) {
      RESTORE_FLAGS(eflags);
      return;
   }

   if (PRDA_IsInitialized()) {
      myPRDA.wantDump = FALSE;
   }
   dumpInProgress = TRUE;

   LOG(1, "BEGIN");

   /*
    * Wait a bit for things to settle.  It will take a little while for
    * the other CPUs to get blocked.  It shouldn't take more than 
    * one timer tick.
    */
   Util_Udelay(50000);

   LOG(1, "Trying network");

   if (NetDebug_Start()) {
      DumpWarning("Starting coredump to network ");
      dumpWriteFunc = DumpNet;
      dumpIsPAECapableFunc = DumpNetPAECapable;
      if (Dump(frame) == VMK_OK) {
	 DumpWarning(" Netdump successful.\n");
      } else {
	 DumpWarning(" Netdump FAILED.\n");
	 DumpWarning(errBuf);
      }
      NetDebug_Stop();
   }

   LOG(1, "Trying SCSI");

   if (dumpHandleID == -1) {
      DumpWarning("No place on disk to dump data\n");
   } else {
      Chipset_MaskAll();
      DumpWarning("Starting coredump to disk ");
      dumpWriteFunc = DumpSCSI;
      dumpIsPAECapableFunc = DumpSCSIPAECapable;
      status = Dump(frame);
      if (status == VMK_OK) {
	 DumpWarning(" Disk dump successful.\n");
      } else if (status == VMK_LIMIT_EXCEEDED) {
	 DumpWarning(" Partial disk dump.\n");
      } else {
	 DumpWarning(" Disk dump FAILED.\n");
	 DumpWarning(errBuf);
      }

      /*
       * If we're in a "live dump", it means that we're currently planning on
       * returning to normal execution after this dump is complete.  However,
       * dumping to the disk completely screws up the scsi driver.  So, since
       * normal execution cannot continue (we'll immediately PSOD), we want to
       * enter the debugger.
       *
       * On the other hand, if we're not doing a live dump, it means we're
       * currently in a PSOD situation, so we're going to enter the debugger
       * anyway.  Thus we don't need to request a breakpoint.  Furthermore,
       * doing so actually hangs the machine.  See bug 35550.
       */
      if (liveDump) {
         IDT_WantBreakpoint();
      }
   }

   dumpInProgress = FALSE;

   LOG(1, "END");

   RESTORE_FLAGS(eflags);   
}


/*
 *----------------------------------------------------------------------
 *
 * Dump_Dump --
 *
 *	Perform a dump of the vmkernel core.
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
Dump_Dump(VMKFullExcFrame *frame)
{
   DumpDump(frame, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Dump_LiveDump --
 *
 *	Perform a "live" dump of the vmkernel core.  This should be called
 *	when Dump_LiveDumpRequested is TRUE.
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
Dump_LiveDump(VMKFullExcFrame *frame)
{
   DumpDump(frame, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Dump --
 *
 *      Write out the core using the given dumper function.
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
Dump(VMKFullExcFrame *frame)
{
   VA va;
   VPN vpn;
   VMK_ReturnStatus status = VMK_OK;
   int i;
   int numWorlds;
   // too big for stack, and this is a non-reentrant function, so static
   static Dump_Info info;
   static Dump_WorldData data;
   static World_ID worlds[MAX_WORLDS];

   _Log("Dumping ");

   /*
    * first zero out coredump header so if we fail in the middle, any old
    * information is not read.
    */
   ASSERT(sizeof(writeBuffer) >= PAGE_SIZE);

   ASSERT(sizeof(writeBuffer) >= sizeof(info));
   ASSERT(sizeof(info) <= DUMP_MULTIPLE);
   memset(writeBuffer, 0, sizeof(writeBuffer));
   status = dumpWriteFunc(0, (VA)writeBuffer, DUMP_MULTIPLE, "header");
   if (status != VMK_OK) {
      return status;
   }

   memset(&info, 0, sizeof(info));
   info.version = DUMP_TYPE_KERNEL | DUMP_VERSION_KERNEL;
   status = Hardware_GetUUID(&info.uuid);
   if (status != VMK_OK) {
      return status;
   }

   currentDumpOffset = DUMP_MULTIPLE;
   info.startOffset = currentDumpOffset;

   status = Compress_Start(&dumpCompressContext, &DumpCompressAlloc, &DumpCompressFree, 
                           dumpCompressBuf, PAGE_SIZE,
                           &DumpCompressOutputFn, &info);
   if (status != VMK_OK) {
      return status;
   }

   _Log("log");

   info.logLength = VMK_LOG_BUFFER_SIZE;
   info.logEnd = nextLogChar;
   info.logOffset = currentDumpOffset;

   ASSERT((VMK_LOG_BUFFER_SIZE % MAX_DUMP_INCR) == 0);
   for (va = (VA)logBuffer;
        va < ((VA)logBuffer) + VMK_LOG_BUFFER_SIZE;
        va += MAX_DUMP_INCR) {
      status = Dump_Range(va, MAX_DUMP_INCR, "log");
      if (status != VMK_OK) {
         return status;
      }
   }


   /*
    * First, write out the world that broke.
    */
   _Log("faulting world regs");
   DumpLogProgress(9);

   info.regOffset = currentDumpOffset;
   info.regEntries = 1;

   data.signal = frame->frame.errorCode;
   data.id = MY_RUNNING_WORLD->worldID;
   strncpy(data.name, MY_RUNNING_WORLD->worldName, DUMP_NAME_LENGTH);

   data.regs.eax = frame->regs.eax;
   data.regs.ecx = frame->regs.ecx;
   data.regs.edx = frame->regs.edx;
   data.regs.ebx = frame->regs.ebx;
   /*
    * The problem is that the esp register is not contained in the
    * VMKFullExcFrame passed to dump, thus a temporary solution was to just copy
    * the frame's ebp into esp.  This is ok in most cases, as esp is rarely used
    * by the debugger.  However, it's not too hard to figure out esp, so we do
    * that now.
    */
   data.regs.esp = (uint32)&frame->frame + sizeof(VMKExcFrame) -
			sizeof(frame->frame.hostESP);
   data.regs.ebp = frame->regs.ebp;
   data.regs.esi = frame->regs.esi;
   data.regs.edi = frame->regs.edi;

   data.regs.eip = frame->frame.eip;
   data.regs.eflags = frame->frame.eflags;

   data.regs.cs = frame->frame.cs;
   data.regs.ss = 0;
   data.regs.ds = frame->regs.ds;
   data.regs.es = frame->regs.es;
   data.regs.fs = frame->regs.fs;
   data.regs.gs = frame->regs.gs;

   status = Dump_Range((VA)&data, sizeof data, "registers");
   if (status != VMK_OK) {
      return status;
   }

   _Log("other world regs");

   /*
    * Now write out the rest of the worlds.
    */
   numWorlds = MAX_WORLDS;
   i = World_AllWorldsDebug(worlds, &numWorlds);
   ASSERT(i == numWorlds);
   for(i = 0; i < numWorlds; i++) {
      World_Handle* world;

      if (MY_RUNNING_WORLD->worldID == worlds[i]) {
	 continue;
      }
      world = World_FindDebug(worlds[i]);
      ASSERT(world != NULL);

      memset(&data, 0, sizeof data);

      data.signal = frame->frame.errorCode;
      data.id = world->worldID;
      strncpy(data.name, world->worldName, DUMP_NAME_LENGTH);

      data.regs.eax = world->savedState.regs[REG_EAX];
      data.regs.ecx = world->savedState.regs[REG_ECX];
      data.regs.edx = world->savedState.regs[REG_EDX];
      data.regs.ebx = world->savedState.regs[REG_EBX];
      data.regs.esp = world->savedState.regs[REG_ESP];
      data.regs.ebp = world->savedState.regs[REG_EBP];
      data.regs.esi = world->savedState.regs[REG_ESI];
      data.regs.edi = world->savedState.regs[REG_EDI];

      data.regs.eip = world->savedState.eip;
      data.regs.eflags = world->savedState.eflags;

      data.regs.cs = world->savedState.segRegs[SEG_CS];
      data.regs.ss = world->savedState.segRegs[SEG_SS];
      data.regs.ds = world->savedState.segRegs[SEG_DS];
      data.regs.es = world->savedState.segRegs[SEG_ES];
      data.regs.fs = world->savedState.segRegs[SEG_FS];
      data.regs.gs = world->savedState.segRegs[SEG_GS];

      if (World_IsVMMWorld(world)) {
         int j;

	 /*
	  * If esp is some offset within one of the world's stacks, set it to
	  * that same offset from the world's mappedStack.  If mappedStack isn't
	  * actually mapped, don't bother updating anything.
	  */
	 for (j = 0; j < WORLD_VMM_NUM_STACKS; j++) {
	    World_VmmInfo *vmmInfo = World_VMM(world);
	    if (vmmInfo->vmmStackInfo[j].stackBase <= data.regs.esp &&
		data.regs.esp < vmmInfo->vmmStackInfo[j].stackTop &&
		vmmInfo->vmmStackInfo[j].mappedStack) {
	       data.regs.esp = 
		  (data.regs.esp - vmmInfo->vmmStackInfo[j].stackBase) +
		  (VA)vmmInfo->vmmStackInfo[j].mappedStack;
	       data.regs.ebp = 
		  (data.regs.ebp - vmmInfo->vmmStackInfo[j].stackBase) +
		  (VA)vmmInfo->vmmStackInfo[j].mappedStack;
	    }
	 }
      }

      status = Dump_Range((VA)&data, sizeof data, "world data");
      if (status != VMK_OK) {
         return status;
      }

      info.regEntries++;
   }

   _Log("vmm code/data");
   DumpLogProgress(8);
   info.vmmOffset = currentDumpOffset;

   // dump VMM code/data/TC
   for (vpn = VMM_FIRST_VPN; vpn < VMM_FIRST_VPN + VMM_NUM_PAGES; vpn ++) {
      Bool dumpZeros = FALSE;
      LA la = VMK_VA_2_LA(VPN_2_VA(vpn));
      LOG(1,"la=0x%x", la);
      if (MY_RUNNING_WORLD->pageRootMA) {
         VMK_PTE* pageTable = PT_GetPageTable(MY_RUNNING_WORLD->pageRootMA,
                                              la, NULL);
         LOG(1,"pte=0x%Lx", (pageTable? pageTable[ADDR_PTE_BITS(la)] : ~(0ULL)));
         if (pageTable && PTE_PRESENT(pageTable[ADDR_PTE_BITS(la)])) {
            MPN mpn = VMK_PTE_2_MPN(pageTable[ADDR_PTE_BITS(la)]);
            if (VMK_IsValidMPN(mpn)) {
               status = Dump_Page(VPN_2_VA(vpn), "VMM memory");
            } else {
               dumpZeros = TRUE;
            }
         } else {
            dumpZeros = TRUE;
         }
         if (pageTable) {
            PT_ReleasePageTable(pageTable, NULL);
         }
      } else {
         dumpZeros = TRUE;
      }
      if (dumpZeros) {
         status = Dump_Page(0, "VMM memory");
      }
      if (status != VMK_OK) {
	 return status;
      }
   }

   _Log("stack");
   DumpLogProgress(7);

   if (World_IsVMMWorld(MY_RUNNING_WORLD)) {
      for (i = 0; i < WORLD_VMM_NUM_STACKS; i++) {
	 int j;
	 World_VmmInfo *vmmInfo = World_VMM(MY_RUNNING_WORLD);
	 if (i == 0) {
	    info.stackOffset = currentDumpOffset;
	    info.stackStartVPN = WORLD_VMM_STACK_PGOFF;
	    info.stackNumMPNs = WORLD_VMM_NUM_STACK_MPNS;
	 } else if (i == 1) {
	    info.stack2Offset = currentDumpOffset;
	    info.stack2StartVPN = WORLD_VMM_2ND_STACK_PGOFF;
	    info.stack2NumMPNs = WORLD_VMM_NUM_STACK_MPNS;
	 } else {
	    NOT_IMPLEMENTED();
	 }
	 FOR_ALL_VMM_STACK_MPNS(MY_RUNNING_WORLD, j) {
	    if (CpuSched_IsHostWorld() || 
		!VMK_IsValidMPN(vmmInfo->vmmStackInfo[i].mpns[j])) {
	       status = Dump_Page(0, "stack");
	    } else {
	       char *mappedStack = NULL;
	       if (vmmInfo->vmmStackInfo[i].mappedStack != NULL) {
		  mappedStack = (char*)vmmInfo->vmmStackInfo[i].mappedStack +
				j * PAGE_SIZE;
	       }

	       status = Dump_Page((VA)mappedStack, "stack");
	    }
	    if (status != VMK_OK) {
	       return status;
	    }
	 }
      }
   }

   info.codeDataOffset = currentDumpOffset;

   _Log("vmk code/data/heap");

   // dump code/data/heap
   for (va = VMK_FIRST_ADDR; va < VMK_FIRST_MAP_ADDR; va += MAX_DUMP_INCR) {
      // Print a dot every 8M
      if (((va - VMK_FIRST_ADDR) & ((8 << 20) - 1)) == 0) {
         DumpLogProgress(6);
      }
      status = Dump_Range(va, MAX_DUMP_INCR, "vmk memory");
      if (status != VMK_OK) {
	 return status;
      }
   }
   ASSERT(currentDumpOffset - info.codeDataOffset == 
          VMK_FIRST_MAP_ADDR - VMK_FIRST_ADDR);

   _Log("kvmap");
   DumpLogProgress(5);

   info.kvmapOffset = currentDumpOffset;

   // dump kvmap
   for (va = VMK_FIRST_MAP_ADDR; va <= VPN_2_VA(VMK_LAST_MAP_VPN); va += PAGE_SIZE) {
      MPN mpn = TLB_GetMPN(va);
      if (VMK_IsValidMPN(mpn)) {
         memcpy(writeBuffer, (void*)va, PAGE_SIZE);

	 /*
	  * As we're writing out the stack for each world, we need to change any
	  * stack pointers that point to some offset in one of the world's VMM stacks
	  * to that offset from the world's mappedStack.  We take the naive, but
	  * simple, approach of just checking every 4 bytes of the stack to see
	  * if it has an integer value within one of the world's stacks.  This
	  * naive approach guarantees that the stack backtrace will be correct,
	  * but some local variables may be incorrect.
	  */
	 for (i = 0; i < numWorlds; i++) {
	    World_Handle *world;

            if (MY_RUNNING_WORLD->worldID == worlds[i]) {
	       continue;
	    }

	    world = World_FindDebug(worlds[i]);
	    if (world == NULL) {
	       Warning("Dump: Can't find world %d\n", worlds[i]);
	       continue;
	    }

	    if (World_IsVMMWorld(world)) {
	       int j;

	       for (j = 0; j < WORLD_VMM_NUM_STACKS; j++) {
		  uint32* k;
		  World_VmmInfo *vmmInfo = World_VMM(world);

		  if (!vmmInfo->vmmStackInfo[j].mappedStack) {
		     continue;
		  }
		  if ((VA)(vmmInfo->vmmStackInfo[j].mappedStack) <= va &&
		      va < (VA)(vmmInfo->vmmStackInfo[j].mappedStack) +
		      VPN_2_VA(WORLD_VMM_NUM_STACK_MPNS)) {
		     for (k = (uint32*)writeBuffer;
			  k < (uint32*)(writeBuffer + PAGE_SIZE);
			  k++) {
			if (vmmInfo->vmmStackInfo[j].stackBase <= *k &&
			    *k < vmmInfo->vmmStackInfo[j].stackTop) {
			   *k = (*k - vmmInfo->vmmStackInfo[j].stackBase) +
			        (VA)vmmInfo->vmmStackInfo[j].mappedStack;
			}
		     }
		     i = numWorlds;
		     break;
		  }
	       }
	    }
         }

	 status = Dump_Range((VA)writeBuffer, PAGE_SIZE, "KVMap");
      } else {
	 status = Dump_Range(0, PAGE_SIZE, "KVMap");
      }
      if (status != VMK_OK) {
         return status;
      }
   }

   _Log("world stack pages");
   DumpLogProgress(4);

   // dump world stack pages
   for (va = VMK_FIRST_STACK_ADDR; va <= VPN_2_VA(VMK_LAST_STACK_VPN); va += PAGE_SIZE) {
      MPN mpn = World_GetStackMPN(va);
      if (VMK_IsValidMPN(mpn)) {
	 World_Handle *world = World_GetWorldFromStack(va);
         memcpy(writeBuffer, (void*)va, PAGE_SIZE);
	 /*
	  * If this is a VMM world we need to search through the stack page
	  * for any values that look like they may be pointers into the 
	  * VMM stack and make them point to the mapped stack.
	  */
	 if (world != NULL && world != MY_RUNNING_WORLD && World_IsVMMWorld(world)) {
	    int j;
	    for (j = 0; j < WORLD_VMM_NUM_STACKS; j++) {
	       uint32* k;	
               World_VmmInfo *vmmInfo = World_VMM(world);

               if (!vmmInfo->vmmStackInfo[j].mappedStack) {
                  continue;
               }
	       for (k = (uint32*)writeBuffer;
		    k < (uint32*)(writeBuffer + PAGE_SIZE); 
		    k++) {
		  if (vmmInfo->vmmStackInfo[j].stackBase <= *k &&
		      *k < vmmInfo->vmmStackInfo[j].stackTop) {
		     *k = (*k - vmmInfo->vmmStackInfo[j].stackBase) + 
			  (VA)vmmInfo->vmmStackInfo[j].mappedStack;
		  }
	       }
	    }
	 }
	 status = Dump_Range((VA)writeBuffer, PAGE_SIZE, "world stack pages");
      } else {
	 status = Dump_Range(0, PAGE_SIZE, "world stack pages");
      }
      if (status != VMK_OK) {
         return status;
      }
   }

   _Log("PRDA");
   DumpLogProgress(3);

   info.prdaOffset = currentDumpOffset;

   // dump prda page
   if (VMK_IsValidMPN(prdaMPNs[myPRDA.pcpuNum])) {
      status = Dump_Page((VA)&myPRDA, "prda page");
   } else {
      status = Dump_Page(0, "prda pagezero");
   }
   if (status != VMK_OK) {
      return status;
   }

   _Log("KSEG");
   DumpLogProgress(2);

   // dump kseg (including rest of prda region, which includes kseg stuff)
   status = Kseg_Dump();
   if (status != VMK_OK) {
      return status;
   }

   /*
    * All the required regions have been dumped (XMap is dumped on a
    * best-effort basis because of its size), so let's write out the header
    * for the stuff so far in case we're unable to dump
    */
   Compress_Flush(&dumpCompressContext, &info.dumpSize);
   _Log("\ncompressed size for required regions %d\n", info.dumpSize);
   memcpy(writeBuffer, &info, sizeof(info));
   status = dumpWriteFunc(0, (VA)writeBuffer, DUMP_MULTIPLE, "header");
   if (status != VMK_OK) {
      return status;
   }

   // dump xmap
   _Log("xmap");
   DumpLogProgress(1);
   info.xmapOffset = currentDumpOffset;

   status = XMap_Dump();
   // xmap_dump dumps a lot of stuff and may run out of space. this is OK
   if (status == VMK_LIMIT_EXCEEDED) {
      status = VMK_OK;
   }
   if (status != VMK_OK) {
      return status;
   }

   /*
    * write the final dumpinfo structure
    */
   _Log("header");
   DumpLogProgress(0);

   // finsh the dump and record the size
   Compress_Finish(&dumpCompressContext, &info.dumpSize);
   _Log("\ncompressed size %d\n", info.dumpSize);

   // free compression dictionary memory
   DumpCompressFreeAll();

   memcpy(writeBuffer, &info, sizeof(info));
   status = dumpWriteFunc(0, (VA)writeBuffer, DUMP_MULTIPLE, "header");
   if (status != VMK_OK) {
      return status;
   }

   // mark the end for coredump by 0 length write (needed for netdumps)
   status = dumpWriteFunc(0, 0, 0, "header");
   if (status != VMK_OK) {
      return status;
   }

   LOG(1, "DONE");

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_SetIPAddr --
 *
 *      Set the IP address where the dumper program is located.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      dumperIPAddr is set.
 *
 *----------------------------------------------------------------------
 */
void
Dump_SetIPAddr(uint32 ipAddr)
{
   dumperIPAddr = ipAddr;
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_GetIPAddr --
 *
 *      Return the IP address where the dumper program is located.
 *
 * Results:
 *      The IP address where the dumper program is located.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
Dump_GetIPAddr(void)
{
   return dumperIPAddr;
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_IsEnabled --
 *
 *      Return whether the dump parrition is setup
 *
 * Results:
 *      TRUE if dumper setup, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Dump_IsEnabled(void)
{
   return (dumpHandleID != -1);
}

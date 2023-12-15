/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cosdump.c --
 *
 *	Coreduming for the console os.
 */

#include "vm_types.h"
#include "libc.h"
#include "vmkernel.h"
#include "kseg.h"
#include "fsSwitch.h"
#include "pagetable.h"
#include "serial.h"
#include "helper.h"
#include "fsNameSpace.h"
#include "bluescreen.h"
#include "debug.h"

#define LOGLEVEL_MODULE Dump
#define LOGLEVEL_MODULE_LEN 7
#include "log.h"

#define COS_COREDUMP_TIMEOUT 600  /* 10 minutes (in seconds) */


/*
 * Cache a valid cr3 for the debugger / cos coredumper to use
 */
struct CosDumpHelperInfo {
   MA hostCR3;
   VA hdr;
};

static struct CosDumpHelperInfo dumpInfo;


/*
 *----------------------------------------------------------------------
 *
 * CosDumpGetCosVPNContents --
 *
 *      Copies the contents of vpn in the COS into outBuf.
 *
 *      Caller must supply a valid cr3 for the console os.    (This
 *      usually means that the console os cannot be running while this
 *      function is called).
 *
 * Results:
 *      FALSE is VA is unmapped or unsafe to read.
 *      TRUE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
CosDumpGetCosVPNContents(VPN vpn, MA cr3, char *outBuf)
{
   Bool valid = FALSE;
   MPN mpn = INVALID_MPN;
   KSEG_Pair *pairDir, *pairTable;
   VMK_PTE *pageTable;
   VMK_PDE *pageDir;
   LA laddr = VPN_2_VA(vpn) /*la = va in cos*/;

   pageDir = PT_GetPageDir(cr3, laddr, &pairDir);
   if (pageDir) {
      if (PTE_PRESENT(pageDir[ADDR_PDE_BITS(laddr)])) {
         if (PTE_LARGEPAGE(pageDir[ADDR_PDE_BITS(laddr)])) {
            MA baseMpn = VMK_PDE_2_MPN(pageDir[ADDR_PDE_BITS(laddr)]);
            uint32 offset = (laddr & (PDE_SIZE - 1)) >> PAGE_SHIFT;
            // Check for & ignore uncached pages.
            valid = !(pageDir[ADDR_PDE_BITS(laddr)] & PTE_PCD);
            mpn = baseMpn + offset;
         } else {
            pageTable = PT_GetPageTableInDir(pageDir, laddr, &pairTable);
            if (pageTable) {
               if (PTE_PRESENT(pageTable[ADDR_PTE_BITS(laddr)])) {
                  mpn = VMK_PTE_2_MPN(pageTable[ADDR_PTE_BITS(laddr)]);
                  // Check for & ignore uncached pages.
                  valid = !(pageTable[ADDR_PTE_BITS(laddr)] & PTE_PCD);
               }
               PT_ReleasePageTable(pageTable, pairTable);
            }
         }
      }
      PT_ReleasePageDir(pageDir, pairDir);
   }

   if (valid) {
      KSEG_Pair *pair;
      void *data = Kseg_MapMPN(mpn, &pair);
      ASSERT(data);
      memcpy(outBuf, data, PAGE_SIZE);
      Kseg_ReleasePtr(pair);
      LOG(5, "%#x mpn is %#x", laddr, mpn);
   }
   return valid;
}


/*
 *----------------------------------------------------------------------
 *
 * CosDumpCopyFromHost --
 *
 *      Slower version of CopyFromHost that can be run from any world.
 *
 *      Caller must supply a valid cr3 for the console os.    (This
 *      usually means that the console os cannot be running while this
 *      function is called).
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
CosDumpCopyFromHost(void *dst, VA src, uint32 length, MA cr3)
{
   Bool valid = FALSE;
   char *buffer;

   if (src < VMNIX_KVA_START || src >= VMNIX_KVA_END) {
      return FALSE;
   }

   buffer = Mem_Alloc(PAGE_SIZE);
   if (!buffer) {
      return FALSE;
   }

   while(length > 0) {
      int nBytes;
      if (!(valid = CosDumpGetCosVPNContents(VA_2_VPN(src), cr3, buffer))) {
         break;
      }
      nBytes = MIN(PAGE_SIZE - ADDR_PGOFFSET_BITS(src), length);
      LOG(5, "dst: %p, src: %x, buf: %p, buf+off: %p, nBytes: %#x", dst, src, 
          buffer, (buffer + ADDR_PGOFFSET_BITS(src)), nBytes);
      memcpy(dst, buffer + ADDR_PGOFFSET_BITS(src), nBytes);
      src += nBytes;
      dst += nBytes;
      length -= nBytes;
   }

   Mem_Free(buffer);
   return valid;
}

/*
 *----------------------------------------------------------------------
 *
 * CosDumpGetChar --
 *
 *      Returns a single character from the console os's virtual address
 *      space.  Used by the debugger after we have hit a oops/panic in
 *      the console os (and therefore have a valid cos cr3 cached away).
 *
 *      Will work from any cpu/world.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CosDumpGetChar(void *addr)
{
   char ch = '\0';

   CosDumpCopyFromHost(&ch, (VA)addr, 1, dumpInfo.hostCR3);
   return ch;
}


/*
 *----------------------------------------------------------------------
 *
 * CosDumpMemory
 *      
 *      Writes out the console kernel virtual address space to the
 *      dump file.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CosDumpMemory(FS_FileHandleID hid, uint32 fOffset, MA hostCR3)
{
   uint32 totalBytesWritten = 0;
   VMK_ReturnStatus status = VMK_OK;
   char *buffer;
   VA vaddr;

   buffer = Mem_Alloc(PAGE_SIZE);
   if (!buffer) {
      return VMK_NO_MEMORY;
   }

   Log("Starting memory dump");
   for (vaddr = VMNIX_KVA_START; vaddr < VMNIX_KVA_END; vaddr += PAGE_SIZE) {
      if (vaddr % (PAGE_SIZE * 256) == 0) {
         Serial_Printf(".");
      }
      if (CosDumpGetCosVPNContents(VA_2_VPN(vaddr), hostCR3, buffer)) {
         uint32 bytesWritten = 0;
         status = FSS_BufferIO(hid, (vaddr - VMNIX_KVA_START) + fOffset, 
                               (uint64)(unsigned long)buffer, PAGE_SIZE, 
                               FS_WRITE_OP, SG_VIRT_ADDR, &bytesWritten);
         if (status != VMK_OK || bytesWritten != PAGE_SIZE) {
            Warning("Write @%#x failed with %#x (written = %d)", 
                    vaddr - VMNIX_KVA_START, status, bytesWritten);
            break;
         }
         totalBytesWritten += PAGE_SIZE;
      }
   }
   Log("Done Dumping memory: bytesWritten = %d", totalBytesWritten);
   Mem_Free(buffer);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CosDumpElfHdr --
 *      
 *      Writes the elf hdr prepared by vmnixmod to the core file.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CosDumpElfHdr(FS_FileHandleID hid, VA hdr, MA cr3)
{
   VMK_ReturnStatus status = VMK_FAILURE;
   uint32 bytesWritten;
   void *buf;

   if (hdr == 0) {
      return VMK_BAD_PARAM;
   }

   buf = Mem_Alloc(PAGE_SIZE);
   if (!buf) {
      return VMK_NO_MEMORY;
   }

   if (CosDumpCopyFromHost(buf, hdr, PAGE_SIZE, cr3)) {

      status = FSS_BufferIO(hid, 0, (uint64)(unsigned long)buf, PAGE_SIZE,
                            FS_WRITE_OP, SG_VIRT_ADDR, &bytesWritten);

      if (status == VMK_OK && bytesWritten != PAGE_SIZE)  {
         status = VMK_IO_ERROR;
         LOG(0, "Status = VMK_OK, but %d != %d", bytesWritten, PAGE_SIZE);
      }
   }
   Mem_Free(buf);
   return status;
}



/*
 *----------------------------------------------------------------------
 *
 * CosDumpCoreHelper --
 *      
 *      Writes out log buffer, memory, and register state to a file in /vmfs.
 *
 *      TODO: replace CONFIG_COS_COREFILE with logic that automagically
 *      finds a suitable vmfs partition.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CosDumpCoreHelper(void *data, void **result)
{
   struct CosDumpHelperInfo *dhi = (struct CosDumpHelperInfo *)data;
   char *fName = Config_GetStringOption(CONFIG_COS_COREFILE);
   VMK_ReturnStatus status;
   FS_FileHandleID hid;
   uint32 offset = PAGE_SIZE;

   if (strcmp(fName, "") == 0) {
      //Return early, because even trying to open a file may result in hitting
      //the shared interrupt problem.
      return VMK_OK;
   }
   Log("Dumping core to '%s'", fName);

   //XXX sets wrong permissions & doesn't seem to truncate an existing file.
   status = FSS_OpenFilePath(fName, FILEOPEN_WRITE,  &hid);
   if (status == VMK_OK) {
      if ((status = CosDumpMemory(hid, offset, dhi->hostCR3) == VMK_OK)) {
         if ((status = CosDumpElfHdr(hid, dhi->hdr, dhi->hostCR3)) != VMK_OK) {
            Warning("Error dumping elf hdr: %#x", status);
         }
      } else {
         Warning("Error dumping console memory: %#x", status);
      }
      FSS_CloseFile(hid);
   } else {
      Warning("Failed to open file %s: %#x", fName, status);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CosDump_Core --
 *      
 *      Fires off a helper request, and then waits for the the dump 
 *      to complete.  
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CosDump_Core(MA hostCR3, VA hdr)
{
   Helper_RequestHandle handle;
   VMK_ReturnStatus status;
   int i = 0;

   dumpInfo.hostCR3 = hostCR3;
   dumpInfo.hdr = hdr;
   Debug_SetCosGetCharFn(CosDumpGetChar);
   handle = Helper_RequestSync(HELPER_MISC_QUEUE, CosDumpCoreHelper, 
                               &dumpInfo, NULL, 0, NULL);

   if (handle != HELPER_INVALID_HANDLE) {
      Log("Waiting for core dump request to complete");
      while ((status = Helper_RequestStatus(handle)) == VMK_STATUS_PENDING) {
         CpuSched_Sleep(1000);
         if (i++ > COS_COREDUMP_TIMEOUT) {
            status = VMK_TIMEOUT;
            break;
         }
      }
      Log("Done waiting");
   } else {
      status = VMK_FAILURE;
   }
   
   if (status != VMK_OK) {
      Warning("Helper request failed: %#x", status);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CosDump_LogBufferInt  --
 *
 *      Writes out the console os log buffer to either the vmkernel log
 *      or a PSOD.
 *
 *      The printk buffer is a ring buffer.  logEnd is offset the next
 *      characters will be written (and therefore the offset of the
 *      oldest data in the log).
 *
 *      This code can be called from any world, so long as the cr3
 *      passed in remains valid.
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
CosDump_LogBufferInt(const VA hostLogBuf, uint32 logEnd, uint32 logBufLen, 
                     uint32 maxDumpLen, MA cr3, Bool bt2psod)
{
   uint32 bufSize = 200;
   uint32 i, copyChunk = bufSize - 1;
   uint32 dumpLength = logBufLen;
   char * buf;
   Bool stackTraceFound = FALSE;

   buf = Mem_Alloc(bufSize);
   if (!buf) {
      return VMK_NO_MEMORY;
   }

   if (maxDumpLen > 0 && logBufLen > maxDumpLen) {
      /*
       * Caller only wants to dump last maxDumpLen bytes of log buffer
       */
      logEnd += logBufLen - maxDumpLen;
      dumpLength = maxDumpLen;
   }

   Log("Dumping COS log buffer (logEnd = %d, logBufLen = %d, dumpLen = %d):", 
       logEnd, logBufLen, dumpLength);
   for (i = 0; i < dumpLength;) {
      uint32 dumpStart = ((i + logEnd) % logBufLen);
      int copyLen = MIN(logBufLen - dumpStart, copyChunk);

      copyLen = MIN(copyLen, dumpLength - i);

      CosDumpCopyFromHost(buf, hostLogBuf + dumpStart, copyLen, cr3);
      buf[copyLen] = '\0';
      if (bt2psod){
         char *start = NULL;
         char *searchStr;
         /*
          * Note: if we get unlucky, the strings we're searching for won't
          * be contained within one buffer, and no ouput will be done.  
          * I can live with that.
          */
         if (vmx86_debug) {
            searchStr = "Smart ";
         } else {
            //On release build no smart stack trace, so try to show full oops
            searchStr = "<4>EIP:";
         }
         if (!stackTraceFound && ((start = simple_strstr(buf, searchStr)) != NULL)) {
            stackTraceFound = TRUE;
         }
         if (stackTraceFound) {
            BlueScreen_Append(start? start : buf);
         }
      } else {
         _Log("%s", buf);
      }
      i += copyLen;
   }
   Log("Done w/ COS log buffer");

   Mem_Free(buf);
   return VMK_OK;
}
 
 
/*
 *----------------------------------------------------------------------
 *
 * CosDump_LogBuffer  --
 *
 *      Dumps cos log buffer to vmkernel log.
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
CosDump_LogBuffer(const VA hostLogBuf, uint32 logEnd, uint32 logBufLen, 
                  uint32 maxDumpLen, MA cr3)
{
   return CosDump_LogBufferInt(hostLogBuf, logEnd, logBufLen, maxDumpLen, cr3, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * CosDump_BacktraceToPSOD  --
 *
 *      Dumps relevant portions of the cos log buffer to psod.
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
CosDump_BacktraceToPSOD(const VA hostLogBuf, uint32 logEnd, uint32 logBufLen, 
                        uint32 maxDumpLen, MA cr3)
{
   BlueScreen_Append("\nStack trace from cos log:\n");
   return CosDump_LogBufferInt(hostLogBuf, logEnd, logBufLen, maxDumpLen, cr3, TRUE);
}

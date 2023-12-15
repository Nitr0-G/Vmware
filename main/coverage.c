/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/
#ifndef VMX86_DEVEL

#endif

#include "vm_types.h"
#include "x86.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "proc.h"
#include "memalloc.h"

static void CoverageProcInit(void);
static int CoverageProcRead(Proc_Entry *entry, char *page, int *len);
static int CoverageProcWrite(Proc_Entry *entry, char *page, int *len);

uint32 *coverageCounters;
uint32 numCoverageCounters = 0;
uint32 coverageCounterSize;
Proc_Entry coverageProcNode;

void
Coverage_Init(uint32 nCounters)
{
   uint32 *cBuf;

   coverageCounterSize = numPCPUs*((nCounters + 31)/32)*sizeof(uint32);

   // Malloc space for the counters
   cBuf = (uint32 *)Mem_Alloc(coverageCounterSize);
   if (cBuf == 0) {
      return;
   }
   memset(cBuf, 0, (coverageCounterSize));
   
   coverageCounters = cBuf;
   numCoverageCounters = nCounters;
   // Set up the proc node
   CoverageProcInit();
}

/*
 * The next few routines provide support for the /proc/vmware/coverage entry
 */


/*
 *----------------------------------------------------------------------
 *
 *  CoverageProcInit--
 *
 *       Sets up the /proc/vmware/coverage entry
 *
 * Results:
 *      none
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static void
CoverageProcInit(void)
{
   char *buf = "coverage";

   coverageProcNode.read = CoverageProcRead;
   coverageProcNode.write = CoverageProcWrite;
   coverageProcNode.parent = NULL;
   coverageProcNode.private = NULL;
   Proc_Register(&coverageProcNode, buf, TRUE);
}



/*
 *----------------------------------------------------------------------
 *
 * CoverageProcRead --
 *
 *       handles read to the /proc/vmware/coverage entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static int
CoverageProcRead(UNUSED_PARAM(Proc_Entry  *entry),
                 char        *page,
                 int         *len)
{
   uint32 i;

   Proc_Printf(page, len, "%u %u\n", numCoverageCounters, numPCPUs);
   for (i=0; i<coverageCounterSize; i++) {
      Proc_Printf(page, len, " %u", coverageCounters[i]);
      if (i%8 == 7) {
         Proc_Printf(page, len, "\n");
      }
   }
   return(VMK_OK);
}





/*
 *----------------------------------------------------------------------
 *
 * CoverageProcWrite --
 *
 *       handles write to the /proc/vmware/coverage entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static int
CoverageProcWrite(UNUSED_PARAM(Proc_Entry  *entry),
                  char        *page,
                  int         *len)
{
   if (!strncmp(page, "reset", 5)) {
      // Reset all the counters
      memset(coverageCounters, 0, (coverageCounterSize));
   }
 
   return(VMK_OK);
}




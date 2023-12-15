/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * bluescreen.c - blue screen, debugging shell, etc
 */

#include "vm_version.h"
#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "timer.h"
#include "debug.h"
#include "util.h"
#include "alloc.h"
#include "bh.h"
#include "kseg.h"
#include "libc.h"
#include "config.h"
#include "proc.h"
#include "vmkstats.h"
#include "kvmap.h"
#include "prda.h"
#include "world_ext.h"
#include "bluescreen.h"
#include "dump.h"
#include "idt.h"
#include "mod_loader.h"
#include "log_int.h"
#include "memalloc.h"
#include "term.h"

char const buildVersion[] = BUILD_VERSION;

#define LOGLEVEL_MODULE BlueScreen
#include "log.h"

#define LINE_WIDTH 80

#define KEYBOARD_CMD_PORT   0x64
#define KEYBOARD_CMD_RESET  0xfe

// how many recursive PSODs to show on screen
#define MAX_PSOD_LEVEL_ON_SCREEN 2

/*
 * This structure holds the private state of the bluescreen module.
 */

static struct BlueScreen {
   Bool posted;
   uint32 term;
   uint32 numRows;
   uint32 numCols;
} bs = {FALSE, TERM_INVALID};

static const Term_AllocArgs blueScreenArgs =
                     {TRUE, FALSE, {ANSI_WHITE, ANSI_MAGENTA, TRUE, 0},
                      TERM_INPUT_NONE, NULL, NULL, NULL, 0};

static void BlueScreenClear(void);
static void BlueScreenReset(int seconds);
static void BlueScreenPrint(const char *fmt, ...);


/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_Init --
 *
 *      Initialize the blue screen module.
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
BlueScreen_Init(void)
{
   /*
    * Setup bluescreen terminal
    */
   ASSERT(bs.term == TERM_INVALID);
   bs.term = Term_Alloc(&blueScreenArgs, &bs.numRows, &bs.numCols);
   ASSERT_NOT_IMPLEMENTED(bs.term != TERM_INVALID);
   ASSERT_NOT_IMPLEMENTED(bs.numCols == LINE_WIDTH);
}


/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_Post --
 *
 *      Post a bluescreen, probably an assertion failure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
BlueScreen_Post(const char *text, VMKFullExcFrame *fullFrame)
{
   char buf[LINE_WIDTH];
   const char *worldName;
   VA cr2;
   uint32 cr3, cr4;
   Bool wpEnabled;
   static int PSODLevel = 0;

   if (bs.term == TERM_INVALID) {
      return FALSE;
   }

   if (!fullFrame) {
      return FALSE;
   }

   Panic_MarkCPUInPanic();
   if (bs.posted) {
      PSODLevel++;
      SysAlert("PSOD level %d: %s ra=%p", PSODLevel, text,
               __builtin_return_address(0));
      if (PSODLevel <= MAX_PSOD_LEVEL_ON_SCREEN) {
         BlueScreen_Append(text);
      }
      return FALSE;
   }
   bs.posted = TRUE;

   if ((PRDA_GetRunningWorldSafe() != NULL) && 
       World_IsVMMWorld(PRDA_GetRunningWorldSafe())) {
      World_ResetDefaultDT();
   }

   wpEnabled = Watchpoint_ForceDisable();

   BlueScreenClear();

   BlueScreen_On();

   BlueScreen_Append(text);

   GET_CR2(cr2);
   GET_CR3(cr3);
   GET_CR4(cr4);
   snprintf(buf, sizeof buf, "gate=0x%x frame=%p eip=0x%x cr2=0x%x cr3=0x%x cr4=0x%x\n", 
            fullFrame->frame.u.in.gateNum,
            fullFrame, fullFrame->frame.eip, cr2, cr3, cr4);
   BlueScreen_Append(buf);
   snprintf(buf, sizeof buf, "eax=%#x ebx=%#x ecx=%#x edx=%#x es=%#x ds=%#x\n",
            fullFrame->regs.eax, fullFrame->regs.ebx,
            fullFrame->regs.ecx, fullFrame->regs.edx, 
            fullFrame->regs.es, fullFrame->regs.ds);
   BlueScreen_Append(buf);
   snprintf(buf, sizeof buf, "fs=%#x gs=%#x ebp=%#x esi=%#x edi=%#x err=%d ef=%#x\n",
            fullFrame->regs.fs, fullFrame->regs.gs,
            fullFrame->regs.ebp, fullFrame->regs.esi,
            fullFrame->regs.edi, fullFrame->frame.errorCode,
            fullFrame->frame.eflags);
   BlueScreen_Append(buf);

   if (PRDA_IsInitialized()) {
      PCPU i;
      // report worlds running on each processor
      for (i=0; i<numPCPUs; i++) {
         World_ID worldID;
         int j = i+1;

         if (prdas[i] && prdas[i]->runningWorld) {
            worldName = prdas[i]->runningWorld->worldName;
            worldID = prdas[i]->runningWorld->worldID;
         } else {
            worldName = "<NULL>";
            worldID = INVALID_WORLD_ID;
         }
         snprintf(buf, sizeof(buf), "%s %d %d %.9s: ", 
                 (myPRDA.pcpuNum == i) ? "CPU" : "cpu", i, worldID, worldName);
         BlueScreen_Append(buf);
         if ((j % 4) == 0) {
            BlueScreen_Append("\n");
         }
      }
      if ((i % 4) != 0) {
         BlueScreen_Append("\n");
      }
   }

   // report backtrace

   _Log("@BlueScreen: %s", text);
   Util_Backtrace(fullFrame->frame.eip, fullFrame->regs.ebp, BlueScreenPrint, FALSE);

   // report vmkernel uptime
   BlueScreen_Append("VMK uptime: ");
   Util_FormatTimestamp(buf, sizeof(buf));
   BlueScreen_Append(buf);
   snprintf(buf, sizeof buf, " TSC: %"FMT64"d\n", RDTSC());
   BlueScreen_Append(buf);

   // if the kernel checksum has been initialized,
   // check for corruption in main vmkernel code region;
   //   compute checksum and compare with expected value
   if (MemRO_GetChecksum()) {
      uint64 checksum = MemRO_CalcChecksum();
      if (checksum != MemRO_GetChecksum()) {
	 snprintf(buf, sizeof buf, "VMK checksum BAD: 0x%Lx 0x%Lx\n",
			checksum, MemRO_GetChecksum());
	 // report checksum results to bluescreen, log
	 BlueScreen_Append(buf);
      }
   }

#ifdef VMX86_DEBUG
   if (vmkernelLoaded) {
      snprintf(buf, sizeof buf, "lastClrIntrRA = 0x%x\n", (uint32)myPRDA.lastClrIntr);
      BlueScreen_Append(buf);
   }
#endif

   Log_PrintSysAlertBuffer(BlueScreen_Append, 5);
      
   if (!CONFIG_OPTION(MINIMAL_PANIC)) {
      // dump vmkernel core and log files
      Dump_Dump(fullFrame);
   }

   if (CONFIG_OPTION(BLUESCREEN_TIMEOUT)) {
      Warning("reseting after %d seconds (%Ld cycles)",
              CONFIG_OPTION(BLUESCREEN_TIMEOUT),
              1000ull * cpuKhzEstimate);
      BlueScreenReset(CONFIG_OPTION(BLUESCREEN_TIMEOUT));
   }

   if (wpEnabled) {
      Watchpoint_ForceEnable();
   }

   Mod_ListPrint();

   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_Append --
 *
 *      Append a message to the bluescreen. Does not show the
 *      blue screen if its not up yet.
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
BlueScreen_Append(const char *text)
{
   _Log("%s", text);
   if (bs.term != TERM_INVALID) {
      Term_Printf(bs.term, 0, "%s", text);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * BlueScreenClearScreen --
 *
 *      Clear the screen and reset the cursor.
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
BlueScreenClear(void)
{
   Term_Clear(bs.term, 0, NULL);

   /*
    * Set background color again for the sake of external terminals
    * (getting the log output) where yellow may be unreadable on default
    * background.
    */
   BlueScreen_Append(ANSI_ATTR_SEQ_BACK_MAGENTA);
   BlueScreen_Append(ANSI_ATTR_SEQ_FORE_YELLOW_BRIGHT PRODUCT_NAME " [");
   BlueScreen_Append(buildVersion);
   BlueScreen_Append("]" ANSI_ATTR_SEQ_RESET "\n"); 
}


/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_On --
 *
 * 	Turn on the blue screen.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */

void
BlueScreen_On(void)
{
   if (bs.term != TERM_INVALID) {
      Term_Display(bs.term);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * BlueScreenReset --
 *
 *      Reset the machine after desired number of seconds.
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
BlueScreenReset(int seconds)
{
   char buf[16];
   int i;
   
   BlueScreen_Append("Resetting machine... ");

   for (i=seconds; i>=0; i--) {
      snprintf(buf, sizeof buf, "%d ", i);
      BlueScreen_Append(buf);
      Util_Udelay(1000000);
   }

   OUTB(KEYBOARD_CMD_PORT, KEYBOARD_CMD_RESET);
}

/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_PostException --
 *
 *      Post a bluescreen for an exception.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A bluescreen may be posted.
 *
 *----------------------------------------------------------------------
 */
void 
BlueScreen_PostException(VMKFullExcFrame *fullFrame)
{
   char buffer[256];

   Panic_MarkCPUInPanic();
   snprintf(buffer, sizeof buffer,
	    "Exception type %d in world %d:%.12s @ 0x%x\n",
	    fullFrame->frame.u.in.gateNum, PRDA_GetRunningWorldIDSafe(),
	    PRDA_GetRunningWorldNameSafe(), fullFrame->frame.eip);
   BlueScreen_Post(buffer, fullFrame);
}

/*
 *----------------------------------------------------------------------
 *
 * BlueScreenPrint --
 *
 *      Do a formatted print to the blue screen.
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
BlueScreenPrint(const char *fmt, ...)
{
   int i;
   char buffer[128];
   va_list args;

   va_start(args, fmt);
   i = vsnprintf(buffer, sizeof buffer, fmt, args);
   va_end(args);

   if (i >= 128) {
      Warning("Formatted string too long");
   } else {
      BlueScreen_Append(buffer);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * BlueScreen_Posted --
 *
 *      Return if a bluescreen has been posted or not.
 *
 * Results:
 *      TRUE if bluescreen has been posted.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
BlueScreen_Posted(void)
{
   return bs.posted;
}

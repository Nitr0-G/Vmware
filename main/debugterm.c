/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * debugterm.c --
 *
 * 	Operations of the terminal dedicated to debug
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "term.h"
#include "debugterm.h"
#include "logterm.h"

#define LOGLEVEL_MODULE DebugTerm
#define LOGLEVEL_MODULE_LEN 9
#include "log.h"


#define DEBUGTERM_RESET_PORT	0x64
#define DEBUGTERM_RESET_CMD	0xfe

#define DEBUGTERM_PROMPT	"VMKDBG> "


static uint32 debugTerm = TERM_INVALID;
static Bool debugBluescreen = FALSE;
static volatile Bool debugDone = FALSE;


static void DebugTermInputCallback(const char *txt);

static const Term_AllocArgs debugTermArgs =
                     {FALSE, TRUE, {ANSI_WHITE, ANSI_BLACK, FALSE, 0},
                      TERM_INPUT_ASYNC_LINE, DebugTermInputCallback,
                      NULL, NULL, 0};


/*
 *-----------------------------------------------------------------------------
 *
 * Debugterm_Init --
 *
 * 	Initialize debug terminal module
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
void
DebugTerm_Init(void)
{
   uint32 numRows;
   uint32 numCols;

   /*
    * Setup debug terminal
    */
   ASSERT(debugTerm == TERM_INVALID);
   debugTerm = Term_Alloc(&debugTermArgs, &numRows, &numCols);
   ASSERT_NOT_IMPLEMENTED(debugTerm != TERM_INVALID);

   /*
    * Display greetings.
    */
   Term_Clear(debugTerm, 0, NULL);
   Term_Printf(debugTerm, 0, ANSI_ATTR_SEQ_REVERSE
			     "vmkernel debugger (h for help)\n"
			     ANSI_ATTR_SEQ_RESET);

   /*
    * Display prompt.
    */
   Term_Printf(debugTerm, 0, DEBUGTERM_PROMPT);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Debugterm_DisplayForBluescreen --
 *
 * 	Display debug terminal and start accepting commands
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
void
DebugTerm_DisplayForBluescreen(void)
{
   /*
    * If the terminal has not been init'ed yet, return immediately.
    */
   if (debugTerm == TERM_INVALID) {
      return;
   }

   /*
    * Bring terminal on screen.
    */
   Term_Display(debugTerm);

   /*
    * Ask for input poll since interrupts are disabled.
    * Term_PollInput will return when debugDone is TRUE so it needs to be
    * set to that value eventually by this module.
    */
   debugBluescreen = TRUE;
   debugDone = FALSE;
   Term_PollInput(&debugDone);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugTermHelp --
 *
 * 	Display help
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static void
DebugTermHelp(Bool bluescreen)
{
   Term_Printf(debugTerm, 0,
		"h      - help\n"
		"r      - reboot\n"
		"l      - display vmkernel log\n"
		"%s",
		bluescreen ?
		"c      - close debug terminal\n" :
		"Alt-Func to switch back to another terminal\n");
}


/*
 *----------------------------------------------------------------------
 *
 * DebugTermInputCallback --
 *
 * 	Callback on input events
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	Command is processed and display changed accordingly
 *
 *----------------------------------------------------------------------
 */
static void
DebugTermInputCallback(const char *txt)
{
   // Process the command
   switch (*txt) {
   case 'r':
      OUTB(DEBUGTERM_RESET_PORT, DEBUGTERM_RESET_CMD);
      break;
   case 'l':
      LogTerm_DisplayForBluescreen();
      Term_Display(debugTerm);
      break;
   case 'h':
      DebugTermHelp(debugBluescreen);
      break;
   case 'c':
      if (debugBluescreen) {
	 debugDone = TRUE;
      }
      break;
   default:
      break;
   }

   // Display prompt.
   Term_Printf(debugTerm, 0, DEBUGTERM_PROMPT);
}

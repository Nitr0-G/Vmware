/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * statusterm.c --
 *
 * 	Operations of the terminal dedicated to status
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_version.h"
#include "vmkernel.h"
#include "term.h"
#include "statusterm.h"
#include "log_int.h"
#include "logterm.h"

#define LOGLEVEL_MODULE StatusTerm
#include "log.h"


static uint32 statusTerm = TERM_INVALID;
static const Term_AllocArgs statusTermArgs =
                     {FALSE, TRUE, {ANSI_WHITE, ANSI_BLACK, FALSE, 0},
                      TERM_INPUT_NONE, NULL,
                      NULL, NULL, TERM_ALT_FN_FOR_STATUS};
static const Ansi_Attr statusTermAlertAnsiAttr = {ANSI_RED, ANSI_BLACK, TRUE,0};
static char const buildVersion[] = BUILD_VERSION;
static char hostName[VMNIX_HOSTNAME_LENGTH];
static VMnix_ScreenUse screenUse;
static Bool showProgress;

/*
 * The status terminal is divided into two parts
 * 	a banner/status window at the top
 * 	an alert window at the bottom
 */
#define STATUSTERM_BANNER_WINDOW	0
#define STATUSTERM_ALERT_WINDOW		1

#define STATUSTERM_NUM_ALERT_ROWS	10


static void StatusTermPrintAlerts(void);
static void StatusTermPrintHeading(void);
static void StatusTermPrintGreetings(void);



/*
 *-----------------------------------------------------------------------------
 *
 * StatusTerm_Init --
 *
 * 	Initialize status terminal module
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
StatusTerm_Init(VMnix_ScreenUse vmnixScreenUse) 
{
   uint32 numRows;
   uint32 numCols;

   screenUse = vmnixScreenUse;
   showProgress = TRUE;

   /*
    * We don't know the hostname yet; it will be set through 
    * /proc/vmware/config. (StatusTerm_HostNameCallback)
    */
   strncpy(hostName, "unknown", sizeof(hostName));

   /*
    * Setup status terminal
    *
    * Note that we do not register on/off screen callbacks because the 
    * status screen is very static (only alerts may appear and hopefully
    * it is a rare occurrence), so there is no point in stopping output
    * when the terminal goes off screen and in refreshing when it comes
    * back on screen. Output is always enabled.
    */
   ASSERT(statusTerm == TERM_INVALID);
   statusTerm = Term_Alloc(&statusTermArgs, &numRows, &numCols);
   ASSERT_NOT_IMPLEMENTED(statusTerm != TERM_INVALID);

   /*
    * The first window is used for banner/status.
    * It is split to create the alert window at the bottom of the terminal.
    */
   ASSERT(STATUSTERM_BANNER_WINDOW == 0);
   Term_Split(statusTerm, STATUSTERM_BANNER_WINDOW,
	      STATUSTERM_NUM_ALERT_ROWS, FALSE, &statusTermAlertAnsiAttr,
              FALSE, TRUE);

   /*
    * Print main heading and alerts that could have happened already.
    */
   StatusTermPrintHeading();
   StatusTermPrintAlerts();

   /*
    * Bring up the appropriate terminal on screen as needed
    */
   if (screenUse == VMNIX_SCREEN_STATUS) {
      Term_Display(statusTerm);
   } else {
      LogTerm_Display();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * StatusTerm_StopShowingProgress --
 *
 *      Callback informing vmkernel to stop displaying progress on the
 *      status screen and have the greetings up instead.
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
StatusTerm_StopShowingProgress(Bool write, Bool changed, int idx)
{
   /*
    * This is used as the cue to stop showing progress for now. XXX
    */
   if (write && showProgress) {
      StatusTermPrintGreetings();
      if (screenUse != VMNIX_SCREEN_LOG) {
         Term_Display(statusTerm);
      } else {
         LogTerm_Display();
      }
      showProgress = FALSE;
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StatusTerm_HostNameCallback --
 *
 *      Config option callback for hostname. 
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      Sets hostname. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
StatusTerm_HostNameCallback(Bool write, Bool changed, int idx)
{
   if (write && changed) {
      strncpy(hostName, Config_GetStringOption(CONFIG_HOSTNAME), 
              sizeof(hostName));
      // Update greetings if up
      if (!showProgress) {
         showProgress = TRUE; // enable output
         StatusTermPrintGreetings();
         showProgress = FALSE;
      }
   }
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * StatusTermPrintAlerts --
 *
 *    Display all alerts that happened already
 *
 * Results:
 *    None
 *
 * Side Effects:
 *    None
 *
 *----------------------------------------------------------------------
 */
static void
StatusTermPrintAlerts(void)
{
   Log_PrintSysAlertBuffer(StatusTerm_PrintAlert, STATUSTERM_NUM_ALERT_ROWS);
}


/*
 *-----------------------------------------------------------------------------
 *
 * StatusTermPrintHeading --
 *
 *      Clear banner window and display heading
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
static void
StatusTermPrintHeading(void)
{
   Term_Clear(statusTerm, STATUSTERM_BANNER_WINDOW, NULL);
   StatusTerm_Printf(
        "\t\t"
        ANSI_ATTR_SEQ_FORE_CYAN_BRIGHT "VMware ESX Server "
        ANSI_ATTR_SEQ_RESET "version %s\n\n", buildVersion);
}


/*
 *-----------------------------------------------------------------------------
 *
 * StatusTermPrintGreetings --
 *
 *	Clear banner window and display greetings
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
static void
StatusTermPrintGreetings(void)
{
   StatusTermPrintHeading();

   StatusTerm_Printf(
	"\tTo access the virtual machines on the system, please go to\n"
	"\tanother machine and point a Web browser to the following URL:\n\n");

   StatusTerm_Printf(
	ANSI_ATTR_SEQ_BRIGHT "\t   http://%s/\n\n" ANSI_ATTR_SEQ_RESET,
	hostName);

   StatusTerm_Printf(
	"\tTo get direct shell access to the "
	ANSI_ATTR_SEQ_FORE_RED_BRIGHT "Service Console" ANSI_ATTR_SEQ_RESET
	", you may\n"
	"\tpress Alt-F1 to switch to a virtual terminal where you may\n"
        "\tlog in.  To come back to this screen, press Alt-F11.\n\n");

   StatusTerm_Printf(
	"\tFor more information see the on-line documentation at\n\n"
	ANSI_ATTR_SEQ_BRIGHT "\t   http://www.vmware.com/support/"
	ANSI_ATTR_SEQ_RESET);
}


/*
 *----------------------------------------------------------------------
 *
 * StatusTerm_Printf --
 *
 * 	Print formatted string in the status window
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
StatusTerm_Printf(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   if (showProgress) {
      Term_PrintfVarArgs(statusTerm, STATUSTERM_BANNER_WINDOW, fmt, args);
   }
   va_end(args);
}


/*
 *----------------------------------------------------------------------
 *
 * StatusTerm_PrintAlert --
 *
 * 	Print a message in the alert window
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
StatusTerm_PrintAlert(const char *message)
{
   if (statusTerm == TERM_INVALID) {
      // Alert happened before StatusTerm_Init(), we'll catch it later
      return;
   }
   Term_Printf(statusTerm, STATUSTERM_ALERT_WINDOW, "%s", message);
}

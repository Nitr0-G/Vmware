/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * logterm.c --
 *
 * 	Operations of the terminal dedicated to log
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "config.h"
#include "log_int.h"
#include "vga.h"
#include "term.h"
#include "logterm.h"

// Be careful about Log as some of the routines are used indirectly by it
#define LOGLEVEL_MODULE LogTerm
#include "log.h"


static Bool logTermOn = FALSE;
static uint32 logTerm = TERM_INVALID;
static const Ansi_Attr logTermLogAnsiAttr = {ANSI_WHITE, ANSI_BLACK, FALSE, 0};
static Bool logTermScrollBack = FALSE;
static Bool logBluescreen = FALSE;
static volatile Bool logDone = FALSE;
static const char logtermInvalidCharInBuffer[] =
                        ANSI_ATTR_SEQ_REVERSE
                        "Invalid characters in buffer"
                        ANSI_ATTR_SEQ_RESET;


/*
 * The log terminal is divided into two parts
 * 	a banner/status window of one line
 * 	a log window of the remaining lines
 */
#define LOGTERM_STATUS_WINDOW	0
#define LOGTERM_LOG_WINDOW	1

#define LOGTERM_NUM_STATUS_ROWS	1
#define LOGTERM_MAX_LOG_ROWS	(VGA_NUM_ROWS*VGA_EXTENSION_FACTOR \
                                 -LOGTERM_NUM_STATUS_ROWS)

#define LOGTERM_BANNER		"\t\tvmkernel log (h for help)"
#define LOGTERM_BANNER_NO_INPUT	"\t\tvmkernel log (not interactive)"


/*
 * For each row of the log window, we record the entry currently displayed
 * and whether it is the start and/or the end of the entry.
 *
 * To avoid scrolling this structure, it is instead viewed as a circular
 * list for which we keep a pointer to the top row.
 */

#define LOGTERM_ENTRY_START	1
#define LOGTERM_ENTRY_END	2

typedef struct LogTermRow {
   uint32 entry;
   uint32 flags;
} LogTermRow;

static LogTermRow logTermRows[LOGTERM_MAX_LOG_ROWS];
static uint32 logTermTop;
static uint32 logTermNumRows;
static uint32 logTermNumCols;


static void LogTermOnScreen(void);
static void LogTermDisplayTail(void);
static void LogTermDisplayHead(void);
static Bool LogTermScrollAhead(Bool locked);
static Bool LogTermScrollBack(void);
static void LogTermInputCallback(const char *txt);


static INLINE void
LogTermPosInc(uint32 *pos)
{
   *pos = (*pos + 1) % logTermNumRows;
}
static INLINE void
LogTermPosDec(uint32 *pos)
{
   *pos = (*pos - 1 + logTermNumRows) % logTermNumRows;
}

static const Term_AllocArgs logTermArgs =
                     {TRUE, FALSE, {ANSI_BLACK, ANSI_WHITE, FALSE, 0},
                      TERM_INPUT_ASYNC_CHAR, LogTermInputCallback,
                      LogTermOnScreen, LogTerm_OffScreen, TERM_ALT_FN_FOR_LOG};



/*
 *-----------------------------------------------------------------------------
 *
 * LogTerm_Init --
 *
 * 	Initialize log terminal module
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
LogTerm_Init(void)
{
   /*
    * Setup log terminal
    *
    * We register on/off screen callbacks so that we don't waste time
    * outputting log messages if the log terminal is not on screen.
    * Log messages are not that common so it may not make sense.
    */
   ASSERT(logTerm == TERM_INVALID);
   logTerm = Term_Alloc(&logTermArgs, &logTermNumRows, &logTermNumCols);
   ASSERT_NOT_IMPLEMENTED(logTerm != TERM_INVALID);

   logTermNumRows -= LOGTERM_NUM_STATUS_ROWS;
   ASSERT(logTermNumRows <= LOGTERM_MAX_LOG_ROWS);

   /*
    * The first window is used for banner/status.
    * It is split to create the log window and left with only one line
    * at the top of the terminal.
    */
   ASSERT(LOGTERM_STATUS_WINDOW == 0);
   Term_Split(logTerm, LOGTERM_STATUS_WINDOW,
		logTermNumRows, FALSE, &logTermLogAnsiAttr, FALSE, TRUE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTerm_LateInit --
 *
 *      Late initialization for log terminal module
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
void
LogTerm_LateInit(void)
{
   /*
    * Update banner now that interrupts should be enabled.
    */
   ASSERT(Term_IsInputPossible());
   Term_Clear(logTerm, LOGTERM_STATUS_WINDOW, NULL);
   Term_Printf(logTerm, LOGTERM_STATUS_WINDOW, LOGTERM_BANNER);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTerm_Display --
 *
 *      Bring log terminal up as screen output
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
void
LogTerm_Display(void)
{
   /*
    * If the terminal has not been init'ed yet, return immediately.
    */
   if (logTerm == TERM_INVALID) {
      return;
   }

   /*
    * Bring terminal on screen.
    */
   Term_Display(logTerm);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Logterm_DisplayForBluescreen --
 *
 * 	Display tail end of log and start accepting commands
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
LogTerm_DisplayForBluescreen(void)
{
   /*
    * If the terminal has not been init'ed yet, return immediately.
    */
   if (logTerm == TERM_INVALID) {
      return;
   }

   /*
    * Bring terminal on screen.
    */
   Term_Display(logTerm);

   /*
    * Ask for input poll since interrupts are disabled.
    * Term_PollInput will return when logDone is TRUE so it needs to be
    * set to that value eventually by this module.
    */
   logBluescreen = TRUE;
   logDone = FALSE;
   Term_PollInput(&logDone);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermOnScreen --
 *
 * 	Callback when log terminal appears on screen
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	log terminal is refreshed
 *
 *-----------------------------------------------------------------------------
 */
static void
LogTermOnScreen(void)
{
   /*
    * Set up the banner.
    */
   Term_Clear(logTerm, LOGTERM_STATUS_WINDOW, NULL);
   if (logBluescreen || Term_IsInputPossible()) {
      Term_Printf(logTerm, LOGTERM_STATUS_WINDOW, LOGTERM_BANNER);
   } else {
      Term_Printf(logTerm, LOGTERM_STATUS_WINDOW, LOGTERM_BANNER_NO_INPUT);
   }

   /*
    * Get the tail end.
    */
   LogTermDisplayTail();

   /*
    * Allow new entries to be added and note there is no scroll back ongoing.
    */
   logTermScrollBack = FALSE;
   logTermOn = TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LogTerm_OffScreen --
 *
 *      Callback when log terminal disappears from screen
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      log output is stopped
 *
 *-----------------------------------------------------------------------------
 */
void
LogTerm_OffScreen(void)
{
   logTermOn = FALSE; // No new entries
}


/*
 *----------------------------------------------------------------------
 *
 * LogTerm_CatchUp --
 *
 *      Update screen log output with latest entries
 *      Only called from LogWarning() with logLck held.
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
LogTerm_CatchUp(void)
{
   if (!logTermOn) {
      return;
   }

   ASSERT(logTerm != TERM_INVALID);

   // No update if someone is scrolling around
   if (logTermScrollBack) {
      return;
   }

   // Scroll ahead till end of log to be sure it is on display
   while (LogTermScrollAhead(TRUE));
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermValidateEntry --
 *
 *      Compute the display length of an entry and sanitize it as needed
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
void
LogTermValidateEntry(char *buffer, uint32 *len, uint32 *displayLen, const char **txt)
{
   *displayLen = Term_Sizeb(logTerm, LOGTERM_LOG_WINDOW, buffer, *len);

   // If it contains a bad character, the entry is replaced by a warning
   if (*displayLen == (uint32)-1) {
      *txt = logtermInvalidCharInBuffer;
      *len = strlen(logtermInvalidCharInBuffer);
      *displayLen = Term_Sizeb(logTerm, LOGTERM_LOG_WINDOW, *txt, *len);
      ASSERT(*displayLen != (uint32)-1);
      ASSERT(*displayLen != 0);
      return;
   }

   // If it ends up not displaying anything, the entry is replaced by a space
   if (*displayLen == 0) {
      buffer[0] = ' ';
      *txt = buffer;
      *len = 1;
      *displayLen = 1;
      return;
   }

   *txt = buffer;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermDisplayTail --
 *
 * 	Display entries so that the latest one is at the bottom of
 * 	the screen
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
LogTermDisplayTail(void)
{
   uint32 logTermRow;
   uint32 entry;
   char buffer[VMK_LOG_ENTRY_SIZE];
   uint32 len = sizeof(buffer);
   uint32 displayLen;
   const char *txt;
   uint32 numRows;


   /*
    * Clear window and reset screen content tracking information.
    */
   Term_Clear(logTerm, LOGTERM_LOG_WINDOW, NULL);
   logTermTop = 0;
   memset(logTermRows, 0, logTermNumRows * sizeof(LogTermRow));

   /*
    * Get the latest entry.
    */
   Log_GetLatestEntry(&entry, buffer, &len);
   ASSERT(len != 0); // There should be at least one log entry by now

   /*
    * Make sure it is sane.
    */
   LogTermValidateEntry(buffer, &len, &displayLen, &txt);

   /*
    * Display it.
    */
   Term_Putb(logTerm, LOGTERM_LOG_WINDOW, txt, len);

   /*
    * Update screen content tracking information.
    */
   numRows = CEILING(displayLen, logTermNumCols);

   // First line of the new entry
   logTermRow = 0;
   logTermRows[logTermRow].entry = entry;
   logTermRows[logTermRow].flags = LOGTERM_ENTRY_START;

   // Subsequent lines of the new entry
   while (--numRows) {
      LogTermPosInc(&logTermRow);
      logTermRows[logTermRow].entry = entry;
      logTermRows[logTermRow].flags = 0;
   }

   // Last line of the new entry
   logTermRows[logTermRow].flags |= LOGTERM_ENTRY_END;

   /*
    * Scroll back at least a screen full to fill the display
    * and then scroll ahead till end of log to be sure it is
    * on display.
    */
   numRows = logTermNumRows;
   while (numRows-- && LogTermScrollBack());
   while (LogTermScrollAhead(FALSE));
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermDisplayHead --
 *
 * 	Display entries so that the earliest one is at the top of
 * 	the screen
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
LogTermDisplayHead(void)
{
   uint32 logTermRow;
   uint32 entry;
   char buffer[VMK_LOG_ENTRY_SIZE];
   uint32 len = sizeof(buffer);
   uint32 displayLen;
   const char *txt;
   uint32 numRows;


   /*
    * Clear window and reset screen content tracking information.
    */
   Term_Clear(logTerm, LOGTERM_LOG_WINDOW, NULL);
   logTermTop = 0;
   memset(logTermRows, 0, logTermNumRows * sizeof(LogTermRow));

   /*
    * Get the earliest entry.
    */
   Log_GetEarliestEntry(&entry, buffer, &len);
   ASSERT(len != 0); // There should be at least one log entry by now

   /*
    * Make sure it is sane.
    */
   LogTermValidateEntry(buffer, &len, &displayLen, &txt);

   /*
    * Display it.
    */
   Term_Putb(logTerm, LOGTERM_LOG_WINDOW, txt, len);

   /*
    * Update screen content tracking information.
    */
   numRows = CEILING(displayLen, logTermNumCols);

   // First line of the new entry
   logTermRow = 0;
   logTermRows[logTermRow].entry = entry;
   logTermRows[logTermRow].flags = LOGTERM_ENTRY_START;

   // Subsequent lines of the new entry
   while (--numRows) {
      LogTermPosInc(&logTermRow);
      logTermRows[logTermRow].entry = entry;
      logTermRows[logTermRow].flags = 0;
   }

   // Last line of the new entry
   logTermRows[logTermRow].flags |= LOGTERM_ENTRY_END;

   /*
    * Scroll ahead till the earliest entry moves offscreen
    * and then scroll back to be sure it is on display (it may not be though
    * because we may be racing with new entries being added and overwriting
    * the oldest ones.)
    */
   while ((logTermTop == 0) && LogTermScrollAhead(FALSE));
   if (logTermTop != 0) {
      LogTermScrollBack();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermScrollAhead --
 *
 * 	Display the next entry at the bottom
 *
 * Results:
 * 	TRUE if a new entry was displayed
 * 	FALSE if there was no new entry to display
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
static Bool
LogTermScrollAhead(Bool locked)
{
   uint32 row;
   uint32 logTermRow;
   char buffer[VMK_LOG_ENTRY_SIZE];
   uint32 len = sizeof(buffer);
   uint32 entry;
   uint32 displayLen;
   const char *txt;
   uint32 numRows;

   /*
    * Find the end of the last entry currently on screen.
    * We assume that there is at least one whole entry on screen.
    */
   row = logTermNumRows - 1; // Last line
   logTermRow = (logTermTop + row) % logTermNumRows;
   while (!(logTermRows[logTermRow].flags & LOGTERM_ENTRY_END)) {
      LogTermPosDec(&logTermRow);
      ASSERT(row > 0); // We should not wrap around
      row--;
   }

   /*
    * Get its successor.
    * XXX Check the case where Log_GetNextEntry returns FALSE because
    * ongoing log activity has caused entry to no longer be in the log buffer.
    */
   entry = logTermRows[logTermRow].entry;
   Log_GetNextEntry(&entry, buffer, &len, locked);
   if (len == 0) { // We are already at the end
      return FALSE;
   }

   /*
    * Make sure it is sane.
    */
   LogTermValidateEntry(buffer, &len, &displayLen, &txt);
 
   /*
    * Tack on the new entry on display starting after the current entry.
    */
   Term_InsertBelow(logTerm, LOGTERM_LOG_WINDOW, row + 1, txt, len);

   /*
    * Update screen content tracking information.
    */
   numRows = CEILING(displayLen, logTermNumCols);

   // First line of the new entry
   LogTermPosInc(&logTermRow);
   logTermRows[logTermRow].entry = entry;
   logTermRows[logTermRow].flags = LOGTERM_ENTRY_START;
   if (logTermRow == logTermTop) { // We have displaced the top
      LogTermPosInc(&logTermTop);
   }

   // Subsequent lines of the new entry
   while (--numRows) {
      LogTermPosInc(&logTermRow);
      logTermRows[logTermRow].entry = entry;
      logTermRows[logTermRow].flags = 0;
      if (logTermRow == logTermTop) { // We have displaced the top
	 LogTermPosInc(&logTermTop);
      }
   }

   // Last line of the new entry
   logTermRows[logTermRow].flags |= LOGTERM_ENTRY_END;

   // Extraneous lines that were cleared
   LogTermPosInc(&logTermRow);
   while (logTermRow != logTermTop) {
      logTermRows[logTermRow].entry = 0;
      logTermRows[logTermRow].flags = 0;
      LogTermPosInc(&logTermRow);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogTermScrollBack --
 *
 * 	Display the previous entry at the top 
 *
 * Results:
 * 	TRUE if a new entry was displayed
 * 	FALSE if there was no new entry to display
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
static Bool
LogTermScrollBack(void)
{
   uint32 row;
   uint32 logTermRow;
   char buffer[VMK_LOG_ENTRY_SIZE];
   uint32 len = sizeof(buffer);
   uint32 entry;
   uint32 displayLen;
   const char *txt;
   uint32 numRows;

   /*
    * Find the start of the first entry currently on screen.
    * We assume that there is at least one whole entry on screen.
    */
   row = 0; // First line
   logTermRow = logTermTop;
   while (!(logTermRows[logTermRow].flags & LOGTERM_ENTRY_START)) {
      LogTermPosInc(&logTermRow);
      ASSERT(row < logTermNumRows-1); // We shouldn't wrap around
      row++;
   }

   /*
    * Get its predecessor.
    */
   entry = logTermRows[logTermRow].entry;
   Log_GetPrevEntry(&entry, buffer, &len);
   if (len == 0) { // We are already at the beginning
      return FALSE;
   }

   /*
    * Make sure it is sane.
    */
   LogTermValidateEntry(buffer, &len, &displayLen, &txt);

   /*
    * Tack on the new entry on display up to the current entry.
    */
   Term_InsertAbove(logTerm, LOGTERM_LOG_WINDOW, row, txt, len, displayLen);

   /*
    * Update screen content tracking information.
    */
   numRows = CEILING(displayLen, logTermNumCols);

   // Last line of the new entry
   if (logTermRow == logTermTop) { // We have displaced the top
      LogTermPosDec(&logTermTop);
   }
   LogTermPosDec(&logTermRow);
   logTermRows[logTermRow].entry = entry;
   logTermRows[logTermRow].flags = LOGTERM_ENTRY_END;

   // Previous lines of the new entry
   while (--numRows) {
      if (logTermRow == logTermTop) { // We have displaced the top
	 LogTermPosDec(&logTermTop);
      }
      LogTermPosDec(&logTermRow);
      logTermRows[logTermRow].entry = entry;
      logTermRows[logTermRow].flags = 0;
   }

   // First line of the new entry
   logTermRows[logTermRow].flags |= LOGTERM_ENTRY_START;

   // Extraneous lines that were cleared
   while (logTermRow != logTermTop) {
      LogTermPosDec(&logTermRow);
      logTermRows[logTermRow].entry = 0;
      logTermRows[logTermRow].flags = 0;
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * LogTermSetScrollback --
 *
 * 	Set/reset scroll back state
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	'Scrollback' is displayed/removed in the status banner
 *
 *----------------------------------------------------------------------
 */
static void
LogTermSetScrollback(Bool on)
{
   const char message[] = "STOPPED";

   logTermScrollBack = on;
   Term_SetPos(logTerm, LOGTERM_STATUS_WINDOW,0,logTermNumCols-sizeof(message));
   Term_Printf(logTerm, LOGTERM_STATUS_WINDOW, "%s%s" ANSI_ATTR_SEQ_RESET,
	   on ? ANSI_ATTR_SEQ_FORE_RED_BRIGHT : ANSI_ATTR_SEQ_HIDDEN, message);
}


/*
 *----------------------------------------------------------------------
 *
 * LogTermHelp --
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
LogTermHelp(Bool bluescreen)
{
   Term_Clear(logTerm, LOGTERM_LOG_WINDOW, NULL);
   Term_Printf(logTerm, LOGTERM_LOG_WINDOW,
		"\n\n"
		"\th                : help\n"
		"\tUp,       Ctrl-U : scroll up one entry\n"
		"\tPageUp,   Ctrl-B : scroll up ten entries\n"
		"\tDown,     Ctrl-D : scroll down one entry\n"
		"\tPageDown, Ctrl-F : scroll down ten entries\n"
		"\tEnd              : scroll to latest entry\n"
		"\tHome             : scroll to earliest entry\n"
		"\tSpace            : resume updates stopped by scrolling\n"
		"\n"
		"%s"
		"\n\n\n"
		"\tAny key to leave this help screen\n",
		bluescreen ?
		"\tEscape           : go back to debugger\n" :
		"\tAlt-F1 .. Alt-F6 : go back to service console terminals\n"
                "\tAlt-F11          : go back to status terminal\n");
}


/*
 *----------------------------------------------------------------------
 *
 * LogTermInputCallback --
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
LogTermInputCallback(const char *txt)
{
   char c = *txt;
   int i;
   static Bool help = FALSE;


   if (help) { // restore log display
      LogTermSetScrollback(FALSE); // resume updates
      if ((c == KEYBOARD_KEY_ESCAPE) && logBluescreen) { // back to bluescreen
	 logDone = TRUE;
	 return;
      }
      LogTermDisplayTail();
      help = FALSE;
      return;
   }

   switch (c) {
   case KEYBOARD_KEY_CTRL('U'):
   case KEYBOARD_KEY_UP:
      // scroll up
      LogTermSetScrollback(TRUE);
      LogTermScrollBack();
      break;
   case KEYBOARD_KEY_CTRL('B'):
   case KEYBOARD_KEY_PAGEUP:
      // scroll up ten entries
      LogTermSetScrollback(TRUE);
      for (i = 0; i < 10; i++) {
	 LogTermScrollBack();
      }
      break;
   case KEYBOARD_KEY_CTRL('D'):
   case KEYBOARD_KEY_DOWN:
      // scroll down
      LogTermSetScrollback(TRUE);
      LogTermScrollAhead(FALSE);
      break;
   case KEYBOARD_KEY_CTRL('F'):
   case KEYBOARD_KEY_PAGEDOWN:
      // scroll down ten entries
      LogTermSetScrollback(TRUE);
      for (i = 0; i < 10; i++) {
         LogTermScrollAhead(FALSE);
      }
      break;
   case KEYBOARD_KEY_END:
      // go to latest entry
      LogTermSetScrollback(TRUE);
      LogTermDisplayTail();
      break;
   case KEYBOARD_KEY_HOME:
      // go to earlies entry
      LogTermSetScrollback(TRUE);
      LogTermDisplayHead();
      break;
   case ' ':
      // cancel scroll
      LogTermSetScrollback(FALSE);
      LogTermDisplayTail();
      break;
   case KEYBOARD_KEY_ESCAPE:
      // close terminal, back to bluescreen
      if (logBluescreen) {
	 LogTermSetScrollback(FALSE);
	 logDone = TRUE;
	 return;
      }
      break;
   case 'h':
      // help
      LogTermSetScrollback(TRUE); // to stop updates
      LogTermHelp(logBluescreen);
      help = TRUE;
      break;
   default:
      break;
   }
}

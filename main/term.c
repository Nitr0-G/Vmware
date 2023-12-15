/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * term.c --
 *
 *	Terminal primitives (screen output/keyboard input)
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "libc.h"
#include "term.h"
#include "vga.h"
#include "keyboard.h"
#include "host.h"
#include "bh.h"

// Be careful about Log as some of the routines are used indirectly by it
#define LOGLEVEL_MODULE Term
#include "log.h"


/*
 * A terminal consists of
 * - screen output, it can be split into two horizontal windows, each
 *   window handles scrolling automatically
 * - keyboard input, it can be key by key with no echo, or line by line
 *   with echo
 */
typedef struct termWindow {
   uint8	top;		// top row (absolute for screen)
   uint8	numRows;	// number of rows
   uint8        numCols;        // number of columns

   /*
    * Current position where the next character will be put in window.
    * It is relative to the window (i.e. top left is always (0,0)).
    * It can be (numRows, 0) as we defer scrolling to avoid wasting
    * a display line.
    */
   uint8	row;		// current row (relative to top)
   uint8	col;		// current col

   uint8	attr;		// current synthetic attribute
   uint8	pendingANSI;	// an ANSI sequence is being parsed
   				// 0 - no
				// '\033' - <ESC> has been seen
				// '[' - <ESC>[ has been seen
   uint8	pendingVal;	// value being parsed

   Ansi_Attr	normal;		// normal attributes
   Ansi_Attr	current;	// current attributes
   Ansi_Attr	pending;	// new attributes if ANSI sequence is valid

   Bool         autoscroll;     // content auto-scrolls at bottom of window
} termWindow;

#define TERM_NUM_WINDOWS 2

typedef struct termInfo {
   uint32	scr;		// VGA screen associated with the terminal

   uint8	inUse;		// terminal has been allocated
   char		altFn;		// Alt-Fn key to press to bring term on screen
   uint16	pad;

   Term_ScreenCallback onScreenCallback;  // callback on getting on screen
   Term_ScreenCallback offScreenCallback; // callback on getting off screen

   termWindow	window[TERM_NUM_WINDOWS];

   Term_Input	input;		// type of input
   Term_InputAsyncCallback inputCallback; // callback on input events

   termWindow	*inputWindow;	// window receiving echoed line input

   char		inputLine[128];	// buffered input line
   uint32	inputSize;	// current valid size
} termInfo;


#define TERM_NUM_TERMS  8

static termInfo terms[TERM_NUM_TERMS];

/*
 * termLock only synchronizes functions affecting the whole module
 * (Term_Alloc(), Term_Display() and input).
 * Functions affecting only one given terminal are safe to be called
 * concurrently for different terminals. It is up to the users of a
 * given terminal to synchronize their concurrent accesses.
 */
static SP_SpinLockIRQ termLock;

static termInfo *termCurrent = NULL;
static Bool termInputPossible = FALSE;
static int termCOSNr = -1;


static void TermReceiveInput(void);
static char *TermBufferLineInput(uint32, char c);



/*
 *----------------------------------------------------------------------
 *
 * Term_Init --
 *
 * 	Initialize term module
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
Term_Init(VMnix_SharedData *sharedData)
{
   Log("");

   memset(terms, 0, sizeof(terms));

   // Around VGA lock, keyboard lock and hostIClock
   SP_InitLockIRQ("termLck", &termLock, SP_RANK_HOSTIC_LOCK-1);
   SHARED_DATA_ADD(sharedData->vgaCOSConsole, int *, &termCOSNr);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_LateInit --
 *
 * 	Late initialization of term module
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
Term_LateInit(void)
{
   SP_IRQL prevIRQL;

   Keyboard_SetCallback(TermReceiveInput);

   // We need to update the keyboard audience
   prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);
   Keyboard_SetAudience(termCurrent ? KEYBOARD_VMK : KEYBOARD_COS);
   SP_UnlockIRQ(&termLock, prevIRQL);

   termInputPossible = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Term_IsInputPossible --
 *
 * 	Return possibility of getting interrupt-based input events
 *
 * Results:
 * 	TRUE if possible, FALSE otherwise
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
Bool
Term_IsInputPossible(void)
{
   return termInputPossible;
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Alloc --
 *
 * 	Allocate a terminal
 *
 * Results:
 * 	Terminal is succesful
 * 	TERM_INVALID otherwise
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
uint32
Term_Alloc(const Term_AllocArgs *args, uint32 *numRows, uint32 *numCols)
{
   SP_IRQL prevIRQL;
   uint32 term;
   termInfo *t;
   termWindow *w;
   int i;


   prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);

   for (term = 0; term < TERM_NUM_TERMS; term++) {

      t = &terms[term];

      if (t->inUse) {
	 continue;
      }

      t->scr = VGA_Alloc(args->extended, numRows, numCols);
      if (t->scr == VGA_SCREEN_INVALID) {
         term = TERM_NUM_TERMS;
         break;
      }

      t->inUse = TRUE;
      break;
   }

   SP_UnlockIRQ(&termLock, prevIRQL);

   if (term == TERM_NUM_TERMS) {
      return TERM_INVALID;
   }

   t->altFn = args->altFn;
   t->onScreenCallback = args->onScreenCallback;
   t->offScreenCallback = args->offScreenCallback;

   // Just one window spanning the whole screen
   w = &t->window[0];
   w->top = 0;
   w->numRows = *numRows;
   w->numCols = *numCols;
   w->normal = args->ansiAttr;
   w->current = w->normal;
   w->attr = VGA_MakeAttribute(&w->current);
   w->pendingANSI = 0;
   w->autoscroll = args->autoscroll;
   for (i = 1; i < TERM_NUM_WINDOWS; i++) {
      t->window[i] = t->window[0];
      t->window[i].numRows = 0;
   }

   t->inputWindow = args->input == TERM_INPUT_ASYNC_LINE ? &t->window[0] : NULL;
   t->input = args->input;
   t->inputCallback = args->inputCallback;
   t->inputSize = 0;

   Term_Clear(term, 0, NULL);

   Log("%d", term);
   return term;
}


/*
 *----------------------------------------------------------------------
 *
 * TermDoDisplay --
 *
 * 	Display a terminal screen as the actual video output
 * 	and have keyboard events go to its handler
 *
 * 	termLock is held already
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
TermDoDisplay(uint32 term)
{
   termInfo *t = term == TERM_COS ? NULL : &terms[term];
   uint32 scr;
   Keyboard_Audience kbdAudience;
   Bool cursor;


   ASSERT((term == TERM_COS) || ((term < TERM_NUM_TERMS) && t->inUse));

   ASSERT(SP_IsLockedIRQ(&termLock));

   /*
    * Nothing to do if the terminal is already on screen unless it
    * is COS in which case we need to poke it so that it can switch
    * its own terminals as needed.
    */
   if (t == termCurrent) {
      if (termCurrent == NULL) {
	 Host_InterruptVMnix(VMNIX_VGA_INTERRUPT);
      }
      return;
   }

   /*
    * Call the off/on screen callbacks as needed
    */
   if (termCurrent && termCurrent->offScreenCallback) {
      termCurrent->offScreenCallback();
   }
   if (t && t->onScreenCallback) {
      t->onScreenCallback();
   }

   /*
    * Bring the new terminal on screen
    */
   if (term == TERM_COS) {
      scr = VGA_SCREEN_COS;
      kbdAudience = KEYBOARD_COS;
      cursor = FALSE;
   } else {
      scr = t->scr;
      kbdAudience = KEYBOARD_VMK;
      cursor = t->inputWindow != NULL;
   }

   VGA_Display(scr);

   if (cursor) {
      // show cursor
      termWindow *w = t->inputWindow;
      ASSERT((term != TERM_COS) && (scr != VGA_SCREEN_COS));
      ASSERT(w != NULL);
      VGA_Cursor(scr, w->top + w->row, w->col, 2);
   } else {
      // hide cursor
      VGA_Cursor(scr, 0, 0, 0);
   }

   termCurrent = t;

   Keyboard_SetAudience(kbdAudience);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Display --
 *
 * 	Display a terminal screen as the actual video output
 * 	and have keyboard events go to its handler
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
Term_Display(uint32 term)
{
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);
   if (term == TERM_COS) {
      // Keep the current COS terminal
      termCOSNr = -1;
   }
   TermDoDisplay(term);
   SP_UnlockIRQ(&termLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * TermSwitch --
 *
 * 	Switch to a different terminal based on an Alt-Fn key
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	Terminals may be switched with corresponding call to screenCallbacks
 *
 *----------------------------------------------------------------------
 */
static void
TermSwitch(char altFn)
{
   SP_IRQL prevIRQL;
   uint32 term;
   termInfo *t;

   prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);

   /*
    * Check vmkernel terminals first as they can hijack a usual COS terminal
    * Alt-Fn key.
    */
   for (term = 0; term < TERM_NUM_TERMS; term++) {
      t = &terms[term];
      if (t->inUse && (t->altFn == altFn)) {
	 break;
      }
   }

   if (term == TERM_NUM_TERMS) {
      // Alt-Fn X go to COS terminals by default (COS numbers from 0).
      int nr = altFn - KEYBOARD_KEY_ALT_FN(1);
      if ((nr >= 0) && (nr < TERM_NUM_COS_TERMINALS)) {
	 termCOSNr = nr;
	 term = TERM_COS;
      } else {
	 term = TERM_INVALID;
      }
   }

   // Bring terminal on screen
   if (term != TERM_INVALID) {
      TermDoDisplay(term);
   }

   SP_UnlockIRQ(&termLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Clear --
 *
 * 	Clear a window with the space character and sets its default
 * 	ANSI attributes possibly
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	current position is reset to (0,0)
 * 	ANSI parsing is reset
 *
 *----------------------------------------------------------------------
 */
void
Term_Clear(uint32 term, uint32 window, const Ansi_Attr *ansiAttr)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];
   uint16 fatc;

   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   if (ansiAttr) {
      w->normal = *ansiAttr;
      w->current = w->normal;
      w->attr = VGA_MakeAttribute(&w->current);
      w->pendingANSI = 0;
   }

   fatc = VGA_MakeFatChar(' ', w->attr);
   VGA_Clear(t->scr, w->top, 0, w->numRows, w->numCols, fatc);
   w->row = 0;
   w->col = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Split --
 *
 *	Create a new window by splitting the given window
 *	The new window takes up the top or bottom numRows of the given
 *	window which is truncated (always at the bottom).
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	- current position in the new window will be top left
 * 	- current position in the old window will be unchanged 
 * 	unless it was in the truncated part then it will be
 * 	just after bottom right.
 *
 *----------------------------------------------------------------------
 */
void
Term_Split(uint32 term, uint32 window, uint32 numRows, Bool top,
           const Ansi_Attr *ansiAttr, Bool getInputEcho, Bool autoscroll)
{
   termInfo *t = &terms[term];
   termWindow *oldW = &t->window[window];
   termWindow *newW;
   uint16 fatc;


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && oldW->numRows);
   
   /*
    * Current implementation has a max. of two windows. XXX
    * The window being split is 0 and takes up the whole screen,
    * the new window will be 1.
    */
   newW = &t->window[1];
   if ((window != 0) || (newW->numRows != 0)) {
      return;
   }

   /*
    * There must be something left for the old window.
    */
   if (numRows >= oldW->numRows) {
      return;
   }

   /*
    * The new window will be above/below the old one so scroll it down/up
    * by as much as needed.
    */
   fatc = VGA_MakeFatChar(' ', oldW->attr);
   VGA_Scroll(t->scr, oldW->top, oldW->top+oldW->numRows, numRows, !top, fatc);

   /*
    * Adjust new window.
    */
   if (top) {
      // the new window comes on top
      newW->top = oldW->top;
   } else {
      // the new window comes below
      newW->top = oldW->top + oldW->numRows - numRows;
   }
   newW->numRows = numRows;
   Term_Clear(term, newW - &t->window[0], ansiAttr ? ansiAttr : &oldW->normal);
   newW->autoscroll = autoscroll;

   /*
    * Adjust old window.
    */
   if (top) {
      // the new window comes on top, so the old window moves down
      oldW->top += numRows;
   }
   oldW->numRows -= numRows;
   if (oldW->row > oldW->numRows) {
      // spot was truncated, move at the very end
      oldW->row = oldW->numRows;
      oldW->col = 0;
   } else if ((oldW->row == oldW->numRows) && (oldW->col != 0)) {
      // spot was truncated, move at the very end
      oldW->col = 0;
   }

   /*
    * Target new window for input echo if requested
    */
   if ((t->input == TERM_INPUT_ASYNC_LINE) && getInputEcho) {
      t->inputWindow = newW;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Term_SetPos --
 *
 * 	Set current position to an arbitrary location
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
Term_SetPos(uint32 term, uint32 window, uint32 row, uint32 col)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];

   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   if (row > w->numRows) {
      return;
   }

   if ((row == w->numRows) && (col != 0)) {
      return;
   }

   if (col >= w->numCols) {
      return;
   }

   w->row = row;
   w->col = col;
}


/*
 *----------------------------------------------------------------------
 *
 * TermPutc --
 *
 *	Put a single character in a window at current position.
 *	If t is NULL, the window data is updated but no actual
 *	display operation is done.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      current position is updated
 *      scrolling may happen if necessary
 *
 *----------------------------------------------------------------------
 */
static void
TermPutc(termInfo *t, termWindow *w, char c)
{
   uint16 fatc;


   if (w->pendingANSI == '\033') { // previous character was <ESC>

      if (c != '[') {
	 // this is not an ANSI escape sequence
	 w->pendingANSI = 0;
	 // the character will be printed normally
      } else {
	 // it looks like an ANSI sequence, wait for the next character
	 w->pendingANSI = c;
	 w->pendingVal = 0;
	 w->pending = w->current;
	 return;
      }

   } else if (w->pendingANSI == '[') { // ongoing ANSI sequence

      if ((c == 'm') || (c == ';')) { // end of one ANSI attribute

	 if (w->pendingVal == ANSI_ATTR_RESET) {

	    w->pending = w->normal;

	 } else if (w->pendingVal == ANSI_ATTR_BRIGHT) {

	    w->pending.bright = TRUE;

	 } else if (w->pendingVal == ANSI_ATTR_DIM) {

	    w->pending.bright = FALSE;

	 } else if (w->pendingVal == ANSI_ATTR_REVERSE) {

	    uint8 swap = w->pending.fore;
	    w->pending.fore = w->pending.back;
	    w->pending.back = swap;
	    w->pending.bright = FALSE;

	 } else if (w->pendingVal == ANSI_ATTR_HIDDEN) {

	    w->pending.fore = w->pending.back;
	    w->pending.bright = FALSE;

	 } else if ((w->pendingVal >= ANSI_ATTR_FORE_COLOR) &&
		    (w->pendingVal < ANSI_ATTR_FORE_COLOR+ANSI_NUM_COLORS)) {

	    w->pending.fore = w->pendingVal - ANSI_ATTR_FORE_COLOR;

	 } else if (w->pendingVal == ANSI_ATTR_FORE_COLOR+ANSI_DEFAULT) {

	    w->pending.fore = w->normal.fore;

	 } else if ((w->pendingVal >= ANSI_ATTR_BACK_COLOR) &&
		    (w->pendingVal < ANSI_ATTR_BACK_COLOR+ANSI_NUM_COLORS)) {

	    w->pending.back = w->pendingVal - ANSI_ATTR_BACK_COLOR;

	 } else if (w->pendingVal == ANSI_ATTR_BACK_COLOR+ANSI_DEFAULT) {

	    w->pending.back = w->normal.back;

	 } else {

	    // Unknow code, just ignore it

	 }

	 w->pendingVal = 0; // atrribute has been parsed

	 if (c == 'm') { // 'm' closes the sequence
	    w->pendingANSI = 0;
	    w->current = w->pending;
	    // Recompute cached synthetic attribute
	    w->attr = VGA_MakeAttribute(&w->current);
	 }
	 return;

      } else if ((c >= '0') && (c <= '9')) {

	 w->pendingVal *= 10;
	 w->pendingVal += c - '0';
	 return;

      } else {

	 // bad ANSI sequence, abort it and print the character normally
	 w->pendingANSI = 0;

      }

   }

   if (c == '\033') { // <ESC>, possible start of an ANSI escape sequence

      w->pendingANSI = c;
      return;

   }

   if ((c == '\n') || (c == '\r') || (c == '\t') || isprint(c)) {

      if (t && (w->row == w->numRows)) {//Scroll if we are at the end of display

         if (!w->autoscroll) {
            return;
         }
         ASSERT(w->col == 0);
         fatc = VGA_MakeFatChar(' ', w->attr);
         VGA_Scroll(t->scr, w->top, w->top + w->numRows, 1, TRUE, fatc);
         w->row--;

      }

   }

   if ((c == '\n') || (c == '\r')) {

      w->row++;
      w->col = 0;

   } else if (c == '\t') {

      w->col += 8;
      w->col &= ~(8-1);
      if (w->col >= w->numCols) {
	 w->row++;
	 w->col = 0;
      }

   } else if (c == '\b') {

      if (w->col) {
	 w->col--;
      } else if (w->row) {
	 w->col = w->numCols-1;
	 w->row--;
      } else {
	 return;
      }

      if (t) {
         fatc = VGA_MakeFatChar(' ', w->attr);
         VGA_Putfb(t->scr, w->top + w->row, w->col, &fatc, 1);
      }

   } else if (isprint(c)) {

      if (t) {
         fatc = VGA_MakeFatChar(c, w->attr);
         VGA_Putfb(t->scr, w->top + w->row, w->col, &fatc, 1);
      }
      w->col++;
      if (w->col == w->numCols) {
	 w->row++;
	 w->col = 0;
      }

   }

   if (t && (w == t->inputWindow)) { // We need to display the cursor

      if (w->row == w->numRows) { // Scroll if we are at the end of display
         if (!w->autoscroll) {
            return;
         }
	 ASSERT(w->col == 0);
	 fatc = VGA_MakeFatChar(' ', w->attr);
	 VGA_Scroll(t->scr, w->top, w->top + w->numRows, 1, TRUE, fatc);
	 w->row--;
      }
      VGA_Cursor(t->scr, w->top + w->row, w->col, 2);

   }
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Putb --
 *
 *	Put a buffer of characters on a terminal at current position
 *
 * Results:
 *      None
 *
 * Side effects:
 *      current position is updated
 *      scrolling may happen if necessary
 *
 *----------------------------------------------------------------------
 */
void
Term_Putb(uint32 term, uint32 window, const char *txt, uint32 len)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   while (len--) {
      TermPutc(t, w, *txt++);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Sizeb --
 *
 *      Size a buffer of characters after tab expansion and ANSI escape
 *      sequences parsing assuming it will be displayed starting in
 *      column 0.
 *
 *      The buffer is not expected to contain \b, \n or \r.
 *
 * Results:
 *      The number of actual character spots taken on the terminal
 *      -1 if the buffer contains \b, \n or \r, or if the display
 *      would span more than one screenful.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
uint32
Term_Sizeb(uint32 term, uint32 window, const char *txt, uint32 len)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];
   termWindow sizeW;


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   memset(&sizeW, 0, sizeof(sizeW));
   sizeW.numRows = w->numRows+1; // To detect screen overflow
   sizeW.numCols = w->numCols;

   while (len--) {
      if ((*txt == '\b') || (*txt == '\n') || (*txt == '\r')) {
         return (uint32)-1;
      }
      if (sizeW.row > w->numRows) {
         return (uint32)-1;
      }
      TermPutc(NULL, &sizeW, *txt++);
   }

   return (uint32)sizeW.col + (uint32)sizeW.row*(uint32)sizeW.numCols;
}


/*
 *----------------------------------------------------------------------
 *
 * Term_InsertAbove --
 *
 *      Insert a buffer of characters on a terminal up to a row.
 *      If there is not enough space above the row, content starting
 *      at the row is scrolled down as necessary.
 *      Content above the row is cleared before insertion.
 *
 *      The buffer must not contain \b, \r or \n.
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      current position is updated to after the last character inserted
 *
 *----------------------------------------------------------------------
 */
void
Term_InsertAbove(uint32 term, uint32 window, uint32 row, const char *txt, uint32 len, uint32 displayLen)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];
   uint16 clearFatc;
   uint32 numRows;


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);
   ASSERT(displayLen == Term_Sizeb(term, window, txt, len));

   numRows = CEILING(displayLen, w->numCols);
   clearFatc = VGA_MakeFatChar(' ', w->attr);

   // Clear content above insertion point
   if (row > 0) {
      VGA_Clear(t->scr, w->top, 0, row, w->numCols, clearFatc);
   }

   // Scroll down content at insertion point as needed
   if (numRows > row) {
      VGA_Scroll(t->scr, w->top + row, w->top + w->numRows, numRows - row, FALSE, clearFatc);
      w->row = 0; // If we need to scroll, we'll have just enough
   } else {
      w->row = row - numRows; // No scroll, we start above by as much as needed
   }

   // Insert buffer above insertion point
   w->col = 0;
   Term_Putb(term, window, txt, len);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_InsertBelow --
 *
 * 	Insert a buffer of characters on a terminal starting at a row.
 * 	If there is not enough space below the row, content above the
 * 	row is scrolled up as necessary.
 * 	Content starting at the row is cleared before insertion.
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	current position is updated to after the last character inserted
 *
 *----------------------------------------------------------------------
 */
void
Term_InsertBelow(uint32 term, uint32 window, uint32 row, const char *txt, uint32 len)
{
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];
   uint16 clearFatc;


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   clearFatc = VGA_MakeFatChar(' ', w->attr);

   // Clear content at insertion point
   if (row < w->numRows) {
      VGA_Clear(t->scr, w->top + row, 0, w->numRows - row,w->numCols,clearFatc);
   }

   // No need to scroll up explicitly as this is the normal semantics of Putb
   w->row = row;
   w->col = 0;
   Term_Putb(term, window, txt, len);
}


/*
 *----------------------------------------------------------------------
 *
 * TermPutcForPrintf --
 *
 *      Put a single character on a terminal at current position
 *
 * Results:
 *      None
 *
 * Side effects:
 *      current position is updated
 *      scrolling may happen if necessary
 *
 *----------------------------------------------------------------------
 */
static void
TermPutcForPrintf(int c, void *cookie)
{
   uint32 term = ((uint32)cookie)>>16;
   uint32 window = ((uint32)cookie)&0xFFFF;
   termInfo *t = &terms[term];
   termWindow *w = &t->window[window];


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   ASSERT((window < TERM_NUM_WINDOWS) && w->numRows);

   TermPutc(t, w, (char)c);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_PrintfVarArgs --
 *
 *      Print formatted string on a terminal
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
Term_PrintfVarArgs(uint32 term, uint32 window, const char *fmt, va_list args)
{
   uint32 cookie = (term<<16) | (window&0xFFFF);
   Printf_WithFunc(fmt, TermPutcForPrintf, (void *)cookie, args);
}


/*
 *----------------------------------------------------------------------
 *
 * Term_Printf --
 *
 * 	Print formatted string on a terminal
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
Term_Printf(uint32 term, uint32 window, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   Term_PrintfVarArgs(term, window, fmt, args);
   va_end(args);
}


/*
 *----------------------------------------------------------------------
 *
 * TermReceiveInput --
 *
 * 	Process characters received from the keyboard
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
TermReceiveInput(void)
{
   SP_IRQL prevIRQL;
   Bool checkTerm = TRUE;
   Term_Input input = TERM_INPUT_NONE;
   Term_InputAsyncCallback inputCallback = NULL;
   uint32 term = TERM_INVALID;
   char c;
   char *line;


   while ((c = Keyboard_Read()) != 0) {

      /*
       * Process special characters that have special meanings and do
       * not appear in a term input stream.
       */
      if ((c >= KEYBOARD_KEY_ALT_FN(1)) && (c <= KEYBOARD_KEY_ALT_FN(12))) {
	 // Possible terminal switch, we'll have to check terminal again
	 TermSwitch(c);
	 checkTerm = TRUE;
	 continue;
      }
      
      if (checkTerm) {
         /*
          * We take a snapshot of termCurrent input callback.
          * Given the speed of the keyboard, any race is pretty much irrelevant
          * and it's unlikely characters typed ahead in one terminal would end
          * up in another. It is also possible that COS is back up (termCurrent
          * is NULL).
          * NOTE that the input callback routines are guaranteed to never
	  * go away.
          */
          prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);
          if (termCurrent && (termCurrent->input != TERM_INPUT_NONE)) {
             input = termCurrent->input;
             inputCallback = termCurrent->inputCallback;
             ASSERT(inputCallback != NULL);
             term = termCurrent - &terms[0];
          } else {
	     input = TERM_INPUT_NONE;
	  }
          SP_UnlockIRQ(&termLock, prevIRQL);
	  checkTerm = FALSE;
      }

      switch (input) {
      case TERM_INPUT_NONE:
         // Exhaust characters
         break;
      case TERM_INPUT_ASYNC_CHAR:
         // Forward characters directly
         inputCallback(&c);
         break;
      case TERM_INPUT_ASYNC_LINE:
         // Buffer to forward only entire lines
	 line = TermBufferLineInput(term, c);
         if (line) {
	    inputCallback(line);
	 }
         break;
      default:
         ASSERT(FALSE);
         break;
      }

   }
}


/*
 *----------------------------------------------------------------------
 *
 * Term_PollInput --
 *
 * 	When interrupts are disabled (such as in bluescreen context),
 * 	the keyboard needs to be polled.
 *
 * 	This function returns when terminate is TRUE, presumably so set
 * 	by the caller eventually.
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
Term_PollInput(volatile Bool *terminate)
{
   SP_IRQL prevIRQL;
   uint32 term = 0;
   Term_Input input = TERM_INPUT_NONE;
   Term_InputAsyncCallback inputCallback = NULL;
   char c;
   char *line;

   // No reason to call this function if interrupts are enabled
   ASSERT_NO_INTERRUPTS();

   /*
    * We take a snapshot of termCurrent input callback.
    * Given the speed of the keyboard, any race is pretty much irrelevant
    * and it's unlikely characters typed ahead in one terminal would end
    * up in another. It is also possible that COS is back up (termCurrent
    * is NULL).
    * NOTE that the input callback routines are guaranteed to never go away.
    */
   prevIRQL = SP_LockIRQ(&termLock, SP_IRQL_KERNEL);
   if (termCurrent && (termCurrent->input != TERM_INPUT_NONE)) {
      input = termCurrent->input;
      inputCallback = termCurrent->inputCallback;
      ASSERT(inputCallback != NULL);
      term = termCurrent - &terms[0];
   }
   SP_UnlockIRQ(&termLock, prevIRQL);

   // If polling, input must be wanted. 
   ASSERT(input != TERM_INPUT_NONE);

   while (!*terminate) {
      c = Keyboard_Poll();
      if (c != 0) {
	 switch (input) {
	 case TERM_INPUT_ASYNC_CHAR:
	    // Forward characters directly
	    inputCallback(&c);
	    break;
	 case TERM_INPUT_ASYNC_LINE:
	    // Buffer to forward only entire lines
	    line = TermBufferLineInput(term, c);
	    if (line) {
	       inputCallback(line);
	    }
	    break;
	 default:
	    ASSERT(FALSE);
	    break;
	 }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * TermBufferLineInput --
 *
 * 	Buffers characters for line by line input
 *
 * Results:
 * 	Address to line if available, NULL otherwise
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
char *
TermBufferLineInput(uint32 term, char c)
{
   char *line = NULL;
   termInfo *t = &terms[term];
   termWindow *w;


   ASSERT((term < TERM_NUM_TERMS) && t->inUse);
   w = t->inputWindow;
   ASSERT(w != NULL);
   ASSERT(w->numRows);

   switch (c) {
   case '\r': // Enter
      t->inputLine[t->inputSize] = '\0';
      t->inputSize = 0;
      TermPutc(t, w, c);
      line = t->inputLine;
   case '\b': // Backspace
      if (t->inputSize) {
	 t->inputSize--;
	 TermPutc(t, w, c);
      }
      break;
   default:
      if (t->inputSize < sizeof(t->inputLine) - 1) { // leave one for '\0'
	 if (isprint(c)) {
	    t->inputLine[t->inputSize++] = c;
	    TermPutc(t, w, c);
	 }
      }
      break;
   }

   return line;
}

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Term.h --
 *
 *	Terminal specific functions.
 */

#ifndef _TERM_H
#define _TERM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "ansi.h"
#include "keyboard.h"


#define TERM_INVALID	((uint32)-1)
#define TERM_COS	((uint32)-2)

typedef enum Term_Input {
   TERM_INPUT_NONE = 0,
   TERM_INPUT_ASYNC_CHAR,
   TERM_INPUT_ASYNC_LINE,
   TERM_INPUT_NUM
} Term_Input;

typedef void (*Term_InputAsyncCallback)(const char *txt);
typedef void (*Term_ScreenCallback)(void);

typedef struct Term_AllocArgs {
   Bool                         extended;
   Bool                         autoscroll;
   Ansi_Attr                    ansiAttr;
   Term_Input                   input;
   Term_InputAsyncCallback      inputCallback;
   Term_ScreenCallback          onScreenCallback;
   Term_ScreenCallback          offScreenCallback;
   char                         altFn;
} Term_AllocArgs;

extern void Term_Init(VMnix_SharedData *sharedData);
extern void Term_LateInit(void);
extern Bool Term_IsInputPossible(void);
extern uint32 Term_Alloc(const Term_AllocArgs *args,
                         uint32 *numRows, uint32 *numCols);
extern void Term_Display(uint32 term);
extern void Term_Clear(uint32 term, uint32 window, const Ansi_Attr *ansiAttr);
extern void Term_Split(uint32 term, uint32 window, uint32 numRows, Bool top,
                       const Ansi_Attr *ansiAttr, Bool getInputEcho,
                       Bool autoscroll);
extern void Term_SetPos(uint32 term, uint32 window, uint32 row, uint32 col);
extern void Term_Putb(uint32 term, uint32 window, const char *txt, uint32 len);
extern uint32 Term_Sizeb(uint32 term, uint32 window, const char *txt,
                         uint32 len);
extern void Term_InsertAbove(uint32 term, uint32 window, uint32 row,
			     const char *txt, uint32 len, uint32 displayLen);
extern void Term_InsertBelow(uint32 term, uint32 window, uint32 row,
			     const char *txt, uint32 len);
extern void Term_Printf(uint32 term, uint32 window, const char *fmt, ...);
extern void Term_PrintfVarArgs(uint32 term, uint32 window, const char *fmt,
                               va_list args);
extern void Term_PollInput(volatile Bool *terminate);


/*
 * AltFn keys usage
 * 1-6 are used for COS virtual terminals (VMNIX_MAX_VT)
 * 7-9 are unused
 *
 * NOTE that adding additional lines in /etc/inittab such as
 * 7:2345:respawn:/sbin/mingetty tty7
 * will create more COS terminals but they may not be accessible.
 */
#define TERM_NUM_COS_TERMINALS  VMNIX_MAX_VT
#define TERM_ALT_FN_FOR_STATUS	KEYBOARD_KEY_ALT_FN(11)
#define TERM_ALT_FN_FOR_USER	KEYBOARD_KEY_ALT_FN(10)
#define TERM_ALT_FN_FOR_LOG	KEYBOARD_KEY_ALT_FN(12)

#endif

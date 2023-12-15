/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * ansi.h --
 *
 *      ANSI escape sequences for terminal control
 */
#ifndef  _ANSI_H
#define _ANSI_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


// Colors
#define ANSI_BLACK		0
#define ANSI_RED		1
#define ANSI_GREEN		2
#define ANSI_YELLOW		3
#define ANSI_BLUE		4
#define ANSI_MAGENTA		5
#define ANSI_CYAN		6
#define ANSI_WHITE		7
#define ANSI_NUM_COLORS		8
#define ANSI_DEFAULT		9

// Attribute escape sequences	"\033[val;val;...m"
#define ANSI_ATTR_RESET		0
#define ANSI_ATTR_BRIGHT	1
#define ANSI_ATTR_DIM		2
#define ANSI_ATTR_UNDERSCORE	4
#define ANSI_ATTR_BLINK		5
#define ANSI_ATTR_REVERSE	7
#define ANSI_ATTR_HIDDEN	8
#define ANSI_ATTR_FORE_COLOR	30
#define ANSI_ATTR_BACK_COLOR	40

// Useful common sequences
#define ANSI_ATTR_SEQ_RESET			"\033[0m"
#define ANSI_ATTR_SEQ_BRIGHT			"\033[1m"
#define ANSI_ATTR_SEQ_REVERSE			"\033[7m"
#define ANSI_ATTR_SEQ_HIDDEN			"\033[8m"
#define ANSI_ATTR_SEQ_FORE_DEFAULT_DIM		"\033[39;2m"
#define ANSI_ATTR_SEQ_FORE_RED_BRIGHT		"\033[31;1m"
#define ANSI_ATTR_SEQ_FORE_YELLOW_BRIGHT	"\033[33;1m"
#define ANSI_ATTR_SEQ_FORE_CYAN_BRIGHT		"\033[36;1m"
#define ANSI_ATTR_SEQ_BACK_MAGENTA		"\033[45m"


typedef struct Ansi_Attr {
   uint8	fore;		// foreground color
   uint8	back;		// background color
   uint8	bright;		// brightness level (1 = bright, 0 = dim)
   uint8	pad;
} Ansi_Attr;

#endif

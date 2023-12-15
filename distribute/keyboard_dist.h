/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * keyboard_dist.h --
 *
 *      Keyboard support
 */

#ifndef _KEYBOARD_DIST_H_
#define _KEYBOARD_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"


// Char values for control characters
#define KEYBOARD_KEY_CTRL(maj)          ((maj) - 'A' + 1)

#define KEYBOARD_KEY_ESCAPE             '\033'

// Char values for special keys (beyond normal ASCII codes)
#define KEYBOARD_KEY_FUNC_BASE          (char)(0x80)

#define KEYBOARD_KEY_FN(num)            (char)(0x80+(num))
#define KEYBOARD_KEY_SHIFT_FN(num)      (char)(0x80+12+(num))
#define KEYBOARD_KEY_CTRL_FN(num)       (char)(0x80+24+(num))
#define KEYBOARD_KEY_CTRLSHIFT_FN(num)  (char)(0x80+36+(num))
#define KEYBOARD_KEY_HOME               (char)(0x80+49)
#define KEYBOARD_KEY_UP                 (char)(0x80+50)
#define KEYBOARD_KEY_PAGEUP             (char)(0x80+51)
#define KEYBOARD_KEY_NUMMINUS           (char)(0x80+52)
#define KEYBOARD_KEY_LEFT               (char)(0x80+53)
#define KEYBOARD_KEY_UNKNOWN            (char)(0x80+54)
#define KEYBOARD_KEY_RIGHT              (char)(0x80+55)
#define KEYBOARD_KEY_NUMPLUS            (char)(0x80+56)
#define KEYBOARD_KEY_END                (char)(0x80+57)
#define KEYBOARD_KEY_DOWN               (char)(0x80+58)
#define KEYBOARD_KEY_PAGEDOWN           (char)(0x80+59)
#define KEYBOARD_KEY_INSERT             (char)(0x80+60)
#define KEYBOARD_KEY_DELETE             (char)(0x80+61)
#define KEYBOARD_KEY_UNUSED2            (char)(0x80+62)
#define KEYBOARD_KEY_UNUSED3            (char)(0x80+63)

#define KEYBOARD_KEY_SPEC_BASE          (char)(0x80+64)
#define KEYBOARD_KEY_ALT_FN(num)        (char)(0x80+64+(num))

#endif

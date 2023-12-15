/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * keyboard.h --
 *
 */
#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "keyboard_dist.h"



typedef enum {
   KEYBOARD_NONE = 0,
   KEYBOARD_COS,
   KEYBOARD_VMK,
} Keyboard_Audience;

typedef void (*Keyboard_Callback)(void);

void Keyboard_EarlyInit(void);
void Keyboard_Init(void);
void Keyboard_SetAudience(Keyboard_Audience audience);
void Keyboard_SetCallback(Keyboard_Callback callback);
char Keyboard_Read(void);
char Keyboard_Poll(void);
void Keyboard_ForwardKeyFromHost(char c);

#endif

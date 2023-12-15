/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * bluescreen.h --
 *
 *	vmkernel bluescreen.
 */

#ifndef _BLUESCREEN_H
#define _BLUESCREEN_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

extern void BlueScreen_Init(void);
extern Bool BlueScreen_Post(const char *text, VMKFullExcFrame *fullFrame);
extern void BlueScreen_PostException(VMKFullExcFrame *fullFrame);
extern void BlueScreen_Append(const char *text);
extern Bool BlueScreen_Posted(void);
extern void BlueScreen_On(void);

#endif

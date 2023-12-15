/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _POST_H_
#define _POST_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


typedef Bool (POSTCallback)(void *clientData,
			    int id,
			    SP_SpinLock *lock,
			    SP_Barrier *barrier);

EXTERN VMK_ReturnStatus POST_Start(void);
EXTERN VMK_ReturnStatus POST_IsDone(void);
EXTERN Bool POST_Register(char *name, POSTCallback *callback, void *clientData);


#endif // ifdef _POST_H_

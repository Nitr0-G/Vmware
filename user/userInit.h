/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userInit.h --
 *
 *	UserWorld initialization.  (See also vmkernel/private/user.h)
 */

#ifndef VMKERNEL_USER_USERINIT_H
#define VMKERNEL_USER_USERINIT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

struct User_CartelInfo;

VMK_ReturnStatus UserInit_CartelInit(User_CartelInfo* uci);
VMK_ReturnStatus UserInit_CartelCleanup(User_CartelInfo* uci);

#endif /* VMKERNEL_USER_USERINIT_H */

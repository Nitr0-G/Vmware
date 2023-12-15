/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userIdent.h - 
 *	UserWorld identity
 */

#ifndef VMKERNEL_USER_IDENT_H
#define VMKERNEL_USER_IDENT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "identity.h"

extern VMK_ReturnStatus UserIdent_CheckAccessMode(Identity *ident,
                                                  uint32 accessMode,
                                                  LinuxUID objUID,
                                                  LinuxGID objGID,
                                                  LinuxMode objMode);

extern VMK_ReturnStatus UserIdent_CheckAccess(Identity *ident,
                                              uint32 openFlags,
                                              LinuxUID objUID,
                                              LinuxGID objGID,
                                              LinuxMode objMode);


#endif // VMKERNEL_USER_IDENT_H

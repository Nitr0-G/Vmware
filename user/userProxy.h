/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userProxy.h - 
 *	UserWorld io proxy objects
 */

#ifndef VMKERNEL_USER_USERPROXY_H
#define VMKERNEL_USER_USERPROXY_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "linuxAPI.h"

struct UserObj;
struct User_CartelInfo;

extern VMK_ReturnStatus UserProxy_OpenRoot(struct User_CartelInfo *uci,
                                           struct UserObj **objOut);

extern VMK_ReturnStatus UserProxy_CartelCleanup(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserProxy_CartelInit(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserProxy_Sync(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserProxy_RegisterThread(struct User_CartelInfo *uci,
                                                 World_ID worldID,
                                                 Identity *ident);
extern VMK_ReturnStatus UserProxy_Setresuid(struct User_CartelInfo *uci,
                                            LinuxUID ruid, LinuxUID euid,
                                            LinuxUID suid);
extern VMK_ReturnStatus UserProxy_Setresgid(struct User_CartelInfo *uci,
                                            LinuxGID rgid, LinuxGID egid,
                                            LinuxGID sgid);
extern VMK_ReturnStatus UserProxy_Setgroups(struct User_CartelInfo *uci,
                                            uint32 ngids, LinuxGID *gids);
extern VMK_ReturnStatus UserProxy_SendStatusAlert(World_ID cartelID,
                                                  const void* msg,
                                                  int length);
extern VMK_ReturnStatus UserProxy_CreateSocket(struct User_CartelInfo* uci,
					       LinuxSocketFamily family,
		                               LinuxSocketType type,
					       LinuxSocketProtocol protocol,
					       struct UserObj** outObj);
extern VMK_ReturnStatus UserProxy_Socketpair(LinuxSocketFamily family,
					     LinuxSocketType type,
					     LinuxSocketProtocol protocol,
					     struct UserObj** obj1,
					     struct UserObj** obj2);
extern VMK_ReturnStatus UserProxy_Uname(struct User_CartelInfo *uci,
                                        LinuxUtsName *utsName);
extern int UserProxy_GetCosProxyPid(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserProxy_IsCosPidAlive(struct User_CartelInfo *uci,
						int cosPid);

/*
 * Proxy connection information for the cartel.
 */
typedef struct {
   int cosPid;
   RPC_Connection cnxToProxyID;
   RPC_Connection cnxToKernelID;
   Bool		  disconnected;
   /*
    * The cartelID is used to set up uniquely named RPC channels between the
    * kernel and the proxy.  The cartelID is chosen because it conveniently
    * doubles as the worldID of the first world in the cartel, which the proxy
    * knows about.  This allows us to bootstrap the connection process.
    */
   World_ID       cartelID;  //XXX fix me
   Semaphore      sema;
   struct UserObj *root;     // UserObj for the "/" directory
   /*
    * The cartel info struct for the cartel that this proxy connection belongs
    * to.  It's OK for us to have a pointer here, as this UserProxy_CartelInfo
    * struct is itself allocated on the cartel's heap, which means that it's
    * impossible for the cartel to have died, because if the cartel dies and
    * this struct is not freed, the machine will PSOD.
    */
   struct User_CartelInfo *uci;
} UserProxy_CartelInfo;

#endif

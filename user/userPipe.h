/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userPipe.h - 
 *	UserWorld pipes
 */

#ifndef VMKERNEL_USER_USERPIPE_H
#define VMKERNEL_USER_USERPIPE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"

#include "semaphore.h"
#include "vmkpoll.h"

struct User_CartelInfo;
struct UserPipe_Buf;

extern VMK_ReturnStatus UserPipe_CreatePipe(struct User_CartelInfo *readCartel,
					    struct User_CartelInfo *writeCartel,
					    struct UserPipe_Buf **pbuf);
extern VMK_ReturnStatus UserPipe_Open(struct User_CartelInfo* uci,
				      int* fd1,
				      int* fd2);

extern VMK_ReturnStatus UserPipe_Poll(struct UserPipe_Buf *pbuf,
				      UserObj_Type type,
				      VMKPollEvent inEvents,
				      VMKPollEvent *outEvents,
				      UserObjPollAction action);
extern VMK_ReturnStatus UserPipe_Read(struct UserPipe_Buf *pbuf,
				      Bool canBlock,
				      UserVA userBuf,
				      uint32 bufLen,
				      uint32 *bytesRead);
extern VMK_ReturnStatus UserPipe_Write(struct UserPipe_Buf *pbuf,
				       Bool canBlock,
				       UserVAConst userBuf,
				       const uint32 bufLen,
				       uint32 *bytesWritten);
extern VMK_ReturnStatus UserPipe_Close(struct UserPipe_Buf *pbuf,
				       UserObj_Type type);
extern VMK_ReturnStatus UserPipe_ToString(struct UserPipe_Buf *pbuf,
					  char *string,
					  int length);
extern VMK_ReturnStatus UserPipe_Sendmsg(struct UserPipe_Buf *pbuf,
					 Bool canBlock,
					 struct LinuxMsgHdr *msg,
					 uint32 len,
					 uint32 *bytesWritten);
extern VMK_ReturnStatus UserPipe_Recvmsg(struct UserPipe_Buf *pbuf,
					 Bool canBlock,
					 struct LinuxMsgHdr *msg,
					 uint32 len,
					 uint32 *bytesRead);
#endif /* VMKERNEL_USER_USERPIPE_H */

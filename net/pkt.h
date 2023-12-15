/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential
 * **********************************************************/

/*
 * pkt.h --
 *    Contains packet structure definitions and accessor functions for the
 *    fields of these structures. All accessors assume that synchronisation
 *    (if required) is done outside the module.
 */

#ifndef _PKT_H_
#define _PKT_H_

#include "net_pkt.h"
#include "vmkstress.h"

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "net_int.h"

#ifdef NO_PKT_INLINE
#undef INLINE
#define INLINE
#endif

#ifdef VMX86_NET_DEBUG
#define NET_DEBUG(x) x
#else
#define NET_DEBUG(x)
#endif

VMK_ReturnStatus  Pkt_ModInit(void);
void              Pkt_ModCleanup(void);

extern MA runtBufferMA;

static INLINE VMK_ReturnStatus
Pkt_PadWithZeroes(PktHandle *pkt, uint32 numZeroBytes)
{
   return Pkt_AppendFrag(runtBufferMA, numZeroBytes, pkt);
}

#ifdef NO_PKT_INLINE
#undef INLINE
#define INLINE inline
#endif

#endif // _PKT_H_

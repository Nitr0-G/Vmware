/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential
 **********************************************************/

#ifndef _LEGACY_ESX_2_H_
#define _LEGACY_ESX_2_H_

#ifdef ESX2_NET_SUPPORT

void NetProc_Init(void);
void NetProc_Cleanup(void);
void NetProc_AddPortset(Portset *ps);
void NetProc_RemovePortset(Portset *ps);
void NetProc_HostChange(Portset *ps, Bool reg);
VMK_ReturnStatus Net_CreatePortsetESX2(const char *name);

/* 
 * we only supported 32 connections to a vswitch in ESX2 plus
 * 1 uplink. (this actually gives us 64, but that's the best
 * we can do and still maintain the old limit)
 */
#define ESX2_MAX_NUM_PORTS_PER_SET 33

#else // !ESX2_NET_SUPPORT

#define NetProc_Init(...)
#define NetProc_Cleanup(...)
#define NetProc_AddPortset(...)
#define NetProc_RemovePortset(...)
#define NetProc_HostChange(...)
#define Net_CreatePortsetESX2(...) VMK_FAILURE

#endif //ESX2_NET_SUPPORT

#endif //_LEGACY_ESX_2_H_

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * net.h  --
 *
 *    Exported interface to vmkernel networking for clients 
 *    within the vmkernel.
 */

#ifndef _NET_H_
#define _NET_H_

#include "vmnix_syscall.h"
#include "net_public.h"
#include "net_sg.h"

/*
 * Portset class and client implementations should rank their specific 
 * locks here, and update SP_RANK_NET_PORTSET to be (<lowest_client> - 1).
 *
 * Also, pay attention to SP_RANK_SCSI_LOWEST if the lock will be
 * held across calls to kseg or others that call into the storage 
 * code.
 *
 */
#ifdef ESX3_NETWORKING_NOT_DONE_YET

// all net(core) locks are strictly less than SP_RANK_NET_HIGHEST
#define SP_RANK_NET_HIGHEST            (SP_RANK_SCSI_LOWEST - 1)
#define SP_RANK_VLANCE_CLIENT          (SP_RANK_NET_HIGHEST - 1) // should be leaf
#define SP_RANK_VMXNET2_CLIENT         (SP_RANK_NET_HIGHEST - 1) // should be leaf
#endif // ESX3_NETWORKING_NOT_DONE_YET

#define SP_RANK_NET_PORTSET            (SP_RANK_VMXNET2_CLIENT - 1)
#define SP_RANK_NET_PORTSET_GLOBAL     (SP_RANK_NET_PORTSET - 1)

// this is the lowest lock rank the networking code has
#define SP_RANK_NET_LOWEST             (SP_RANK_NET_PORTSET_GLOBAL)

// tcpip lock is ranked higher than all core net locks
#define SP_RANK_NET_TCPIP              (SP_RANK_NET_HIGHEST)

// forward decls
typedef struct PktList PktList;
typedef struct Portset Portset;
struct World_InitArgs;
struct World_Handle;

// XXX should really be getting this from devices/net/public/net.h
#define MAX_ETHERNET_CARDS 4

// MAX_ETHERNET_CARDS is the max number of virtual NICs a guest can have
#define MAX_VMM_GROUP_NET_PORTS MAX_ETHERNET_CARDS
typedef struct Net_VmmGroupInfo {
   Net_PortID     portIDs[MAX_VMM_GROUP_NET_PORTS]; // array of connected portIDs
   int            numPorts;                         // active entries
} Net_VmmGroupInfo;

/*
 * we allow creation of multiple types of network (portset), the main 
 * difference between these is the policy they use for routing frames 
 * between their member nodes (ports)
 */
typedef enum {
   NET_TYPE_NULL,           // black hole
   NET_TYPE_LOOPBACK,       // reflects to sender
   NET_TYPE_HUBBED,         // broadcasts to all but the sender
   NET_TYPE_ETHER_SWITCHED, // routes based on destination ethernet address
   NET_TYPE_BOND,           // routes based on loadbalance or failover algorithms
   NET_TYPE_INVALID     
} Net_Type;


// init and cleanup
void              Net_EarlyInit(void);
void              Net_Init(VMnix_SharedData *sharedData);
void              Net_Cleanup(void);

// vmkernel exports
VMK_ReturnStatus  Net_Create(char *name, Net_Type type, unsigned int n);
VMK_ReturnStatus  Net_Destroy(char *name);
VMK_ReturnStatus  Net_Connect(World_ID worldID,
                              const char * name,
                              Net_PortID * portID);
VMK_ReturnStatus  Net_Disconnect(World_ID worldID,
                                 Net_PortID portID);
VMK_ReturnStatus  Net_PortEnable(Net_PortID portID);
VMK_ReturnStatus  Net_PortDisable(Net_PortID portID, Bool force);
VMK_ReturnStatus  Net_WorldInit(struct World_Handle *world, struct World_InitArgs *args);
void              Net_WorldPreCleanup(struct World_Handle* world);
void              Net_WorldCleanup(struct World_Handle* world);
VMK_ReturnStatus  Net_Tx(Net_PortID portID, PktList *pktList);
VMK_ReturnStatus  Net_ConnectUplinkPort(char *portsetName, char *uplinkDevName, 
                                        Net_PortID *portID);
VMK_ReturnStatus  Net_DisconnectUplinkPort(char *portsetName, char *uplinkName);
void             *Net_FindDevice(const char *name);
VMK_ReturnStatus  Net_TxOne(Net_PortID portID, void *srcBuf, uint32 srcBufLen,
                            uint32 flags);
VMK_ReturnStatus  Net_TcpipTx(PktList *pktList);

Proc_Entry       *Net_GetProcRoot(void);

// vmm vmk calls
VMK_ReturnStatus  
Net_VMMDisconnect(DECLARE_ARGS(VMK_NET_VMM_PORT_DISCONNECT));
VMK_ReturnStatus  
Net_VMMPortEnableVmxnet(DECLARE_ARGS(VMK_NET_VMM_PORT_ENABLE_VMXNET));
VMK_ReturnStatus  
Net_VMMPortDisable(DECLARE_ARGS(VMK_NET_VMM_PORT_DISABLE));
VMK_ReturnStatus  
Net_VMMGetPortCapabilities(DECLARE_ARGS(VMK_NET_VMM_GET_PORT_CAPABILITIES));
VMK_ReturnStatus  
Net_VMMVmxnetTx(DECLARE_ARGS(VMK_NET_VMM_VMXNET_TX));
VMK_ReturnStatus  
Net_VMMPinVmxnetTxBuffers(DECLARE_ARGS(VMK_NET_VMM_PIN_VMXNET_TX_BUFFERS));
VMK_ReturnStatus  
Net_VMMPortEnableVlance(DECLARE_ARGS(VMK_NET_VMM_PORT_ENABLE_VLANCE));
VMK_ReturnStatus  
Net_VMMVmxnetUpdateEthFRP(DECLARE_ARGS(VMK_NET_VMM_VMXNET_UPDATE_FRP));
VMK_ReturnStatus
Net_VMMVlanceUpdateIFF(DECLARE_ARGS(VMK_NET_VMM_VLANCE_UPDATE_IFF));
VMK_ReturnStatus
Net_VMMVlanceUpdateLADRF(DECLARE_ARGS(VMK_NET_VMM_VLANCE_UPDATE_LADRF));
VMK_ReturnStatus
Net_VMMVlanceUpdateMAC(DECLARE_ARGS(VMK_NET_VMM_VLANCE_UPDATE_MAC));
VMK_ReturnStatus  
Net_VMMVlanceTx(DECLARE_ARGS(VMK_NET_VMM_VLANCE_TX));
VMK_ReturnStatus  
Net_VMMVlanceRxDMA(DECLARE_ARGS(VMK_NET_VMM_VLANCE_RX_DMA));

// helper functions
uint32           Net_GetIPAddr(const char *cp);

// raw interface, 
// XXX callbacks may be unnecessary
typedef VMK_ReturnStatus (*Net_RxDataCB)(struct Port *, void *, struct PktList *);
typedef struct NetRawCBData {
   Net_RxDataCB routine;
   void *data;
} NetRawCBData;

VMK_ReturnStatus  Net_GetRawCapabilities(Net_PortID portID, uint32 *capabilities);
VMK_ReturnStatus  Net_SetRawCB(Net_PortID portID, NetRawCBData *cbArg);
VMK_ReturnStatus  Net_SetRawTxCompleteCB(Net_PortID portID, NetRawCBData *cbArg);

static INLINE VMK_ReturnStatus  
Net_RawConnect(const char *name, Net_PortID *portID) 
{
   return Net_Connect(INVALID_WORLD_ID, name, portID);
}

static INLINE VMK_ReturnStatus  
Net_RawDisconnect(Net_PortID portID) 
{
    return Net_Disconnect(INVALID_WORLD_ID, portID);
}  

// COS calls

VMK_ReturnStatus Net_HostConnect(VMnix_NetConnectArgs *hostConnectArgs,
                                 Net_PortID *hostPortID);
void             Net_HostDisconnect(World_ID worldID, Net_PortID portID);
VMK_ReturnStatus Net_HostPortEnable(void *data, void **resultp);
VMK_ReturnStatus Net_HostPortDisable(void *data, void **resultp);
VMK_ReturnStatus Net_HostUpdateEthFRP(Net_PortID portID);
VMK_ReturnStatus Net_HostTx(Net_PortID portID);


// everything else after here is "todo"
#ifdef ESX3_NETWORKING_NOT_DONE_YET 

static INLINE VMK_ReturnStatus
Net_GetMACAddrForUUID(uint8 *macAddr)
{
   return VMK_FAILURE;
}

static INLINE VMK_ReturnStatus 
Net_HostGetNICStats(char *devName, void *result)
{
   return VMK_FAILURE;
}

static INLINE VMK_ReturnStatus 
Net_HostIoctl(char *devName, int32 cmd, void *args, int32 *result)
{
   return VMK_FAILURE;
}

static INLINE VMK_ReturnStatus 
Net_HostGetNicState(char *nicName, VMnix_CosVmnicInfo *vmnicInfo)
{
   return VMK_FAILURE;
}

#endif // ESX3_NETWORKING_NOT_DONE_YET 

#endif // _NET_H_

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vmxnet2_vmkdev.h  --
 *
 *   Interface to vmkernel networking for vmxnet2 style devices.
 */

#ifndef _VMXNET2_VMKDEV_H_
#define _VMXNET2_VMKDEV_H_

VMK_ReturnStatus Vmxnet2VMKDev_Tx(Net_PortID portID);
VMK_ReturnStatus Vmxnet2VMKDev_Enable(Port *port, 
                                      VA ddMapped, 
                                      uint32 ddLen, 
                                      uint32 ddOffset, 
                                      uint32 intrActionIdx);
VMK_ReturnStatus Vmxnet2VMKDev_PinTxBuffers(Net_PortID portID);
VMK_ReturnStatus Vmxnet2VMKDev_UpdateEthFRP(Port *port, Eth_Address *unicastAddr);


#endif // _VMXNET2_VMKDEV_H_

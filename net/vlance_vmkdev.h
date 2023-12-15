/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vlance_vmkdev.h  --
 *
 *   Interface to vmkernel networking for vlance devices.
 */

#ifndef _VLANCE_VMKDEV_H_
#define _VLANCE_VMKDEV_H_

VMK_ReturnStatus VlanceVMKDev_Enable(Port *port, uint32 intrActionIdx);
VMK_ReturnStatus VlanceVMKDev_Tx(Port *port, NetSG_Array *sg);
VMK_ReturnStatus VlanceVMKDev_RxDMA(Port *port, 
                                    NetSG_Array *sg,
                                    uint32 *byteCount);

#endif // _VLANCE_VMKDEV_H_

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * cos_vmkdev.h  --
 *
 *   Interface to vmkernel networking for host (aka COS, aka vmnix) 
 *   devices.
 */

#ifndef _COS_VMKDEV_H_
#define _COS_VMKDEV_H_

VMK_ReturnStatus COSVMKDev_Tx(Port *port);
VMK_ReturnStatus COSVMKDev_Enable(Port *port, 
                                  VA cosStateVA, 
                                  uint32 cosStateLen, 
                                  VA cosStateVP);
VMK_ReturnStatus COSVMKDev_UpdateEthFRP(Port *port);

#endif // _COS_VMKDEV_H_

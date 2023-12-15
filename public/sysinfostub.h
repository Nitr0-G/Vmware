/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Placeholder handlers that will be removed shortly.
 */

#ifndef _VMKERNEL_SYSINFOSTUB_H_
#define _VMKERNEL_SYSINFOSTUB_H_

#include "vsiDefs.h"

/*

- root
   |
    --> net
         |
          --> netStats
         |
          --> (vmnic0)
         |      |
         |       ---> netNicsConfig
         |      |
         |       ---> netNicsStats
         |
          --> (vmnic1)
                |
                 ---> netNicsConfig
                |
                 ---> netNicsStats
*/

VSI_DEF_STRUCT(VSI_netStatsStruct, "net stats") {
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, allocqueue, "Queue size: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, freequeue, "Queue free: ");
};

VSI_DEF_STRUCT(VSI_netNicsStatsStruct, "nic stats") {
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, interrupts, "interrupts: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, rx, "Tx packets: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, tx, "Rx packets: ");
};

VSI_DEF_STRUCT(VSI_netNicsConfigStruct, "nic config") {
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, status, "Status: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, promisc, "Promisc: ");
};

VSI_DEF_BRANCH(net, root, "Net config");

VSI_DEF_LEAF(netStats, net, VSI_netStatsGet, 
                           VSI_NULL, 
                           VSI_netStatsStruct, "Net stats");

VSI_DEF_INST_BRANCH(netNics, net, VSI_netNicsList, "Nics list");

VSI_DEF_LEAF(netNicsConfig, netNics, VSI_netNicsConfigGet, 
                                    VSI_netNicsConfigSet, 
                                    VSI_netNicsConfigStruct, "Nic config");

VSI_DEF_LEAF(netNicsStats, netNics, VSI_netNicsStatsGet, 
                                   VSI_NULL, 
                                   VSI_netNicsStatsStruct, "Nic stats");

#endif //_VMKERNEL_SYSINFOSTUB_H_


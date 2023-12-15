/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * reliability_vsi.h --
 *
 * Define sysinfo nodes for relibility module.
 */
#ifndef _RELIABILITY_VSI_H
#define _RELIABILITY_VSI_H

VSI_DEF_ARRAY(VSI_ReliabilityCharArray12, char, 12);
VSI_DEF_ARRAY(VSI_ReliabilityCharArray20, char, 20);
VSI_DEF_ARRAY(VSI_ReliabilityCharArray80, char, 80);
/* Reliability module sysinfo tree 
 * /root
 *   |
 *    --> reliability
 *            | 
 *             --> (relibility items)
 */

VSI_DEF_BRANCH(reliability, 
               root,
               "Reliability Module for VMKernel");

/* The sysinfo tree for the heartbeat reliability item
 * - root
 *    |
 *      --> reliability
 *              |
 *               --> heartbeat
 *                        |
 *                         --> SCSIMonitorStatus
 *                        |
 *                         --> (heartbeatInfo)
 *
 *
 */

VSI_DEF_STRUCT (Heartbeat_InfoStruct, "Heartbeat Information Struct Entry") {
   VSI_DEF_STRUCT_FIELD (VSI_DEC_U64, timestampInMS, "Timestamp(ms)");
   VSI_DEF_STRUCT_FIELD (VSI_DEC_U64, lastNMISentAt, "Last NMI was sent at(ms)");
   VSI_DEF_STRUCT_FIELD (VSI_DEC_U64, maxDelayBetweenTimestamps, 
                                      "Max delay (ms) between timestamps");
   VSI_DEF_STRUCT_FIELD (VSI_DEC_U32, nmiCount, "NMI Count");
};

/* Sysinfo tree for heartbeat reliability module definition starts here */
VSI_DEF_BRANCH (heartbeat, 
                reliability, 
                "PCPU Heartbeat");
VSI_DEF_LEAF (heartbeatStatus, 
              heartbeat, 
              Heartbeat_StatusGet,
              Heartbeat_StatusSet,
              VSI_BOOL,
              "Heartbeat Status");

VSI_DEF_INST_LEAF (PCPUList, 
                   heartbeat, 
                   Heartbeat_PCPUList,
                   Heartbeat_InfoGet,
                   VSI_NULL,
                   Heartbeat_InfoStruct,
                   "Heartbeat Info");
#endif

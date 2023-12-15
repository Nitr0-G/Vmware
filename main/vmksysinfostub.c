/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Placeholder handlers that will be removed shortly.
 */

#include "vm_basic_types.h"
#include "vm_basic_defs.h"
#include "vm_assert.h"

#include "sysinfostub.h"

#define LOGLEVEL_MODULE VSI
#include "log.h"

VMK_ReturnStatus
VSI_netStatsGet(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                VSI_netStatsStruct *outputStruct)
{
   VSI_Param *param;

   Log("params count=%d", VSI_ParamListUsedCount(instanceArgs));
   param = VSI_ParamListGetParam(instanceArgs, 0);
   Log("param0 - type=%d val=%Lx", VSI_ParamGetType(param), 
                                   VSI_ParamGetInt(param));

   outputStruct->allocqueue = 0 + VSI_ParamGetInt(param);
   
   return VMK_NO_ACCESS;
}

VMK_ReturnStatus
VSI_netNicsConfigGet(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                     VSI_netNicsConfigStruct *outputStruct)
{
   VSI_Param *param;

   Log("params count=%d", VSI_ParamListUsedCount(instanceArgs));
   param = VSI_ParamListGetParam(instanceArgs, 0);
   Log("param0 - type=%d val=%Lx", VSI_ParamGetType(param), 
                                   VSI_ParamGetInt(param));

   outputStruct->status = 0 + VSI_ParamGetInt(param);
   
   return VMK_NO_ACCESS;
}

VMK_ReturnStatus
VSI_netNicsStatsGet(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                    VSI_netNicsStatsStruct *outputStruct)
{
   VSI_Param *param;

   Log("params count=%d", VSI_ParamListUsedCount(instanceArgs));
   param = VSI_ParamListGetParam(instanceArgs, 0);
   Log("param0 - type=%d val=%Lx", VSI_ParamGetType(param), 
                                   VSI_ParamGetInt(param));

   outputStruct->tx = 0 + VSI_ParamGetInt(param);
   
   return VMK_NO_ACCESS;
}

VMK_ReturnStatus
VSI_netNicsConfigSet(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, VSI_ParamList *inputArgs)
{
   VSI_Param *param;

   Log("params count=%d", VSI_ParamListUsedCount(inputArgs));
   param = VSI_ParamListGetParam(inputArgs, 0); 
   Log("param type=%d val=%Lx", VSI_ParamGetType(param), VSI_ParamGetInt(param));

   return VMK_NO_SUCH_ZOMBIE;
}

VMK_ReturnStatus
VSI_netNicsList(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, VSI_ParamList *instanceListOut)
{
   VSI_Param *param;
   VMK_ReturnStatus status;

   Log("params count=%d", VSI_ParamListUsedCount(instanceArgs));
   param = VSI_ParamListGetParam(instanceArgs, 0);
   Log("param0  - type=%d val=%Lx", VSI_ParamGetType(param), 
                                    VSI_ParamGetInt(param));
   Log("out count=%d", VSI_ParamListAllocCount(instanceListOut));

   status = VSI_ParamListAddString(instanceListOut, "vmnic0");
   if (status != VMK_OK) {
      return status;
   }

   status = VSI_ParamListAddString(instanceListOut, "vmnic1");
   if (status != VMK_OK) {
      return status;
   }

   return VMK_CROSS_DEVICE_LINK;
}

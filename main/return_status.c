/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * return_status.c --
 *
 * 	VMkernel return status operations.
 */

#include "vm_types.h"
#include "return_status.h"

#define DEFINE_VMK_ERR(_err, _str, _uerr) _str,
#define DEFINE_VMK_ERR_AT(_err, _str, _val, _uerr) _str, 
const char *returnStatusStrs[] = {
   VMK_ERROR_CODES
};
#undef DEFINE_VMK_ERR
#undef DEFINE_VMK_ERR_AT

/*
 *----------------------------------------------------------------------
 *
 * VMK_ReturnStatusToString --
 *
 * 	Returns human-readable string corresponding to "status".
 *
 * Results: 
 * 	Returns human-readable string corresponding to "status".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
VMK_ReturnStatusToString(VMK_ReturnStatus status)
{
   if (status >= VMK_GENERIC_LINUX_ERROR) {
      return "Opaque service console status";
   }

   if (status >= VMK_FAILURE) {
      status -= VMK_FAILURE - 1;
   }

   if ((uint32)status >= ARRAYSIZE(returnStatusStrs)) {
      return "Unknown status";
   }

   return returnStatusStrs[status];
}

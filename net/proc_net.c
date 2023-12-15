/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * proc_net.c  --
 *
 *   procfs implementation for vmkernel networking.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "parse.h"

static Bool        procNetInitialized = FALSE;
static Proc_Entry  procNetRootNode;
static Proc_Entry  procNetCommandNode;

/*
 *----------------------------------------------------------------------------
 *
 * ProcNetCommandRead --
 *
 *    /proc/vmware/net/command proc read handler. 
 *    Dumps the help blurb the proc node.
 *
 * Results:
 *    VMK_OK
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static int
ProcNetCommandRead(Proc_Entry  *entry, 
                   char        *page, 
                   int         *len)
{
   *len = 0;

   Proc_Printf(page, len, 
               "commands:\n\n"
               "\tcreate <name> <ports> <type>\n\n"
               "\t\twhere <name> is any string to uniquely identify the device\n"
               "\t\t<ports> is number of ports to create and type is one of:\n"
               "\t\t[null | loopback | hub | bond | switch] \n\n"
               "\tdestroy <name>\n\n"
               "\tlink <portset> <uplink>\n\n"
               "\tunlink <portset> <uplink>\n\n");

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcNetCommandWrite --
 *
 *      Update configuration info for the adapter.
 *
 * Results: 
 *	VMK_OK.
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
static int
ProcNetCommandWrite(Proc_Entry  *entry,
                    char        *page,
                    int         *lenp)
{
   char *argv[4] = {NULL, NULL, NULL, NULL};
   int argc = Parse_Args(page, argv, 4);

   if (argc == 2) {
      if (strcmp(argv[0], "destroy") == 0) {
         Net_Destroy(argv[1]);
         return VMK_OK;
      }
   } else if (argc == 3) {
      if (strcmp(argv[0], "link") == 0) {
         Net_PortID portID;
         Net_ConnectUplinkPort(argv[1], argv[2], &portID);
         return VMK_OK;
      } else if (strcmp(argv[0], "unlink") == 0) {
         Net_DisconnectUplinkPort(argv[1], argv[2]);
         return VMK_OK;
      }
   } else if (argc == 4) {
      if (strcmp(argv[0], "create") == 0) {
         Net_Type type = NET_TYPE_INVALID;
         int numports;
         
         if (strncmp(argv[3], "null", strlen("null")) == 0) {
            type = NET_TYPE_NULL;
         } else if (strncmp(argv[3], "loopback", strlen("loopback")) == 0) {
            type = NET_TYPE_LOOPBACK;
         } else if (strncmp(argv[3], "hub", strlen("hub")) == 0) {
            type = NET_TYPE_HUBBED;
         } else if (strncmp(argv[3], "switch", strlen("switch")) == 0) {
            type = NET_TYPE_ETHER_SWITCHED;
         } else if (strncmp(argv[3], "bond", strlen("bond")) == 0) {
            type = NET_TYPE_BOND;
         } else {
            Log("bad type for create: %s", argv[3]);
            return VMK_BAD_PARAM;
         }

        
         Parse_Int(argv[2], 4, &numports);
         if ((numports < 0) || (numports > MAX_NUM_PORTS_PER_SET)){
            Log("bad number of ports %s", argv[2]);
            return VMK_BAD_PARAM;
         }

         Net_Create(argv[1], type, numports);

         return VMK_OK;
      }   
   }

   Log("Bad command: %s %s %s %s", 
       argv[0] != NULL ? argv[0] : "<NULL>", 
       argv[1] != NULL ? argv[1] : "", 
       argv[2] != NULL ? argv[2] : "", 
       argv[3] != NULL ? argv[3] : "");

   return VMK_BAD_PARAM;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcNet_ModInit --
 *
 *      Initialize networking proc root nodes.
 *
 * Results: 
 *	Root /proc/net node is created and populated with common 
 *      subnodes.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
ProcNet_ModInit(void)
{
   Proc_InitEntry(&procNetRootNode);
   Proc_Register(&procNetRootNode, "net", TRUE);
  
   Proc_InitEntry(&procNetCommandNode);
   procNetCommandNode.parent = &procNetRootNode;
   procNetCommandNode.read = ProcNetCommandRead;
   procNetCommandNode.write = ProcNetCommandWrite;
   Proc_Register(&procNetCommandNode, "command", FALSE);

   procNetInitialized = TRUE;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcNet_ModCleanup --
 *
 *      Cleanup the proc stuff for networking module.
 *
 * Results: 
 *	Networking procfs resources are released.
 *
 * Side effects:
 *	Many.
 *
 *----------------------------------------------------------------------
 */
void
ProcNet_ModCleanup(void)
{
   procNetInitialized = FALSE;

   Proc_Remove(&procNetRootNode);
}

/*
 *----------------------------------------------------------------------------
 *
 * ProcNet_GetRootNode --
 *
 *    Get the networking proc root node.
 *
 * Results:
 *    The root proc node for networking is returned.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Proc_Entry *
ProcNet_GetRootNode(void)
{
   ASSERT(procNetInitialized);

   return &procNetRootNode;
}


/*
 *----------------------------------------------------------------------------
 *
 * ProcNet_Register --
 *
 *    Conditional wrapper for Proc_Register().
 *
 * Results:
 *    proc node might be registered.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
ProcNet_Register(Proc_Entry *entry, char *name, Bool isDirectory)
{
   if (CONFIG_OPTION(NET_USE_PROC)) {
      LOG(5, "processing registration request for %p (%s)", 
          entry, name);
      Proc_Register(entry, name, isDirectory);
   } else {
      LOG(1, "ignoring registration request for %p (%s)", 
          entry, name);
   }
}

/*
 *----------------------------------------------------------------------------
 *
 * ProcNet_Remove --
 *
 *    Conditional wrapper for Proc_Remove().
 *
 * Results:
 *    proc node might be removed.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
ProcNet_Remove(Proc_Entry *entry)
{
   if (CONFIG_OPTION(NET_USE_PROC)) {
      LOG(5, "processing removal request for %p", entry);
      Proc_Remove(entry);
   } else {
      LOG(1, "ignoring removal request for %p", entry);
   }
}



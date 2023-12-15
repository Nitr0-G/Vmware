/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "libc.h"
#include "splock.h"
#include "proc.h"
#include "vmk_scsi_dist.h"
#include "config.h"
#include "bluescreen.h"
#include "serial.h"
#include "memalloc_dist.h"
#include "vmnix_if_dist.h"
#include "world.h"
#include "migrateBridge.h"
#include "conduit_bridge.h"
#include "fsSwitch.h"
#include "logterm.h"
#include "cpusched.h"
#include "swap.h"
#include "memsched.h"
#include "timer.h"
#include "statusterm.h"

#include "gen_vmksysinfodefs.h"
#include "vsiDefs.h"
#include "config_vsi.h"

#define LOGLEVEL_MODULE Config
#include "log.h"

#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	OBJ_BUILD	(1)
#else
#define	OBJ_BUILD	(0)
#endif

#ifdef VMX86_DEBUG
#define DEBUG_BUILD     (1)
#else
#define DEBUG_BUILD     (0)
#endif

#define CONFIG_READ FALSE
#define CONFIG_WRITE TRUE

/*
 * Define the Proc_Entry variables for the config module subdirectories
 */
#undef CONFIG_DEFMODULE
#define CONFIG_DEFMODULE(_module, _moduleName) CONFIGMOD_##_module,
typedef enum {
   CONFIG_MODULES_LIST
   CONFIG_NUM_MODULES
} Config_Module;

static Proc_Entry configModuleDirs[CONFIG_NUM_MODULES];

typedef struct Config_Descriptor {
   Config_Module module;
   char *name;
   unsigned minVal;
   unsigned maxVal;
   unsigned defaultVal;
   char *help;
   /*
    * If set, called on every successful proc read or write.
    * "write" is true if this is a proc write function, false otw.
    * valueChanged is true iff the new val differs from the old one.
    */
   ConfigCallback callback;
   Bool hidden;
   Proc_Entry entry;
} Config_Descriptor;

typedef struct Config_StringDescriptor {
   Config_Module module;
   char *name;
   char *defaultVal;
   /* 
    * Valid contains a pointer to the vaild input characters
    * for a config string.  The string "**" allows any input.
    */
   char *valid;
   char *help;
   ConfigCallback callback;
   Bool hidden;
   Proc_Entry entry;
} Config_StringDescriptor;

#undef CONFIG_DEFMODULE
#define CONFIG_DEFMODULE(_module, _moduleName) CONFIG_##_module##_OPTS(_##_module, _moduleName)

/*
 * Integer configuration variables.
 * See vmkernel/distribute/config_dist.h to add variables.
 * Do not attempt to add variables here.
 */
#define D(_module, _moduleName, _macro, _name, _args...) {CONFIGMOD##_module, XSTR(_name) , ## _args},
#define S(unused...)
static Config_Descriptor configDesc[] = {
  CONFIG_MODULES_LIST 
};

/*
 * String configuration variables.
 * See vmkernel/distribute/config_dist.h to add variables.
 * Do not attempt to add variables here.
 */
#undef D
#undef S
#define D(unused...) 
#define S(_module, _moduleName, _macro,  _name, _args...) {CONFIGMOD##_module, XSTR(_name) , ## _args},
static Config_StringDescriptor configStrDesc[] = {
   CONFIG_MODULES_LIST
};
#undef D
#undef S

#define NUM_CONFIG_INT_DESC  (sizeof(configDesc)/sizeof(Config_Descriptor))
#define NUM_CONFIG_STR_DESC (sizeof(configStrDesc)/sizeof(Config_StringDescriptor))
#define CONFIG_NUM_STR (CONFIG_TOTAL_NUM - CONFIG_NUM_INT)

static Proc_Entry configDir;


#define D(_module, _moduleName, _macro, _name, _min, _max, _default, _args...) _default, 
#define S(unused...) 
unsigned configOption[CONFIG_NUM_INT] = {
   CONFIG_MODULES_LIST
};
#undef D
#undef S

#define D(unused...) 
#define S(_module, _moduleName, _macro, _name, _default, _args...) _default, 
static char *configStrOption[CONFIG_NUM_STR] = {
   CONFIG_MODULES_LIST
};
#undef D
#undef S

static int ConfigReadString(Proc_Entry *entry, char *buffer, int *len);
static int ConfigReadInteger(Proc_Entry *entry, char *buffer, int *len);
static int ConfigWriteString(Proc_Entry *entry, char *buffer, int *len);
static int ConfigWriteInteger(Proc_Entry *entry, char *buffer, int *len);

/*
 *----------------------------------------------------------------------
 *
 * Config_Init --
 *
 *      Initialization routine for config subsystem.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Config_Init(void)
{
   int i;

   ASSERT(NUM_CONFIG_INT_DESC == CONFIG_NUM_INT);
   ASSERT(NUM_CONFIG_STR_DESC == CONFIG_NUM_STR);

   Proc_InitEntry(&configDir);
   Proc_Register(&configDir, "config", TRUE);

   #undef CONFIG_DEFMODULE
   #define CONFIG_DEFMODULE(_module, _moduleName)                             \
      Proc_InitEntry(&configModuleDirs[CONFIGMOD_##_module]);                 \
      configModuleDirs[CONFIGMOD_##_module].parent = &configDir;              \
      Proc_Register(&configModuleDirs[CONFIGMOD_##_module], XSTR(_moduleName), TRUE);
   CONFIG_MODULES_LIST
   

   for (i=0; i<CONFIG_NUM_INT; i++) {
      Proc_InitEntry(&configDesc[i].entry);

      configDesc[i].entry.read = ConfigReadInteger;
      configDesc[i].entry.write = ConfigWriteInteger;
      configDesc[i].entry.parent = &configModuleDirs[configDesc[i].module];
      configDesc[i].entry.canBlock = FALSE;
      configDesc[i].entry.private = (void*)i;

      if (configDesc[i].hidden) {
         Proc_RegisterHidden(&configDesc[i].entry, configDesc[i].name, FALSE);
      } else {
         Proc_Register(&configDesc[i].entry, configDesc[i].name, FALSE);
      }
   }
   
   for (i=0; i<CONFIG_NUM_STR; i++) {
      Proc_InitEntry(&configStrDesc[i].entry);
      /*
       * Copy the ro data into a rw memory area.
       */
      if(configStrDesc[i].defaultVal != NULL) {
         if((configStrOption[i] = Mem_Alloc(strlen(configStrDesc[i].defaultVal) + 1)) != NULL) {
            strcpy(configStrOption[i], configStrDesc[i].defaultVal);
         }
      }
      configStrDesc[i].entry.read = ConfigReadString;
      configStrDesc[i].entry.write = ConfigWriteString;
      configStrDesc[i].entry.parent = &configModuleDirs[configStrDesc[i].module];
      configStrDesc[i].entry.canBlock = FALSE;
      configStrDesc[i].entry.private = (void*)i;

      if (configStrDesc[i].hidden) {
         Proc_RegisterHidden(&configStrDesc[i].entry, configStrDesc[i].name, FALSE);
      } else {
         Proc_Register(&configStrDesc[i].entry, configStrDesc[i].name, FALSE);
      }


   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Config_RegisterCallback --
 *
 *      Installs the function "callback" as the callback handler for
 *      the config option specified by "index". Callback should not be NULL.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers the callback on the config option.
 *
 *-----------------------------------------------------------------------------
 */
void
Config_RegisterCallback(uint32 index, ConfigCallback callback)
{
   ASSERT(callback != NULL);
   if (callback == NULL) {
      Warning("cannot register null config callback");
      return;
   }

   configDesc[index].callback = callback;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigReadInteger --
 *
 *      Callback for read operation on config proc entry.
 *
 * Results: 
 *      VMK_OK, value of config option in buffer
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigReadInteger(Proc_Entry *entry,
                  char       *buffer,
                  int        *len)
{
   *len = 0;

   int indx = (int)entry->private;
   Config_Descriptor *desc = &configDesc[indx];
   if (desc->help) {
      Proc_Printf(buffer, len, "%s (%s) [default = %u]: %u\n",
                  desc->name, desc->help, desc->defaultVal, configOption[indx]);
   } else {
      Proc_Printf(buffer, len, "%s [default = %u]: %u\n", 
                  desc->name, desc->defaultVal, configOption[indx]);
   }
   
   if(desc->callback) {
      desc->callback(CONFIG_READ, FALSE, indx);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigReadString --
 *
 *      Callback for read operation on config proc string entry.
 *
 * Results: 
 *      VMK_OK, value of config option in buffer
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigReadString(Proc_Entry *entry,
                 char       *buffer,
                 int        *len)
{
   *len = 0;

   int indx = (int)entry->private;
   Config_StringDescriptor *desc = &configStrDesc[indx];

   if (desc->help) {
      Proc_Printf(buffer, len, "%s (%s) [default = \"%s\"]: %s\n",
                     desc->name, desc->help, desc->defaultVal, configStrOption[indx]);
   } else {
      Proc_Printf(buffer, len, "%s [default = \"%s\"]: %s\n", 
                     desc->name, desc->defaultVal, configStrOption[indx]);
   }
   
   if(desc->callback) {
      desc->callback(CONFIG_READ, FALSE, indx);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigWriteIntegerInternal --
 *
 *      Internal helper function for config writes
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
ConfigWriteIntegerInternal(int indx, unsigned val)
{
   Config_Descriptor *desc = &configDesc[indx];  
   VMK_ReturnStatus status = VMK_OK;
   Bool valueChanged = FALSE;

   ASSERT_NOT_IMPLEMENTED(indx < CONFIG_NUM_INT);

   if ((val < desc->minVal) || (val > desc->maxVal)) {
      LOG(0, "\"%s\" %d != %d-%d", configDesc[indx].name, val,
          configDesc[indx].minVal, configDesc[indx].maxVal);
      return VMK_BAD_PARAM;
   }

   LOG(1, "\"%s\" = %d", configDesc[indx].name, val);

   if (configOption[indx] != val) {
      valueChanged = TRUE;
      Log("\"%s\" = %d", configDesc[indx].name, val);
   }

   configOption[indx] = val;
   
   if(desc->callback) {
      /*status = */desc->callback(CONFIG_WRITE, valueChanged, indx);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigWriteStringInternal --
 *
 *      Internal helper function for config string writes.
 *      Note that newVal isn't null terminated.
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
ConfigWriteStringInternal(int indx, const char *newVal, int newValSize)
{
   Config_StringDescriptor *desc = &configStrDesc[indx];  
   VMK_ReturnStatus status = VMK_OK;
   Bool valueChanged = FALSE;
   char *newValCopy;
   const char *ptr;

   ASSERT_NOT_IMPLEMENTED(indx < CONFIG_NUM_STR);

   if (!strncmp(newVal, "default", 7)) {
      newVal = desc->defaultVal;
      newValSize = strlen(newVal);
   }


   /* Ensure newVal has only the allowed characters */
   if (strcmp(desc->valid, "**")) {
      for (ptr = newVal; ptr < (newVal + newValSize); ptr++) {
         if (strchr(desc->valid, *ptr) == NULL) {
            return VMK_BAD_PARAM;
         }
      }
   }

   valueChanged = strcmp(configStrOption[indx], newVal) != 0;

   if (valueChanged) {
      if ((newValCopy = Mem_Alloc(newValSize + 1)) == NULL) {
         return VMK_NO_MEMORY;
      }

      memcpy(newValCopy, newVal, newValSize);
      newValCopy[newValSize] = '\0';

      Mem_Free(configStrOption[indx]);
      configStrOption[indx] = newValCopy;
      Log("\"%s\" = \"%s\"", configStrDesc[indx].name, newValCopy);
   }

   if (desc->callback) {
      /*status = */desc->callback(CONFIG_WRITE, valueChanged, indx);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigWriteInteger --
 *
 *      Callback for write operation on config proc entry.
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigWriteInteger(Proc_Entry *entry,
                   char       *buffer,
                   int        *len)
{
   int indx = (int)entry->private;
   Config_Descriptor *desc = &configDesc[indx];  
   unsigned val;
   char *endp;
   
   if (!strncmp(buffer, "default", 7)) {
      val = desc->defaultVal;
      endp = buffer + 7;
   } else {
      val = simple_strtoul(buffer, &endp, 0);
   }

   /* Check for extra garbage on the line */
   while (endp < (buffer+*len)) {
      if ((*endp != '\n') && (*endp != ' ')) {
         return VMK_BAD_PARAM;
      }
      endp++;
   }

   return ConfigWriteIntegerInternal(indx, val);
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigWriteString --
 *
 *      Callback for write operation on config string proc entry.
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigWriteString(Proc_Entry *entry,
                  char       *buffer,
                  int        *len)
{
   int indx = (int)entry->private;
   int length = *len;

   /* We must account for when the user input config string has a trailing
    * newline (i.e. echo string > config), and when it does not have
    * a trailing newline (i.e. echo -n string > config).
    *
    * Note that buffer isn't necessarily null terminated.
    */
   if(buffer[length - 1] == '\n') {
      length--;
   } 
   return ConfigWriteStringInternal(indx, buffer, length);
}

/*
 *----------------------------------------------------------------------
 *
 * Config_GetOption --
 *
 *      Accessor fuction for use in linux drivers / vmklinux where the
 *      configOption array isn't exported.
 *
 * Results: 
 *	the current value of the config option opt
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
unsigned
Config_GetOption(ConfigOptions opt)
{
   ASSERT(opt < CONFIG_NUM_INT);
   return configOption[opt];
}

/*
 *----------------------------------------------------------------------
 *
 * Config_GetStringOption --
 *
 *      Accessor fuction for use in linux drivers / vmklinux where the
 *      configStrOption array isn't exported.
 *
 * Results: 
 *	the current string value of the config option opt
 *	The strings are read-only in early initialization, then
 *	become read-write.
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
char *
Config_GetStringOption(ConfigStrOptions opt)
{
   ASSERT((opt >= CONFIG_NUM_INT) && (opt < CONFIG_TOTAL_NUM));
   return configStrOption[opt - CONFIG_NUM_INT];
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigSysInfoIntNodeToIndex --
 *
 *      Maps the sysinfo node id to the index of the config option in
 *      the configDesc array.
 *
 *      Assumes that sysinfo node ids are allocated from contiguous 
 *      blocks.
 *
 * Results: 
 *      The index, or -1 on failure
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
static int
ConfigSysInfoIntNodeToIndex(VSI_NodeID node)
{
   int cfgIndex = node + CONFIG_NUM_INT - VSI_NODE_CFG_LastIntegerNode; /* (cfg node last ) */
   LOG(2, "Mapping %d -> %d", node, cfgIndex);
   if (cfgIndex < 0 || cfgIndex >= CONFIG_NUM_INT) {
      ASSERT(FALSE);
      return -1;
   }
   return cfgIndex;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigSysInfoStrNodeToIndex --
 *
 *      Maps the sysinfo node id to the index of the config option in
 *      the configStrDesc array.
 *
 *      Assumes that sysinfo node ids are allocated from contiguous 
 *      blocks.
 *
 * Results: 
 *      The index, or -1 on failure
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
static int
ConfigSysInfoStrNodeToIndex(VSI_NodeID node)
{
   int cfgIndex = node - VSI_NODE_CFG_LastIntegerNode - 1; /* (cfg node last int node ) */
   if (cfgIndex < 0 || cfgIndex >= CONFIG_NUM_STR) {
      ASSERT(FALSE);
      return -1;
   }
   LOG(2, "Mapping %d -> %d", node, cfgIndex);
   return cfgIndex;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Config_SysInfoGetInt --
 *
 *      Returns data about the config option passed in, and triggers the
 *      callback associated with the node.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Config_SysInfoGetInt(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, ConfigVsiData *out)
{
   int cfgIndex = ConfigSysInfoIntNodeToIndex(nodeID);
   Config_Descriptor *d = &configDesc[cfgIndex];
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(VSI_ParamListUsedCount(instanceArgs) == 0);

   if (cfgIndex < 0) {
      return VMK_BAD_PARAM;
   }

   memset(out, 0, sizeof *out);
   out->cur = configOption[cfgIndex];
   out->min = d->minVal;
   out->max = d->maxVal;
   out->def = d->defaultVal;
   strncpy(out->description, d->help, sizeof out->description - 1);

   if (d->callback) {
      /*status = */d->callback(CONFIG_READ, FALSE, cfgIndex);
   }

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Config_SysInfoSetInt --
 *
 *      Sets data about the config option passed in, and triggers the
 *      callback associated with the node.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers the callback on the config option.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Config_SysInfoSetInt(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, VSI_ParamList *inputArgs)
{
   int cfgIndex = ConfigSysInfoIntNodeToIndex(nodeID);
   VSI_Param *param;

   if (cfgIndex < 0 || VSI_ParamListUsedCount(inputArgs) != 1) {
      return VMK_BAD_PARAM;
   }

   param = VSI_ParamListGetParam(inputArgs, 0); 
   if (VSI_ParamGetType(param) != VSI_PARAM_INT64) {
      return VMK_BAD_PARAM;
   }

   return ConfigWriteIntegerInternal(cfgIndex, VSI_ParamGetInt(param));
}


/*
 *-----------------------------------------------------------------------------
 *
 * Config_SysInfoGetStr --
 *
 *      Returns data about the config option passed in, and triggers the
 *      callback associated with the node.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Config_SysInfoGetStr(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, ConfigVsiStrData *out)
{
   int cfgIndex = ConfigSysInfoStrNodeToIndex(nodeID);
   Config_StringDescriptor *d = &configStrDesc[cfgIndex];
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(VSI_ParamListUsedCount(instanceArgs) == 0);

   if (cfgIndex < 0) {
      return VMK_BAD_PARAM;
   }
   memset(out, 0, sizeof *out);
   /* 
    * Subtract one from the length of the dest strings so that everything is
    * automatically null terminated.
    */
   strncpy(out->def, d->defaultVal, sizeof out->def - 1);
   strncpy(out->valid, d->valid, sizeof out->def - 1);
   strncpy(out->cur, configStrOption[cfgIndex], sizeof out->cur - 1);
   strncpy(out->description, d->help, sizeof out->description - 1);

   if (d->callback) {
      /*status = */d->callback(CONFIG_READ, FALSE, cfgIndex);
   }

   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Config_SysInfoSetStr --
 *
 *      Sets data about the config option passed in, and triggers the
 *      callback associated with the node.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers the callback on the config option.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Config_SysInfoSetStr(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, VSI_ParamList *inputArgs)
{
   int cfgIndex = ConfigSysInfoStrNodeToIndex(nodeID);
   VSI_Param *param;
   char *newVal;

   if (cfgIndex < 0 || VSI_ParamListUsedCount(inputArgs) != 1) {
      return VMK_BAD_PARAM;
   }

   param = VSI_ParamListGetParam(inputArgs, 0); 
   if (VSI_ParamGetType(param) != VSI_PARAM_STRING128) {
      return VMK_BAD_PARAM;
   }

   newVal = VSI_ParamGetString(param);

   return ConfigWriteStringInternal(cfgIndex, newVal, sizeof *newVal);
}

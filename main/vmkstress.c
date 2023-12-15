/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "libc.h"
#include "proc.h"

#include "vmkstress_dist.h"
#include "vmkstress.h"

#define LOGLEVEL_MODULE VmkStress
#define LOGLEVEL_MODULE_LEN 9
#include "log.h"

static Proc_Entry stressDir;

VmkStressOption vmkStressOptions[] = {
   VMK_STRESS_OPTIONS
};

int VmkStressOptionRead(Proc_Entry *, char *, int *);
int VmkStressOptionWrite(Proc_Entry *, char *, int *);

/*
 * Stress callback handler prototype.
 */
typedef void (*VmkStressCBHandler)(uint32, VmkStressProcFlag);
static VmkStressCBHandler 
vmkStressCBHandlers[NUM_VMK_STRESS_OPTIONS];
static void VmkStressRegisterCBHandler(VmkStressOptionIndex,
				   VmkStressCBHandler) __attribute__((unused));

/*
 * ---------------------------------------------------------------------
 * 
 * VmkStressRegisterCBHandler --
 * 	
 * 	Register a function which will be notified when the 
 * 	corresponding stress value is read/written.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	Registers a callback handler.
 * 	 
 * --------------------------------------------------------------------- 
 */
static void
VmkStressRegisterCBHandler(VmkStressOptionIndex index,
		       VmkStressCBHandler handler)
{
   ASSERT(index >=0 && index < NUM_VMK_STRESS_OPTIONS);
   if (vmkStressCBHandlers[index] != NULL) {
      Warning("Overwriting handler %p for index %u\n",
	      vmkStressCBHandlers[index], index);
   }
   LOG(3, "Registering Handler %p for stress option %d", handler, index);
   vmkStressCBHandlers[index] = handler;
}

/*
 * ---------------------------------------------------------------------
 * 
 * VmkStressCBNotify --
 *
 * 	Notify the callback handler for a vmkernel stress option.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	Side effects of the corresponding stress handler.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VmkStressCBNotify(VmkStressOption *option,    
		  VmkStressOptionIndex index, 
		  VmkStressProcFlag flag)
{
   ASSERT(index >=0 && index < NUM_VMK_STRESS_OPTIONS);
   VmkStressCBHandler handler = vmkStressCBHandlers[index];
   if (handler != NULL) {
      LOG(4, "Calling stress callback handler %p for option %s,"
	  " index %d", handler, option->name, index);
      handler(option->val, flag);
   }
   else {
      LOG(3, "Stress callback handler was not called for option %s,"
	  " index %d. No Handler registered", option->name, index);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * VmkStress_Init --
 *
 *      Initialize vmkernel stress options.
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
VmkStress_Init(void)
{
   int i;

   stressDir.read = NULL;
   stressDir.write = NULL;
   stressDir.parent = NULL;
   stressDir.private = 0;
   Proc_RegisterHidden(&stressDir, "stress", TRUE);

   for (i = 0; i < NUM_VMK_STRESS_OPTIONS; i++) {
      Proc_InitEntry(&vmkStressOptions[i].proc);

      vmkStressOptions[i].proc.read = VmkStressOptionRead;
      vmkStressOptions[i].proc.write = VmkStressOptionWrite;
      vmkStressOptions[i].proc.parent = &stressDir;
      vmkStressOptions[i].proc.canBlock = FALSE;
      vmkStressOptions[i].proc.private = (void*)i;

      Proc_RegisterHidden(&vmkStressOptions[i].proc, vmkStressOptions[i].name, FALSE);

      if (vmkStressOptions[i].rand != 0) {
         vmkStressOptions[i].seed = Util_RandSeed();
      }
   }
   /* Register callback handlers */
}


/*
 *----------------------------------------------------------------------
 *
 * VmkStressOptionRead --
 *
 *      Callback for read operation on vmkernel stress option proc entry.
 *
 * Results: 
 *      VMK_OK, value of stress option option in buffer
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
VmkStressOptionRead(Proc_Entry *entry,
                    char       *buffer,
                    int        *len)
{
   VmkStressOption *option = &vmkStressOptions[(int)entry->private];
   
   *len = 0;
   Proc_Printf(buffer, len,
               "%s\n\n"
               "%s\n\n"
               "\tdefault:        %u\n"
               "\tmin:            %u\n"
               "\tmax:            %u\n"
               "\trecommended:    %u\n"
               "\tcurrent:        %u\n"
               "\thits:           %u\n"
               "\tcountdown:      %u\n"
               "\trandomization:  %u (only affects countdown mode)\n\n",
               option->name, option->help, 
               option->def,
               option->min,
               option->max,
               option->rec,
               option->val,
               option->hits,
               option->count,
               option->rand);
   VmkStressCBNotify(option, (int)entry->private, VMK_STRESS_PROC_READ);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VmkStressOptionWrite --
 *
 *      Callback for write operation on vmkernel stress option proc entry.
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
VmkStressOptionWrite(Proc_Entry *entry,
                     char       *buffer,
                     int        *len)
{
   VmkStressOption *option = &vmkStressOptions[(int)entry->private];
   uint32 val = option->val;
   uint32 rand = option->rand;
   uint32 seed = option->seed;
   char *endp;
   
   if (strncmp(buffer, "default", 7) == 0) {
      val = option->def;
      endp = buffer + 7;
   } else if (strncmp(buffer, "recommended", 11) == 0) {
      val = option->rec;
      endp = buffer + 11;
   } else if (strncmp(buffer, "random", 6) == 0) {
      endp = buffer + 6;
      LOG(0, "random %s", endp);
      rand = simple_strtoul(endp, &endp, 0);
      LOG(0, "random %s %u", endp, rand);
      if (rand != 0) {
         if (option->seed == 0) {
            seed = Util_RandSeed();
         }
      }
   } else{
      val = simple_strtoul(buffer, &endp, 0);
   }

   while (endp < (buffer+*len)) {
      if ((*endp != '\n') && (*endp != ' ')) {
         return VMK_BAD_PARAM;
      }
      endp++;
   }
   
   if ((val < option->min) || (val > option->max)) {
      return VMK_BAD_PARAM;
   }

   if (rand != 0 && ((0xffffffff - val) < val/rand)) {
      return VMK_BAD_PARAM;
   }
  
   if (option->val != val) {
      Log("\"%s\" val %u -> %u", option->name, option->val, val);
   }

   if (option->rand != rand) {
      Log("\"%s\" rand %u -> %u", option->name, option->rand, rand);
   }

   option->val = val;
   option->rand = rand;
   option->seed = seed;

   VmkStress_CounterReset(option);

   VmkStressCBNotify(option, (int)entry->private, VMK_STRESS_PROC_WRITE);
   return VMK_OK;
}

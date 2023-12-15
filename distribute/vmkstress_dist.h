/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkstress_dist.h --
 *
 *	vmkernel stress options set from the host.
 */

#ifndef _VMKSTRESS_DIST_H_
#define _VMKSTRESS_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"
#include "proc_dist.h"
#include "util_dist.h"
#include "netstress_dist.h"

#ifdef  VMX86_DEBUG
#define VMK_STRESS_DEBUG
#endif // VMX86_DEBUG

/* stress callback flags */
#define VMK_STRESS_PROC_READ  1
#define VMK_STRESS_PROC_WRITE 2
typedef char VmkStressProcFlag;


/*
 * Declare all VMK_STRESS_OPTIONS here, including a min, max,
 * default, and recommended value, and a randomization factor.  
 *
 * The recommended value is what somone who doesn't have intimate
 * knowledge of the system might use as a starting point for this
 * option.  For options which are used as counters, a large nonzero
 * default may be useful to excercise uncommon paths without overly
 * affecting performance.
 *
 * The random field is used to randomize the period for options used
 * as counters, which is useful when setting several counters which
 * may affect the same event or data.  The value of random is used as
 * a fraction of the counter value to bound the random period offsets.
 * For example setting random to 100 will result in a range of periods
 * (val - val/100) to (val + val/100), setting random to 1, will
 * result in periods ranging from 1 to (2 * val)
 *
 * Each option is configurable at runtime via 
 * /proc/vmware/stress/<ProcNodeName>
 * 
 * Declaration format:
 *
 *    VMK_STRESS_DECL_OPTION(<MACRO_NAME>,                                             \
 *                           "ProcNodeName",                                           \
 *                           <min>, <max>, <default>, <recommended>, <random>,         \
 *                           "Brief description or help blurb")                        \
 *
 *
 * Usage:
 *
 *    Any of the following may be used with any option (release or debug).
 *
 *       VMK_STRESS_DEBUG_OPTION(<MACRO_NAME>)
 *
 *          Returns the boolean value of the given option,
 *          only present in debug builds.
 *
 *
 *       VMK_STRESS_DEBUG_VALUE(<MACRO_NAME>)
 *
 *          Returns the integer value of the given option,
 *          only present in debug builds.
 *
 *
 *       VMK_STRESS_DEBUG_COUNTER(<MACRO_NAME>)
 *          
 *          Returns TRUE every Nth time called, where N is the option's 
 *          current value, only present in debug builds.
 *
 *   These "RELEASE" versions do the same thing as their counterparts 
 *   above, but will not compile away in release builds, which is useful 
 *   for options which may be help in debugging or reproducing customer
 *   issues in the field, and which don't lie in performance critical paths.
 * 
 *       VMK_STRESS_RELEASE_OPTION(<MACRO_NAME>)
 *       VMK_STRESS_RELEASE_VALUE(<MACRO_NAME>)
 *       VMK_STRESS_RELEASE_COUNTER(<MACRO_NAME>)
 *
 */

// list of stress options available only in debug builds
#define VMK_STRESS_DEBUG_OPTIONS                                                  \
   VMK_NET_STRESS_DEBUG_OPTIONS                                                   \
   VMK_STRESS_DECL_OPTION(WORLD_PANIC,                                            \
                          "WorldPanicStress",                                     \
                          0, 10000, 0, 0, 0,                                      \
                          "Panic VMM World on Nth BH_Check")                      \
   VMK_STRESS_DECL_OPTION(ASSERT_STRESS,                                          \
                          "AssertStress",                                         \
                          0, 0xffffffff, 0, 0xffffff, 0,                          \
                          "Force the Nth vmkernel Assert check to fail\n"         \
                          "(obj only)")                                           \
   VMK_STRESS_DECL_OPTION(IRQ_VECTOR_MIGRATE,                                     \
                          "InterruptTrackerMigrate",                              \
                          0, -1, 0, 100, 0,                                       \
                          "Migrate interrupt vectors monitored by the\n"          \
                          "interrupt tracker to the next CPU every Nth time\n"    \
                          "the IT timer runs (whose frequency can be modified\n"  \
                          "in /proc/vmware/config/InterruptTrackingPeriod)")      \
   VMK_STRESS_DECL_OPTION(RPC_WAKEUP,                                             \
                          "RpcWakeup",                                            \
                          0, 0xffffffff, 0, 100, 0,                               \
                          "Force wakeup on all RPC connections every Nth\n"       \
                          "RPC Get/Send/Post")                                    \
   VMK_STRESS_DECL_OPTION(MIG_NET_FLAKE,                                          \
			  "MigNetFlake",                                          \
			  0, -1, 0, 30,	0,                                        \
                          "Induce a networking error every N seconds.")           \
   VMK_STRESS_DECL_OPTION(CPU_GROUP_CACHE_WRAP,                                   \
                          "CpuGroupCacheWrap",                                    \
                          0xff, 0xffffffff, 0xfffffff, 0xffff, 0,                 \
                          "Force simulated wraparound for cpu scheduler\n"        \
                          "group vtime cache at specified generation count.\n"    \
                          "Note that smaller values are more stressful.\n")       \

// list of stress options available in all builds
#define VMK_STRESS_RELEASE_OPTIONS                                                \
   VMK_NET_STRESS_RELEASE_OPTIONS                                                 \
   VMK_STRESS_DECL_OPTION(MEM_SWAP,                                               \
			  "MemSwap",                                              \
			  0, 1, 0, 1, 0,                                          \
                          "Force VM to swap if it uses more than half of its\n"   \
                          "physical memory, regardless of actual memory\n"        \
                          "pressure on the system.")                              \
   VMK_STRESS_DECL_OPTION(MEM_SHARE,                                              \
			  "MemShare",                                             \
			  0, 1, 0, 1, 0,                                          \
                          "Force vmkernel to share pages even if the contents\n"  \
                          "don't match some existing page.  In other words,\n"    \
                          "every candidate page for which sharing is attempted\n" \
                          "is marked COW, even if there is no actual sharing.\n"  \
                          "Caution: MemSwap and MemShare stress should *not*\n"   \
                          "both be enabled at the same time.")                    \
   VMK_STRESS_DECL_OPTION(MEM_SHARE_COS,                                          \
			  "MemShareCOS",                                          \
			  0, 1, 0, 1, 0,                                          \
                          "When set simulates the case where the COS touches a\n" \
                          "large number of guest pages.  This causes the COS\n"   \
                          "to touch a lot of shared pages, and any access to\n"   \
                          "a page (read or write) from the COS will break\n"      \
                          "sharing.")                                             \
   VMK_STRESS_DECL_OPTION(MEM_REMAP_LOW,                                          \
			  "MemRemapLow",                                          \
			  0, -1, 0, 64, 1,                                        \
                          "When set causes the vmkernel to remap pages even if\n" \
                          "the pages are already in low memory.  Note that\n"     \
                          "this flag is only really effective for VMs doing a\n"  \
                          "lot of network activity, since the vmkernel only\n"    \
                          "remaps pages used by network transmits.  With this\n"  \
                          "option enabled,  every Nth page used for a network\n"  \
                          "transmit is remapped.")                                \
   VMK_STRESS_DECL_OPTION(MEM_REMAP_NODE,                                         \
			  "MemRemapNode",                                         \
			  0, -1, 0, 60, 0,                                        \
                          "Stress page migration code by altering memory node\n"  \
                          "affinity and page migration rates every N seconds.")   \
   VMK_STRESS_DECL_OPTION(IO_FORCE_COPY,                                          \
			  "IOForceCopy",                                          \
			  0, 1, 0, 1, 0,                                          \
                          "Force a copy on I/O transfers even if data is\n"       \
                          "below 4GB")                                            \
   

#define VMK_STRESS_RELEASE_COUNTER(_x)  (UNLIKELY(VmkStress_Counter(VMK_STRESS_##_x)))
#define VMK_STRESS_RELEASE_OPTION(_x)   (UNLIKELY(VmkStress_Option(VMK_STRESS_##_x)))
#define VMK_STRESS_RELEASE_VALUE(_x)    (VmkStress_Value(VMK_STRESS_##_x))


#ifdef VMK_STRESS_DEBUG

#define VMK_STRESS_OPTIONS            VMK_STRESS_DEBUG_OPTIONS VMK_STRESS_RELEASE_OPTIONS
#define vmk_stress_debug              1
#define VMK_STRESS_DEBUG_COUNTER      VMK_STRESS_RELEASE_COUNTER
#define VMK_STRESS_DEBUG_OPTION       VMK_STRESS_RELEASE_OPTION
#define VMK_STRESS_DEBUG_VALUE        VMK_STRESS_RELEASE_VALUE

#else // ! VMK_STRESS_DEBUG

#define VMK_STRESS_OPTIONS            VMK_STRESS_RELEASE_OPTIONS
#define vmk_stress_debug              0
#define VMK_STRESS_DEBUG_COUNTER(_x)  FALSE
#define VMK_STRESS_DEBUG_OPTION(_x)   FALSE
#define VMK_STRESS_DEBUG_VALUE(_x)    0

/*
 * just so the callsites won't have undefined refs to the debug options, 
 * generate an enum of them that we don't ever use
 */
#define VMK_STRESS_DECL_OPTION(mname, name, min, max, def, rec, rand, help) \
   VMK_STRESS_##mname,
typedef enum DummyVmkStressOptionIndex{
   VMK_STRESS_DEBUG_OPTIONS
} DummyVmkStressOptionIndex;
#undef VMK_STRESS_DECL_OPTION

#endif // (!) VMK_STRESS_DEBUG


/*
 *  generate the enumeration of all of the stress counter indexes
 */
#define VMK_STRESS_DECL_OPTION(mname, name, min, max, def, rec, rand, help) \
   VMK_STRESS_##mname,

typedef enum VmkStressOptionIndex{
   VMK_STRESS_OPTIONS
   NUM_VMK_STRESS_OPTIONS
} VmkStressOptionIndex;

#undef VMK_STRESS_DECL_OPTION

typedef struct VmkStressOption {
   char       *name;  // node name
   uint32      min;   // minimum value
   uint32      max;   // maximum value
   uint32      def;   // default value
   uint32      rec;   // recommended value for stress
   uint32      val;   // current value
   uint32      count; // current count
   uint32      hits;  // hit count
   int32       rand;  // random period offset bound for counters
   uint32      seed;  // seed for random mode
   char       *help;  // short description/help
   Proc_Entry  proc;  // proc node for this option
} VmkStressOption;

/*
 * redefine the generator to initialize the array defined in vmkstress.c
 */
#define VMK_STRESS_DECL_OPTION(mname, name, min, max, def, rec, rand, help) \
   {name, min, max, def, rec, def, def, 0, rand, 0, help},


extern VmkStressOption vmkStressOptions[];

static INLINE void
VmkStress_CounterReset(VmkStressOption *option)
{
   /*
    * use private copies of these two values to avoid dividing by zero
    * when someone else changes either to zero while we're in here.
    * the other effects of that race don't prevent things from working
    * sufficiently well for our purposes, so we only protect against the
    * divide by zero issue.
    */
   int32 val = option->val;    
   int32 rand = option->rand; 

   option->count = val;
   // randomize the period if configured
   if ((rand != 0) && (val != 0)) {
      // sign extend the generated numbers as they are only 31 bits
      int32 sseed = option->seed << 1; // unsigned shift
      sseed >>= 1;                     // signed shift
      // XXX ignoring wraparound
      option->count += sseed % (val / rand);
      option->seed = Util_FastRand(option->seed);
   }
}   

static INLINE Bool
VmkStress_Counter(VmkStressOptionIndex i)
{
   if (vmkStressOptions[i].count > 1) {
      vmkStressOptions[i].count--;
      return FALSE;
   } else if (vmkStressOptions[i].count == 1) {
      vmkStressOptions[i].hits++;
      VmkStress_CounterReset(&vmkStressOptions[i]);
      return TRUE;
   }
   return FALSE;
}



static INLINE Bool
VmkStress_Option(VmkStressOptionIndex i)
{
   return (vmkStressOptions[i].val > 0);
}

static INLINE uint32
VmkStress_Value(VmkStressOptionIndex i)
{
   return vmkStressOptions[i].val;
}

#endif // _VMKSTRESS_DIST_H_


/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pshare.c --
 *
 * 	Content-based transparent page sharing.
 *
 *
 * Overview:
 *
 *   Transparent page sharing was introduced in the Disco project.
 *   The basic idea is to save memory by eliminating redundant copies
 *   of pages, such as program text or read-only data.  Once copies
 *   are identified, multiple guest PPNs are mapped to the same MPN
 *   copy-on-write.  Disco added explicit hooks to observe copies
 *   as they were created, which required some guest modifications
 *   and/or restrictions on sharing (e.g., must use same non-persistent
 *   virtual disk).
 *
 *   We take a different approach, identifying copies based only on
 *   page contents, without requiring any guest modifications.
 *   This is done by hashing the contents of MPNs, and collapsing
 *   copies as they are found.  Many variations and extensions are
 *   possible.
 *
 * Enabling Page Sharing
 *
 *   The page sharing module is enabled by default.  To disable
 *   page sharing, specify the "-m" option to vmkloader.
 *   
 * Data Structure
 *
 *   Pshare is internally organized as a large hash table.  Pshare
 *   allocates a table of "chains" upon initialization.
 *
 *   Each element of the "chains" table contains an MPN, which is
 *   interpreted as pointer to a "frame" or PSHARE_MPN_NULL.  This
 *   organization is used to implement a hash table with chaining;
 *   each non-null entry represents a linked list of frames that
 *   collide at the same hash table index (i.e., same low-order key
 *   bits).
 *
 *   "frames" are allocated by MPage module, then cast to
 *   "PShareFrame".  Each element of the "frame" contains information
 *   about a shared page frame, a key computed by hashing the contents
 *   of the shared page, and a reference count indicating the level of
 *   sharing.  Each frame also contains a "next" field to support
 *   singly-linked lists of frames.
 *
 * Memory Overhead
 *
 *   One "chain" is allocated for each MPN of boot time memory.  One
 *   "frame" is allocated for each MPN of memory (hot added or boot
 *   time).
 *
 * Locking
 *
 *   A single lock currently protects all page-sharing state.  If
 *   contention becomes a problem, an array of locks could be used
 *   based on low-order key bits (i.e., each lock covers many chains)
 *   and a separate alloc lock could be used for frame allocation.
 *
 * Hash Key Aliasing
 *
 *   The use of a 64-bit hash function with good statistical properties
 *   makes it extremely unlikely that two pages with different contents
 *   will hash to the same 64-bit value.  Theoretically, the number of
 *   pages needed before we would expect to see a single false match
 *   is approximately sqrt(2^64), or 4G pages.  Since a large PAE-enabled
 *   system can have at most 64GB physical memory, or 16M pages, the odds
 *   against encountering even a single false match across all pages in 
 *   memory are more than 100 : 1.  An implementation must compare the
 *   actual page contents after a hash key match to ensure they match,
 *   but it can simply punt on sharing pages that are false matches.
 *
 * Speculative Hints
 *
 *   As an optimization, COW traces need not be placed on unshared
 *   pages.  Instead, a frame with a single reference (i.e., first
 *   page with a given key) can be marked as a speculative "hint".  A
 *   hint encodes a subset of the full 64-bit key, plus a "backmap"
 *   reference to the first page; for example { worldID, PPN,
 *   LOW32(key) }.  On a subsequent match with a second page (e.g.,
 *   low-order key bits match, followed by a confirming rehash or full
 *   page comparison), a client of this module can post an action to
 *   worldID to retry sharing the PPN.  Similarly, worldID could be
 *   informed if a hint for one of its pages becomes stale.  Note that
 *   speculative frames are truly hints; without a COW trace, there is
 *   no notification if the first page is modified, so its hint key
 *   bits may be incorrect.
 *
 * Future Modifications
 *
 *   Originally one idea was to use of IA32 memory type range
 *   registers (MTRRs) as an alternative or additional mechanism for
 *   write-protecting regions containing shared pages.  However, the
 *   MTRRs aren't very useful, since the processor doesn't generate a
 *   fault when a read-only region is written (IIRC, read-only regions
 *   just prevent cache writebacks to memory).
 *
 *   Could expand the key in hint frames by one byte, since hint
 *   frames are 1 byte smaller than regular frames
 *
 *   Could get rid GetNumContMPNs/AssignContMPNs interface by just 
 *   allocating chains and keeping them mapped via xmap.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "kseg.h"
#include "libc.h"
#include "memmap.h"
#include "memalloc.h"
#include "proc.h"
#include "memsched.h"
#include "parse.h"
#include "world.h"
#include "numa.h"
#include "util.h"
#include "hash.h"
#include "post.h"
#include "mpage.h"
#include "pshare.h"

#define	LOGLEVEL_MODULE PShare
#include "log.h"

/*
 * Compilation flags
 */

// debugging
#define	PSHARE_DEBUG		(vmx86_debug && vmx86_devel)
#define	PSHARE_DEBUG_VERBOSE	(0)

// targeted debugging
#define	PSHARE_DEBUG_COLLIDE	(0)

// stats
#define	PSHARE_STATS_HOT	(vmx86_stats)

/*
 * Constants
 */

// expected sizes (in bytes)
#define	PSHARE_CHAIN_SIZE        (3)
#define	PSHARE_FRAME_SIZE       (16)

// known keys
#define	PSHARE_KNOWN_NAME_LEN	(32)
#define	PSHARE_NKNOWN		(16)

// statistics limits
#define	PSHARE_STATS_HOT_MAX		(10)
#define	PSHARE_STATS_COLLIDE_MAX	(10)

/*
 * Macros
 */

#define	LOW32(x)		((x) & 0xffffffff)
#define	LOW24(x)		((x) & 0xffffff)
#define	LOW16(x)		((x) & 0xffff)
#define	LOW8(x)			((x) & 0xff)

// structured logging macros
#define PShareDebug(fmt, args...) \
 if (PSHARE_DEBUG) LOG(0, fmt , ##args)
#define PShareDebugVerbose(fmt, args...) \
 if (PSHARE_DEBUG_VERBOSE) LOG(0, fmt , ##args)

/*
 * Types
 */

// A chain holds a 24-bit MPN number, which names the first MPN in
// this chain.
typedef struct {
   uint16 lo __attribute__ ((packed));
   uint8  hi __attribute__ ((packed));
} PShareChain __attribute__ ((packed));

// Note: The ppn field must not come at the end of the struct, 
// since GCC generates 32-bit accesses to that field and we want
// don't want access to this field to touch memory outside
// of the frame.  Otherwise, we could fault.
// (Same logic applies to 'PShareFrame::next')
typedef struct {
   uint32 key;          // lower 32 bits of key hash
   unsigned ppn   : 24; // back map to world/ppn
   World_ID worldID;    //  mapping this hint
} PShareHintFrame  __attribute__ ((packed));

typedef struct {
   uint64 key;     // page hash
   uint32 count;   // refcount
} PShareRegularFrame __attribute__ ((packed));

typedef struct {
   MPage_Tag tag;
   unsigned next   : 24; // link to next MPN
   union {
      PShareHintFrame hint;
      PShareRegularFrame regular;
   } u;
} PShareFrame __attribute__ ((packed));

typedef struct {
   uint64 key;    // page hash
   uint32 count;  // refcount
   MPN mpn;       // machine page number
} PShareHotFrame;

typedef struct {
   uint64 key;	  	// page hash
   uint32 count;  	// total collisions
   World_ID worldID;	// last colliding world with key
   PPN ppn;	  	// last colliding PPN with key
} PShareCollision;

typedef struct {
   uint32 hashtblPages;         // # pages in the hash table
   uint32 hashtblHints;         // # hints in the hash table

   uint32 pageAdd;		// pages added
   uint32 pageRemove;		// pages removed
   uint32 pageCount;		// pages active
   uint32 pageUnshared;		// pages unshared
   uint32 hintAdd;		// hints added
   uint32 hintRemove;		// hints removed
   uint32 hintCount;		// hints active
   uint32 peakCount;		// peak frame count

   // shared pages with largest counts
   PShareHotFrame hot[PSHARE_STATS_HOT_MAX];

   // reported hash collisions
   PShareCollision collide[PSHARE_STATS_COLLIDE_MAX];
   uint32 collisionCount;
   uint32 collisionLog;
} PShareStats;

typedef struct {
   uint64 key;				// hash key
   char   name[PSHARE_KNOWN_NAME_LEN];	// descriptive name
} PShareKnownKey;

typedef struct {
   SP_SpinLockIRQ lock;		// for mutual exclusion

   Bool enabled;		// page sharing enabled?
   Bool debug;			// debugging flag

   MA chains;			// array of 24-bit indexes
   uint32 nChains;		// number of chains (power of 2)
   uint32 nChainPages;		// chains data size (in pages)
   uint32 chainsMask;		// table size mask

   // known hash values (e.g. 0xff-filled page)
   PShareKnownKey known[PSHARE_NKNOWN];
   uint32 nKnown;

   // zero pages, one replica per NUMA node
   uint64 zeroKey[NUMA_MAX_NODES];
   MPN zeroMPN[NUMA_MAX_NODES];

   // boot time memory range, for POST test
   MPN bootTimeMinMPN;
   MPN bootTimeMaxMPN;

   PShareStats stats;		// statistics
   MPN readMPN;			// current MPN for procfs "mpn" node

   Proc_Entry procDir;		// procfs node "/proc/vmware/pshare"
   Proc_Entry procStatus;	// procfs node "/proc/vmware/pshare/status"
   Proc_Entry procHot;		// procfs node "/proc/vmware/pshare/hot"
   Proc_Entry procOverhead;	// procfs node "/proc/vmware/pshare/overhead"
   Proc_Entry procCollisions;	// procfs node "/proc/vmware/pshare/collisions"
   Proc_Entry procMPN;		// procfs node "/proc/vmware/pshare/mpn"   
} PShare;

/*
 * Globals
 */
static Bool   pshareEnabledFlag = FALSE;
static PShare pshare;


/*
 * Local functions
 */

// primitive operations
static PShareChain *PShareMapChain(PShare *p, uint32 index, KSEG_Pair **pair);

// higher-level operations
static void PShareInit(MPN minMPN, MPN maxMPN, uint32 reqSize, MPN startMPN);
static void PShareReset(PShare *pshare);
static VMK_ReturnStatus PShareRemovePage(PShare *p,
                                         MPN mpn,
                                         uint64 key,
                                         Bool unsharedOnly,
                                         uint32 *count);
static VMK_ReturnStatus PShareAddPage(PShare *p,
                                      MPN mpn,
                                      uint64 key,
                                      Bool sharedOnly,
                                      MPN *mpnShared,
                                      uint32 *count,
                                      MPN *mpnHint);
static VMK_ReturnStatus PShareLookupPage(PShare *p, MPN mpn,
                                         uint64 *key, uint32 *count);
static VMK_ReturnStatus PShareKeyToMPN(PShare *p,
                                       uint64 key,
                                       MPN *mpn);

// hint operations
static VMK_ReturnStatus PShareLookupHint(PShare *p,
                                         MPN mpn,
                                         uint64 *key,
                                         World_ID *worldID,
                                         PPN *ppn);
static VMK_ReturnStatus PShareAddHint(PShare *p,
                                      uint64 key,
                                      MPN mpn,
                                      World_ID worldID,
                                      PPN ppn);
static VMK_ReturnStatus PShareRemoveHint(PShare *p,
                                         MPN mpn,
                                         World_ID worldID,
                                         PPN ppn);

// procfs operations
static void PShareStatsUpdateHot(PShareStats *stats, uint64 key, MPN mpn, uint32 count);
static int  PShareProcStatusRead(Proc_Entry *e, char *buf, int *len);
static int  PShareProcHotRead(Proc_Entry *e, char *buf, int *len);
static int  PShareProcCollisionsRead(Proc_Entry *e, char *buf, int *len);
static int  PShareProcOverheadRead(Proc_Entry *e, char *buf, int *len);
static int  PShareProcMPNRead(Proc_Entry *e, char *buf, int *len);
static int  PShareProcMPNWrite(Proc_Entry *e, char *buf, int *len);

/*
 * Utility operations
 */

static INLINE int32
Percentage(int32 n, int32 d)
{
   if (d == 0) {
      return(0);
   } else {
      return((n * 100) / d);
   }
}

static INLINE SP_IRQL
PShareLock(PShare *p)
{
   return(SP_LockIRQ(&p->lock, SP_IRQL_KERNEL));
}

static INLINE void
PShareUnlock(PShare *p, SP_IRQL prevIRQL)
{
   SP_UnlockIRQ(&p->lock, prevIRQL);
}

static INLINE Bool
PShareIsLocked(PShare *p)
{
   return SP_IsLockedIRQ(&p->lock);
}

static INLINE MPN
PShareChainGet(PShareChain *c)
{
   return((((uint32) c->hi) << 16) | c->lo);
}

static INLINE void
PShareChainSet(PShareChain *c, uint32 value)
{
   c->lo = value & 0xffff;
   c->hi = (value >> 16) & 0xff;
}


static INLINE void
PShareFrameSetRegular(PShareFrame *f,
                      uint64 key,
                      uint32 count,
                      MPN next)
{
   f->next   = next;
   f->tag    = MPAGE_TAG_PSHARE_REGULAR;
   f->u.regular.key = key;
   f->u.regular.count = count; 

   // check no bits get lost
   ASSERT(f->next == next);
}


static INLINE void
PShareFrameSetInvalid(PShareFrame *f)
{
   f->next = PSHARE_MPN_NULL;
   f->tag  = MPAGE_TAG_INVALID;
}


static INLINE void
PShareFrameSetHint(PShareFrame *f,
                   uint64 key,
                   World_ID worldID,
                   PPN ppn,
                   uint32 next)
{
   f->next           = next;
   f->tag            = MPAGE_TAG_PSHARE_HINT;
   // note: hint key is the lower 32 bit of the full key
   // (the type conversion is implicit here)
   f->u.hint.key     = key;
   f->u.hint.worldID = worldID;
   f->u.hint.ppn     = ppn;

   // check no bits got lost
   ASSERT(f->u.hint.key == LOW32(key));
   ASSERT(f->next == next);
   ASSERT(f->u.hint.ppn == ppn);
}


static INLINE Bool
PShareFrameIsHint(const PShareFrame *f)
{
   // sanity
   ASSERT(f->tag == MPAGE_TAG_PSHARE_REGULAR ||
          f->tag == MPAGE_TAG_PSHARE_HINT);
   return(f->tag == MPAGE_TAG_PSHARE_HINT);
}

static INLINE Bool
PShareFrameIsRegular(const PShareFrame *f)
{
   // sanity
   ASSERT(f->tag == MPAGE_TAG_PSHARE_REGULAR ||
          f->tag == MPAGE_TAG_PSHARE_HINT);
   return(f->tag == MPAGE_TAG_PSHARE_REGULAR);
}

static INLINE Bool
PShareFrameIsInvalid(const PShareFrame *f)
{
   return (f->tag == MPAGE_TAG_INVALID);
}

static INLINE Bool
PShareHintKeyMatch(uint64 hintKey, uint64 key)
{
   return(LOW32(hintKey) == LOW32(key));
}

static INLINE Bool
PShareFrameHintMatch(const PShareFrame *f, uint64 key)
{
   ASSERT(PShareFrameIsHint(f));
   return(PShareHintKeyMatch(f->u.hint.key, key));
}

/*
 * Internal operations
 */

/*
 *-----------------------------------------------------------------------------
 *
 * PShare_HashToNodeHash --
 *
 *     Converts a standard 64-bit page hash into a NUMA-aware hash,
 *     which uses the NUMA node as the least-significant bits of the key.
 *
 * Results:
 *     Returns 64-bit NUMA-aware hash
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint64
PShare_HashToNodeHash(uint64 hash, NUMA_Node nodeNum)
{
   ASSERT(nodeNum != INVALID_NUMANODE);
   if (NUMA_GetNumNodes() > 1) {
      return ((hash >> NUMA_LG_MAX_NODES) << NUMA_LG_MAX_NODES) | ((uint64)nodeNum);
   } else {
      return hash;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PShare_HashPage --
 *
 *     Returns a 64-bit hash code to represent the content of this page
 *     along with its NUMA node location (if on a NUMA system).
 *     Note: two pages located on different nodes, but with identical
 *     contents, will have different hash keys. They will differ only
 *     in the last log2(NUMA_MAX_NODES) bits.
 *
 * Results:
 *     Returns 64-bit hash
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint64
PShare_HashPage(MPN mpn)
{
   KSEG_Pair *mpnPair;
   uint64 contentHash;
   int nodeNum;
   void *data;

   data = Kseg_MapMPN(mpn, &mpnPair);
   contentHash = Hash_Page(data);
   Kseg_ReleasePtr(mpnPair);

   nodeNum = NUMA_MPN2NodeNum(mpn);
   if (nodeNum == INVALID_NUMANODE) {
      nodeNum = 0;
   }

   return PShare_HashToNodeHash(contentHash, nodeNum);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareKnownKeyName --
 *
 *      Find and return name associated with "key" in "p".
 *
 * Results: 
 *	Returns name associated with "key", or NULL if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
PShareKnownKeyName(const PShare *p, uint64 key)
{
   uint32 i;

   // search for known key name
   for (i = 0; i < p->nKnown; i++) {
      if (p->known[i].key == key) {
         return(p->known[i].name);
      }
   }

   // not found
   return(NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareKnownKeyAdd --
 *
 *      Associate "name" with "key" in "p".
 *
 * Results: 
 *	Returns TRUE iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PShareKnownKeyAdd(PShare *p, uint64 key, const char *name)
{
   // fail if no space or existing entry for key
   if ((p->nKnown >= PSHARE_NKNOWN) ||
       (PShareKnownKeyName(p, key) != NULL)) {
      return(FALSE);
   }

   // add new entry
   (void) strncpy(p->known[p->nKnown].name, name, PSHARE_KNOWN_NAME_LEN);
   p->known[p->nKnown].key = key;
   p->nKnown++;

   // succeed
   return(TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareStatsUpdateHot --
 *
 *      Update "stats" to maintain set of most-shared pages to reflect
 *	that the shared page with the specified "key" and "mpn" has
 *	the current "count".
 *
 * Results: 
 *	Modifies "stats".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
PShareStatsUpdateHot(PShareStats *stats, uint64 key, MPN mpn, uint32 count)
{
   int i, min;

   if (!PSHARE_STATS_HOT) {
      return;
   }

   // update max count
   stats->peakCount = MAX(stats->peakCount, count);

   // search table for match
   min = 0;
   for (i = 0; i < PSHARE_STATS_HOT_MAX; i++) {
      // already in table?
      if (stats->hot[i].key == key) {
         // update count, done
         stats->hot[i].count = count;
         return;
      }

      // track minimum count
      if (stats->hot[i].count < stats->hot[min].count) {
         min = i;
      }
   }

   // replace existing entry?
   if (count > stats->hot[min].count) {
      stats->hot[min].key = key;
      stats->hot[min].mpn = mpn;
      stats->hot[min].count = count;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcStatusRead --
 *
 *      Callback for read operation on "pshare/status" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcStatusRead(UNUSED_PARAM(Proc_Entry *entry),
                     char *buffer,
                     int  *len)
{
   PShare *p = &pshare;
   PShareStats *stats = &p->stats;

   int32 i, nCOW, nCOW1, nHint, nTrack, nUnique, nReclaim, nConsume, nZero;
   SP_IRQL prevIRQL;
   uint32 nUsed;
   *len = 0;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_BAD_PARAM);
   }

   // snapshot total memory usage
   nUsed = MemSched_TotalVMPagesUsed();

   // acquire lock
   prevIRQL = PShareLock(p);

   // convenient abbrevs
   nCOW = stats->pageCount;
   nCOW1 = stats->pageUnshared;
   nUnique = stats->hashtblPages + stats->hashtblHints;
   nHint = stats->hintCount;
   nTrack = nCOW + nHint;

   // compute total savings (n.b. same as "nTrack - nUnique")
   nConsume = nUnique - nHint;
   nReclaim = nCOW - nConsume;

   // format main header
   Proc_Printf(buffer, len,
               "%-10s %8s  %6s  %6s  %6s\n",
               "name", "pages", "MB", "%track", "%used");

   // lookup empty page count
   nZero = 0; 
   for (i=0; i < NUMA_GetNumNodes(); i++) {
      uint64 zeroKey;
      uint32 zeroCount;
      if (PShareLookupPage(p, p->zeroMPN[i], &zeroKey, &zeroCount) == VMK_OK) {
         nZero += zeroCount;
      } 
   }

   // format main statistics
   Proc_Printf(buffer, len,
               "size       %8u  %6d\n"
               "track      %8d  %6d  %6d  %6d\n"
               "cow        %8d  %6d  %6d  %6d\n"
               "cow1       %8d  %6d  %6d  %6d\n"
               "unique     %8d  %6d  %6d  %6d\n"
               "hint       %8d  %6d  %6d  %6d\n"
               "consume    %8d  %6d  %6d  %6d\n"
               "reclaim    %8d  %6d  %6d  %6d\n"
               "zero       %8d  %6d  %6d  %6d\n",
               nUsed,
               PagesToMB(nUsed),
               nTrack,
               PagesToMB(nTrack),
               100,
               Percentage(nTrack, nUsed),
               nCOW,
               PagesToMB(nCOW),
               Percentage(nCOW, nTrack),
               Percentage(nCOW, nUsed),
               nCOW1,
               PagesToMB(nCOW1),
               Percentage(nCOW1, nTrack),
               Percentage(nCOW1, nUsed),               
               nUnique,
               PagesToMB(nUnique),
               Percentage(nUnique, nTrack),
               Percentage(nUnique, nUsed),
               nHint,
               PagesToMB(nHint),
               Percentage(nHint, nTrack),
               Percentage(nHint, nUsed),
               nConsume,
               PagesToMB(nConsume),
               Percentage(nConsume, nTrack),
               Percentage(nConsume, nUsed),               
               nReclaim,
               PagesToMB(nReclaim),
               Percentage(nReclaim, nTrack),
               Percentage(nReclaim, nUsed),
               nZero,
               PagesToMB(nZero),
               Percentage(nZero, nTrack),
               Percentage(nZero, nUsed));

   // format low-level header
   Proc_Printf(buffer, len,
               "\n"
               "%-10s %8s  %8s  %8s\n",
               "primitive", "added", "removed", "active");

   // format low-level statistics
   Proc_Printf(buffer, len,
               "primPages  %8u  %8u  %8u\n"
               "primHints  %8u  %8u  %8u\n"
	       "peakCount  %8u\n",
	       stats->pageAdd,
               stats->pageRemove,
               stats->pageAdd - stats->pageRemove,
               stats->hintAdd,
               stats->hintRemove,
               stats->hintAdd - stats->hintRemove,
	       stats->peakCount);

   // release lock
   PShareUnlock(p, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcHotRead --
 *
 *      Callback for read operation on "pshare/hot" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcHotRead(UNUSED_PARAM(Proc_Entry *entry),
                  char *buffer,
                  int  *len)
{
   PShare *p = &pshare;
   PShareStats *stats = &p->stats;

   SP_IRQL prevIRQL;
   int i;

   // initialize
   *len = 0;

   // format header
   Proc_Printf(buffer, len,
               "%-16s %-8s %6s %6s\n",
               "hash", "name", "mpn", "count");

   // acquire lock
   prevIRQL = PShareLock(p);
      
   // format stats
   for (i = 0; i < PSHARE_STATS_HOT_MAX; i++) {
      PShareHotFrame *hot = &stats->hot[i];
      if (hot->count > 0) {
         // use symbolic name for key, if any
         const char *name = PShareKnownKeyName(p, hot->key);
         Proc_Printf(buffer, len,
                     "%016Lx %-8s %6x %6u\n",
                     hot->key,
                     (name == NULL) ? "" : name,
                     hot->mpn,
                     hot->count);
      }
   }

   // release lock
   PShareUnlock(p, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcCollisionsRead --
 *
 *      Callback for read operation on "pshare/collisions" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcCollisionsRead(UNUSED_PARAM(Proc_Entry *entry),
                         char *buffer,
                         int  *len)
{
   PShare *p = &pshare;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   prevIRQL = PShareLock(p);

   // report total
   Proc_Printf(buffer, len,
               "total: %u\n",
               p->stats.collisionCount);
               
   // report details, if any
   if (p->stats.collisionCount > 0) {
      uint32 i;

      // format header
      Proc_Printf(buffer, len,
                  "\n"
                  "%-16s %6s %6s %6s\n",
                  "hash", "count", "vmid", "ppn");

      // format stats
      for (i = 0; i < PSHARE_STATS_COLLIDE_MAX; i++) {
         PShareCollision *c = &p->stats.collide[i];
         if (c->count > 0) {
            Proc_Printf(buffer, len,
                        "%016Lx %6u %6u %6x\n",
                        c->key, c->count, c->worldID, c->ppn);
         }
      }
   }

   PShareUnlock(p, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcOverheadRead --
 *
 *      Callback for read operation on "pshare/overhead" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcOverheadRead(UNUSED_PARAM(Proc_Entry *entry),
                       char *buffer,
                       int  *len)
{
   PShare *p = &pshare;
   uint32 totalPages, totalFramePages, frames, nChains, nChainPages;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // acquire lock
   prevIRQL = PShareLock(p);

   // record values (with lock held)
   nChains = p->nChains;
   nChainPages = p->nChainPages;

   // release lock, as soon as possible since
   // Printf()ing is relatively slow
   PShareUnlock(p, prevIRQL);

   // Don't access 'p' below this point.

   // format header
   Proc_Printf(buffer, len,
               "%-8s %8s %6s %6s\n",
               "name", "count", "pages", "KB");

   // there is one frame per machine page
   frames = MPage_GetNumMachinePages();
   totalFramePages = MPage_GetNumOverheadPages();
   totalPages =  totalFramePages + nChainPages;

   // format statistics
   Proc_Printf(buffer, len,
	       "chains   %8u %6u %6u\n"
	       "frames   %8u %6u %6u\n"
               "total    %-8s %6u %6u\n",
	       nChains, nChainPages, PagesToKB(nChainPages),
	       frames, totalFramePages, 
	       PagesToKB(totalFramePages), "", totalPages, 
	       PagesToKB(totalPages));


   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcMPNRead --
 *
 *      Callback for read operation on "pshare/mpn" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcMPNRead(UNUSED_PARAM(Proc_Entry *entry),
                  char *buffer,
                  int  *len)
{
   static const int bytesPerLine = 16;
   PShare *p = &pshare;
   KSEG_Pair *dataPair;
   char *copy, *data;
   SP_IRQL prevIRQL;
   uint64 key;
   MPN mpn;
   int i, node;

   // initialize
   *len = 0;

   // allocate copy storage, fail if unable
   copy = Mem_Align(PAGE_SIZE, PAGE_SIZE);
   if (copy == NULL) {
      return(VMK_NO_MEMORY);
   }
   
   // acquire lock
   prevIRQL = PShareLock(p);
   
   // map MPN, copy contents
   mpn = p->readMPN;
   data = Kseg_MapMPN(mpn, &dataPair);
   memcpy(copy, data, PAGE_SIZE);
   Kseg_ReleasePtr(dataPair);

   // release lock
   PShareUnlock(p, prevIRQL);

   // compute hash
   node = NUMA_MPN2NodeNum(mpn);
   if (node == INVALID_NUMANODE) {
      node = 0;
   }
   key = PShare_HashToNodeHash(Hash_Page(copy), node);

   // format header
   Proc_Printf(buffer, len,
               "mpn  0x%x\n"
               "hash 0x%016Lx\n\n",
               mpn,
               key);

   // format data
   for (i = 0; i < PAGE_SIZE; i += bytesPerLine) {
      uint8 x;
      int j;

      for (j = 0; j < bytesPerLine; j++) {
         x = copy[i + j];
         Proc_Printf(buffer, len, "%02x ", x);
      }
      Proc_Printf(buffer, len, "   ");
      for (j = 0; j < bytesPerLine; j++) {
         x = copy[i + j];
         if ((x < 0x20) || (x > 0x7f)) {
            Proc_Printf(buffer, len, ".");
         } else {
            Proc_Printf(buffer, len, "%c", x);
         }
      }
      Proc_Printf(buffer, len, "\n");
   }
   
   // release copy storage
   Mem_Free(copy);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareProcMPNWrite --
 *
 *      Callback for read operation on "pshare/mpn" procfs node.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_BAD_PARAM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
PShareProcMPNWrite(UNUSED_PARAM(Proc_Entry *entry),
                   char *buffer,
                   int  *len)
{
   PShare *p = &pshare;
   SP_IRQL prevIRQL;
   MPN mpn;
   
   // parse value from buffer
   if (Parse_Hex(buffer, *len, &mpn) != VMK_OK) {
      return(VMK_BAD_PARAM);
   }   

   // update MPN associated with "pshare/mpn" node
   prevIRQL = PShareLock(p);
   p->readMPN = mpn;
   PShareUnlock(p, prevIRQL);
   
   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareMapChain --
 *
 *      Map the PShareChain at "index", returning a pointer to the
 *	mapped chain, and setting "pair" to the associated kseg mapping.
 *
 * Results: 
 *      Returns the mapped chain.
 *	Sets "pair" to the associated kseg mapping.
 *
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static PShareChain *
PShareMapChain(PShare *p, uint32 index, KSEG_Pair **pair)
{
   PShareChain *vaddr;
   MA maddr;

   // sanity check
   ASSERT(LOW24(index) == index);

   // compute machine address of chain from index
   maddr = p->chains + index * sizeof(PShareChain);
   
   // map using kseg
   vaddr = Kseg_GetPtrFromMA(maddr, sizeof(PShareChain), pair);

   return(vaddr);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareHashTableWalk --
 *
 *      Walks the hash table looking for a match -- essentially just a
 *      hash table lookup.  However, this function is complicated a
 *      bit because the hash table is not just a vanilla hash table.
 *      There are both hint frames and regular frames stored in the
 *      hash table.  And they each overload the hash key in their own
 *      special way.
 *
 *      If 'matchMPN' is TRUE, matching is based on 'mpn', otherwise on
 *      'key' (see implemention for details).  In either case, the
 *      lower bits of 'key' are used to determine which chain of
 *      the hash table to search.
 *
 *      *mpnShared -- set to the MPN of the matching PShareFrame
 * 
 *      *mpnPrev -- set to the MPN of the PShareFrame before the matching
 *                  *PShareFrame in the chain.  Useful if you want to
 *                  *remove 'mpn' from the hash table.
 *
 *      *mpnHint -- set to the MPN of the hint frame which matches
 *                  'key'.  Only set if matchMPN is FALSE.
 *
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_NOT_FOUND.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- */
static VMK_ReturnStatus
PShareHashTableWalk(PShare *p, Bool matchMPN, MPN mpn, uint64 key, 
                    MPN *mpnShared, MPN *mpnPrev, MPN *mpnHint)
{
   uint32 firstMPN, curMPN, prevMPN;
   uint32 chainIndex;
   KSEG_Pair *chainPair, *framePair;
   PShareFrame *frame;
   PShareChain *chain;

   // sanity
   ASSERT(PShareIsLocked(p));

   // initialize reply values
   *mpnShared = INVALID_MPN;
   *mpnPrev = INVALID_MPN;
   *mpnHint = INVALID_MPN;

   // lookup first frame in hash chain
   chainIndex = key & p->chainsMask;
   chain = PShareMapChain(p, chainIndex, &chainPair);
   firstMPN = PShareChainGet(chain);
   Kseg_ReleasePtr(chainPair);

   // search chain for key
   prevMPN = PSHARE_MPN_NULL;
   curMPN = firstMPN;
   while (curMPN != PSHARE_MPN_NULL) {
      // find next frame in chain
      frame = (PShareFrame *)MPage_Map(curMPN, &framePair);
      ASSERT(frame);
      if (!frame) {
         return(VMK_NOT_FOUND);
      }

      // debugging
      if (p->debug) {
         PShareDebug("search: curMPN 0x%x, mpn 0x%x", curMPN, mpn);
      }

      // what type of match is sought?
      if (matchMPN) {
         // only need the mpn to match
         if (curMPN == mpn) {
            goto match;
         }
      } else {
         // check for key match
         if (PShareFrameIsHint(frame)) {
            // matching hint frame?
            if (PShareFrameHintMatch(frame, key)) {
               *mpnHint = curMPN;
            }
         } else {
            // exact key match needed for normal frames
            if (frame->u.regular.key == key) {
               goto match;
            }
         }
      }

      // advance to next frame in chain
      prevMPN = curMPN;
      curMPN = frame->next;
      MPage_Unmap(framePair);
   }

   // fail, no key match
   return(VMK_NOT_FOUND);

 match:
   // set reply values
   *mpnShared = curMPN;
   *mpnPrev = prevMPN;
   // release mappings
   MPage_Unmap(framePair);
   // success
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareHashTableAddHead --
 *
 *      Adds 'frame' to the head of the hash chain in which it
 *      belongs.  'mpn' must be the MPN of 'frame'.
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
PShareHashTableAddHead(PShare *p, PShareFrame *frame, MPN mpn)
{
   MPN firstMPN;
   uint32 chainIndex;
   KSEG_Pair *chainPair;
   PShareChain *chain;
   uint64 key;

   // sanity
   ASSERT(PShareIsLocked(p));

   // lookup first frame in hash chain
   key = PShareFrameIsHint(frame) ? frame->u.hint.key : frame->u.regular.key;
   chainIndex = key & p->chainsMask;
   chain = PShareMapChain(p, chainIndex, &chainPair);

   // not found: push new frame onto front of chain
   firstMPN = PShareChainGet(chain);
   frame->next = firstMPN;
   PShareChainSet(chain, mpn);
   Kseg_ReleasePtr(chainPair);

   // update stats
   if (PShareFrameIsHint(frame)) {
      p->stats.hashtblHints++;
   } else {
      p->stats.hashtblPages++;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PShareHashTableRemove --
 *
 *      Removes 'frame' from the hash table.  'mpn' must be the MPN of
 *      'frame'.  'mpnPrev' must be the predecessor of 'frame' in the
 *      hash chain that contains 'frame'.
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- */
static void
PShareHashTableRemove(PShare *p, PShareFrame *frame, MPN mpn,
                      MPN mpnPrev)
{
   uint32 chainIndex;
   KSEG_Pair *chainPair, *prevFramePair;
   PShareChain *chain;
   PShareFrame *prevFrame;
   uint64 key;

   // sanity
   ASSERT(PShareIsLocked(p));

   // unlink frame from hash chain
   if (mpnPrev == PSHARE_MPN_NULL) {
      // case 1: frame is first in chain
      key = PShareFrameIsHint(frame) ? frame->u.hint.key : frame->u.regular.key;
      chainIndex = key & p->chainsMask;
      chain = PShareMapChain(p, chainIndex, &chainPair);
      // sanity
      ASSERT(PShareChainGet(chain) == mpn);
      PShareChainSet(chain, frame->next);
      Kseg_ReleasePtr(chainPair);
   } else {
      // case 2: frame is in middle or at end of chain
      prevFrame = (PShareFrame *)MPage_Map(mpnPrev, &prevFramePair);
      ASSERT(prevFrame);
      if (!prevFrame) {
         return;
      }
      prevFrame->next = frame->next;
      MPage_Unmap(prevFramePair);
   }

   // update stats
   if (PShareFrameIsHint(frame)) {
      ASSERT(p->stats.hashtblHints > 0);
      p->stats.hashtblHints--;
   } else {
      ASSERT(p->stats.hashtblPages > 0);
      p->stats.hashtblPages--;
   }

   PShareFrameSetInvalid(frame);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareAddPage --
 *
 *      Updates "p" to reflect an additional reference to the shared
 *      page associated with "key".  If a page whose key equals "key"
 *      already exists in the hash table, its ref count is incremented
 *      and "*count" and "*mpnShared" are set to that page's refcount
 *      and MPN, respectively -- and VMK_OK is returned.
 *
 *      If no page with a matching key is found, the behaviour depends
 *      on "sharedOnly".  If sharedOnly is TRUE, VMK_NOT_FOUND is
 *      returned ("*count,*mpnShared" are invalid).  But is sharedOly
 *      is FALSE, the page for "mpn" is added to the hash table under
 *      the key "key", "*count" is set to 1, "*mpnShared" is set to
 *      "mpn" and VMK_OK is returned.
 *
 *      In all cases, sets "*mpnHint" to the MPN of a hint page
 *      matching "key".
 * 
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      see above
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- */
static VMK_ReturnStatus
PShareAddPage(PShare *p, MPN mpn, uint64 key, Bool sharedOnly,
              MPN *mpnShared, uint32 *count, MPN *mpnHint)
{
   PShareFrame *frame;
   VMK_ReturnStatus ret;
   MPN mpnPrev; // not used
   KSEG_Pair *framePair;

   // sanity
   ASSERT(PShareIsLocked(p));

   // debugging
   if (p->debug) {
      PShareDebug("n %u, mpn %x, key 0x%16Lx, sharedOnly %d",
                  p->stats.pageAdd, mpn, key, sharedOnly);
   }

   // initialize reply values
   *mpnShared = INVALID_MPN;
   *count = 0;
   *mpnHint = INVALID_MPN;

   // search for a matching key
   ret = PShareHashTableWalk(p, FALSE, INVALID_MPN, key, mpnShared, &mpnPrev, mpnHint);
   if (ret == VMK_OK) {
      PShareFrame *frame = (PShareFrame *)MPage_Map(*mpnShared, &framePair);
      // sanity check
      ASSERT(frame);
      ASSERT(PShareFrameIsRegular(frame));
      ASSERT(frame->u.regular.count != 0 && frame->u.regular.key == key);
      if (!frame) {
         return(VMK_BAD_PARAM);
      }
      
      // update frame count
      frame->u.regular.count++;

      // update stats
      PShareStatsUpdateHot(&p->stats, key, *mpnShared, frame->u.regular.count);
      p->stats.pageAdd++;
      p->stats.pageCount++;
      if (frame->u.regular.count == 2) {
         // no longer unshared (refcount bumped from 1 to 2)
         p->stats.pageUnshared--;
      }

      // set reply values (*mpnShared already set)
      *count = frame->u.regular.count;
      // release mapping
      MPage_Unmap(framePair);
      return(VMK_OK);
   } else if (ret == VMK_NOT_FOUND) {
      // fail if "sharedOnly"
      if (sharedOnly) {
         return(VMK_NOT_FOUND);
      }

      // push new frame onto front of chain
      frame = (PShareFrame *)MPage_Map(mpn, &framePair);
      // sanity
      ASSERT(frame);
      if (!frame) {
         return(VMK_BAD_PARAM);
      }

      // initialize frame and push it on the hash table
      PShareFrameSetRegular(frame, key, 1, PSHARE_MPN_NULL);
      PShareHashTableAddHead(p, frame, mpn);
      MPage_Unmap(framePair);
      
      // update stats
      p->stats.pageAdd++;
      p->stats.pageCount++;
      p->stats.pageUnshared++;

      // set reply values
      *mpnShared = mpn;
      *count = 1;
      // success
      return(VMK_OK);
   } else {
      // failure, propagate error
      return(ret);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PShareRemovePage --
 *
 *      Updates "p" to reflect a dropped reference to the shared page
 *	associated with "key".  Removes frame when its count drops to
 *	zero.  Sets "count" to the updated reference count.  Does not
 *	drop reference count if "unsharedOnly" is set and the reference
 *	count is not one.  Caller must hold pshare lock. 
 *
 * Results: 
 *	Sets "count" to the shared page count, if successful.
 *      Returns VMK_OK if successful, otherwise error code.
 *      Caller must hold pshare lock.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareRemovePage(PShare *p, MPN mpn, uint64 key, Bool unsharedOnly,
                 uint32 *count)
{
   PShareStats *stats = &p->stats;
   VMK_ReturnStatus ret;
   MPN mpnShared, mpnPrev, mpnHint;
   KSEG_Pair *framePair;
   PShareFrame *frame;

   // sanity
   ASSERT(PShareIsLocked(p));

   // debugging
   if (p->debug) {
      PShareDebug("n %u, mpn 0x%x, key 0x%16Lx, unsharedOnly %u",
                  stats->pageRemove, mpn, key, unsharedOnly);
   }

   // initialize reply values
   *count = 0;

   ret = PShareHashTableWalk(p, TRUE, mpn, key, &mpnShared, &mpnPrev, &mpnHint);
   if (ret == VMK_OK) {
      frame = (PShareFrame *)MPage_Map(mpnShared, &framePair);
      // sanity
      ASSERT(frame);
      ASSERT(PShareFrameIsRegular(frame));
      ASSERT(frame->u.regular.count > 0);
      
      // fail if "unsharedOnly" specified, and frame is shared
      if (unsharedOnly && (frame->u.regular.count != 1)) {
         MPage_Unmap(framePair);
         return(VMK_LIMIT_EXCEEDED);
      }
      
      // decrement refcount
      frame->u.regular.count--;

      // update stats
      PShareStatsUpdateHot(stats, key, mpn, *count);
      stats->pageRemove++;
      stats->pageCount--;
      if (frame->u.regular.count == 1) {
         // refcount bumped 2 to 1, so frame is now unshared.
         stats->pageUnshared++;
      } else if (frame->u.regular.count == 0) {
         // refcount bumped 1 to 0, so frame is no longer
         // unshared.
         stats->pageUnshared--;
         // only referenced frames are in hash table
         PShareHashTableRemove(p, frame, mpnShared, mpnPrev);
      }
      
      // set return value
      *count = frame->u.regular.count;
      // release mapping
      MPage_Unmap(framePair);
      return(VMK_OK);
   }


   // not found: fail
   return(VMK_NOT_FOUND);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareLookupPage --
 *
 *      Finds shared page with MPN "mpn" in "p".
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      if successful,
 *          sets *key to the shared page's key 
 *          sets *count to the shared page's ref count 
 *          returns VMK_OK
 *      otherwise
 *          "*key,*count" are undefined
 *          returns VMK_NOT_FOUND
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareLookupPage(PShare *p, MPN mpn, uint64 *key, uint32 *count)
{
   KSEG_Pair *framePair;
   PShareFrame *frame;

   // sanity
   ASSERT(PShareIsLocked(p));

   // map frame, fail if unable
   frame = (PShareFrame *)MPage_Map(mpn, &framePair);
   if (frame == NULL) {
      return(VMK_NOT_FOUND);
   }

   // fail if hint frame
   if (PShareFrameIsInvalid(frame) || PShareFrameIsHint(frame)) {
      // release frame, fail
      MPage_Unmap(framePair);
      return(VMK_NOT_FOUND);
   }

   // set reply values
   ASSERT(PShareFrameIsRegular(frame));
   *key   = frame->u.regular.key;
   *count = frame->u.regular.count;

   // sanity
   ASSERT(*count != 0);

   // release frame, succeed
   MPage_Unmap(framePair);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareKeyToMPN --
 *
 *      Finds shared page frame associated with "key" in "p".
 *      Caller must hold pshare lock.
 *
 * Results: 
 *      if successful,
 *          sets "*mpn" to the shared page's mpn 
 *          returns VMK_OK
 *      otherwise
 *          "*mpn" is undefined
 *          returns VMK_NOT_FOUND
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareKeyToMPN(PShare *p, uint64 key, MPN *mpn)
{
   MPN mpnHint, mpnPrev;
   VMK_ReturnStatus ret;

   // sanity
   ASSERT(PShareIsLocked(p));

   ret = PShareHashTableWalk(p, FALSE, INVALID_MPN, key, mpn, &mpnPrev, &mpnHint);
   if (ret == VMK_OK) {
      return(VMK_OK);
   } 

   return(VMK_NOT_FOUND);
}


/*
 *----------------------------------------------------------------------
 *
 * PShareLookupHint --
 *
 *      Find shared page hint frame data at MPN "mpn" in "p"
 *      Caller must hold pshare lock.
 *
 * Results:
 *      If successful,
 *         set "*key" to the hint's key
 *         set "*worldID" to the hint's worldID
 *         set "*ppn" to hint's PPN
 *         Returns VMK_OK.
 *	Otherwise, 
 *         *key,*worldID,*ppn are invalid
 *         Returns VMK_NOT_FOUND.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareLookupHint(PShare *p,
                 MPN mpn,
                 uint64 *key,
                 World_ID *worldID,
                 PPN *ppn)
{
   KSEG_Pair *framePair;
   PShareFrame *frame;

   // sanity
   ASSERT(PShareIsLocked(p));

   // map frame, fail if unable
   frame = (PShareFrame *)MPage_Map(mpn, &framePair);
   if (frame == NULL) {
      return(VMK_NOT_FOUND);
   }

   // fail if frame not hint
   if (!PShareFrameIsHint(frame)) {
      // release frame and fail
      MPage_Unmap(framePair);
      return(VMK_NOT_FOUND);
   }
   
   // set reply values
   *key     = frame->u.hint.key;
   *worldID = frame->u.hint.worldID;
   *ppn     = frame->u.hint.ppn;

   // release frame, succeed
   MPage_Unmap(framePair);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareAddHint --
 *
 * 	Adds a speculative hint frame to "p" indicating that the
 *	page associated with "worldID" at "ppn" and "mpn" currently
 *	has the specified "key".
 *      Caller must hold pshare lock.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareAddHint(PShare *p,
              uint64 key,
              MPN mpn,
              World_ID worldID,
              PPN ppn)
{
   KSEG_Pair *framePair;
   PShareFrame *frame;

   // sanity
   ASSERT(PShareIsLocked(p));

   // map frame, fail if unable
   frame = (PShareFrame *)MPage_Map(mpn, &framePair);
   if (frame == NULL) {
      return(VMK_NOT_FOUND);
   }

   PShareFrameSetHint(frame, key, worldID, ppn, PSHARE_MPN_NULL);

   // link new frame into hash chain (at front)
   PShareHashTableAddHead(p, frame, mpn);

   // update stats
   p->stats.hintAdd++;
   p->stats.hintCount++;
   
   // debugging
   if (p->debug) {
      PShareDebug("n %u, mpn 0x%x", p->stats.hintAdd, mpn);
   }

   // release frame
   MPage_Unmap(framePair);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PShareRemoveHint --
 *
 * 	Removes a speculative hint frame from "p" at specified "mpn".
 *	Fails unless both "worldID" and "ppn" match the hint.
 *      Caller must hold pshare lock.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PShareRemoveHint(PShare *p, MPN mpn, World_ID worldID, PPN ppn)
{
   VMK_ReturnStatus ret;
   PShareStats *stats = &p->stats;
   KSEG_Pair *framePair;
   PShareFrame *frame;
   MPN mpnShared, mpnPrev, mpnHint;
   uint32 checkWorld;
   PPN checkPPN;
   uint64 key;

   // sanity
   ASSERT(PShareIsLocked(p));

   // map frame, fail if unable
   frame = (PShareFrame *)MPage_Map(mpn, &framePair);
   if (frame == NULL) {
      return(VMK_NOT_FOUND);
   }

   // fail if frame not hint
   if (!PShareFrameIsHint(frame)) {
      MPage_Unmap(framePair);
      return(VMK_NOT_FOUND);
   }

   // extract data for match
   key         = frame->u.hint.key;
   checkWorld  = frame->u.hint.worldID;
   checkPPN    = frame->u.hint.ppn;

   // fail if hint contents don't match specified values
   if ((checkWorld != worldID) || (checkPPN != ppn)) {
      MPage_Unmap(framePair);
      return(VMK_NOT_FOUND);      
   }

   ret = PShareHashTableWalk(p, TRUE, mpn, key, &mpnShared, &mpnPrev, &mpnHint);
   if (ret == VMK_OK) {
      ASSERT(mpnShared == mpn);

      // unlink frame from hash table
      PShareHashTableRemove(p, frame, mpn, mpnPrev);

      // update stats
      stats->hintRemove++;
      stats->hintCount--;
      
      // succeed
      MPage_Unmap(framePair);
      return(VMK_OK);
   }

   MPage_Unmap(framePair);
   return(VMK_NOT_FOUND);
}

/*
 * External operations
 */

/*
 *----------------------------------------------------------------------
 *
 * PShare_GetNumContMPNs --
 *    
 *    Get the number of contiguous MPNs that are required for 
 *    storing the chains.
 *
 * Results:
 *    Number of contiguous MPNs required
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
uint32
PShare_GetNumContMPNs(MPN minMPN, 
                      MPN maxMPN,
                      Bool hotAdd)
{
   uint32 nPages = maxMPN - minMPN + 1;

   // zero pages required if page sharing disabled, use pshareEnabledFlag directly
   // as PShare module is not initialized when this function is called
   // during boot
   if (!pshareEnabledFlag) {
      return 0;
   }

   // boot time memory or hot add?
   if (!hotAdd)  {
      // Allocate one chain for each page of boot time machine memory.
      // Each chain corresponds to a bucket in the hash table, as such
      // the number of chains is a matter of performance not
      // correctness.  So we could tune the # of chains, if desired.

      uint32 nChainBytes, nChainPages, nChains;

      // set nChains to the smallest power of 2 >= nPages
      nChains = Util_RoundupToPowerOfTwo(nPages);

      PShareDebugVerbose("nPages=%u, nChains=%u", nPages, nChains);

      // compute number of pages to allocate for chains
      nChainBytes = nChains * sizeof(PShareChain);
      nChainPages = CEILING(nChainBytes, PAGE_SIZE);
      return nChainPages;
   } else {
      // No chains are allocated for hotadd memory ranges.  NB This
      // could cause performance issue if there was a very small
      // amount of boot memory, but large amounts of hot add memory.
      return 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PShare_AssignContMPNs --
 *    
 *    Use the contiguous MPNs allocated. If we are booting initialize the
 *    pshare module
 *
 * Results:
 *    VMK_OK
 *
 * Side effects:
 *    Page sharing module is initialized or additional segments for 
 *    hot add memory are initialized
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
PShare_AssignContMPNs(MPN minMPN,
                      MPN maxMPN,
                      Bool hotAdd,
                      uint32 reqSize, // in pages
                      MPN startMPN)
{
   // Only care about boot time memory
   if (!hotAdd) {
      PShareInit(minMPN, maxMPN, reqSize, startMPN);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PShare_EarlyInit --
 *
 *      Enable page sharing iff "enabled" is TRUE.
 *	Must be invoked early to prevent data structure allocation
 *	when page sharing is disabled.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Sets global "pshareEnabledFlag".
 *
 *----------------------------------------------------------------------
 */
void
PShare_EarlyInit(Bool enabled)
{
   pshareEnabledFlag = enabled;
}

/*
 *----------------------------------------------------------------------
 *
 * PShareInit --
 *
 *      Initializes the page sharing module.  Sizes data structures
 *	based on the total number of memory pages "nPages" that may
 *	be shared.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Internal state is initialized.
 *
 *----------------------------------------------------------------------
 */
static void
PShareInit(MPN minMPN,
           MPN maxMPN,
           uint32 reqSize,
           MPN startMPN)
{
   int i;
   PShare *p = &pshare;
   uint32 nPages, nChains, nChainBytes, nChainPages;

   nPages = maxMPN - minMPN + 1;

   // zero global state
   memset(p, 0, sizeof(PShare));

   // non-zero initialization
   for (i=0; i < NUMA_MAX_NODES; i++) {
      p->zeroMPN[i] = INVALID_MPN;
   }

   // initialize
   SP_InitLockIRQ("PShare", &p->lock, SP_RANK_PSHARE);

   // enable feature depending on load-time option
   p->enabled = pshareEnabledFlag;

   // done if page sharing disabled
   if (!p->enabled) {
      LOG(0, "page sharing disabled");
      return;
   }

   // log enabled message
   LOG(0, "page sharing enabled");

   // debugging
   PShareDebugVerbose("nPages=%u", nPages);
   PShareDebugVerbose("sizeof(MPage)=%d, sizeof(PShareChain)=%d",
                      sizeof(MPage),
                      sizeof(PShareChain));

   // sanity checks
   ASSERT(sizeof(PShareChain) == PSHARE_CHAIN_SIZE);
   ASSERT(sizeof(PShareFrame) == PSHARE_FRAME_SIZE);
   ASSERT(sizeof(PShareFrame) == sizeof(MPage));

   // set nChains to the smallest power of 2 >= nPages
   nChains = Util_RoundupToPowerOfTwo(nPages);
   PShareDebugVerbose("nPages=%u, nChains=%u", nPages, nChains);

   // compute number of pages to allocate early for chains, frames
   nChainBytes = nChains * sizeof(PShareChain);
   nChainPages = CEILING(nChainBytes, PAGE_SIZE);
   ASSERT(nChainPages == reqSize);

   // sanity check: consume less than one percent of physical memory
   ASSERT(reqSize < nPages / 100);

   // chains storaged contiguously starting at startMPN
   ASSERT(startMPN != INVALID_MPN);
   if (startMPN == INVALID_MPN) {
      // disable page sharing, issue warning
      p->enabled = FALSE;
      Warning("unable to allocate storage (chains)");
      return;
   }

   // sanity
   ASSERT(Util_IsPowerOf2(nChains));
   
   p->chains = MPN_2_MA(startMPN);
   p->nChains = nChains;
   p->nChainPages = nChainPages;
   p->chainsMask = (nChains - 1);
   PShareDebugVerbose("nChains=%u, nChainPages=%u chainsMask=%x",
                      p->nChains, p->nChainPages, p->chainsMask);
   
   // record boot time memory range, for POST test
   p->bootTimeMinMPN = minMPN;
   p->bootTimeMaxMPN = maxMPN;

   // debugging
   PShareDebugVerbose("early init complete");
}


/*
 *----------------------------------------------------------------------
 *
 * PShareReset --
 *
 *      Reinitializes all data structures and statistics for "p".
 *      Caller must hold lock for "p".  Note: Called only at boot time
 *	therefore only initializes the first segment of allocated frames
 *
 * Results:
 *      Modifies "p".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
PShareReset(PShare *p)
{
   uint32 i;

   // debugging
   PShareDebugVerbose("reset");

   // initialize chains (could be sped up if needed)
   for (i = 0; i < p->nChains; i++) {
      KSEG_Pair *chainPair;
      PShareChain *chain;

      chain = PShareMapChain(p, i, &chainPair);
      ASSERT(chain);
      PShareChainSet(chain, PSHARE_MPN_NULL);
      Kseg_ReleasePtr(chainPair);
   }

   // reset stats
   memset(&p->stats, 0, sizeof(PShareStats));
}


/*
 *----------------------------------------------------------------------
 *
 * PSharePost --
 *
 *      Performs simple tests of PShare.
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static Bool
PSharePOST(UNUSED_PARAM(void *clientData), int id,
           UNUSED_PARAM(SP_SpinLock *lock), SP_Barrier *barrier)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   MPN mpn;
   uint64 uniqueKey;
   uint64 i;
   uint32 count;
   MPN mpnFirstIteration = INVALID_MPN;
   MPN mpnShared;
   KSEG_Pair *framePair;
   PShareFrame *frame;
   uint32 maxCount;

   // test is simple.  Only pcpu 0 run.
   if (MY_PCPU != 0) {
      return TRUE;
   }

   LOG(0, "boot time memory [0x%x, 0x%x]", 
       p->bootTimeMinMPN, p->bootTimeMaxMPN);

   //////////////////////////////////////////////////////////////////////
   // This first POST test mainly exercises the refcounts, by sharing
   // one single MPN a huge number of times (from 1 to ~#memory pages).

   // hash table should contain a zero page for each numa node and
   // nothing else
   ASSERT(p->stats.hashtblPages == NUMA_GetNumNodes());
   ASSERT(p->stats.pageCount == NUMA_GetNumNodes());

   // Get an unused key.
   uniqueKey = 0xdead;
   ASSERT(PShare_LookupByKey(uniqueKey, &mpn, &count) == VMK_NOT_FOUND);

   // Add all boot time memory to the hash table under the 'zero key'
   maxCount = 0;
   for (i = 1, mpn = p->bootTimeMinMPN; mpn <= p->bootTimeMaxMPN; mpn++) {
      // avoid the zero pages already inserted in the hash table
      // by PShare_LateInit()
      if (PShare_IsZeroMPN(mpn)) {
         continue;
      }

      frame = (PShareFrame *)MPage_Map(mpn, &framePair);
      // frame should be cleared initially
      ASSERT(frame);
      ASSERT(PShareFrameIsInvalid(frame));
      ASSERT(frame->u.regular.key == 0);
      ASSERT(frame->u.regular.count == 0);
      ASSERT(frame->next == PSHARE_MPN_NULL);

      // set frame's key to the zero key..
      status = PShare_Add(uniqueKey, mpn, &mpnShared, &count);
      ASSERT(status == VMK_OK);

      // no hints have been added
      ASSERT(p->stats.hashtblHints == 0);

      // all 'i' pages map to the same page.  So that pages refcount
      // should increase with 'i'.
      ASSERT(i == count);
      maxCount = MAX(maxCount, count);

      // checks specific to the iteration
      if (i == 1) {
         // on the first iteration..
         // expect: must be at end of chain. There is only 1 page in hash table.
         ASSERT(frame->next == PSHARE_MPN_NULL);
         // expect: 'mpn' didn't find any other mpn to share with
         ASSERT(mpnShared == mpn);
         // save for later checking..
         mpnFirstIteration = mpn;

         ASSERT(frame->u.regular.count == 1);
      } else {
         // on subsequent iterations..
         // should be shared with mpn from first iteration
         ASSERT(mpnFirstIteration == mpnShared);
         ASSERT(frame->u.regular.count == 0);
      }

      MPage_Unmap(framePair);
      i++;
   }

   // Remove all boot time memory from the hash table.
   for (i = 1; i <= maxCount; i++) {
      status = PShare_Remove(uniqueKey, mpnFirstIteration, &count);
      ASSERT(status == VMK_OK);
      ASSERT(count == maxCount - i);
   }


   // hash table should now be back to its original contents at the
   // start of this test.
   ASSERT(p->stats.hashtblPages == NUMA_GetNumNodes());
   ASSERT(p->stats.pageCount == NUMA_GetNumNodes());


   //////////////////////////////////////////////////////////////////////
   // The 2nd POST test is mainly designed to stress the hash table itself,
   // by inserting and deleting a lot pages from it.


   // for all mpns (skipping zeroes), insert under unique key
   uniqueKey = 0;
   for (i = 1, mpn = p->bootTimeMinMPN; mpn <= p->bootTimeMaxMPN; mpn++) {
      uint32 ignoreCount;
      MPN ignoreMPN;

      // avoid the zero pages already inserted in the hash table
      // by PShare_LateInit()
      if (PShare_IsZeroMPN(mpn)) {
         continue;
      }

      // find an unused key
      while (PShare_LookupByKey(uniqueKey, &ignoreMPN, &ignoreCount)
             != VMK_NOT_FOUND) {
         uniqueKey++;
      }


      // add "mpn" to hash table under the unused key
      status = PShare_Add(uniqueKey, mpn, &mpnShared, &count);
      // must succeed
      ASSERT(status == VMK_OK);
      // key was unique, so no sharing possible 
      ASSERT(mpnShared == mpn && count == 1);
      i++;
   }


   // for all mpns (skipping zeroes), delete
   uniqueKey = 0;
   for (i = 1, mpn = p->bootTimeMinMPN; mpn <= p->bootTimeMaxMPN; mpn++) {
      // avoid the zero pages already inserted in the hash table
      // by PShare_LateInit()
      if (PShare_IsZeroMPN(mpn)) {
         continue;
      }

      // must be in the hash table, with 1 ref
      status = PShare_LookupByMPN(mpn, &uniqueKey, &count);
      ASSERT(status == VMK_OK && count == 1);

      // delete, bringing ref count to 0
      status = PShare_Remove(uniqueKey, mpn, &count);
      ASSERT(status == VMK_OK && count == 0);

      i++;
   }

   // hash table should now be back to its original contents at the
   // start of this test.
   ASSERT(p->stats.hashtblPages == NUMA_GetNumNodes());
   ASSERT(p->stats.pageCount == NUMA_GetNumNodes());

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * PShare_LateInit --
 *
 *      Final initialization of page sharing module.
 *	Resets data structures, and registers procfs nodes.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Internal state is initialized.
 *
 *----------------------------------------------------------------------
 */
void
PShare_LateInit(void)
{
   PShare *p = &pshare;
   int node;

   LOG(0, "PShare_LateInit: enabled %d", p->enabled);

   // done if page sharing disabled
   if (!p->enabled) {
      return;
   }

   // reset chains
   PShareReset(p);

   // initialize collision logging threshold
   p->stats.collisionLog = 1;

   // register "pshare" directory
   Proc_InitEntry(&p->procDir);
   Proc_Register(&p->procDir, "pshare", TRUE);

   // register "pshare/status" entry
   Proc_InitEntry(&p->procStatus);
   p->procStatus.parent = &p->procDir;
   p->procStatus.read = PShareProcStatusRead;
   Proc_Register(&p->procStatus, "status", FALSE);

   // register "pshare/collisions" entry
   Proc_InitEntry(&p->procCollisions);
   p->procCollisions.parent = &p->procDir;
   p->procCollisions.read = PShareProcCollisionsRead;
   Proc_Register(&p->procCollisions, "collisions", FALSE);

   // register "pshare/hot" entry, if enabled
   Proc_InitEntry(&p->procHot);
   if (PSHARE_STATS_HOT) {
      p->procHot.parent = &p->procDir;
      p->procHot.read = PShareProcHotRead;
      Proc_Register(&p->procHot, "hot", FALSE);
   }

   // register "pshare/overhead" entry
   Proc_InitEntry(&p->procOverhead);
   p->procOverhead.parent = &p->procDir;
   p->procOverhead.read = PShareProcOverheadRead;
   Proc_Register(&p->procOverhead, "overhead", FALSE);   
   
   // register "pshare/mpn" entry
   Proc_InitEntry(&p->procMPN);
   if (PSHARE_DEBUG) {
      p->procMPN.parent = &p->procDir;
      p->procMPN.read = PShareProcMPNRead;
      p->procMPN.write = PShareProcMPNWrite;
      Proc_Register(&p->procMPN, "mpn", FALSE);
   }

   // compute hashes for well-known page contents
   // setup these well-known pages separately on each NUMA node
   for (node = 0; node < NUMA_GetNumNodes(); node++) {
      VMK_ReturnStatus status;
      MPN mpn, mpnShared;
      KSEG_Pair *dataPair;
      uint32 count;
      void *data;

      // allocate page
      mpn = MemMap_AllocKernelPage((1 << node), MM_COLOR_ANY, MM_TYPE_ANY);
      ASSERT(mpn != INVALID_MPN);

      data = Kseg_MapMPN(mpn, &dataPair);

      memset(data, 0x3f, PAGE_SIZE);
      PShareKnownKeyAdd(p, PShare_HashToNodeHash(Hash_Page(data), node), "0x3f's");
      memset(data, 0xff, PAGE_SIZE);
      PShareKnownKeyAdd(p, PShare_HashToNodeHash(Hash_Page(data), node), "0xff's");   
      memset(data, 0x00, PAGE_SIZE);
      p->zeroKey[node] = PShare_HashToNodeHash(Hash_Page(data), node);
      PShareKnownKeyAdd(p, p->zeroKey[node], "0x00's");

      Kseg_ReleasePtr(dataPair);
      
      // preload zero-filled page into hash table
      p->zeroMPN[node] = mpn;
      status = PShare_Add(p->zeroKey[node], p->zeroMPN[node], &mpnShared, &count);
      PShareDebug("zero page: key 0x%Lx, mpn 0x%x, node %d",
                  p->zeroKey[node], p->zeroMPN[node], node);


      // sanity checks
      ASSERT(status == VMK_OK);
      ASSERT(p->zeroMPN[node] == mpnShared);
      ASSERT(p->zeroMPN[node] != INVALID_MPN);
      ASSERT(count == 1);
   }

   // debugging: test collision reporting
   if (PSHARE_DEBUG_COLLIDE) {
      // generate some fake collisions (5x each for 10 keys)
      uint32 i, j;
      for (i = 0; i < 5; i++) {
         for (j = 0; j < 10; j++) {
            uint32 rnd = Util_FastRand(j + 1);
            PShare_ReportCollision((uint64) rnd, rnd & 0xfff, rnd & 0xffff);
         }
      }
   }

   // log initialization message
   LOG(0, "initialized");
   POST_Register("PShare", PSharePOST, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * PShare_Add --
 *
 *      Update page-sharing state to add MPN "mpn" with hash value
 *	"key".  If "key" is already present, its refcount is incremented.
 *	Otherwise adds new hash table entry for "mpn" under the key "key'.
 *      If successful,	sets "mpnShared", and "count" to the updated
 *	page-sharing state.
 *
 * Results:
 *	Sets "mpnShared" to the shared page MPN, if successful.
 *	Sets "count" to the shared page count, if successful.
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_Add(uint64 key, MPN mpn, MPN *mpnShared, uint32 *count)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;
   MPN hintMPN;


   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareAddPage(p, mpn, key, FALSE,
                          mpnShared, count, &hintMPN);
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_AddIfShared --
 *
 *      Update page-sharing state to add MPN "mpn" with hash value
 *	"key".  If "key" is already present, updates its count.
 *	Sets "hintMPN" if a matching speculative hint is found.
 *	If successful, sets "mpnShared", "count", and "hintMPN" to
 *	the updated	page-sharing state.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	Sets "mpnShared" to the shared page MPN, if successful.
 *	Sets "count" to the shared page count, if successful.
 *	Sets "hintMPN" to matching hint MPN, or PSHARE_MPN_NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_AddIfShared(uint64 key,
                   MPN mpn,
                   MPN *mpnShared,
                   uint32 *count,
                   MPN *hintMPN)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareAddPage(p, mpn, key, TRUE,
                          mpnShared, count, hintMPN);
   PShareUnlock(p, prevIRQL);
   
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_Remove --
 *
 *      Update page-sharing state to remove MPN "mpn" with hash
 *	value "key".  If "key" is already present, its count is
 *	decremented, and the entry is removed if its count becomes
 *	zero.
 *
 * Results:
 *	Sets "count" to the shared page count, if successful.
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_Remove(uint64 key,
              MPN mpn,
              uint32 *count) // OUT
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;


   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareRemovePage(p, mpn, key, FALSE, count);
   PShareUnlock(p, prevIRQL);

   return(status);   
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_RemoveIfUnshared --
 *
 *      Update page-sharing state to remove MPN "mpn" with hash value
 *	"key", iff "key" is already present and its count is one.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_RemoveIfUnshared(uint64 key, MPN mpn)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;
   uint32 count;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareRemovePage(p, mpn, key, TRUE, &count);
   PShareUnlock(p, prevIRQL);

   return(status);   
}


/*
 *----------------------------------------------------------------------
 *
 * PShare_LookupByMPN --
 *
 *      Find shared page frame data at "mpn", and
 *	set "key", and "count" appropriately.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	Sets "key", and "count" if successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_LookupByMPN(MPN mpn, uint64 *key, uint32 *count)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareLookupPage(p, mpn, key, count);
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_LookupByKey --
 *
 *      Find shared page frame data associated with "key", and
 *	set "mpn", "count", appropriately.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	Sets "mpn", "count", if successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_LookupByKey(uint64 key, MPN *mpn, uint32 *count)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitives, holding lock
   prevIRQL = PShareLock(p);
   // convert key to frame index
   status = PShareKeyToMPN(p, key, mpn);
   // lookup by index
   if (status == VMK_OK) {
      uint64 tmpKey; 
      status = PShareLookupPage(p, *mpn, &tmpKey, count);
   }
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_LookupHint --
 *
 *      Find shared page hint frame data at "mpn", and
 *	set "key", "mpn", "ppn", and "worldID" appropriately.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	Sets "key", "mpn", "ppn", and "worldID" if successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_LookupHint(MPN mpn,
                  uint64 *key,
                  World_ID *worldID,
                  PPN *ppn)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;


   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareLookupHint(p, mpn, key, worldID, ppn);
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_AddHint --
 *
 *      Adds speculative hint frame indicating that the page
 *	associated with "worldID" at "ppn" and "mpn" currently
 *	has the specified "key".  
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PShare_AddHint(uint64 key,
               MPN mpn,
               World_ID worldID,
               PPN ppn)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareAddHint(p, key, mpn, worldID, ppn);
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_RemoveHint --
 *
 *      Removes speculative hint frame at specified "mpn".
 *	Fails if specified "mpn", "worldID", or "ppn" do not match.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
PShare_RemoveHint(MPN mpn, World_ID worldID, PPN ppn)
{
   PShare *p = &pshare;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;

   // fail if page sharing disabled
   if (!p->enabled) {
      return(VMK_NOT_SUPPORTED);
   }

   // invoke primitive, holding lock
   prevIRQL = PShareLock(p);
   status = PShareRemoveHint(p, mpn, worldID, ppn);
   PShareUnlock(p, prevIRQL);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_HintKeyMatch --
 *
 *      Check if "hintKey" matches "key".
 *
 * Results:
 *      Returns TRUE if "hintKey" matches "key", otherwise FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PShare_HintKeyMatch(uint64 hintKey, uint64 key)
{
   return(PShareHintKeyMatch(hintKey, key));
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_IsEnabled --
 *
 *      Query whether or not page sharing is enabled.
 *
 * Results:
 *      Returns TRUE if page sharing is enabled, otherwise FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PShare_IsEnabled(void)
{
   PShare *p = &pshare;
   return(p->enabled);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_IsZeroMPN --
 *
 *      Query if "mpn" is one of the special zero MPNs.
 *
 * Results:
 *      Returns TRUE if so, otherwise FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PShare_IsZeroMPN(MPN mpn)
{
   PShare *p = &pshare;
   int node;

   for (node = 0; node < NUMA_GetNumNodes(); node++) {
      if (p->zeroMPN[node] == mpn) {
         return TRUE;
      }
   }

   return FALSE;
}
     
/*
 *----------------------------------------------------------------------
 *
 * PShare_IsZeroKey --
 *
 *      Query if "key" matches the key for the zero-filled empty page.
 *
 * Results:
 *      Returns TRUE if "key" matches the zero page, otherwise FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PShare_IsZeroKey(uint64 key)
{
   PShare *p = &pshare;
   if (NUMA_GetNumNodes() > 1) {
      return((key >> NUMA_LG_MAX_NODES) ==
             ((p->zeroKey[0] >> NUMA_LG_MAX_NODES)));
   } else {
      return (key == p->zeroKey[0]);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_TotalShared --
 *
 *      Obtain a snapshot of current page sharing statistics.
 *
 * Results:
 *      Sets "nCOW"  to total pages marked copy-on-write.
 *	Sets "nCOW1" to total COW pages with a single reference.
 *	Sets "nUsed" to total pages referenced by COW pages.
 *	Sets "nHint" to total pages tracked as hints, not marked COW.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
PShare_TotalShared(uint32 *nCOW,
                   uint32 *nCOW1,
                   uint32 *nUsed,
                   uint32 *nHint)
{
   PShare *p = &pshare;
   PShareStats *stats = &p->stats;
   SP_IRQL prevIRQL;

   // no sharing if disabled
   if (!p->enabled) {
      *nCOW = *nCOW1 = *nUsed = *nHint = 0;
      return;
   }

   // obtain stats, holding lock
   prevIRQL = PShareLock(p);
   *nCOW  = stats->pageCount;
   *nCOW1 = stats->pageUnshared;   
   *nUsed = stats->hashtblPages;
   *nHint = stats->hintCount;
   PShareUnlock(p, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * PShare_ReportCollision --
 *
 *      Report false match caused by hash collision.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates global PShare statistics.
 *
 *----------------------------------------------------------------------
 */
void
PShare_ReportCollision(uint64 key, World_ID worldID, PPN ppn)
{
   PShare *p = &pshare;
   PShareStats *stats = &p->stats;

   PShareCollision *empty;
   SP_IRQL prevIRQL;
   Bool found;
   uint32 i;

   prevIRQL = PShareLock(p);

   // update total collisions
   stats->collisionCount++;

   // throttled logging
   if (stats->collisionCount >= stats->collisionLog) {
      Log("false match: total %u: key=0x%Lx, vm=%u, ppn=0x%x",
          stats->collisionCount, key, worldID, ppn);
      stats->collisionLog *= 2;
   }

   // search table for matching entry (or empty slot)
   empty = NULL;
   found = FALSE;
   for (i = 0; i < PSHARE_STATS_COLLIDE_MAX; i++) {
      PShareCollision *c = &stats->collide[i];
      if (c->key == key) {
         c->count++;
         c->worldID = worldID;
         c->ppn = ppn;
         found = TRUE;
         break;
      }
      if ((c->count == 0) && (empty == NULL)) {
         empty = c;         
      }
   }

   if (!found && (empty != NULL)) {
      // start new collision entry
      empty->count = 1;
      empty->key = key;
      empty->worldID = worldID;
      empty->ppn = ppn;
   }

   PShareUnlock(p, prevIRQL);
}

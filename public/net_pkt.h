/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * net_pkt.h --
 *
 *    Contains packet structure definitions and accessor functions for the
 *    fields of these structures. All accessors assume that synchronisation
 *    (if required) is done outside.
 */

#ifndef _NET_PKT_PUBLIC_H_
#define _NET_PKT_PUBLIC_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "list.h"
#include "world_dist.h"
#include "vm_basic_defs.h"
#include "net_sg.h"
#include "vmkstress_dist.h"

typedef uint32 VLanID;

typedef void *IOData;
typedef SG_Array SG_MA;
typedef NetSG_Array SG_PA;
typedef SG_Array SG_VA;

typedef NetSG_Array SG_CA; /* console virtual */


typedef struct {
   World_Handle *worldLeader;
   SG_PA sgPA;
} SG_GuestPA;

typedef char *FrameHdrVA;

/* Client address type is one of the following */
typedef enum {
   ADDR_TYPE_VA,
   ADDR_TYPE_PA,
   ADDR_TYPE_MA,
   ADDR_TYPE_CA,
} SrcSGTypes;

typedef struct {
   SrcSGTypes addrType;
   union {
      SG_VA sgVA;
      SG_MA sgMA;
      SG_GuestPA sgGuestPA;
      SG_CA sgCA;
   };
} SrcSG;

#define IS_SET(flags, bit)                                                   \
   ((flags & bit) ? TRUE : FALSE)

#define IS_CLR(flags, bit)                                                   \
   !IS_SET(flags, bit)

#define SET_FLAG(flags, bit)                                                 \
   (flags |= bit)

#define CLR_FLAG(flags, bit)                                                 \
   (flags &= ~bit)

/*
 * Data structures --
 *
 *    PktHandles are the exported pointers to packets for all clients.
 *
 *    PktDescriptors hold immutable data about the packet, only the
 *    master handle (that held by the creator of the packet) may be
 *    used to modify these fields, and only then if there are no other
 *    references to the PktDescriptor (clones or partial copies.)
 *
 *    PktBufDescriptors hold information about the buffers containing
 *    the frame data.  The master handle and all its clones share the
 *    same PktBufDescriptor.  Any partial copies of the packet will
 *    refernce their own private PktBufDescriptor.
 *
 *    see pkt-api.fig for a picture of these relationships.
 */

typedef struct PktDbgInfo PktDbgInfo;
typedef struct PktDescriptor PktDescriptor;
typedef struct PktBufDescriptor PktBufDescriptor;

#ifdef VMX86_DEBUG
#define PKT_DEBUG // turn this on for backtraces and other debugging helpers
#endif // VMX86_DEBUG

typedef struct PktHandle {
   List_Links        pktLinks;         // This packet handle is a part of a list
   PktDescriptor    *pktDesc;          // PktDescriptor this handle refers to
   PktBufDescriptor *bufDesc;          // PktBufDescriptor this handle refers to
   void             *headroom;         // pointer to headroom if any
   FrameHdrVA        frameVA;          // part of the frame mapped
   uint16            frameMappedLen;   // number of bytes mapped
   enum {                              //
      PKT_FLAG_FRAME_HEADER_MAPPED   = 0x00000001, // is frameVA a valid pointer?
      PKT_FLAG_PRIVATE_BUF_DESC      = 0x00000002, // is bufDesc private?
      PKT_FLAG_ALLOCATED             = 0x00000004, // is the packet in use?
      PKT_FLAG_FREE                  = 0x00000008, // Complement of PKT_ALLOCATED
      PKT_VALID_FLAGS                = 0x0000000f  // everything allowed
   }                 flags;            // flags private to this handle
#ifdef PKT_DEBUG
   PktDbgInfo       *dbg;
#endif // PKT_DEBUG
} PktHandle;

struct PktBufDescriptor {
   uint16            bufLen;          // Total length of the buffer(s) described
   uint16            frameLen;        // length of the data
   uint16            sgSize;          // actual number of sg entries
   uint16            headroomLen;     // length of headroom
   SG_MA             sgMA;            // list of machine addresses of the buffer
   /*
    * don't put anything else here, sgMA *must* be the last field so
    * that we can alloc larger blocks in order to have more than
    * NET_PKT_SG_DEFAULT_SIZE scatter gather elements, which is
    * required in some cases, such as when we get elements from a
    * client that cross page boundarys (which may be contiguous
    * in PA space, but not in MA space)
    */
};
#define NET_PKT_SG_DEFAULT_SIZE SG_DEFAULT_LENGTH

struct PktDescriptor {
   Atomic_uint32     refCount;
   PktHandle        *master;          // Only the master handle can modify this.
   enum {                             //
      PKTDESC_FLAG_ALLOCATED        = 0x00000001, // is the packet in use?
      PKTDESC_FLAG_FREE             = 0x00000002, // Complement of PKT_ALLOCATED
      PKTDESC_FLAG_NOTIFY_COMPLETE  = 0x00000004, // completion notification needed?
      PKTDESC_VALID_FLAGS           = 0x00000007  // everything allowed
   }                 flags;           // flags for this descriptor shared by all handles
   VLanID            vlanID;          // which vlan does this packet belong to?
   Net_PortID        srcPortID;       // on which port did this pkt originate?
   IOData            ioCompleteData;  // kernel context for io-complete routine
   SrcSG             srcSG;           // The client's SG array
   PktBufDescriptor  bufDesc;         // describes the buffer
   uint32            magic;           // for sanity checking
};


#define INFINITY (-1)


PktHandle        *Pkt_Alloc(size_t headroom, size_t size);
PktHandle        *Pkt_PartialCopy(PktHandle *pkt, size_t headroom, size_t numBytes);
PktHandle        *Pkt_CopyWithDescriptor(const PktHandle *srcPkt);
VMK_ReturnStatus  Pkt_ReserveBytes(PktHandle *handle, unsigned int n);
VMK_ReturnStatus  Pkt_CopyBytesToSGMA(const SG_MA *baseSG,
                                      size_t numBytes, size_t offset, char *buf);
VMK_ReturnStatus  Pkt_CopyBytesFromSGMA(const SG_MA *baseSG,
                                        size_t numBytes, size_t offset, char *buf);
PktHandle        *Pkt_ReleaseOrComplete(PktHandle *pkt);
void              Pkt_Complete(PktHandle *pkt);


#ifdef NO_PKT_INLINE
#undef INLINE
#define INLINE
#endif

/*
 *----------------------------------------------------------------------------
 *
 *  PktIsMaster --
 *
 *    Is the given packet handle the master?
 *
 *  Results:
 *    True if the handle is the master. FALSE otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE Bool
PktIsMaster(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return (handle->pktDesc->master == handle)? TRUE:FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktGetRefCount --
 *
 *    Return the reference count on the packet.
 *
 * Results:
 *    The current reference count on the packet.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetRefCount(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return Atomic_Read(&handle->pktDesc->refCount);
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktIsPktDescWritable --
 *
 *    Verifies if the handle has sufficient privileges to modify the packet
 *    descriptor. Only the master handle can modify the common packet
 *    descriptor. The only field that clones can modify is the reference
 *    count.
 *
 *  Results:
 *    TRUE if the check succeeds, FALSE otherwise.
 *
 *  Side effects:
 *    None
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PktIsPktDescWritable(const PktDescriptor *pktDesc, const PktHandle *handle)
{
   return (PktIsMaster(handle) &&
           PktGetRefCount(handle) <= 1);
}



/*
 *----------------------------------------------------------------------------
 *
 *  PktIsBufDescWritable --
 *
 *    Verifies if the handle has sufficient privileges to modify the packet
 *    buffer descriptor. Only the master handle can modify the common packet
 *    buffer descriptor.  If the packet handle has a private copy of the
 *    buffer descriptor then it can modify that.
 *
 *  Results:
 *    TRUE if the check succeeds, FALSE otherwise.
 *
 *  Side effects:
 *    None
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PktIsBufDescWritable(const PktHandle *handle)
{

   if (PktIsPktDescWritable(handle->pktDesc, handle) ||
       IS_SET(handle->flags, PKT_FLAG_PRIVATE_BUF_DESC)) {
      return TRUE;
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------------
 *
 *  PktIncRefCount --
 *
 *    Increment the packet's descriptor reference count.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktIncRefCount(PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return Atomic_FetchAndInc(&handle->pktDesc->refCount);
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktDecRefCount --
 *
 *    Decrement the packet's reference count
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktDecRefCount(PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   ASSERT(PktGetRefCount(handle) >= 1);
   return Atomic_FetchAndDec(&handle->pktDesc->refCount);
}

/*
 *----------------------------------------------------------------------------
 *
 * PktSetRefCount --
 *
 *    Set the reference count on the packet.  Asserts that this is only done
 *    for setting it from 0 -> 1 or 1 -> 0 since anything else is unsafe.
 *    (Those transitions are the common case and so we provide this method
 *     to avoid locking the bus for them)
 *
 * Results:
 *    The packet's refcount is set.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetRefCount(const PktHandle *handle, uint32 value)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   ASSERT(((PktGetRefCount(handle) == 0) && (value == 1)) ||
          ((PktGetRefCount(handle) == 1) && (value == 0)));
   Atomic_Write(&handle->pktDesc->refCount, value);
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktIsBufWritable --
 *
 *    Verifies if the handle has sufficient privileges to modify the
 *    packet buffer. Only the master handle can modify the common
 *    packet buffer, and then only if there are no clones.  If the
 *    packet handle has a private copy of the buffer descriptor then
 *    it can modify that.
 *
 *  Results:
 *    TRUE if the check succeeds, FALSE otherwise.
 *
 *  Side effects:
 *    None
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PktIsBufWritable(const PktHandle *handle)
{

   if ((PktIsMaster(handle) && PktGetRefCount(handle) == 1) ||
       (IS_SET(handle->flags, PKT_FLAG_PRIVATE_BUF_DESC))) {
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetIOCompleteData --
 *
 *      Get the io-complete data associated with a packet handle
 *
 *  Results:
 *    The io-complete data associated with the packet handle.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE IOData
PktGetIOCompleteData(PktHandle *handle)
{
   ASSERT(handle != NULL);
   return handle->pktDesc->ioCompleteData;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktClearIOCompleteData --
 *
 *    Clears the "context" for the IOCompletion routine after verifying that the
 *    packet handle has sufficient privileges to modify the packet descriptor.
 *
 *  Results:
 *    None
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktClearIOCompleteData(PktHandle *handle)
{
   ASSERT(handle != NULL);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->ioCompleteData = 0;
   CLR_FLAG(handle->pktDesc->flags, PKTDESC_FLAG_NOTIFY_COMPLETE);
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetIOCompleteData --
 *
 *    Sets the "context" for the IOCompletion routine after verifying that the
 *    packet handle has sufficient privileges to modify the packet descriptor.
 *
 *  Results:
 *    None
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetIOCompleteData(PktHandle *handle, IOData *ioCompleteData)
{
   ASSERT(handle != NULL);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->ioCompleteData = ioCompleteData;
   SET_FLAG(handle->pktDesc->flags, PKTDESC_FLAG_NOTIFY_COMPLETE);
}


/*
 *----------------------------------------------------------------------------
 *
 * PktSetVLanID --
 *
 *    Set the VLanId of the packet to the given vlan id after verifying
 *    privileges.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetVLanID(PktHandle *handle, VLanID vlanID)
{
   ASSERT(handle);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->vlanID = vlanID;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetVLanID --
 *
 *    Get the id of the vlan the packet is associated with.
 *
 *  Results:
 *    vlan id of the packet.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VLanID
PktGetVLanID(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return handle->pktDesc->vlanID;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetSrcPort --
 *
 *    Get the id of the port on which the packet originated.
 *
 *  Results:
 *    src port on which the packet originated
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE Net_PortID
PktGetSrcPort(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return handle->pktDesc->srcPortID;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetDescBufDesc --
 *
 *    Get the base buffer descriptor for the packet.
 *
 *  Results:
 *    src port on which the packet originated
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktBufDescriptor *
PktGetDescBufDesc(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return &handle->pktDesc->bufDesc;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetSrcPort --
 *
 *    Verifies that the caller has sufficient privileges and sets the source
 *    port.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetSrcPort(PktHandle *handle, Net_PortID srcPortID)
{
   ASSERT(handle);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->srcPortID = srcPortID;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetDescFlags --
 *
 *    Return the packet flags.
 *
 *  Results:
 *    Packet flags
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetDescFlags(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return handle->pktDesc->flags;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktIsSetDescFlag --
 *
 *    Check if the specified flag is set or not.
 *
 *  Results:
 *    TRUE if flag is set, FALSE if not.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE Bool
PktIsSetDescFlag(const PktHandle *handle, uint32 flag)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return IS_SET(handle->pktDesc->flags, flag);
}


/*
 *----------------------------------------------------------------------------
 *
 * PktOverwriteDescFlags --
 *
 *    Overwrite Packet flags.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktOverwriteDescFlags(PktHandle *handle, uint32 flags)
{
   ASSERT(handle);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->flags = flags;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetMaster --
 *
 *     Return the master handle.
 *
 *  Results:
 *     master handle.
 *
 *  Side effects:
 *     None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
PktGetMaster(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return handle->pktDesc->master;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktSetMaster --
 *
 *    Set the master handle for the packet.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetMaster(PktHandle *handle, PktHandle *master)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc->master == NULL);
   handle->pktDesc->master = handle;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetMagic --
 *
 *     Set magic value for the packet.
 *
 *  Results:
 *     None.
 *
 *  Side effects:
 *     None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetMagic(PktHandle *handle, uint32 magic)
{
   ASSERT(handle);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->magic = magic;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetMagic --
 *
 *     Get the packet's magic value.
 *
 *  Results:
 *     packet's magic value.
 *
 *  Side effects:
 *     None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetMagic(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return handle->pktDesc->magic;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetBufLen --
 *
 *    Get the length of the buffer associated with this packet.
 *
 *  Results:
 *    length of the buffer
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetBufLen(const PktHandle *handle)
{
   ASSERT(handle);
   return handle->bufDesc->bufLen;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetBufLen --
 *
 *    Set the length of the buffer associated with this packet.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetBufLen(const PktHandle *handle, size_t len)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   ASSERT(handle->bufDesc->frameLen <= len);
   handle->bufDesc->bufLen = len;
}

/*
 *----------------------------------------------------------------------------
 *
 *  PktGetBufType --
 *
 *    Set the address type of the buffer associated with this packet.
 *
 *  Results:
 *    The scatter gather array address type is returned.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE NetSG_AddrType
PktGetBufType(const PktHandle *handle)
{
   ASSERT(handle);

   return handle->bufDesc->sgMA.addrType;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetBufType --
 *
 *    Set the address type of the buffer associated with this packet.
 *
 *  Results:
 *    The scatter gather array address type is set.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetBufType(const PktHandle *handle, NetSG_AddrType type)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   ASSERT(handle->bufDesc->sgMA.length == 0);

   handle->bufDesc->sgMA.addrType = type;
}

/*
 *----------------------------------------------------------------------------
 *
 *  PktGetHeadroomLen --
 *
 *    Get the length of the headroom associated with this packet.
 *
 *  Results:
 *    length of the headroom
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetHeadroomLen(const PktHandle *handle)
{
   ASSERT(handle);
   return handle->bufDesc->headroomLen;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetHeadroomLen --
 *
 *    Set the length of the headroom associated with this packet.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetHeadroomLen(const PktHandle *handle, size_t len)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   handle->bufDesc->headroomLen = len;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetFrameLen --
 *
 *    Get the length of the frame.
 *
 *  Results:
 *    length of the frame.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
PktGetFrameLen(const PktHandle *handle)
{
   ASSERT(handle);
   return handle->bufDesc->frameLen;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetFrameLen --
 *
 *    Set the length of the frame.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetFrameLen(const PktHandle *handle, size_t len)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   ASSERT(handle->bufDesc->bufLen >= len);
   handle->bufDesc->frameLen = len;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktIncFrameLen --
 *
 *    Increment the length of the frame.
 *
 *  Results:
 *    None.
 * 
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktIncFrameLen(const PktHandle *handle, size_t len)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   ASSERT((handle->bufDesc->bufLen - handle->bufDesc->frameLen) >= len);
   handle->bufDesc->frameLen += len;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktDecFrameLen --
 *
 *    Decrement the length of the frame.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktDecFrameLen(const PktHandle *handle, size_t len)
{
   ASSERT(handle);
   ASSERT(PktIsBufDescWritable(handle));
   ASSERT(handle->bufDesc->frameLen >= len);
   handle->bufDesc->frameLen -= len;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktGetSrcSG --
 *
 *    Get the source SG for the given packet handle.
 *
 *  Results:
 *    The Source SG for the given packet handle.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE SrcSG *
PktGetSrcSG(const PktHandle *handle)
{
   ASSERT(handle);
   ASSERT(handle->pktDesc);
   return &handle->pktDesc->srcSG;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktSetSrcSG --
 *
 *    Set the source SG for the packet.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktSetSrcSG(PktHandle *handle, SrcSG *srcSG)
{
   ASSERT(handle);
   ASSERT(PktIsPktDescWritable(handle->pktDesc, handle));
   handle->pktDesc->srcSG = *srcSG;
}




/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_Clone --
 *
 *    Dup the given packet handle.
 *
 *  Results:
 *    A pointer to a handle that is a clone of the given PktHandle.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
Pkt_Clone(PktHandle *handle)
{
   ASSERT(handle);
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_CLONE_FAIL))) {
      return NULL;
   }
   return Pkt_PartialCopy(handle, 0, 0);
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_FrameCopy --
 *
 *    Return a copy of the entire frame.
 *
 *  Results:
 *    A pointer to the copy of the frame.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
Pkt_FrameCopy(PktHandle *handle, size_t *frameHdrLen)
{
   ASSERT(handle);
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_FRAME_COPY_FAIL))) {
      return NULL;
   }
   return Pkt_PartialCopy(handle, 0, INFINITY);
}

/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_CopyBytesOut --
 *
 *    Copy the frame data out of packet, starting at offset, and extending
 *    for len, into the provided buffer.
 *
 *  Results:
 *    VMK_ReturnStatus.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Pkt_CopyBytesOut(void *dst, size_t len, unsigned int offset,
                 const PktHandle *handle)
{
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_COPY_BYTES_OUT_FAIL))) {
      return VMK_FAILURE;
   }

   if (handle->frameMappedLen >= (len + offset)) {
      memcpy(dst, (char *)handle->frameVA + offset, len);
      return VMK_OK;
   } else {
      // have to iterate SG elems and do mappings
      return Pkt_CopyBytesFromSGMA(&handle->bufDesc->sgMA, len, offset, dst);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_CopyBytesIn --
 *
 *    Copy the frame data, starting at offset, and extending for len, into a
 *    Pkt from the provided buffer
 *
 *  Results:
 *    VMK_ReturnStatus.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Pkt_CopyBytesIn(void *src, size_t len, unsigned int offset,
                const PktHandle *handle)
{
   ASSERT(PktIsBufWritable(handle));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_COPY_BYTES_IN_FAIL))) {
      return VMK_FAILURE;
   }

   if (handle->frameMappedLen >= (len + offset)) {
      memcpy((char *)handle->frameVA + offset, src, len);
      return VMK_OK;
   } else {
      // have to iterate SG elems and do mappings
      return Pkt_CopyBytesToSGMA(&handle->bufDesc->sgMA, len, offset, src);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_AppendBytes --
 *
 *    Copy the len bytes into the Pkt and increment the pkt's framelen.
 *
 *  Results:
 *    VMK_ReturnStatus.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Pkt_AppendBytes(void *src, size_t len, PktHandle *handle)
{
   VMK_ReturnStatus status;

   ASSERT(PktIsBufDescWritable(handle));
   ASSERT(PktIsBufWritable(handle));

   status = Pkt_CopyBytesIn(src, len, PktGetFrameLen(handle), handle);
   if (status == VMK_OK) {
      PktIncFrameLen(handle, len);
   }

   return status;
}

/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_AppendFrag --
 *
 *    Add the fragment to the packet's scatter gather array, possibly
 *    breaking it into smaller fragments to avoid spanning pages.
 *
 *  Results:
 *    VMK_ReturnStatus.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Pkt_AppendFrag(MA fragMA, size_t fragSize, PktHandle *handle)
{
   uint16 idx = handle->bufDesc->sgMA.length;

   ASSERT(PktIsBufDescWritable(handle));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_APPEND_FRAG_FAIL))) {
      return VMK_FAILURE;
   }

   while ((fragSize > 0) && (idx < handle->bufDesc->sgSize)) {
      size_t subFragSize = MIN(fragSize,
                               PAGE_SIZE - (fragMA & PAGE_MASK));

      handle->bufDesc->sgMA.sg[idx].addr = fragMA;
      handle->bufDesc->sgMA.sg[idx].length = subFragSize;
      handle->bufDesc->sgMA.length++;
      handle->bufDesc->bufLen += subFragSize;
      idx++;
      fragSize -= subFragSize;
      fragMA += subFragSize;
   }

   if (fragSize == 0) {
      return VMK_OK;
   } else {
      ASSERT(FALSE);
      return VMK_LIMIT_EXCEEDED;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_Release --
 *
 *    Release a packet back to the free pool.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
Pkt_Release(PktHandle *pkt)
{
   ASSERT(pkt != NULL);
   ASSERT(!IS_SET(pkt->pktDesc->flags, PKTDESC_FLAG_NOTIFY_COMPLETE));
   pkt = Pkt_ReleaseOrComplete(pkt);
   ASSERT(pkt == NULL);
}


#ifdef NO_PKT_INLINE
#undef INLINE
#define INLINE inline
#endif

#endif //_NET_PKT_PUBLIC_H_

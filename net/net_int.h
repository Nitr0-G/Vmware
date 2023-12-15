/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * net_int.h  --
 *
 *    Interfaces shared only within the vmkernel net module.
 */

#ifndef _NET_INT_H_
#define _NET_INT_H_

// system stuff
#include "vmkernel.h"
#include "vm_libc.h"
#include "list.h"
#include "net_public.h"
#include "net.h"
#include "net_pkt.h"
#include "net_pktlist.h"
#include "kseg.h"
#include "proc.h"
#include "netDebug.h"


/*
 * IO model:
 *
 * Vmkernel networking code is implemented in small functional units
 * (described by IOChainLink structs) which are linked together
 * either statically, at runtime, or a combination of the two, to
 * form policies for routing frames on virtual networks.  Each
 * virtual network is described by a Portset struct.  Each entity
 * on the virtual network has a connection which is described by
 * a Port struct.  Each Port has an IOchain for input, output, and
 * IO completion.  We use the terms "input" and "output" with respect
 * to the Portset itself (ie to transmit a frame, the input chain
 * on the source port is run, and the output chain on the destination
 * port is run.) 
 *  
 * One key aspect to note about the IO model is that the functional
 * units are executed iteratively, rather than inline.  This greatly
 * simplifies lock ranking as each functional unit only has to rank
 * its lock(s) with respect to the portset lock.
 *
 * Locking model:
 *
 * Network port access is synchronized by per portset reader/writer
 * locks.  These locks are acquired by indexing into a global array
 * of portsets (portsetArray).  The array is static for the life of
 * the net module, and each entry is protected by its own lock so no 
 * further synchronisation is needed for single device access.  The 
 * portsets are never accessed directly from interrupt context so the 
 * locks do not disable interrupts.  Non-destructive accessors to the
 * portset (like input and output paths) take a non-exclusive reader
 * lock on the portset.  Destructive accessors (like connect, config,
 * and disconnect) take an exclusive writer lock on the portset.
 *
 * For destructive access to the global portset array (like create or
 * destroy) or for iterations or searches of it, a global lock
 * (portsetGlobalLock) is acquired to provide synchronisation.
 * portsetGlobalLock also protects the networking portion of the world 
 * cleanup sequence from normal connect/disconnect.
 *
 * XXX this locking model depends on some way to prevent entry to the
 *     module before it is fully initialized.  we will be adding 
 *     a function table pointer for all the netcalls before shipping 
 *     so that the net module is loadable at runtime, and that will 
 *     suffice.  Until then, we'll live dangerously (sortof.)  
 *
 * The lock rankings are specified in vmkernel/private/net.h
 */

#define NET_MAX_PKT_SIZE	1536

// forward decls
typedef struct Port Port;

#include "pkt.h"
#include "pktlist.h"
#include "pkt_dbg.h"

// core implementation
#include "iochain.h"
#include "eth.h"
#include "port.h"
#include "portset.h"
#include "uplink.h"
#include "bond.h"
#include "proc_net.h"

// vmkernel virtual device implementations
#include "vmxnet2_vmkdev.h"
#include "vlance_vmkdev.h"
#include "cos_vmkdev.h"

// portset class (device) implementations
VMK_ReturnStatus Nulldev_Activate(Portset *ps);
VMK_ReturnStatus Loopback_Activate(Portset *ps);
VMK_ReturnStatus Hub_Activate(Portset *ps);
VMK_ReturnStatus Bond_Activate(Portset *ps);

/*
 * XXX these should come from including <net/if.h> but just try and figure
 *     which one of the many copies of the file you'll get if you do include
 *     it.  opting for pulling the flags we need since these can't ever
 *     change anyway.
 */
#define	IFF_UP		0x0001
#define	IFF_BROADCAST	0x0002
#define	IFF_PROMISC	0x0100
#define	IFF_ALLMULTI	0x0200
#define	IFF_MULTICAST	0x8000

// printf helpers
#define IFF_FMT_STR     "%s%s%s%s%s"
#define IFF_FMT_ARGS(a) (a) & IFF_UP        ? "  UP"        : "", \
                        (a) & IFF_BROADCAST ? "  BROADCAST" : "", \
                        (a) & IFF_PROMISC   ? "  PROMISC"   : "", \
                        (a) & IFF_ALLMULTI  ? "  ALLMULTI"  : "", \
                        (a) & IFF_MULTICAST ? "  MULTICAST" : ""

#endif // _NET_INT_H_


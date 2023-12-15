This document describes a general model for various types of network
IO, including, but not limited to, the virtual switch implementation
that users have today in ESX 2.x. 

There are also 3 .fig files in this directory which provide diagrams
that may be useful as companions to this document.  Please feel free
to rip off any or all of this for slideware or other presentation
material for interal viewing, but please double check with vmkernel
networking developers (try vmknet-nanny@vmware.com) to verify the
freshness before doing so.  -wal



The vmkernel is required to multiplex and demultiplex network data
streams between virtual and physical networks.  The ESX 2.x module was
created to do just that and no more.  As we enhance ESX to provide
more functionality to users, a more general and flexible model is
needed in order to keep the code fast and maintainable.


Terminology / High Level Concepts
---------------------------------

Ports

        A port is a data structure encapsulating the state of an
        endpoint capable of transmitting and receiving network frames.
        The analogy is to ports on a hub or a switch in the physical
        world.  

        Examples in the vmkernel are a virtual network connection to a 
        VM or the COS, or virtual bridge to a physical adapter which is 
        made available by the vmkernel for connecting a virtual switch 
        or virtual network adapter to a physical network.

        Ports are similar in scope and purpose to the Net_EtherHandle 
        structs in ESX 2.x


Portsets 

        By itself a single port is not useful, except possibly as a
        loopback device.  However when two or more ports are connected
        together, a virtual network is formed.  Ports within a common
        portset may communicate with each other in a manner described 
        by the set.  The analogy is to a box (like a hub or a switch)
        containing some type of backplane for connecting multiple
        ports in the physical world.

        Examples of portsets are a simple repeater for connecting
        two ports together, or an ethernet switch which can route
        frames intelligently between ports.

        Portsets are similar in scope and purpose to Net_EtherDev 
        structs in ESX 2.x

        In addition to encapsualting traditional switch/hub 
        functionalities, portsets are also being used in ESX 3
        to implement bonds, which are groups of uplink ports
        with frame routing policies which provide load balancing
        or failover across the uplinks.


Input, Output

        I use these terms from the perspective of the portset.  For
        example, a VM transmitting a packet over a virtual switch
        consists first of an input to the switch, some switching logic
        in the middle, and then an output on zero or more ports.


IO Chains

        IO chains are a simple way to allow multiple functionalities
        to be enabled or disabled at runtime, without having every
        function in the IO path turn into a giant tangled mess of
        interdependent branches. (see NetDoTransmit() for an example
        of what I'm trying to avoid here) In pure form, IO chains are
        simply a linked list of functions which are called one at
        time.  In practice, some common paths will be statically
        programmed, to avoid indirect calls.  Each port in a
        set will have associated with it an input io chain and
        an output io chain.  This allows multiple features to be made
        available to users who want them, without penalizing users who
        don't.  

        IO chains will operate on lists of packets, with each element
        of the chain having the ability to remove packets from the
        list and either queue them until later or return them to the
        free pool for reuse.  An io chain will be run until either the
        last element is reached, or the packet list has been emptied.
        This allows for filters and shapers to be implemented.  

        In order for a portset to be able to present the same set of
        packets to more than one of its member ports, any IO chain
        link which may modify the packet list it's passed (such as a
        filter or a shaper) must indicate this fact to the portset
        upon registration.  The generic IO chain call mechanism will
        handle making a shallow copy of the packet list before passing
        it to any such function.

        In order to allow sensible runtime configuration, IOChains
        are actually broken down into several canonical levels, like
        prefilter, filter, postfilter, etc.  Ordering of calls within
        a level is indeterminate, but by grouping certain types of
        calls together, and maintaining a consistant ordering of the 
        groups, the necessary ordering dependencies should be able to
        be maintained.

        Examples of optional elements in IO chains are VLAN support,
        NIC teaming, and traffic shaping.  Another important use of IO
        chains will be to provide backward compatibility with older
        VMs, without impacting the speed or maintainability of the
        latest fast path through the code.


Packet Descriptor

        I'll use the term packet descriptor to refer to the entire
        collection of vmkernel specific data and network frame data.
        That is, a packet descriptor consists of a network frame
        (typically ethernet) as well as some vmkernel specific data
        structures used to describe the history and state of the
        encapsulated network frame.

        Examples of vmkernel specific data stored in a packet
        descriptor are flags describing whether the encapsulated frame
        is expected to receive some further processing like checksum
        offload, and information used by the vmkernel to notify the
        sender of the frame when the io has completed.


Clone

        In reference to packet descriptors and packet descriptor
        lists, "clone" refers to a variably shallow copy.  Most of the
        vmkernel packet descriptor data will be shared by all clones
        of a packet descriptor (e.g. buffer mappings, port of origin,
        the actual buffers, etc.)  A few fields like the list pointers
        and some tracing flags will be private to each clone.
        Additionally some variable ammount of frame data itself may be
        copied to an area private to the clone.  Each clone accounts
        for one reference count on the master.


Portset clients

        Every portset client will have some specialized entry point
        which will perform any necessary translation and then call the
        input io chain for the port.  Similarly each portset client will
        also have a specialized output function which will be called
        at the end of the port's output chain to perform any necessary
        translation, and possibly an IO completion handler if the 
        client allows other clients downstream to hold onto packets
        it inputs to the portset.

        These functions taken together with attach, detach, and
        various configuration functions can be thought of as the
        vmkernel portion of device emulation/implementation for a
        given physical or virtual device.

        I was originally planning to just have these be the
        extremities of the port IO chain, but Gustav suggests
        formalizing the distinction.  That seems like a decent idea,
        so I'll think about how to name things to do so.  The existing
        plan already takes care of isolating this code, there just
        wasn't any formal distinction between the internal links of a
        port io chain and its terminating link.


Frame Routing Policies

        Each class of portset is distinguished by a set of frame
        routing policies, and each instance is distinguished from
        its fellow class members by parameters to those frame
        routing policies.  Three types of frame routing policy
        exist: ethernet, bond, and security.  Each port on a portset 
        maintains a set of the parameters for the portset to
        apply in order to route frames to or from that port.


Ethernet Frame Routing Policy

        If a portset implements this type of policy it routes
        frames based on their source and/or destination MAC
        address, and potentially based on 802.1q VLAN IDs and/or
        802.1p priority tags.  The per port encapsulation of
        the policy paramters includes flags for broadcast and
        multicast reception, a MAC addr to use for unicast frame 
        reception, etc.

Bond Frame Routing Policy

        If a portset implements this type of policy it routes
        frames based on the source port ID (or potentially based
        on source MAC, or destination IP, if those modes are 
        enabled)  This type of policy is used to implement teams
        of bridged physical NICS to provide failover and/or
        load balancing capabilities.

Security Frame Routing Policy

        If a portset implements this type of policy it passes or
        blocks frames based on parameters which allow or disallow
        each portset client to perform actions like enable
        promiscuous mode, change its MAC addr, or send frames
        with a source MAC addr different from that configured 
        for the client.

        


Data Flow
---------

General

        Data flow is always initiated by a port input.  The port's 
        client specific translation code  creates a packet list and
        calls the port's input io chain.

        Once the input chain has run, any remaining packets are passed
        to the port's parent set's dispatch function, which may call
        output chains for one or more of the ports in the set.  

        Each port may have one or more chain entries for io completion.  
        If provided, these calls will be made to indicate the completion 
        of any packets orginating from the port.

Vmkernel Device specific

        vmxnet ports (virtual adapters)
               
              Transmitting data to the physical and/or virtual network

                     Upon a call from the vmm to perform a transmit,
                     or opportunistically from the receive path, the
                     vmxnet specific port client call is made.  This
                     call will first create a packet list by accepting
                     any available entries in the client's tx ring,
                     and grabbing the associated premapped,
                     pretranslated vmkernel packet structure from an
                     array created at when the port is initialized.
                     The resulting packet list is first passed to the
                     port's input io chain, and then to the parent
                     set's dispatch function as described above.

              
              Receiving data from the physical and/or virtual  network

                     Once a portset has routed a packet to the
                     port, it's output io chain is called and once
                     that is complete, any remaining packets are
                     copied into the guest's buffers and an interrupt
                     is possibly delivered to the guest to indicate
                     their arrival.
            

        vlance ports (virtual adapters)

              Similar to vmxnet ports except that receive data is queued
              by the port and the guest must make a vmkernel call to
              retrieve the data upon notification.


        COS ports (shared NICs)

              Similar to vmxnet ports except for the details of mapping 
              buffers and delivering interrupts.

              
        Uplink ports (physical network devices)

              Receiving data from the physical network

                     All physical network devices will have a port
                     assigned to them when the driver registers the
                     device with the vmkernel.  When a device
                     interrupts, packets are passed to netif_rx() for
                     receives and kfree_skb() for completed transmits.
                     Both functions will simply queue the skbs on a
                     device specific queue and schedule a bottom half
                     to complete their work.

                     The bottom half handler will take packets from
                     the rx queue and translate any skb specific info
                     into the into the vmkernel packet header (eg vlan
                     tag, checksum computed flag, etc) and create a
                     packet list. This list will then be processed the
                     port's input io chain and the portset's
                     output function as described above.

                     The bottom half handler will also empty the queue
                     of tx completions from the device, translate the
                     skbs int vmkernel packets and pass the packet
                     list to the set's io completion function,
                     which will take whatever steps are necessary to
                     notify senders of the completions.


              Transmitting data to the physical network
              
                     Once a portset has routed a packet to an
                     uplink port, the port's output io chain is
                     called, and when that is complete, any remaining
                     packets in the list are translated to skbs.  Some
                     fields of the skb, such as the machine address of
                     a pinned vmkernel buffer, are constant from one
                     use to the next and are not retranslated, while
                     others such as the length of the encapsulated
                     frame must be translated every time.  The
                     resulting skbs are passed to the underlying
                     driver's hard_start_xmit() function where the
                     frames are set up for transmission by the
                     hardware.



Support APIs
------------

Pkt API

        API for allocating and manipulating network buffers described
        by Pkt structs.

        Each frame being handled by the system is assigned a single
        packet descriptor.  References to that descriptor are in the
        form of packet handles.  Each descriptor has one base handle
        embedded in it.  Clones of the frame result in new handles
        pointing to the base descriptor handle.  The base handle
        points to itself.  Accessor macros/inlines always access
        handle->base->whatever for static fields to keep the size of
        the clones down.

        When cloning a packet, a user may specify a non-zero length of
        the frame data to be copied to a handle private buffer.  This
        is useful for io chains which modify a small amount of the
        frame, but not all of it.

        When the last reference to a Pkt is released it is returned to 
        a free pool for reuse.  In addition, or alternatively, other
        actions may be taken based on a set of optional flags in the Pkt.

        The API will support allocating packets of various sizes, by
        maintaining multiple free lists of various sizes.  Probably
        small (~100), medium (~2k), and large (~9k, for jumbo frames)
        is sufficient.

PktList API
                             
        Specialized packet list manipulation API to help us batch
        packet processing as much as possible.  All of the IO chain
        calls operate on packet lists.  Mostly this API is a standard
        linked list implementation (and in fact much of the code will
        just rely on the List_Links implementation we already have)
        but it offers some things which are tailored specifically to
        the needs of networking code.

        Links for use by the PktList API are embedded in the PktHandle
        struct.  When placing a packet on more than one list, a shallow
        copy is made and the refcount of the original is bumped.  A
        shallow copy of an entire list may be made in some cases, such
        as before calling an IO chain link which has indicated that it 
        may modify the list.


XXX GuestMem API

        Protecting the vmkernel and guests from rogue or buggy guests
        is critical.  Our reliance on shared data for vmxnet is at
        cross purposes with this requirement.  We need a simple set of
        accessor functions and macros to access data shared with the
        guests.  This API will be the only mechanism for accessing
        shared data structures.  To enforce this, all shared data
        structures will be opaque outside of this code.

        XXX currently this isn't implemented, I've opted for careful 
        coding of the vmxnet module and isolation of any references
        to guest memory to vmxnet2_vmkdev.c  When I implement
        vmxnet3_vmkdev.c, it might make sense to look at this again,
        although there may be very little to factor out.


Names and Naming conventions
----------------------------

        Rx and Tx - don't use recv or xmit or any other abbrev.

        Connect/Disconnect - whenever a logical linkage is made
                             between two packet producing/consuming
                             entities.  don't use attach/detach or
                             open/close for these actions.  For example
                             the vmxnet monitor device implementation 
                             will call down to connect the device when
                             the device is powered on (or reconnected 
                             via the RUI)

        Enable/Disable - distinguished from Connect/Disconnect in
                         that they affect whether packets actually
                         are able to flow but don't affect the logical
                         relationship between entities.  Reserved for
                         client side actions. for example the vmxnet
                         monitor device implementation will call down
                         to the vmkernel to enable the device when the
                         driver opens the device in the guest. 

        Block/Unblock - similar to Enable/Disable but reserved for
                        portset side actions.  For example the etherswitch
                        portset implementation may block a port because
                        the client is not in compliance with the configured
                        security policy.

        Prefixes:

             Pkt

             PktList

             Port

             Portset

             Eth - not ethernet or ether

             VLAN

             Bond - not team (although I like team better, but if we change
                    it, I want to simple to a global search and replace)

             Vmxnet - vmxnet has transcended from partial acronym 
                      to proper noun, and finally to common noun

             VmxnetN - used to distinguish version specific vmxnet code.

             Vlance - noun

             XXXVMKDev - for anything that implements the vmkernel side of a
                         virtual device (aka portset client)





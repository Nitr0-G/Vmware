/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkdriver.h --
 *
 *	vmkdriver is (or rather, will become) the central interface to the
 *	vmkernel. All kernel modules will access internal vmkernel functions
 *	through the wrappers that exist inside the vmkdriver layer.
 *
 *	Essentially, this should end up looking like the picture below:
 *
 *	   ------------------------------------------------------
 *	   |                                                    |
 *	   |                     vmkernel                       |
 *	   |                                                    |
 *	   ------------------------------------------------------
 *	   |                  public header files               |
 *	   ------------------------------------------------------
 *	   |                                             |   ^
 *	   |                     vmkdriver               |  /|\
 *	   |                                             |   |
 *	   -----------------------------------------------   |
 *	   |    dist header files, exported symbols      |   |
 *	   -----------------------------------------------   |
 *	           ^       ^      ^                  	     |
 *	          /|\     /|\    /|\       	      ------------------    
 *	           |       |      |	              |	         |	 
 *	   ------------    | ------------        ----------- -------
 *	   |          |    | |          |	 |         | |     | 
 *	   | vmklinux |    | | freebsd? | ...    | conduit | | net | ... 
 *	   |          |    | |          |        |         | |     |
 *	   ------------    | ------------        ----------- -------
 *	        ^          |
 *	       /|\         |
 *	        |          |
 *	   ----------------------
 *	   |                     |
 *	   |  typical   |  mod   |  ...
 *	   |  driver    |  heap  |
 *	   |                     |
 *	   -----------------------
 *
 *				Note: (hopefully) not drawn to scale
 *
 *
 *	Think of this as sort of a higher-level syscall interface, or just as a
 *	layer of indirection for making actual vmkernel calls.
 *
 *	With any luck, this extra layer of indirection (the vmkdriver) will
 *	provide the following benefits:
 *
 *	- enable a stable interface that can be fully exposed to 3rd parties for
 *	  writing drivers
 *
 *	- allow for extra error-handling and sanity checks before passing values
 *	  on to interior kernel functions
 *
 *	- can limit usage of standard kernel functions -- i.e. a vmkernel
 *	  function may typically take 4 arguments, whereas its vmkdriver wrapper
 *	  only exposes 2 arguments to the outside world
 *
 *
 *	Note that some modules will remain outside of vmkdriver interaction.
 *	These modules, for now, include the conduit and bond modules, and the
 *	soon-to-be-complete net and fs modules. These modules are tied very
 *	tightly to the kernel's operation and use functionality of the kernel
 *	that should not be exposed to 3rd parties.
 *
 */

#ifndef _VMKDRIVER_H
#define _VMKDRIVER_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

/*
 * This will be a list of *_dist.h files that should each have within them a list
 * of EXPORT_SYMBOL function names, etc. that will be available to modules.
 * There will be a close connection between *_public.h files and *_dist.h
 * files... The former will be used to provide internal vmware code (i.e. the
 * monitor) with access to certain kernel functions, and the latter will be used
 * to specify exactly which functions, variables, etc. will be exported to 3rd
 * party driver writers, etc.
 */
#include "heap_dist.h"
#include "mod_loader_dist.h"

#endif //_VMKDRIVER_H


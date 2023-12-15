/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Internal file used to resolve dependencies on vsiTree_int.h
 */

/*
 * vsiTree_Int.h should only be used by this file for dependency generation, and 
 * the parser.   Try to prevent innocent inclusions by erroring out unless 
 * VSITREE_ALLOW_INCLUDE is defined.
 */
#define VSITREE_ALLOW_INCLUDE
#include "vsiTree_int.h"

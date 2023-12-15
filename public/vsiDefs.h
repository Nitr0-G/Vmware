/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vsiDefs.h --
 *
 *	Various vmkernel sysinfo interface definitions
 */


#ifndef _VMK_SYSINFO_DEFS_H_
#define _VMK_SYSINFO_DEFS_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include "return_status.h"
#include "vsiParams.h"

#ifndef VSIDEF_VSINODEID
typedef uint32 VSI_NodeID;
#endif /* VSIDEF_VSINODEID */

#define VSI_INVALID_NODEID 0

// maximum nested instances
#define VSI_MAX_INSTANCE_ARGS 100
// maximum input args for SET handler
#define VSI_MAX_INPUT_ARGS    100

#ifndef VSIDEF_PARSER
#define VSI_DEF_TYPE(name, baseType, formatStr) typedef baseType name

#define VSI_DEF_ARRAY(name, baseType, size) typedef baseType name[size]

#define VSI_DEF_STRUCT(name, helpstr) \
           typedef struct name name; \
           struct name 

#define VSI_DEF_ENUM(name, helpstr) \
           typedef enum name name; \
           enum name 

#define VSI_DEF_STRUCT_FIELD(type, name, helpStr) type name

#define VSI_DEF_BRANCH(name, parent, helpstr) 

#define VSI_DEF_INST_BRANCH(name, parent, listfunc, helpstr) \
                VMK_ReturnStatus listfunc(VSI_NodeID, VSI_ParamList *, VSI_ParamList *) 

#define VSI_DEF_LEAF(name, parent, getfunc, setfunc, inoutstruct, helpstr) \
                VMK_ReturnStatus getfunc(VSI_NodeID, VSI_ParamList *, inoutstruct *); \
                VMK_ReturnStatus setfunc(VSI_NodeID, VSI_ParamList *, VSI_ParamList *)

#define VSI_DEF_INST_LEAF(name, parent, \
                         listfunc, getfunc, setfunc, inoutstruct, helpstr) \
                VMK_ReturnStatus listfunc(VSI_NodeID, VSI_ParamList *, VSI_ParamList *); \
                VMK_ReturnStatus getfunc(VSI_NodeID, VSI_ParamList *, inoutstruct *); \
                VMK_ReturnStatus setfunc(VSI_NodeID, VSI_ParamList *, VSI_ParamList *)
#endif /* VSIDEF_PARSER */

VSI_DEF_TYPE(VSI_CHAR_U8, unsigned char, "%c");
VSI_DEF_TYPE(VSI_BOOL,   unsigned char, "%u");
VSI_DEF_TYPE(VSI_DEC_U8 , unsigned char, "%u");
VSI_DEF_TYPE(VSI_DEC_S32 , int32, "%d");
VSI_DEF_TYPE(VSI_DEC_U32 , uint32, "%u");
VSI_DEF_TYPE(VSI_HEX_U32 , uint32, "%x");
VSI_DEF_TYPE(VSI_DEC_S64 , int64, "%Ld");
VSI_DEF_TYPE(VSI_DEC_U64 , uint64, "%Lu");
VSI_DEF_TYPE(VSI_HEX_U64 , uint64, "%Lx");

struct _VSI_TypeDef;

typedef struct _VSI_StructField {
   char                *fieldName;
   struct _VSI_TypeDef *fieldType;
   size_t              fieldOffset;
   char                *helpStr;
} VSI_StructField;

typedef enum _VSITypeDefType { 
   VSITypeBase,
   VSITypeArray,
   VSITypeStruct
} VSITypeDefType;

typedef struct _VSI_TypeDef {
   char            *name;
   VSITypeDefType  type;
   uint32          size;
   char            *helpStr;

   union { 
      struct _array_t {
         uint32    nElement;
      } array_t;

      struct _struct_t {
         uint32          nStructField;
         VSI_StructField *structFields;
      } struct_t;
   } u;
} VSI_TypeDef;

typedef struct VSI_NodeInfo {
   VSI_NodeID nodeID;
   char *nodeName;
   Bool isLeaf;
   Bool isInstance;

   VSI_NodeID parent;
   VSI_NodeID nextSibling;

   // only valid for branches
   VSI_NodeID firstChild;

   // only valid for leaf
   VSI_TypeDef *outputType;
} VSI_NodeInfo;

typedef VMK_ReturnStatus (*VSI_GetHandler)(VSI_NodeID nodeID, 
                                           VSI_ParamList *instanceArgs,
                                           void *outputStruct);

typedef VMK_ReturnStatus (*VSI_SetHandler)(VSI_NodeID nodeID, 
                                           VSI_ParamList *instanceArgs,
                                           VSI_ParamList *inputArgs);

typedef VMK_ReturnStatus (*VSI_ListHandler)(VSI_NodeID nodeID,
                                            VSI_ParamList *instanceArgs,
                                            VSI_ParamList *instanceListOut);

typedef struct VSI_Handlers {
   VSI_ListHandler listHandler;
   VSI_GetHandler  getHandler;
   VSI_SetHandler  setHandler;
} VSI_Handlers;

extern uint32 vsiMaxNodes;
extern VSI_NodeInfo vsiNodesLookupTab[];
extern VSI_Handlers vsiHandlers[];
extern VSI_TypeDef vsiTypeDefsLookupTab[];

void * VSI_Alloc(uint32 size);
void VSI_Free(void *ptr);

/*
 *----------------------------------------------------------------------
 *
 * VSI_GetNodeInfo --
 *
 *	Return VSI_NodeInfo struct associated with given Node ID
 *
 * Results:
 *	Pointer to VSI_NodeInfo or NULL if invalid Node ID
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VSI_NodeInfo *
VSI_GetNodeInfo(VSI_NodeID node)
{
   if (node >= vsiMaxNodes) {
      return NULL;
   }
   return &vsiNodesLookupTab[node];
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_IsValidNode --
 *
 *	Get validity of given nodeID
 *
 * Results:
 *	TRUE if given nodeID is valid, FALSE otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VSI_IsValidNode(VSI_NodeID node)
{
   VSI_NodeInfo *info = VSI_GetNodeInfo(node);
   if (info == NULL) {
      return FALSE;
   } else {
      return TRUE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_IsLeafNode --
 *
 *	Determine if given node is a leaf
 *
 * Results:
 *	TRUE if given nodeID is a leaf, FALSE otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VSI_IsLeafNode(VSI_NodeID node)
{
   VSI_NodeInfo *info = VSI_GetNodeInfo(node);
   if (info == NULL) {
      return TRUE;
   }
   return info->isLeaf;
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_IsInstanceNode --
 *
 *	Determine if given node is an instance node
 *
 * Results:
 *	TRUE if given nodeID is an instance node, FALSE otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VSI_IsInstanceNode(VSI_NodeID node)
{
   VSI_NodeInfo *info = VSI_GetNodeInfo(node);
   if (info == NULL) {
      return TRUE;
   }
   return info->isInstance;
}


/*
 *----------------------------------------------------------------------
 *
 * VSI_GetOutputSize
 *
 *	Determine the output struct size associated with this node's GET
 *	handler.
 *
 * Results:
 *	Number of bytes in output struct
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
VSI_GetOutputSize(VSI_NodeID node)
{
   VSI_NodeInfo *info = VSI_GetNodeInfo(node);
   if (info == NULL) {
      return 0;
   }

   return info->outputType->size;
}


#ifdef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VSI_NodeGetHandler --
 *
 *	Return Get() handler associated with given node ID
 *
 * Results:
 *	Pointer to Get() handler or NULL if invalid Node ID
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VSI_GetHandler 
VSI_NodeGetHandler(VSI_NodeID nodeID)
{
   if (nodeID >= vsiMaxNodes) {
      return (VSI_GetHandler)NULL;
   }
   return vsiHandlers[nodeID].getHandler;
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_NodeSetHandler --
 *
 *	Return Set() handler associated with given node ID
 *
 * Results:
 *	Pointer to Set() handler or NULL if invalid Node ID
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VSI_SetHandler 
VSI_NodeSetHandler(VSI_NodeID nodeID)
{
   if (nodeID >= vsiMaxNodes) {
      return (VSI_SetHandler)NULL;
   }
   return vsiHandlers[nodeID].setHandler;
}

/*
 *----------------------------------------------------------------------
 *
 * VSINodeGetListHandler --
 *
 *	Return List() handler associated with given node ID
 *
 * Results:
 *	Pointer to List() handler or NULL if invalid Node ID
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VSI_ListHandler 
VSI_NodeGetListHandler(VSI_NodeID nodeID)
{
   if (nodeID >= vsiMaxNodes) {
      return (VSI_ListHandler)NULL;
   }
   return vsiHandlers[nodeID].listHandler;
}
#endif // VMKERNEL

#endif /* _VMK_SYSINFO_DEFS_H_ */

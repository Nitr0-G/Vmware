/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * parse.h --
 *
 *	Simple parsing utility routines.
 */

#ifndef _PARSE_H
#define _PARSE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

EXTERN int Parse_Args(char *buf, char *argv[], int argc);
EXTERN void Parse_ConsolidateString(char *str);
EXTERN Bool Parse_RangeList(const char *str, unsigned long val);
EXTERN VMK_ReturnStatus Parse_Int(const char *buf, int len, uint32 *value);
EXTERN VMK_ReturnStatus Parse_Hex(const char *buf, int len, uint32 *value);
EXTERN VMK_ReturnStatus Parse_IntMask(char *buf, uint32 max, uint32 *value, char **badToken);
EXTERN VMK_ReturnStatus Parse_IntMask64(char *buf, uint32 max, uint64 *value, char **badToken);
EXTERN VMK_ReturnStatus Parse_Int64(char *buf, int len, int64 *result);
#endif

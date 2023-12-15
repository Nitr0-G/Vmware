/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * serial.h --
 *
 *	Serial port module.
 */

#ifndef _SERIAL_H
#define _SERIAL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

extern void Serial_EarlyInit(VMnix_ConfigOptions *vmnixOptions);
extern void Serial_LateInit(VMnix_ConfigOptions *vmnixOptions);
extern void Serial_OpenPort(uint32 portNum);
extern Bool Serial_PutChar(unsigned char ch);
extern void Serial_PutString(const unsigned char *str);
extern void Serial_PutLenString(const unsigned char *str, int len);
extern unsigned char Serial_GetChar(void);
extern unsigned char Serial_PollChar(void);
extern VMKERNEL_ENTRY Serial_GetPort(DECLARE_1_ARG(VMK_GET_SERIAL_PORT, int32 *, port));
extern void Serial_Printf(const char *fmt, ...);
extern void Serial_PrintfVarArgs(const char *fmt, va_list args);

#define SERIAL_MIN_BAUD_RATE		9600
#define SERIAL_MAX_BAUD_RATE		115200

#endif


/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userTime.h --
 *
 *	UserWorld time
 */

#ifndef VMKERNEL_USER_USERTIME_H
#define VMKERNEL_USER_USERTIME_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

struct User_ThreadInfo;

typedef struct UserTime_ProfTimer {
   Timer_RelCycles remaining;
   Timer_RelCycles period;
} UserTime_ProfTimer;

typedef struct UserTime_CartelInfo {
   UserVA               pseudoTSCGet;
   UserVA               criticalSection;
   uint32               criticalSectionSize;
} UserTime_CartelInfo;

typedef struct UserTime_ThreadInfo {
   SP_SpinLock	lock;
   Timer_Handle realTimer;
   UserTime_ProfTimer virtualTimer;
   UserTime_ProfTimer profTimer;
} UserTime_ThreadInfo;

extern VMK_ReturnStatus UserTime_Init(void);
extern VMK_ReturnStatus UserTime_Cleanup(void);
extern VMK_ReturnStatus UserTime_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserTime_CartelCleanup(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserTime_ThreadInit(struct User_ThreadInfo* uti);
extern VMK_ReturnStatus UserTime_ThreadCleanup(struct User_ThreadInfo* uti);

extern VMK_ReturnStatus UserTime_SetITimer(LinuxITimerWhich which,
                                           const LinuxITimerVal* itv,
                                           LinuxITimerVal* oitv);
extern VMK_ReturnStatus UserTime_GetITimer(LinuxITimerWhich which,
                                           LinuxITimerVal* itv);

#endif /* VMKERNEL_USER_USERSIG_H */

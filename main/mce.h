/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mce.h -- 
 *
 *	Machine check exception definitions.
 */

#ifndef	_MCE_H
#define	_MCE_H


/*
 * MCE-related MSR constants (cf. Intel vol3,chap13)
 */

// MSR_MCG_CAP
#define MCG_CNT       0x000000FF
#define MCG_CTL_P     (1<<8)
#define MCG_EXT_P     (1<<9)
#define MCG_EXT_CNT   0x00FF0000

// MSR_MCG_STATUS
#define MCG_RIPV      (1<<0)
#define MCG_EIPV      (1<<1)
#define MCG_MCIP      (1<<2)

// MSR_MC0_STATUS
#define MC0_PCC       (1<<25)
#define MC0_ADDRV     (1<<26)
#define MC0_MISCV     (1<<27)
#define MC0_EN        (1<<28)
#define MC0_UC        (1<<29)
#define MC0_OVER      (1<<30)
#define MC0_VAL       (1<<31)


/*
 * MCE operations
 */

extern void MCE_Init(void);
extern void MCE_Handle_Exception(void);
extern void MCE_Exception(uint32 cs, uint32 eip, uint32 esp, uint32 ebp);

#endif

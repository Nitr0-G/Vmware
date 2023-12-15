/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * summit.h --
 *	This is the header file for the IBM Summit chipset.
 */


#ifndef _SUMMIT_H
#define _SUMMIT_H

#include "proc.h"


/*
 * IBM X440/Twister specific stuff 
 */
#define IBM_LOCAL_CYCLONE_MA               (0xfeb00000)
#define IBM_LOCAL_TWISTER_MA               (IBM_LOCAL_CYCLONE_MA + 0x80000)
#define IBM_TWISTER_OFFSET                 (0x00080000)
#define IBM_TWISTER_REG_SPACE              (0x0f000)
#define IBM_CYCLONE_OFFSET                 (0x00000000)
#define IBM_CYCLONE_PMC_OFFSET             (0x00005000)

/* IBM TWISTER register indexes.  Use with TwisterReg[] array. */
#define TWISTER_CBAR                       (0x6200 / 8)
#define TWISTER3_CBAR                      (0x6600 / 8)
#define TWISTER_MMIO0BASE0                 (0x6130 / 8)
#define TWISTER_MMIO1BASE0                 (0x6160 / 8)
#define TWISTER3_MMIO0BASE0                (0x6200 / 8)
#define TWISTER_ECID                       (0xc0a0 / 8)
#define TWISTER_NODECONFIG                 (0x61c8 / 8)
#define TWISTER3_NODECONFIG                (0x6608 / 8)
#define TWISTER_SCRATCH0                   (0xc1e0 / 8)
#define TWISTER_PMCC                       (0xe100 / 8)
#define TWISTER_PMCS                       (0xe108 / 8)
#define TWISTER_PMC0                       (0xe110 / 8)
#define TWISTER_PMC1                       (0xe118 / 8)
#define TWISTER_PMC2                       (0xe120 / 8)
#define TWISTER_PMC3                       (0xe128 / 8)
#define TWISTER_PMCS_QUAD                  (0x10f8 / 8)
#define TWISTER_PMCS_PQ                    (0x50f8 / 8)
#define TWISTER_PQ_PRICTL                  (0x5130 / 8)

#define TWISTER_ID_MASK                    (0x0000ffff)
#define TWISTER_ID                         (0x1031 & TWISTER_ID_MASK)
#define TWISTER3_ID                        (0x103a & TWISTER_ID_MASK)
#define TWISTER_BAD_ID                     (0xffffffff & TWISTER_ID_MASK)
#define TWISTER_VER_MASK                   (0x000f0000)
#define TWISTER_VER_SHIFT                  (16)
#define TWISTER_NODE_MASK                  (0x00000003)
#define TWISTER_PMC_ENABLE                 (0x0f)
#define TWISTER_PMC_MASK                   (CONST64U(0x00ffffffffff))
#define TWISTER_PMC_LATENCY                (1 << 24)

/* IBM Cyclone Jr. register indexes.
 * NB: because we only map the page with the counters, the offset is
 * 0x01a0 instead of 0x51a0.
 */
#define CYCLONE_PMCC                       (0x01a0 / 8)
#define CYCLONE_PMCS                       (0x01a8 / 8)
#define CYCLONE_MPMC0                      (0x01d0 / 8)

/* Compute register index given Summit_CounterGroup below */
#define TWISTER_PMCS_GROUP(g)              ((((g & 0x0f) << 12) + 0x0f8) / 8)

#define TWISTER_PMC_SHIFT(a, pc)           (a << (8 * pc))

/* Twister regs are 64-bits each and 64-bits apart */
typedef volatile uint64 TwisterReg;
typedef volatile uint64 CycloneReg;

/* Performance Counter Select */
typedef enum {
   PMCS_QUAD  = 0x01,
   PMCS_QT    = 0x02,
   PMCS_CD    = 0x03,
   PMCS_L3    = 0x04,
   PMCS_PQ    = 0x05,
   PMCS_RH    = 0x06,
   PMCS_SCP   = 0x07,
   PMCS_REG   = 0x09,
   PMCS_CYCLES = 0x0F
} Summit_CounterGroup;


/* IBM X440 chipsets */
typedef struct {
   Bool          present;
   uint32        ID;
   TwisterReg *  reg;

   Proc_Entry    procTwister;
   uint64        tsZeroed[4];     // timestamp at which each counter was zeroed
} IBM_Twister;

typedef struct {
   Bool          present;
   CycloneReg *  PMCreg;

   Proc_Entry    procCyclone;
   uint64        tsZeroed;     // timestamp at which each counter was zeroed
} IBM_Cyclone;


/*
 * Public Functions
 */

extern void Summit_EarlyInit(void);
extern Bool Summit_LocalInit(PCPU pcpuNum, Proc_Entry *parent);

// Streamlined function to return the low 32 bits of the cycles count
// in the Cyclone chipset.
extern INLINE uint32
Summit_GetCycloneCycles32(int node)
{
   extern CycloneReg * Summit_CycloneCyclesReg[];

   return *(uint32*) &Summit_CycloneCyclesReg[node][0];
}


#endif // _SUMMIT_H

/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxCDROM.h:
 *
 *    Linux-compatible CDROM support.
 */

#ifndef _VMKERNEL_USER_LINUX_CDROM_H
#define _VMKERNEL_USER_LINUX_CDROM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * CDROM ioctls (byte 0x53)
 */
#define LINUX_CDROMPAUSE              0x5301   /* Pause audio */
#define LINUX_CDROMRESUME             0x5302   /* Resume paused audio */
#define LINUX_CDROMPLAYMSF            0x5303   /* Play audio MSF */
#define LINUX_CDROMPLAYTRKIND         0x5304   /* Play audio track/index */
#define LINUX_CDROMREADTOCHDR         0x5305   /* Read TOC header */
#define LINUX_CDROMREADTOCENTRY       0x5306   /* Read TOC entry */
#define LINUX_CDROMSTOP               0x5307   /* Stop drive */
#define LINUX_CDROMSTART              0x5308   /* Start drive */
#define LINUX_CDROMEJECT              0x5309   /* Eject media */
#define LINUX_CDROMVOLCTRL            0x530a   /* Control volume */
#define LINUX_CDROMSUBCHNL            0x530b   /* Read subchannel data */
#define LINUX_CDROMREADMODE2          0x530c   /* Read mode 2 data */
#define LINUX_CDROMREADMODE1          0x530d   /* Read mode 1 data */
#define LINUX_CDROMREADAUDIO          0x530e   /* Read audio data */
#define LINUX_CDROMEJECT_SW           0x530f   /* enable=1/disable=0 auto-ejecting */
#define LINUX_CDROMMULTISESSION       0x5310   /* Obtain start of last session */
#define LINUX_CDROM_GET_MCN           0x5311   /* Obtain UPC */
#define LINUX_CDROMRESET              0x5312   /* Hard reset */
#define LINUX_CDROMVOLREAD            0x5313   /* Get the drive's volume setting */
#define LINUX_CDROMREADRAW            0x5314   /* Raw mode read data (2352 Bytes) */

#define LINUX_CDROMCLOSETRAY          0x5319   /* Close tray */
#define LINUX_CDROM_SET_OPTIONS       0x5320   /* Set options */
#define LINUX_CDROM_CLEAR_OPTIONS     0x5321   /* Clear options */
#define LINUX_CDROM_SELECT_SPEED      0x5322   /* Set speed */
#define LINUX_CDROM_SELECT_DISC       0x5323   /* Select disc */

#define LINUX_CDROM_MEDIA_CHANGED     0x5325   /* Check media changed */
#define LINUX_CDROM_DRIVE_STATUS      0x5326   /* Get tray position */
#define LINUX_CDROM_DISC_STATUS       0x5327   /* Get disc type */
#define LINUX_CDROM_CHANGER_NSLOTS    0x5328   /* Get number of slots */
#define LINUX_CDROM_LOCKDOOR          0x5329   /* Lock or unlock door */
#define LINUX_CDROM_DEBUG             0x5330   /* Turn debug messages on/off */
#define LINUX_CDROM_GET_CAPABILITY    0x5331   /* get capabilities */

#define LINUX_CDROM_SEND_PACKET       0x5393   /* Send a packet to drive */
#define LINUX_CDROM_NEXT_WRITABLE     0x5394   /* Get next writable block */
#define LINUX_CDROM_LAST_WRITTEN      0x5395   /* Get last block written */

/*
 * MSF (minute, second, frame) format address
 */
struct Linux_cdrom_msf0
{
   unsigned char minute;
   unsigned char second;
   unsigned char frame;
};

/*
 * MSF or logical format address
 */ 
union Linux_cdrom_addr
{
   struct Linux_cdrom_msf0 msf;
   int lba;
};

/*
 * LINUX_CDROMPLAYMSF ioctl
 */ 
struct Linux_cdrom_msf 
{
   unsigned char cdmsf_min0;
   unsigned char cdmsf_sec0;
   unsigned char cdmsf_frame0;
   unsigned char cdmsf_min1;
   unsigned char cdmsf_sec1;
   unsigned char cdmsf_frame1;
};

/*
 * LINUX_CDROMPLAYTRKIND ioctl
 */
struct Linux_cdrom_ti 
{
   unsigned char cdti_trk0;
   unsigned char cdti_ind0;
   unsigned char cdti_trk1;
   unsigned char cdti_ind1;
};

/*
 * LINUX_CDROMREADTOCHDR ioctl
 */
struct Linux_cdrom_tochdr
{
   unsigned char cdth_trk0;
   unsigned char cdth_trk1;
};

/*
 * LINUXVOLCTRL ioctl
 * LINUX_CDROMVOLREAD ioctl
 */
struct Linux_cdrom_volctrl
{
   unsigned char channel0;
   unsigned char channel1;
   unsigned char channel2;
   unsigned char channel3;
};

/* 
 * LINUX_CDROMSUBCHNL ioctl
 */
struct Linux_cdrom_subchnl 
{
   unsigned char cdsc_format;
   unsigned char cdsc_audiostatus;
   unsigned char cdsc_adr:4;
   unsigned char cdsc_ctrl:4;
   unsigned char cdsc_trk;
   unsigned char cdsc_ind;
   union Linux_cdrom_addr cdsc_absaddr;
   union Linux_cdrom_addr cdsc_reladdr;
};

/*
 * LINUX_CDROMREADTOCENTRY ioctl
 */
struct Linux_cdrom_tocentry
{
   unsigned char cdte_track;
   unsigned char cdte_adr:4;
   unsigned char cdte_ctrl:4;
   unsigned char cdte_format;
   union Linux_cdrom_addr cdte_addr;
   unsigned char cdte_datamode;
};

/*
 * LINUX_CDROMMULTISESSION ioctl
 */
struct Linux_cdrom_multisession
{
   union Linux_cdrom_addr addr;
   unsigned char xa_flag;
   unsigned char addr_format;
};

/* 
 * LINUX_CDROM_GET_MCN ioctl  
 */  
struct Linux_cdrom_mcn 
{
   unsigned char medium_catalog_number[14];
};

/* 
 * LINUX_CDROMPLAYBLK ioctl
 */
struct Linux_cdrom_blk 
{
   unsigned from;
   unsigned short len;
};

#endif // VMKERNEL_USER_LINUX_CDROM_H

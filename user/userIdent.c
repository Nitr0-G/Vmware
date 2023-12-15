/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "user_int.h"
#include "userIdent.h"

#define LOGLEVEL_MODULE UserIdent
#include "userLog.h"


/*
 *----------------------------------------------------------------------
 *
 * UserIdent_CheckAccessMode --
 *
 *      Check whether the given identity has permission for the
 *      specified access mode to an object with the given user, group,
 *      and mode bits.  Checking is against the effective uid and gid.
 *
 * Results:
 *      VMK_OK, VMK_NO_ACCESS, or VMK_NO_PERMISSION
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserIdent_CheckAccessMode(Identity *ident, uint32 accessMode,
                          LinuxUID objUID, LinuxGID objGID,
                          LinuxMode objMode)
{
   unsigned i;

   UWLOG(2, "euid=%u egid=%u ngids=%u, gids=(%u, %u, ...) "
         "accessMode=%u objUID=%u objGID=%u objMode=0%o)",
         ident->euid, ident->egid,
         ident->ngids, ident->gids[0], ident->gids[1],
         accessMode, objUID, objGID, objMode);

   if (ident->euid == 0) {
      return VMK_OK;
   }

   /*
    * The bits of accessMode match the corresponding low-order
    * (others) bits of objMode.  F_OK does not need to test any bits;
    * it is successful if the object exists at all, which is already
    * known to be true by this point.
    */
   ASSERT(LINUX_R_OK == LINUX_MODE_IROTH && LINUX_W_OK == LINUX_MODE_IWOTH &&
          LINUX_X_OK == LINUX_MODE_IXOTH && LINUX_F_OK == 0);

   /*
    * Determine whether to test accessMode against objMode's user,
    * group, or others mode bits.  Shift accessMode to align with the
    * appropriate set of objMode bits.
    */
   if (ident->euid == objUID) {
      // Test against user bits
      accessMode = accessMode << 6;
   } else if (ident->egid == objGID) {
      // Test against group bits
      accessMode = accessMode << 3;
   } else {
      for (i = 0; i < ident->ngids; i++) {
         if (ident->gids[i] == objGID) {
            // Test against group bits
            accessMode = accessMode << 3;
            break;
         }
      }
      // Otherwise test against others bits
   }
   return (accessMode & objMode) == accessMode ? VMK_OK : VMK_NO_ACCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * UserIdent_CheckAccess --
 *
 *      Check whether the given identity has permission for the
 *      specified openFlags to an object with the given user, group,
 *      and mode bits.  Checking is against the effective uid and gid.
 *
 * Results:
 *      VMK_OK, VMK_NO_ACCESS, or VMK_NO_PERMISSION
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserIdent_CheckAccess(Identity *ident, uint32 openFlags,
                      LinuxUID objUID, LinuxGID objGID,
                      LinuxMode objMode)
{
   uint32 accessMode;
   unsigned i;

   ASSERT(sizeof(LinuxUID) == sizeof(Identity_UserID));
   ASSERT(sizeof(LinuxGID) == sizeof(Identity_GroupID));

   UWLOG(1, "euid=%u egid=%u ngids=%u, gids=(%u, %u, ...) "
         "openFlags=%u objUID=%u objGID=%u objMode=0%o)",
         ident->euid, ident->egid,
         ident->ngids, ident->gids[0], ident->gids[1],
         openFlags, objUID, objGID, objMode);

   if (ident->euid == 0) {
      return VMK_OK;
   }

   /*
    * Switch on the type of access requested in openFlags.  We either
    * complete the access checking within the switch statement, or
    * translate the type of access into a bitmask over {LINUX_W_OK,
    * LINUX_R_OK, LINUX_X_OK} and call UserIdent_CheckAccessMode to
    * finish up.
    */
   switch (openFlags & USEROBJ_OPEN_FOR) {
   case USEROBJ_OPEN_RDONLY:
      accessMode = LINUX_R_OK;
      break;

   case USEROBJ_OPEN_WRONLY:
      accessMode = LINUX_W_OK;
      break;

   case USEROBJ_OPEN_RDWR:
      accessMode = LINUX_R_OK | LINUX_W_OK;
      break;

   case USEROBJ_OPEN_STAT:
      return VMK_OK;

   case USEROBJ_OPEN_SEARCH:
      accessMode = LINUX_X_OK;
      break;

   case USEROBJ_OPEN_OWNER:
      return ident->euid == objUID ? VMK_OK : VMK_NO_PERMISSION;

   case USEROBJ_OPEN_GROUP:
      if (ident->euid != objUID) {
         return VMK_NO_PERMISSION;
      }
      // Note: objGID is the proposed *new* gid in this case
      for (i = 0; i < ident->ngids; i++) {
         if (ident->gids[i] == objGID) {
            return VMK_OK;
         }
      }
      return VMK_NO_PERMISSION;

   default:
      ASSERT(FALSE);
      return VMK_NO_PERMISSION;
   }

   return UserIdent_CheckAccessMode(ident, accessMode, objUID, objGID, objMode);
}

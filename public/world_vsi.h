/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * world_vsi.h --
 *
 * Define sysinfo nodes for world related data.
 */

#ifndef _WORLD_VSI_H
#define _WORLD_VSI_H

VSI_DEF_TYPE(SI_WorldID, uint32, "world id: %d");
VSI_DEF_TYPE(SI_Pid, uint32, "process id: %d");

VSI_DEF_ARRAY(SI_DisplayName, char, 128);
VSI_DEF_ARRAY(SI_WorldName, char, 32);
VSI_DEF_ARRAY(SI_UUIDStr, char, 128);
VSI_DEF_ARRAY(SI_CfgPath, char, 1024);

VSI_DEF_STRUCT(WorldVsiInfo, "world info") {
   VSI_DEF_STRUCT_FIELD(SI_WorldID, worldID, "world id");
   VSI_DEF_STRUCT_FIELD(SI_Pid, pid, "process id");
   VSI_DEF_STRUCT_FIELD(SI_DisplayName, displayName, "display name");
   VSI_DEF_STRUCT_FIELD(SI_WorldName, name, "world name");
   VSI_DEF_STRUCT_FIELD(SI_UUIDStr, uuid, "uuid");
   VSI_DEF_STRUCT_FIELD(SI_CfgPath, cfgPath, "config file path");
};

VSI_DEF_STRUCT(WorldVsiGroupMember, "world group member") {
   VSI_DEF_STRUCT_FIELD(SI_WorldID, leaderID, "leader id");
   VSI_DEF_STRUCT_FIELD(SI_WorldName, name, "world name");
};

VSI_DEF_INST_BRANCH(world, root, World_VsiGetIDsList, "all vmkernel worlds");
VSI_DEF_LEAF(info, world, World_VsiGetInfo, VSI_NULL, WorldVsiInfo, "random world data");

// Bogus test function
VSI_DEF_INST_LEAF(groupMembers, world, World_VsiGetGroupList, World_VsiGetGroupMember, VSI_NULL, WorldVsiGroupMember, "group members");

#endif /* _WORLD_VSI_H */

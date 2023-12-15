/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * config_vsi.h --
 *
 * Define sysinfo nodes for all vmkernel config options.   The familar
 * two level structure from /proc is preserved.  Currently
 * all config options are non-instance nodes.  It is slightly
 * simpler this way, but there is no reason we couldn't just define
 * a single instance branch and a single instance leaf.
 *
 */

#ifndef _CONFIG_VSI_H
#define _CONFIG_VSI_H
#include "config_dist.h"

VSI_DEF_ARRAY(SI_ConfigStr32,  char, 32);
VSI_DEF_ARRAY(SI_ConfigStr128, char, 128);
VSI_DEF_ARRAY(SI_ConfigStr512, char, 512);

VSI_DEF_STRUCT(ConfigVsiData, "Vmkernel Config Option") {
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, def, "Default value: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, min, "Min value: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, max, "Max value: ");
   VSI_DEF_STRUCT_FIELD(VSI_DEC_U32, cur, "Current value: ");
   VSI_DEF_STRUCT_FIELD(SI_ConfigStr128, description, "Description of ");
};

VSI_DEF_STRUCT(ConfigVsiStrData, "Vmkernel Config Option [string]") {
   VSI_DEF_STRUCT_FIELD(SI_ConfigStr512, def, "Default value: ");
   VSI_DEF_STRUCT_FIELD(SI_ConfigStr32, valid, "valid characters: ");
   VSI_DEF_STRUCT_FIELD(SI_ConfigStr512, cur, "Current value: ");
   VSI_DEF_STRUCT_FIELD(SI_ConfigStr128, description, "Description of ");
};

VSI_DEF_BRANCH(Config, root, "Vmkernel config options");

#ifndef VSIDEF_PARSER
/*
 * The VSI_NEWLINE_HACK stuff is used to make sure the post preprocessor output
 * has linebreaks.   This makes line number errors from vsiParser useful. 
 */
#define VSI_NEWLINE_HACK
#endif

/*
 * Generate a branch for each config module.
 */
#undef CONFIG_DEFMODULE
#define CONFIG_DEFMODULE(_module, _moduleName) \
   VSI_DEF_BRANCH(CFG_MODULE_##_moduleName, Config, XSTR(_moduleName)" subsystem options"); VSI_NEWLINE_HACK
CONFIG_MODULES_LIST


/*
 * Generate a leaf node for each integer option.
 */
#undef D
#undef S
#undef CONFIG_DEFMODULE
#define CONFIG_DEFMODULE(_module, _moduleName) \
   CONFIG_##_module##_OPTS(CFG_MODULE_##_moduleName, _moduleName)
#define D(_module, _moduleName, _macro, _name, _min, _max, _def, _help, _ignore...) \
   VSI_DEF_LEAF(CFG_##_moduleName##_name, _module, Config_SysInfoGetInt, Config_SysInfoSetInt, \
               ConfigVsiData, _help); VSI_NEWLINE_HACK
#define S(unused...) 
CONFIG_MODULES_LIST

/*
 * Special node for mapping vsi node ids into config options
 */
VSI_DEF_BRANCH(CFG_LastIntegerNode, Config, "Last integer node placeholder");

/*
 * Generate  a leaf node for each string option.
 */
#undef D
#undef S
#undef CONFIG_DEFMODULE
#define CONFIG_DEFMODULE(_module, _moduleName) \
   CONFIG_##_module##_OPTS(CFG_MODULE_##_moduleName, _moduleName)
#define S(_module, _moduleName, _macro, _name, _def, _valid, _help, _ignore...) \
   VSI_DEF_LEAF(CFG_##_moduleName##_name, _module, Config_SysInfoGetStr, Config_SysInfoSetStr, \
               ConfigVsiStrData, _help); VSI_NEWLINE_HACK
#define D(unused...) 
CONFIG_MODULES_LIST
#undef D
#undef S

#endif /* _CONFIG_VSI_H */

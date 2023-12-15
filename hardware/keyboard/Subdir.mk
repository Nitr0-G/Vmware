###
### Makefile component for the VMKernel keyboard sub-system
###
GLOBAL_DEFS += -DVMK_KBD
SUBDIR_FILES = kbd.c vmk_impl.c atkbd.c atkbdc.c

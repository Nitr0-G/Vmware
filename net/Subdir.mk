SUBDIR_FILES = vmkernel_exports.c vmm_vmkcalls.c host_vmkcalls.c \
               vmklinux_exports.c pkt.c port.c portset.c uplink.c \
               iochain.c bond.c proc_net.c eth.c \
               netDebug.c netARP.c \
               nulldev.c loopback.c hub.c \
	       vmxnet2_vmkdev.c vlance_vmkdev.c cos_vmkdev.c 

ifdef ESX2_NET_SUPPORT
   SUBDIR_FILES += legacy_esx2.c
endif

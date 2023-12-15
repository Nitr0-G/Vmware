SUBDIR_FILES = action.c alloc.c async_io.c bh.c common.S \
	       bluescreen.c debug.c eventhisto.c \
	       host.c hostAsm.S idt.c init.c \
	       kvmap.c libc.c log.c vmktag.c\
	       memalloc.c memmap.c migrateBridge.c mod_loader.c mpage.c\
	       prda.c rpc.c event.c statusterm.c \
	       semaphore.c serial.c splock.c term.c logterm.c debugterm.c \
	       timer.c thermmon.c trace.c util.c vsprintf.c world.c sharedArea.c \
	       kseg.c tlb.c config.c proc.c vmkstats.c \
               nmi.c helper.c post.c \
	       parse.c dump.c it.c hash.c pshare.c swap.c pagetable.c \
	       return_status.c mce.c vmkperf.c watchpoint.c vmkevent.c \
	       worldswitch.S \
	       testworlds.c coverage.c histogram.c vmmstats.c \
	       vmkstress.c mtrr.c xmap.c buddy.c vmksysinfo.c \
               dlmalloc.c heap.c softirq.c identity.c conduit_bridge.c \
	       infiniband.c \
	       cosdump.c debugAsm.S heapMgr.c \
               vmksysinfostub.c

SUBDIR_GENERATED_FILES = asmdefn.sinc

ASFLAGS += $(DIR_DEFS)
INCLUDE += -I$(SUBDIR)
INCLUDE += -I$(SRCROOT)/lib/shared
INCLUDE += -I$(SUBDIR)/../../lib/vmksysinfo

$(SUBDIR)/asmdefn.sinc:	$(SUBDIR)/genasmdefn
	@echo "** Generating       $@"
	./$< --output $@

$(SRCROOT)/$(SUBDIR)/worldswitch.S:	$(SUBDIR)/asmdefn.sinc
$(SRCROOT)/$(SUBDIR)/debugAsm.S:	$(SUBDIR)/asmdefn.sinc
$(SRCROOT)/$(SUBDIR)/hostAsm.S:		$(SUBDIR)/asmdefn.sinc
$(SRCROOT)/$(SUBDIR)/common.S:		$(SUBDIR)/asmdefn.sinc

$(SUBDIR)/genasmdefn:	$(SRCROOT)/$(SUBDIR)/genasmdefn.c $(SUBDIR)/genasmdefn.d
	@echo "** Building         $@"
	$(CC) -o $@ $(LD_OPTS) -I$(SRCROOT)/vmkernel/sched -I $(KROOTLIBC)/usr/include \
	$(filter-out -mregparm=%,$(CC_OPTS)) $(INCLUDE) $(LD_LIBS) $<

$(SUBDIR)/genasmdefn.d:	$(SRCROOT)/$(SUBDIR)/genasmdefn.c
	@echo '** Dependencies for $(SUBDIR)/$(<F)'
	$(CC) -I $(KROOTLIBC)/usr/include \
	   -MM $(filter-out -mregparm=%,$(filter-out -g3,$(CC_OPTS))) \
	   $(INCLUDE) $< | \
		$(SED) '1s,[^:]*:,$(@:.d=) $@:,' > $@
	[ -s $@ ] || (rm -f $@; false)

-include $(SUBDIR)/genasmdefn.d

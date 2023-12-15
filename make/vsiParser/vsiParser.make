BUILDDIR = $(BUILDROOT)/$(OBJDIR)/server/vmkernel/make/vsiParser
THISMAKEFILE = $(SRCROOT)/vmkernel/make/vsiParser/vsiParser.make
include $(MAKEFILEDIR)/defs.mk

include $(MAKEFILEDIR)/default.mk

INCLUDE = -I$(SRCROOT)/public -I$(SRCROOT)/vmkernel/public -I$(SRCROOT)/lib/public

build: $(BUILDDIR)/lex.yy.c $(BUILDDIR)/vsiParser.tab.c
	$(CC) $(INCLUDE) $(BUILDDIR)/lex.yy.c $(BUILDDIR)/vsiParser.tab.c -o $(BUILDDIR)/vsiParser

$(BUILDDIR)/vsiParser.tab.c: $(SRCROOT)/vmkernel/make/vsiParser/vsiParser.y
	$(BISON) -v -d -o $@ $(SRCROOT)/vmkernel/make/vsiParser/vsiParser.y

$(BUILDDIR)/lex.yy.c: $(SRCROOT)/vmkernel/make/vsiParser/vsiParser.l
	$(FLEX) -o$@ $(SRCROOT)/vmkernel/make/vsiParser/vsiParser.l


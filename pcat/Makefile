#
#	Makefile (es1371snd) — GNU make
#
#	Ensoniq ES1371 / Creative AudioPCI sound driver for B-right/V (超漢字)
#	on VMware.
#
#	Portions Copyright (C) 2004 Yurio Miyazawa, GPL-2.0: this Makefile is
#	derived from the intelsounddrv driver's pcat/Makefile.
#
#	Build (in the brightv driver tree):
#	  place this driver at  brightv/driver/es1371snd/
#	  then:  cd brightv/driver/es1371snd/pcat && make
#	  debug build:  make mode=debug
#	  install:      make install
#

DEPS = Dependencies
DEPENDENCIES_OUTPUT := $(DEPS)

# device-driver standard build rules (brightv kerext toolchain)
include ../../etc/makerules

#----------------------------------------------------------------------
TARGET = es1371snd

S = ../src
VPATH = $(S)
HEADER := $(S) $(HEADER)

SRC = main.c

OBJ    = $(addsuffix .o, $(basename $(SRC)))
WC_SRC = $(filter %.C, $(SRC))

ifneq ($(mode), debug)
    CFLAGS += -Wall
endif
CFLAGS += -DDEBUG

#----------------------------------------------------------------------
.PHONY: all clean install

ALL = $(TARGET) $(TARGET).map

all: $(ALL)

$(TARGET): $(OBJ)
	$(LINK.o) $(LDOBJS) $^ $(LOADLIBES) $(LDLIBS) $(OUTPUT_OPTION)

clean:
	$(RM) $(OBJ) $(WC_SRC:%.C=%.c) $(ALL) $(DEPS)

install: $(ALL:%=$(EXE_INSTALLDIR)/%)

$(TARGET:%=$(EXE_INSTALLDIR)/%): $(EXE_INSTALLDIR)/%: %
	$(EXE_INSTALL_BZCOMP)

ifdef DEPENDENCIES_OUTPUT
  $(DEPS): ; touch $(DEPS)
else
  $(DEPS): $(SRC) ; $(MAKEDEPS) $@ $?
endif
include $(DEPS)

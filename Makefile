EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o
EE_LIBS = -ldebug -lpad -lmc -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

# Briscoe uses the open-source PS2SDK SECRMAN + SECRSIF pair.  These are
# embedded so the diagnostic ELF remains self-contained and does not depend on
# a particular FMCB installer package or on external IRX files.
IRX_FILES = secrman.irx secrsif.irx
EE_OBJS += $(IRX_FILES:.irx=_irx.o)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

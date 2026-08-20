EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o
EE_LIBS = -ldebug -lpad -lmc -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
EE_OBJS += $(IRX_FILES:.irx=_irx.o)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

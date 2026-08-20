EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o freesio2_irx.o freepad_irx.o mcman_irx.o mcserv_irx.o
EE_LIBS = -ldebug -lpad -lmc -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

clean-extra:
	rm -f $(IRX_FILES:.irx=_irx.c)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

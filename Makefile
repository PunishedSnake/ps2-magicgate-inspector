EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o
EE_LIBS = -ldebug -lpad -lmc -lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

# Briscoe embeds one coherent PS2SDK 2.0 IOP stack.  SECRMAN is placed into a
# tiny runtime-generated IOPRP and becomes resident during the IOP reboot;
# afterwards freesio2/freepad/XMCMAN/XMCSERV/SECRSIF are loaded from the same
# PS2SDK build so XMCMAN can register its MagicGate callbacks with SECRMAN.
# The generated IOPRP itself is never shipped as a borrowed binary blob.
IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx secrman.irx secrsif.irx
EE_OBJS += $(IRX_FILES:.irx=_irx.o)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

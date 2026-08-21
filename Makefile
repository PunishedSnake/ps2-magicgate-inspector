EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o
EE_LIBS = -ldebug -lpad -lmc -lfileXio -lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

# Briscoe dev4 keeps the real-hardware-validated Sony ROM X stack for ordinary
# memory-card I/O and also uses ROM XSIO2MAN/XMCMAN/XMCSERV inside the isolated
# MagicGate session. Only the special open-source SECRMAN/SECRSIF pair needs to
# be embedded for security diagnostics.
#
# The optional FMCB package preflight embeds iomanX/fileXio/USBD/USBHDFSD from
# PS2SDK 2.0 for read-only access to user-supplied mass:/FMCB files.
IRX_FILES = secrman.irx secrsif.irx iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx
EE_OBJS += $(IRX_FILES:.irx=_irx.o)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

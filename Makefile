.DEFAULT_GOAL := MC_INSPECTOR.ELF

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o src/dev10_nomcserv.o
EE_LIBS = -ldebug -lpad -lmc -lfileXio -lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections \
	-Wl,--wrap=SifExecModuleBuffer \
	-Wl,--wrap=mcInit \
	-Wl,--wrap=mcGetInfo \
	-Wl,--wrap=mcSync

# Briscoe dev10 keeps the hardware-validated Sony ROM X stack for ordinary
# Inspector work. The isolated MagicGate session still uses PS2SDK v1-era
# freesio2/freepad/mcman plus the pinned FMCB SECRMAN/SECRSIF export-1.3 pair,
# but deliberately skips the temporary MCSERV v1 RPC server.
#
# Real hardware showed MCSERV v1 returning RESIDENT_END and the immediately
# following LOADFILE RPC never returning. dev10 therefore isolates the IOP-side
# MCMAN -> SECRMAN card-command callback path from the old EE libmc/MCSERV path.
# A tiny linker-wrap shim fakes only the isolated mcInit/mcGetInfo check; normal
# pre-probe and restored ROM X operation still use the real libmc implementation.
#
# CI stages the v1.0 IOP modules before invoking this Makefile. The EE program
# itself remains built with PS2DEV v2.0.
FMCB_SECR_COMMIT = ac53a47a5c6eae675cc2611c7bebe62f56c7845c
FMCB_SECR_BASE = https://raw.githubusercontent.com/israpps/FreeMcBoot-Installer/$(FMCB_SECR_COMMIT)/installer/irx/compiled
FMCB_SECR_DIR = .build/fmcb-secr-1.3
FMCB_SECRMAN = $(FMCB_SECR_DIR)/secrman.irx
FMCB_SECRSIF = $(FMCB_SECR_DIR)/secrsif.irx

FMCB_COMPAT_DIR ?= .build/fmcb-ps2sdk-v1
FMCB_COMPAT_IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
FMCB_COMPAT_OBJS = $(addprefix fmcb_,$(FMCB_COMPAT_IRX_FILES:.irx=_irx.o))

PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx
EE_OBJS += secrman_irx.o secrsif_irx.o $(FMCB_COMPAT_OBJS) $(PS2SDK_IRX_FILES:.irx=_irx.o)

$(FMCB_SECR_DIR):
	mkdir -p $@

$(FMCB_SECRMAN): | $(FMCB_SECR_DIR)
	@echo "Fetching pinned FMCB SECRMAN compatibility module..."
	wget -q -O $@ $(FMCB_SECR_BASE)/secrman.irx
	@test "$$(wc -c < $@)" -eq 15533 || { echo "Unexpected secrman.irx size"; rm -f $@; exit 1; }
	@sha256sum $@

$(FMCB_SECRSIF): | $(FMCB_SECR_DIR)
	@echo "Fetching pinned FMCB SECRSIF compatibility bridge..."
	wget -q -O $@ $(FMCB_SECR_BASE)/secrsif.irx
	@test "$$(wc -c < $@)" -eq 4685 || { echo "Unexpected secrsif.irx size"; rm -f $@; exit 1; }
	@sha256sum $@

secrman_irx.c: $(FMCB_SECRMAN)
	$(PS2SDK)/bin/bin2c $< $@ secrman_irx

secrsif_irx.c: $(FMCB_SECRSIF)
	$(PS2SDK)/bin/bin2c $< $@ secrsif_irx

fmcb_%_irx.c: $(FMCB_COMPAT_DIR)/%.irx
	@test -f $< || { echo "Missing FMCB compatibility IRX: $<"; echo "Stage PS2DEV v1.0 IOP modules before building."; exit 1; }
	$(PS2SDK)/bin/bin2c $< $@ fmcb_$*_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

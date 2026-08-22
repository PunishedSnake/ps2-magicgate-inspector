.DEFAULT_GOAL := MC_INSPECTOR.ELF

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o src/dev10_nomcserv.o src/dev12_diag.o
EE_LIBS = -ldebug -lpad -lmc -lfileXio -lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections \
	-Wl,--wrap=SifExecModuleBuffer \
	-Wl,--wrap=mcInit \
	-Wl,--wrap=mcGetInfo \
	-Wl,--wrap=mcSync \
	-Wl,--wrap=sceSifBindRpc \
	-Wl,--wrap=sceSifCallRpc \
	-Wl,--wrap=MagicGateResultText \
	-Wl,--wrap=MagicGateStageText

# Briscoe dev12 keeps the hardware-validated Sony ROM X stack for ordinary
# Inspector work and the dev10 no-MCSERV isolated MagicGate personality.
# The isolated session now uses a source-built, instrumented copy of the exact
# FMCB SECRMAN 1.3 code. Successful behavior is unchanged; on GET_KBIT failure
# only, SECRMAN returns a compact diagnostic record through the existing Kbit
# buffer, so the failing Mechacon/CardAuth stage is observed in-path rather than
# replayed afterward.
FMCB_SECR_COMMIT = ac53a47a5c6eae675cc2611c7bebe62f56c7845c
FMCB_SECR_BASE = https://raw.githubusercontent.com/israpps/FreeMcBoot-Installer/$(FMCB_SECR_COMMIT)/installer/irx/compiled
FMCB_SECRSIF_DIR = .build/fmcb-secr-1.3
FMCB_SECRMAN ?= .build/fmcb-secr-dev12/secrman.irx
FMCB_SECRSIF = $(FMCB_SECRSIF_DIR)/secrsif.irx

FMCB_COMPAT_DIR ?= .build/fmcb-ps2sdk-v1
FMCB_COMPAT_IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
FMCB_COMPAT_OBJS = $(addprefix fmcb_,$(FMCB_COMPAT_IRX_FILES:.irx=_irx.o))

PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx
EE_OBJS += secrman_irx.o secrsif_irx.o $(FMCB_COMPAT_OBJS) $(PS2SDK_IRX_FILES:.irx=_irx.o)

$(FMCB_SECRSIF_DIR):
	mkdir -p $@

$(FMCB_SECRMAN):
	@test -f $@ || { echo "Missing instrumented dev12 SECRMAN: $@"; echo "Stage it with the PS2DEV v1.0 CI step before building."; exit 1; }

$(FMCB_SECRSIF): | $(FMCB_SECRSIF_DIR)
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

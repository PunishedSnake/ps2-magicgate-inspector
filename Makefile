.DEFAULT_GOAL := $(EE_BIN)

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o
EE_LIBS = -ldebug -lpad -lmc -lfileXio -lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

# Briscoe dev6 keeps the real-hardware-validated Sony ROM X stack for ordinary
# memory-card I/O. The isolated MagicGate session deliberately uses the exact
# open-source SECRMAN/SECRSIF compatibility pair from FreeMcBoot Installer
# rather than PS2SDK 2.0's newer secrman ABI.
#
# Real hardware showed that this SECR 1.3 pair changes the first isolated
# mcGetInfo result from sceMcResFailResetAuth (-11) to sceMcResChangedCard (-1)
# while returning valid PS2-card metadata. dev6 therefore treats CHANGED CARD
# as a transient state notification and retries instead of aborting the probe.
# This comment also marks the first CI build containing that retry semantics.
FMCB_SECR_COMMIT = ac53a47a5c6eae675cc2611c7bebe62f56c7845c
FMCB_SECR_BASE = https://raw.githubusercontent.com/israpps/FreeMcBoot-Installer/$(FMCB_SECR_COMMIT)/installer/irx/compiled
FMCB_SECR_DIR = .build/fmcb-secr-1.3
FMCB_SECRMAN = $(FMCB_SECR_DIR)/secrman.irx
FMCB_SECRSIF = $(FMCB_SECR_DIR)/secrsif.irx

PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx
EE_OBJS += secrman_irx.o secrsif_irx.o $(PS2SDK_IRX_FILES:.irx=_irx.o)

$(FMCB_SECR_DIR):
	mkdir -p $@

$(FMCB_SECRMAN): | $(FMCB_SECR_DIR)
	@echo "Fetching pinned FMCB SECRMAN 1.3 compatibility module..."
	wget -q -O $@ $(FMCB_SECR_BASE)/secrman.irx
	@test "$$(wc -c < $@)" -eq 15533 || { echo "Unexpected secrman.irx size"; rm -f $@; exit 1; }
	@sha256sum $@

$(FMCB_SECRSIF): | $(FMCB_SECR_DIR)
	@echo "Fetching pinned FMCB SECRSIF 1.3 compatibility bridge..."
	wget -q -O $@ $(FMCB_SECR_BASE)/secrsif.irx
	@test "$$(wc -c < $@)" -eq 4685 || { echo "Unexpected secrsif.irx size"; rm -f $@; exit 1; }
	@sha256sum $@

secrman_irx.c: $(FMCB_SECRMAN)
	$(PS2SDK)/bin/bin2c $< $@ secrman_irx

secrsif_irx.c: $(FMCB_SECRSIF)
	$(PS2SDK)/bin/bin2c $< $@ secrsif_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

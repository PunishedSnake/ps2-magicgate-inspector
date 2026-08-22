.DEFAULT_GOAL := MC_INSPECTOR.ELF

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o src/card.o src/magicgate.o src/fmcb_install.o \
	src/magicgate_session.o src/magicgate_diag.o
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

# MagicGate backend profiles.
#
# fmcb13 is the hardware-validated baseline: FMCB SECRMAN/SECRSIF 1.3 with the
# PS2SDK-v1 card stack.
#
# ps2sdk14 is the controlled comparison profile: source-built PS2SDK 2.0
# SECRMAN 1.4 + SECRSIF and the matching PS2SDK 2.0 card stack. Both profiles
# use the same EE-side physical-port correction and the same failure-record
# format, so hardware results can be compared directly.
SECR_PROFILE ?= fmcb13

MG_CARD_IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
MG_CARD_OBJS = $(addprefix fmcb_,$(MG_CARD_IRX_FILES:.irx=_irx.o))

ifeq ($(SECR_PROFILE),fmcb13)
MG_CARD_DIR ?= .build/fmcb-ps2sdk-v1
MG_SECRMAN ?= .build/fmcb-secr-1.3-diag/secrman.irx
MG_SECRSIF_DIR = .build/fmcb-secr-1.3
MG_SECRSIF = $(MG_SECRSIF_DIR)/secrsif.irx
EE_CFLAGS += -DMG_SECR_PROFILE_FMCB13=1

FMCB_SECR_COMMIT = ac53a47a5c6eae675cc2611c7bebe62f56c7845c
FMCB_SECR_BASE = https://raw.githubusercontent.com/israpps/FreeMcBoot-Installer/$(FMCB_SECR_COMMIT)/installer/irx/compiled

$(MG_SECRSIF_DIR):
	mkdir -p $@

$(MG_SECRSIF): | $(MG_SECRSIF_DIR)
	@echo "Fetching pinned FMCB SECRSIF 1.3 compatibility bridge..."
	wget -q -O $@ $(FMCB_SECR_BASE)/secrsif.irx
	@test "$$(wc -c < $@)" -eq 4685 || { echo "Unexpected secrsif.irx size"; rm -f $@; exit 1; }
	@sha256sum $@

else ifeq ($(SECR_PROFILE),ps2sdk14)
MG_CARD_DIR ?= .build/ps2sdk2-mg
MG_SECRMAN ?= .build/ps2sdk2-secr14/secrman.irx
MG_SECRSIF ?= .build/ps2sdk2-secr14/secrsif.irx
EE_CFLAGS += -DMG_SECR_PROFILE_PS2SDK14=1

$(MG_SECRSIF):
	@test -f $@ || { echo "Missing PS2SDK 2.0 SECRSIF: $@"; echo "Stage the ps2sdk14 profile before building."; exit 1; }

else
$(error Unknown SECR_PROFILE '$(SECR_PROFILE)'; use fmcb13 or ps2sdk14)
endif

$(MG_SECRMAN):
	@test -f $@ || { echo "Missing SECRMAN for profile $(SECR_PROFILE): $@"; echo "Stage the selected MagicGate profile before building."; exit 1; }

PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx
EE_OBJS += secrman_irx.o secrsif_irx.o $(MG_CARD_OBJS) $(PS2SDK_IRX_FILES:.irx=_irx.o)

secrman_irx.c: $(MG_SECRMAN)
	$(PS2SDK)/bin/bin2c $< $@ secrman_irx

secrsif_irx.c: $(MG_SECRSIF)
	$(PS2SDK)/bin/bin2c $< $@ secrsif_irx

fmcb_%_irx.c: $(MG_CARD_DIR)/%.irx
	@test -f $< || { echo "Missing MagicGate card-stack IRX: $<"; echo "Stage profile $(SECR_PROFILE) before building."; exit 1; }
	$(PS2SDK)/bin/bin2c $< $@ fmcb_$*_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

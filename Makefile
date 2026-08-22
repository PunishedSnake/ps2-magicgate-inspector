.DEFAULT_GOAL := MC_INSPECTOR.ELF

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/app_main.o src/gui.o src/progress.o src/card.o src/magicgate.o src/fmcb_install.o \
	src/fmcb_transaction.o src/console_profile.o src/magicgate_session.o src/magicgate_diag.o src/video_mode.o \
	src/ui_layout.o src/settings.o
EE_LIBS = -ldebug -ldraw -lgraph -lpacket -ldma -lpad -lmc -lfileXio -lcdvd -lsecr \
	-lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections \
	-DMG_SECR_PROFILE_PS2SDK14=1
EE_LDFLAGS = -Wl,--gc-sections \
	-Wl,--wrap=SifExecModuleBuffer \
	-Wl,--wrap=mcInit \
	-Wl,--wrap=mcGetInfo \
	-Wl,--wrap=mcSync \
	-Wl,--wrap=sceSifBindRpc \
	-Wl,--wrap=sceSifCallRpc \
	-Wl,--wrap=MagicGateResultText \
	-Wl,--wrap=MagicGateStageText

# 0.4.x keeps the single hardware-validated production security backend from
# Briscoe: PS2SDK 2.0 SECRMAN 1.4, matching SECRSIF and the matching PS2SDK 2.0
# SIO2/PAD/MCMAN generation. Display/settings and installer work do not weaken
# the separation between ordinary ROM X filesystem I/O and the SECR session.
MG_CARD_DIR ?= .build/ps2sdk2-mg
MG_SECR_DIR ?= .build/ps2sdk2-secr14
MG_SECRMAN ?= $(MG_SECR_DIR)/secrman.irx
MG_SECRSIF ?= $(MG_SECR_DIR)/secrsif.irx

MG_CARD_IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
MG_CARD_OBJS = $(addprefix fmcb_,$(MG_CARD_IRX_FILES:.irx=_irx.o))
PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx

EE_OBJS += secrman_irx.o secrsif_irx.o $(MG_CARD_OBJS) $(PS2SDK_IRX_FILES:.irx=_irx.o)

$(MG_SECRMAN) $(MG_SECRSIF):
	@test -f $@ || { \
		echo "Missing staged PS2SDK 2.0 security module: $@"; \
		echo "Run the CI staging step or stage the pinned PS2SDK 2.0 modules locally."; \
		exit 1; \
	}

secrman_irx.c: $(MG_SECRMAN)
	$(PS2SDK)/bin/bin2c $< $@ secrman_irx

secrsif_irx.c: $(MG_SECRSIF)
	$(PS2SDK)/bin/bin2c $< $@ secrsif_irx

# The fmcb_* generated symbol prefix is retained for source compatibility with
# the Briscoe runtime. The embedded files are PS2SDK 2.0 modules.
fmcb_%_irx.c: $(MG_CARD_DIR)/%.irx
	@test -f $< || { echo "Missing staged MagicGate card-stack IRX: $<"; exit 1; }
	$(PS2SDK)/bin/bin2c $< $@ fmcb_$*_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

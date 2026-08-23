.DEFAULT_GOAL := MC_INSPECTOR.ELF

EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/app_main.o src/gui.o src/progress.o src/diag_log.o src/diag_wrap.o src/card.o src/magicgate.o src/fmcb_install.o \
	src/usb_search.o src/fmcb_transaction.o src/fmcb_recovery.o src/fmcb_recovery_marker.o src/console_profile.o \
	src/magicgate_session.o src/magicgate_diag.o src/video_mode.o src/ui_layout.o src/settings.o \
	src/kelf_cache.o src/card_raw_session.o src/card_image.o src/card_image_fs.o
EE_LIBS = -ldebug -ldraw -lgraph -lpacket -ldma -lpad -lmc -lfileXio -lcdvd -lsecr \
	-lioprpgen -liopreboot -lpatches -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections \
	-DMG_SECR_PROFILE_PS2SDK14=1
EE_LDFLAGS = -Wl,--gc-sections \
	-Wl,--wrap=SifExecModuleBuffer \
	-Wl,--wrap=mcInit \
	-Wl,--wrap=mcGetInfo \
	-Wl,--wrap=mcSync \
	-Wl,--wrap=fileXioInit \
	-Wl,--wrap=fileXioExit \
	-Wl,--wrap=fileXioClose \
	-Wl,--wrap=MciRawCardSessionStart \
	-Wl,--wrap=MciRawCardSessionStop \
	-Wl,--wrap=MciCardImageProbeGeometry \
	-Wl,--wrap=MciCardImageExport \
	-Wl,--wrap=MciCardImageVerifyFile \
	-Wl,--wrap=MciCardImageRestoreExact \
	-Wl,--wrap=MciCardForceFormatWithBackup \
	-Wl,--wrap=FmcbInstallNormalTransactional \
	-Wl,--wrap=sceSifBindRpc \
	-Wl,--wrap=sceSifCallRpc \
	-Wl,--wrap=MagicGateResultText \
	-Wl,--wrap=MagicGateStageText \
	-Wl,--wrap=FmcbRecoveryProbe \
	-Wl,--wrap=FmcbRecoveryBegin \
	-Wl,--wrap=FmcbRecoveryRun \
	-Wl,--wrap=FmcbRecoveryFinish

# 0.4.x keeps the hardware-validated Briscoe security backend: PS2SDK 2.0
# SECRMAN 1.4 plus the normal matching X-style card generation. Raw page RPCs
# are a different interface contract. Modern PS2SDK's default mcserv is built
# with BUILDING_XMCSERV=1, so libmc identifies it as XMC and deliberately blocks
# mcReadPage/mcWritePage/mcEraseBlock. Drebin therefore embeds a second, pinned
# legacy MCMAN/MCSERV pair built with the XMC compatibility switches disabled.
MG_CARD_DIR ?= .build/ps2sdk2-mg
MG_SECR_DIR ?= .build/ps2sdk2-secr14
RAW_CARD_DIR ?= .build/ps2sdk2-raw
MG_SECRMAN ?= $(MG_SECR_DIR)/secrman.irx
MG_SECRSIF ?= $(MG_SECR_DIR)/secrsif.irx
RAW_MCMAN ?= $(RAW_CARD_DIR)/mcman.irx
RAW_MCSERV ?= $(RAW_CARD_DIR)/mcserv.irx

MG_CARD_IRX_FILES = freesio2.irx freepad.irx mcman.irx mcserv.irx
MG_CARD_OBJS = $(addprefix fmcb_,$(MG_CARD_IRX_FILES:.irx=_irx.o))
RAW_CARD_OBJS = raw_mcman_irx.o raw_mcserv_irx.o
PS2SDK_IRX_FILES = iomanX.irx fileXio.irx usbd.irx usbhdfsd.irx

EE_OBJS += secrman_irx.o secrsif_irx.o $(MG_CARD_OBJS) $(RAW_CARD_OBJS) $(PS2SDK_IRX_FILES:.irx=_irx.o)

$(MG_SECRMAN) $(MG_SECRSIF):
	@test -f $@ || { \
		echo "Missing staged PS2SDK 2.0 security module: $@"; \
		echo "Run the CI staging step or stage the pinned PS2SDK 2.0 modules locally."; \
		exit 1; \
	}

$(RAW_MCMAN) $(RAW_MCSERV):
	@test -f $@ || { \
		echo "Missing staged legacy raw-card module: $@"; \
		echo "Build PS2SDK MCMAN/MCSERV with XMC compatibility disabled."; \
		exit 1; \
	}

secrman_irx.c: $(MG_SECRMAN)
	$(PS2SDK)/bin/bin2c $< $@ secrman_irx

secrsif_irx.c: $(MG_SECRSIF)
	$(PS2SDK)/bin/bin2c $< $@ secrsif_irx

raw_mcman_irx.c: $(RAW_MCMAN)
	$(PS2SDK)/bin/bin2c $< $@ raw_mcman_irx

raw_mcserv_irx.c: $(RAW_MCSERV)
	$(PS2SDK)/bin/bin2c $< $@ raw_mcserv_irx

# The fmcb_* generated symbol prefix is retained for source compatibility with
# the Briscoe/MagicGate runtime. These are the normal PS2SDK 2.0 modules and are
# not the legacy raw-page pair above.
fmcb_%_irx.c: $(MG_CARD_DIR)/%.irx
	@test -f $< || { echo "Missing staged MagicGate card-stack IRX: $<"; exit 1; }
	$(PS2SDK)/bin/bin2c $< $@ fmcb_$*_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

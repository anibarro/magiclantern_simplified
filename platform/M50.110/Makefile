MODEL = M50
FW_VERSION = 110

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR  = CCF19110.FIR
#UPDATE_NAME_FIR = D_M50110.FIR
FIRMWARE_ID     = 0x00000412
MAIN_FIRMWARE_ADDR = 0xE0040000

# user_mem_start from cstart struct. Align to 0x10 up
RESTARTSTART = 0xdc870

# Cortex A9, binaries loaded as Thumb
CFLAGS_PLATFORM = -mthumb -mlong-calls
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o

# XPan: panoramic 65:24 photo mode (README-XPAN.md, README-XPAN-M50.md).
# Physical camera only. Enable with:  make XPAN=y CONFIG_QEMU=n
# Phase 1 of the M50 port: framing, HUD and HUD controls. Nothing in Canon's
# photo pipeline is hooked: DIGIC 8 has no runtime ROM hooks (MMU patches are
# fixed at build time) and its still develop path is not known yet.
# xpan-m50.c stands in for the look, JPEG crop, grain and watermark stages and
# checks the Canon settings XPan needs (it cannot change them on this body).
XPAN ?= n
ifeq ($(XPAN),y)
ifeq ($(CONFIG_QEMU),y)
$(error XPAN=y is for the physical camera; build it with CONFIG_QEMU=n)
endif
# No module loader or TCC: the XPan package ships no modules, and MMU remap
# (back on for XPan's colour-table hook) needs that memory: Canon must keep
# its boot minimum (build_xpan.sh memory check; README-XPAN-M50.md).
override CONFIG_MODULES = n
override CONFIG_TCC = n
ML_SRC_EXTRA_OBJS += $(addprefix $(BUILD_DIR)/, xpan-hud-controls.o xpan-m50.o xpan-cube.o xpan-lut.o xpan-native-table.o)
endif

include ../Makefile

ifeq ($(XPAN),y)
CFLAGS += -DCONFIG_XPAN=1
# XPan is incompatible with almost every module, so ship none.
# The user looks (data/XPAN/LUTS/*.cube) go to ML/XPAN/LUTS for the HUD's LUT
# field (names only on the M50 so far; the look stage is not ported).
XPAN_DATA_DIR = $(TOP_DIR)/data/XPAN
zip_contents:: $(ZIP_DIR) $(BUILD_DIR)/magiclantern.sym | $(BUILD_DIR)
	# M50_110.sym still marks an M50 card for xpan_update_card.ps1 (no modules use it).
	mkdir -p $(ZIP_MODULES_DIR)
	$(CP) $(BUILD_DIR)/magiclantern.sym $(ZIP_MODULES_DIR)/M50_110.sym
	rm -rf $(ZIP_DIR)/ML/XPAN
	mkdir -p $(ZIP_DIR)/ML/XPAN/LUTS
	find $(XPAN_DATA_DIR)/LUTS -maxdepth 1 -type f -iname '*.cube' -exec $(CP) {} $(ZIP_DIR)/ML/XPAN/LUTS/ \;
	# Cineon tone base for the Custom LUT (xpan-m50.c m50_base_load).
	$(CP) $(XPAN_DATA_DIR)/CINLV.cube $(ZIP_DIR)/ML/XPAN/
	# Boot-flag installer (Canon menu > Firmware update), as other ML zips carry
	# ML-SETUP.FIR; xpan_update_card.ps1 never copies it to the working card.
	$(CP) $(TOP_DIR)/extras/M50/BOOT_M50.FIR $(ZIP_DIR)/
	rm -f $(ZIP_MODULES_DIR)/*.mo $(ZIP_MODULES_DIR)/*.MO $(ZIP_MODULES_DIR)/*.hid $(ZIP_MODULES_DIR)/*.HID
endif
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

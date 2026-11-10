MODEL = 6D
FW_VERSION = 116

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR = 6D000116.FIR
MAIN_FIRMWARE_ADDR = 0xFF0C0000
PLATFORM_ARCH = armv5te

# Load ML at the beginning of the AllocateMemory pool
# default 0x44C000 - 0xD3C000, patched to 0x4E0000 - 0xD3C000 (592K for us).
RESTARTSTART = 0x44C100
ML_BOOT_OBJ = boot-d45-am.o

ML_BITRATE_OBJ = bitrate-5d3.o
ML_SOUND_CODEC_OBJ = ak4646.o

ML_SRC_EXTRA_OBJS = $(BUILD_DIR)/afma.o $(BUILD_DIR)/reloc.o $(BUILD_DIR)/gps.o

# XPan: panoramic 65:24 photo mode (see README-XPAN.md at the repository root).
# Physical camera only. Enable with:  make XPAN=y CONFIG_QEMU=n
# The 6D port shares the HUD, controls, menus, LUT parsing and the native
# colour-table look with the EOS M. The EOS M-only stages (JPEG crop, saved-
# image grain/watermark, Live View LUT) are replaced by xpan-6d.c, which also
# carries the capture probe used to port them.
XPAN ?= n
ifeq ($(XPAN),y)
ifeq ($(CONFIG_QEMU),y)
$(error XPAN=y hooks Canon's photo developer; build it with CONFIG_QEMU=n)
endif
ML_SRC_EXTRA_OBJS += $(addprefix $(BUILD_DIR)/,     xpan-hud-controls.o     xpan-native-look.o     xpan-native-table.o     xpan-native-cube.o     xpan-cube.o     xpan-lut.o     xpan-6d.o)
endif

include ../Makefile

ifeq ($(XPAN),y)
CFLAGS += -DCONFIG_XPAN=1
$(addprefix $(BUILD_DIR)/,xpan-native-table.o xpan-cube.o xpan-lut.o): CFLAGS += -O2
# Card data, packaged as ML/XPAN/ in magiclantern.zip (same as the EOS M).
XPAN_DATA_DIR = $(TOP_DIR)/data/XPAN
zip_contents:: $(ZIP_DIR) | $(BUILD_DIR)
	rm -rf $(ZIP_DIR)/ML/XPAN
	mkdir -p $(ZIP_DIR)/ML/XPAN/LUTS
	$(CP) $(XPAN_DATA_DIR)/*.cube $(XPAN_DATA_DIR)/*.BIN $(ZIP_DIR)/ML/XPAN/
	find $(XPAN_DATA_DIR)/LUTS -maxdepth 1 -type f -iname '*.cube' -exec $(CP) {} $(ZIP_DIR)/ML/XPAN/LUTS/ \;
# XPan is incompatible with almost every module, so ship none.
	rm -f $(ZIP_MODULES_DIR)/*.mo $(ZIP_MODULES_DIR)/*.MO $(ZIP_MODULES_DIR)/*.hid $(ZIP_MODULES_DIR)/*.HID
endif
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

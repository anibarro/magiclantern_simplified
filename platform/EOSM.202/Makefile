MODEL = EOSM
FW_VERSION = 202

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120
ML_BOOT_OBJ = boot-d45.o

#CANON_FIR = EOSM1202.FIR

MAIN_FIRMWARE_ADDR = 0xFF0C0000
PLATFORM_ARCH = armv5te

# DryOSmemory map
# RESTARTSTART is selected to be just above the end of the bss
#
RESTARTSTART = 0x9E1E0

ML_AF_PATTERNS_OBJ = n

# XPan: panoramic 65:24 photo mode, native-JPEG look/LUT pipeline, film grain,
# watermark and HUD for the EOS M (see README-XPAN.md at the repository root).
# Physical camera only. Enable with:  make XPAN=y CONFIG_QEMU=n
XPAN ?= n
ifeq ($(XPAN),y)
ifeq ($(CONFIG_QEMU),y)
$(error XPAN=y hooks Canon's photo developer and LCD path; build it with CONFIG_QEMU=n)
endif
# Everything runs from existing Canon/ML tasks: no extra task is created and
# no image buffer is allocated at boot.
ML_SRC_EXTRA_OBJS += $(addprefix $(BUILD_DIR)/, \
    xpan-hud-controls.o \
    xpan-native-look.o \
    xpan-native-table.o \
    xpan-native-cube.o \
    xpan-cube.o \
    xpan-lut.o \
    xpan-live-color-probe.o \
    xpan-live-native.o \
    xpan-native-jpeg.o \
    xpan-film-grain.o)
endif

include ../Makefile

# CFLAGS must be appended after ../Makefile has initialised them.
ifeq ($(XPAN),y)
CFLAGS += -DCONFIG_XPAN=1
$(BUILD_DIR)/xpan-live-native.o: $(SRC_DIR)/xpan-live-neutral.inc $(SRC_DIR)/xpan-live-neutral-enabled.inc
# The 1000-node colour-table preparation is the only hot loop; keep the rest
# of Magic Lantern at its usual size-oriented -Os.
$(addprefix $(BUILD_DIR)/,xpan-native-table.o xpan-cube.o xpan-lut.o): CFLAGS += -O2

# Card data, packaged as ML/XPAN/ in magiclantern.zip:
#   data/XPAN/*.CUBE   Cineon tone bases (photo +1/+2 EV, Live View)
#   data/XPAN/*.BIN    WMARK_1/2.BIN (orange/white logo), WMFONT.BIN (LUT name
#                      font), GRAIN.BIN (Canon grain texture)
#   data/XPAN/LUTS/    user looks (*.cube); the folder is created even if empty
# Rebuilt on every zip so files removed from the repository leave the card too.
XPAN_DATA_DIR = $(TOP_DIR)/data/XPAN
zip_contents:: $(ZIP_DIR) | $(BUILD_DIR)
	rm -rf $(ZIP_DIR)/ML/XPAN
	mkdir -p $(ZIP_DIR)/ML/XPAN/LUTS
	$(CP) $(XPAN_DATA_DIR)/*.CUBE $(XPAN_DATA_DIR)/*.BIN $(ZIP_DIR)/ML/XPAN/
	find $(XPAN_DATA_DIR)/LUTS -maxdepth 1 -type f -iname '*.cube' -exec $(CP) {} $(ZIP_DIR)/ML/XPAN/LUTS/ \;
# XPan is incompatible with almost every module, so ship none: ML/modules/
# keeps only the symbol file. This rule runs after the generic zip_contents
# rules in ../Makefile, which have already copied the default modules.
	rm -f $(ZIP_MODULES_DIR)/*.mo $(ZIP_MODULES_DIR)/*.MO $(ZIP_MODULES_DIR)/*.hid $(ZIP_MODULES_DIR)/*.HID
endif
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

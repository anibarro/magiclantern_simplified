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
endif
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

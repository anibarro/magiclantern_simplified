MODULE_NAME = livecomp
MODULE_OBJS = $(addprefix $(BUILD_DIR)/,livecomp.o livecomp-lv.o native-max.o fullres-check.o)
TOP_DIR = ../..
MODULE_CFLAGS = -include livecomp-config.h
include $(TOP_DIR)/modules/Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/module_complete

$(BUILD_DIR)/effective-config.txt: Makefile livecomp-config.h $(BUILD_DIR)/module_strings.h
	$(CC) $(filter-out -Wp%,$(CFLAGS)) -dM -E livecomp.c > $@

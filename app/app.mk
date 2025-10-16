# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
# Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>

PKG_CONFIG = PKG_CONFIG_PATH=$(SRC_ROOT)/build pkg-config
TF=false

CFLAGS += -I$(SRC_ROOT)/include/lib -Iinclude
CFLAGS += $(shell $(PKG_CONFIG) --cflags libtpa-internal)

LDFLAGS := $(shell $(PKG_CONFIG) --libs --static libtpa-internal)
LDFLAGS += -lm

ifeq ($(TF),true)

# Require TF_LIB_PATH if not set
ifndef TF_LIB_PATH
$(error TF=true requires TF_LIB_PATH to be set, e.g. make TF=true TF_LIB_PATH=$(HOME)/.anaconda/envs/cnn_offrac/lib/tensorflow)
endif

CFLAGS += -I$(TF_LIB_PATH)
CFLAGS += -I$(TF_LIB_PATH)/xla
LDFLAGS += -L$(TF_LIB_PATH)/lib -ltensorflow -pthread -ldl -lm

CFLAGS += -DTF_ENABLED # compile code for tensorflow
endif

OBJ_DIR = $(OBJ_ROOT)/app/$(APP)
BIN_DIR = $(BIN_ROOT)/app

OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)
DEPS = $(SRCS:%.c=$(OBJ_DIR)/%.d)

BIN = $(BIN_DIR)/$(APP)

all: $(BIN)

$(BIN): $(OBJS) | OUT_DIRS
	$(Q)echo "  LD $(notdir $@)"
	$(Q)$(CC) $^ -o $@ $(LDFLAGS)

$(OBJS): $(OBJ_DIR)/%.o: %.c | OUT_DIRS
	$(Q)echo "  CC $(notdir $@)"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

OUT_DIRS:
	$(Q)mkdir -p $(OBJ_DIR) $(BIN_DIR)

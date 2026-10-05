# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
# Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>

SRCS := fperf.c
SRCS += client.c
SRCS += server.c
SRCS += test.c
SRCS += conn.c
SRCS += integrity.c
SRCS += options.c
SRCS += utils.c
SRCS += mbuf.c
SRCS += event.c
SRCS += stats.c
SRCS += offrac.c
SRCS += trace.c

APP  := fperf

# saved model the server loads for the CNN function (TF=true builds only)
FPERF_TF_MODEL_DIR ?= $(CURDIR)/tf/saved_model
CFLAGS += -DFPERF_TF_MODEL_DIR=\"$(FPERF_TF_MODEL_DIR)\"

include ../app.mk

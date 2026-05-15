#
# QSOE — /sbin/repath build.
#
# repath is a one-shot CLI wrapper around qsoe_pathmgr_repath() — used
# by init.sh to swap /dev/console at boot, and by any script that
# needs to mutate a pathmgr entry.  Uses libqsoe + libc only.
#
# Hand-run: cd userland/sbin/repath && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

REPATH_DIR  := $(TOP)/userland/sbin/repath
TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/sbin/repath
REPATH_ELF  := $(BUILD)/sbin-repath.elf

LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

REPATH_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include \
    -isystem $(MUSL_GEN)/include \
    -isystem $(MUSL_DIR)/include \
    -isystem $(MUSL_DIR)/arch/riscv64 \
    -isystem $(MUSL_DIR)/arch/generic

.PHONY: all clean
all: $(REPATH_ELF)

$(OBJDIR)/start.o: $(REPATH_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(REPATH_CFLAGS) -c -o $@ $<

$(OBJDIR)/main.o: $(REPATH_DIR)/main.c
	@mkdir -p $(@D)
	$(CC) $(REPATH_CFLAGS) -c -o $@ $<

REPATH_OBJS := \
    $(OBJDIR)/start.o \
    $(OBJDIR)/main.o

$(REPATH_ELF): $(REPATH_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(REPATH_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(REPATH_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(REPATH_ELF)

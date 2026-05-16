#
# QSOE — /sbin/slogger build.
#
# System logger.  Owns a ring buffer of slog events; registers
# /dev/slog with the path manager.  Used by slogf() and sloginfo.
#
# Hand-run: cd userland/sbin/slogger && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

SLOG_DIR    := $(TOP)/userland/sbin/slogger
TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/sbin/slogger
SLOG_ELF    := $(BUILD)/sbin-slogger.elf

LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

# Shared userland crt0.
CRT0_O      := $(BUILD)/crt0.o

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

SLOG_CFLAGS := $(ARCH_CFLAGS) \
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

SLOG_HEADERS := \
    $(TASKMAN_DIR)/sel4_types.h \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(LIBQSOE_DIR)/include/sys/slog.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(SLOG_ELF)

$(OBJDIR)/main.o: $(SLOG_DIR)/main.c $(SLOG_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(SLOG_CFLAGS) -c -o $@ $<

SLOG_OBJS := \
    $(OBJDIR)/main.o

$(SLOG_ELF): $(CRT0_O) $(SLOG_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(SLOG_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(CRT0_O) $(SLOG_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(SLOG_ELF)

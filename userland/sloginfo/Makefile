#
# QSOE — sloginfo CLI.
#
# Reads /dev/slog and pretty-prints the ring buffer.  Lives in /bin.
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

SLOGINFO_DIR := $(TOP)/userland/sloginfo
TASKMAN_DIR  := $(TOP)/userland/taskman
LIBQSOE_DIR  := $(TOP)/userland/libqsoe
CORE         := $(TOP)/core
MUSL_DIR     := $(CORE)/userland/libc
MUSL_GEN     := $(BUILD)/libc-gen
GEN          := $(BUILD)/gen

OBJDIR       := $(BUILD)/sloginfo
SLOGINFO_ELF := $(BUILD)/sloginfo.elf

LIBQSOE_A    := $(BUILD)/libqsoe.a
LIBC_A       := $(BUILD)/libc.a
CRT0_O       := $(BUILD)/crt0.o

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

SI_CFLAGS := $(ARCH_CFLAGS) \
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
all: $(SLOGINFO_ELF)

$(OBJDIR)/main.o: $(SLOGINFO_DIR)/main.c
	@mkdir -p $(@D)
	$(CC) $(SI_CFLAGS) -c -o $@ $<

$(SLOGINFO_ELF): $(CRT0_O) $(OBJDIR)/main.o $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(SI_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(CRT0_O) $(OBJDIR)/main.o \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(SLOGINFO_ELF)

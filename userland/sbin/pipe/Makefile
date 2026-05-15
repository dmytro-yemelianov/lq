#
# QSOE — pipe manager build.
#
# /sbin/pipe is QSOE's POSIX pipe / FIFO resource manager.  Registers
# at /dev/pipe via the path manager; serves IO_READ / IO_WRITE / CLOSE
# requests on its libqsoe channel.  Per the project rule (CLAUDE.md
# "Requirements for resource managers") this binary uses only libqsoe
# and libc — no direct seL4 syscalls.
#
# Hand-run: cd userland/sbin/pipe && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

PIPE_DIR    := $(TOP)/userland/sbin/pipe
TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/sbin/pipe
PIPE_ELF    := $(BUILD)/sbin-pipe.elf

LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

# Same posture every QSOE userland TU uses: strict c11, freestanding,
# nostdlib, nostdinc.  musl headers wired via -isystem for main.c.
PIPE_CFLAGS := $(ARCH_CFLAGS) \
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

PIPE_HEADERS := \
    $(TASKMAN_DIR)/sel4_types.h \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(PIPE_ELF)

$(OBJDIR)/start.o: $(PIPE_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(PIPE_CFLAGS) -c -o $@ $<

$(OBJDIR)/main.o: $(PIPE_DIR)/main.c $(PIPE_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(PIPE_CFLAGS) -c -o $@ $<

PIPE_OBJS := \
    $(OBJDIR)/start.o \
    $(OBJDIR)/main.o

# Link: --whole-archive libqsoe.a so start_main / time / etc. land
# even when only referenced through libqsoe-internal paths.
$(PIPE_ELF): $(PIPE_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(PIPE_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(PIPE_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(PIPE_ELF)

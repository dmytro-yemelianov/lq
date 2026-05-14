#
# QSOE — taskman build.
#
# taskman is QSOE's rootserver: process/path/memory manager, equivalent
# to QNX's procnto.  It is the very first userland process the seL4
# kernel starts after boot.  taskman embeds the userland CPIO archive
# (containing init / tester / hello / devc-ser8250) in its .userland_cpio
# section via .incbin, and at runtime serves files from it through the
# cpiofs handler.
#
# Hand-run: cd userland/taskman && make clean all
#
# Build-time precondition: $(USERLAND_CPIO) must exist.  The top-level
# Makefile builds it from the user-side ELFs and then invokes us; if
# you're running this submake standalone after a clean, build the user
# ELFs first (`make -C $(TOP) build/userland.cpio`).
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
CORE        := $(TOP)/core
LIBCPIO     := $(CORE)/lib/cpio
GEN         := $(BUILD)/gen

OBJDIR     := $(BUILD)/taskman
TASKMAN_ELF := $(BUILD)/taskman.elf

# Inputs from sibling submakes.  The libqsoe submake produces these
# alongside its own outputs.
LIBQSOE_TM_A := $(BUILD)/libqsoe-tm.a
USERLAND_CPIO := $(BUILD)/userland.cpio

# Compile flags — mirror the top-level TM_CFLAGS, plus libcpio for the
# few TUs that talk to the embedded CPIO directly.
ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

TM_CFLAGS := $(ARCH_CFLAGS) \
    -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include

# Headers shared across taskman/libqsoe TUs.
TM_HEADERS := \
    $(TASKMAN_DIR)/sel4_syscalls.h \
    $(TASKMAN_DIR)/sel4_types.h \
    $(TASKMAN_DIR)/qsoe_invoke.h \
    $(TASKMAN_DIR)/server.h \
    $(TASKMAN_DIR)/spawn.h \
    $(TASKMAN_DIR)/pathmgr.h \
    $(TASKMAN_DIR)/console.h \
    $(TASKMAN_DIR)/cpiofs.h \
    $(LIBQSOE_DIR)/include/qsoe/qrv.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(LIBQSOE_DIR)/src/state.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(TASKMAN_ELF)

# Compile rules.
$(OBJDIR)/start.o: $(TASKMAN_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/main.o: $(TASKMAN_DIR)/main.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

$(OBJDIR)/server.o: $(TASKMAN_DIR)/server.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

$(OBJDIR)/spawn.o: $(TASKMAN_DIR)/spawn.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/pathmgr.o: $(TASKMAN_DIR)/pathmgr.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/console.o: $(TASKMAN_DIR)/console.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/cpiofs.o: $(TASKMAN_DIR)/cpiofs.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

# libcpio from core/.  Same .o as elfloader uses to navigate its own
# CPIO; we just compile a second copy with taskman's flags.
$(OBJDIR)/cpio.o: $(LIBCPIO)/src/cpio.c
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

# Embed the userland CPIO archive in a custom section via .incbin.
# Two symbols (_userland_cpio_start / _end) bracket it so taskman's
# C side can walk it at runtime.
$(OBJDIR)/userland_archive.S: $(USERLAND_CPIO)
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '.section .userland_cpio,"a"' \
	    '.balign 4096' \
	    '.globl _userland_cpio_start, _userland_cpio_end' \
	    '_userland_cpio_start:' \
	    '.incbin "$<"' \
	    '_userland_cpio_end:' \
	    > $@

$(OBJDIR)/userland_archive.o: $(OBJDIR)/userland_archive.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

TASKMAN_OBJS := \
    $(OBJDIR)/start.o \
    $(OBJDIR)/main.o \
    $(OBJDIR)/server.o \
    $(OBJDIR)/spawn.o \
    $(OBJDIR)/pathmgr.o \
    $(OBJDIR)/console.o \
    $(OBJDIR)/cpiofs.o \
    $(OBJDIR)/cpio.o \
    $(OBJDIR)/userland_archive.o

# Link.  --whole-archive on libqsoe-tm.a so all 7 IPC handlers come in
# even when taskman's own code doesn't directly reference them.
$(TASKMAN_ELF): $(TASKMAN_OBJS) $(LIBQSOE_TM_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(TASKMAN_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_TM_A) -Wl,--no-whole-archive

clean:
	rm -rf $(OBJDIR) $(TASKMAN_ELF)

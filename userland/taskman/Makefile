#
# QSOE — taskman build.
#
# taskman is QSOE's rootserver: process/path/memory manager, equivalent
# to QNX's procnto.  It is the very first userland process the seL4
# kernel starts after boot.  taskman embeds the userland CPIO archive
# (containing init / tester / qsh / devc-ser8250 / pipe) in its .userland_cpio
# section via .incbin, and at runtime serves files from it through the
# cpiofs handler.
#
# v0.7 layout — code is split across four subsystem dirs:
#   sys/   — system services (console)
#   proc/  — processes / threads / channels / connections / pulses + spawn
#   mem/   — memory manager (mmap)
#   path/  — pathmgr / cpiofs / open / close / io
#
# Hand-run: cd userland/taskman && make clean all
#
# Build-time precondition: $(USERLAND_CPIO) must exist.  The top-level
# Makefile builds it from the user-side ELFs and then invokes us; if
# you're running this submake standalone after a clean, build the user
# ELFs first (`make -C $(TOP) build/userland.cpio`).
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
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

LIBQSOE_TM_A := $(BUILD)/libqsoe-tm.a
USERLAND_CPIO := $(BUILD)/userland.cpio

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

TM_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include

# All taskman .h headers — used as a coarse "if anything changed,
# rebuild" trigger for the .o pattern rules below.
TM_HEADERS := \
    $(TASKMAN_DIR)/sel4_syscalls.h \
    $(TASKMAN_DIR)/sel4_types.h \
    $(TASKMAN_DIR)/qsoe_invoke.h \
    $(TASKMAN_DIR)/proc/proc.h \
    $(TASKMAN_DIR)/proc/spawn.h \
    $(TASKMAN_DIR)/mem/mem.h \
    $(TASKMAN_DIR)/path/path.h \
    $(TASKMAN_DIR)/path/pathmgr.h \
    $(TASKMAN_DIR)/path/cpiofs.h \
    $(TASKMAN_DIR)/sys/console.h \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
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

# proc/ — process registry, channels, connections, threads, pulses, spawn.
$(OBJDIR)/proc/%.o: $(TASKMAN_DIR)/proc/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

# mem/ — memory manager.
$(OBJDIR)/mem/%.o: $(TASKMAN_DIR)/mem/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# path/ — pathmgr + cpiofs + IO dispatch.
$(OBJDIR)/path/%.o: $(TASKMAN_DIR)/path/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

# sys/ — console (other system services later).
$(OBJDIR)/sys/%.o: $(TASKMAN_DIR)/sys/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# libcpio from core/.  Same .o as elfloader uses to navigate its own
# CPIO; we just compile a second copy with taskman's flags.
$(OBJDIR)/cpio.o: $(LIBCPIO)/src/cpio.c
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

# Embed the userland CPIO archive in a custom section via .incbin.
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
    $(OBJDIR)/proc/process.o \
    $(OBJDIR)/proc/channel.o \
    $(OBJDIR)/proc/connect.o \
    $(OBJDIR)/proc/thread.o \
    $(OBJDIR)/proc/pulse.o \
    $(OBJDIR)/proc/spawn.o \
    $(OBJDIR)/proc/timer.o \
    $(OBJDIR)/mem/mmap.o \
    $(OBJDIR)/path/pathmgr.o \
    $(OBJDIR)/path/cpiofs.o \
    $(OBJDIR)/path/io.o \
    $(OBJDIR)/sys/console.o \
    $(OBJDIR)/sys/devnull.o \
    $(OBJDIR)/sys/devzero.o \
    $(OBJDIR)/sys/fdt.o \
    $(OBJDIR)/sys/irq.o \
    $(OBJDIR)/sys/syscfg.o \
    $(OBJDIR)/cpio.o \
    $(OBJDIR)/userland_archive.o

# Link.  --whole-archive on libqsoe-tm.a so all IPC handlers come in
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

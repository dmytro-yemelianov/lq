#
# QSOE — taskman build.
#
# taskman is QSOE's rootserver: process/path/memory manager.  First
# userland process the seL4 kernel starts after boot.
#
# The userland modpkg.cpio lives in the umbrella-level quser/ tree
# and is built by `make -C ../quser cpio`.
# Taskman embeds it as section `.userland_cpio` via .incbin; the
# emitted symbols _userland_cpio_start[] / _userland_cpio_end[] are
# consumed by main.c to seed the cpiofs.  This mirrors QRV's startup-
# archive idiom: a minimal package (devc-*, devb-*, fs-*, sh, init)
# travels inside taskman.elf so the kernel needs no separate initrd
# protocol.  The rest of the system lives on a partition mounted by
# /sbin/init.
#
# A vestigial FDT-driven initrd loader lives in sys/initrd.{c,h}.
# It is excluded from the link by default; activate by adding
# -DTM_USE_INITRD_LOADER to TM_CFLAGS and re-adding sys/initrd.o to
# TASKMAN_OBJS.  Reason it is parked: seL4 zero-fills frames on
# retype-to-Frame for security, which clobbers the kernel-placed
# initrd payload.  Reviving it needs an elfloader patch that hands
# the cpio bytes to taskman through a reserved-memory + device-
# untyped path.  Kept on disk because that work is on the roadmap.
#
# Taskman code is split across subsystem dirs:
#   sys/   — system services (console + initrd loader)
#   proc/  — processes / threads / channels / connections / pulses + spawn
#   mem/   — memory manager (mmap)
#   path/  — pathmgr / cpiofs / open / close / io
#
# Hand-run: cd taskman && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

TASKMAN_DIR := $(TOP)/taskman
# Public QSOE/libc headers (sys/qsoe.h, qsoe/tm_msgs.h, ...) live in
# the umbrella-level shared libc tree.  Taskman doesn't link libc but
# needs the public header surface for wire shapes and slot constants.
LIBC_INCLUDE := $(abspath $(TOP)/..)/libc/include

# Generated seL4 ABI headers (4 files) live in the kernel build's
# gen_headers/ dir at the QSOE umbrella root.  See top-level Makefile
# for the bootstrap layout.
SEL4_BOOTSTRAP := $(abspath $(TOP)/..)/sel4-bootstrap
SEL4BUILD      := $(SEL4_BOOTSTRAP)/build-qsoe-riscv64
SEL4_GEN_HDRS  := $(SEL4BUILD)/gen_headers

LIBCPIO     := $(TOP)/../common
GEN         := $(BUILD)/gen

OBJDIR     := $(BUILD)/taskman
TASKMAN_ELF := $(BUILD)/taskman.elf

# Path to the userland module package (modpkg.cpio) produced by
# quser/.  Top-level lq/Makefile passes it down; if invoking this
# sub-Makefile by hand, override on the command line or rely on the
# default below.
MODPKG_CPIO ?= $(abspath $(TOP)/..)/quser/build/modpkg.cpio

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

# Taskman owns its own private copies of the seL4 IPC primitives it
# needs (state/msg/process/thread/channel/connect/io/time -- the
# IN_TASKMAN flavour of the retired libqsoe).  They live under
# qsoe/ and link directly into taskman.elf -- taskman never links
# libc, mirroring the NQ pattern.
# libtaskman -- portable taskman body lives at the umbrella root.
# Top-level lq Makefile passes paths down; sub-Makefile-invoked
# builds fall back to defaults.
LIBTASKMAN_INC ?= $(abspath $(TOP)/..)/libtaskman/include
LIBTASKMAN_A   ?= $(BUILD)/libtaskman/libtaskman.a

TM_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(SEL4_GEN_HDRS) \
    -I$(LIBC_INCLUDE) \
    -I$(TASKMAN_DIR) \
    -I$(TASKMAN_DIR)/qsoe \
    -I$(LIBTASKMAN_INC)

# All taskman .h headers — used as a coarse "if anything changed,
# rebuild" trigger for the .o pattern rules below.
TM_HEADERS := \
    $(TASKMAN_DIR)/sel4_syscalls.h \
    $(TASKMAN_DIR)/sel4_types.h \
    $(TASKMAN_DIR)/qsoe_invoke.h \
    $(TASKMAN_DIR)/tm_log.h \
    $(TASKMAN_DIR)/proc/proc.h \
    $(TASKMAN_DIR)/proc/spawn.h \
    $(TASKMAN_DIR)/mem/mem.h \
    $(TASKMAN_DIR)/path/path.h \
    $(TASKMAN_DIR)/path/pathmgr.h \
    $(TASKMAN_DIR)/path/cpiofs.h \
    $(TASKMAN_DIR)/sys/console.h \
    $(TASKMAN_DIR)/qsoe/state.h \
    $(TASKMAN_DIR)/qsoe/slots.h \
    $(LIBC_INCLUDE)/sys/qsoe.h \
    $(LIBC_INCLUDE)/qsoe/tls.h \
    $(LIBC_INCLUDE)/qsoe/tm_msgs.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(TASKMAN_ELF)

# Compile rules.
$(OBJDIR)/start.o: $(TASKMAN_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/main.o: $(TASKMAN_DIR)/main.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO) -c -o $@ $<

# tm_log — the sole sel4_debug_* callsite in taskman.
$(OBJDIR)/tm_log.o: $(TASKMAN_DIR)/tm_log.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# proc/ — process registry, channels, connections, threads, pulses, spawn.
$(OBJDIR)/proc/%.o: $(TASKMAN_DIR)/proc/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO) -c -o $@ $<

# mem/ — memory manager.
$(OBJDIR)/mem/%.o: $(TASKMAN_DIR)/mem/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# path/ — pathmgr + cpiofs + IO dispatch.
$(OBJDIR)/path/%.o: $(TASKMAN_DIR)/path/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO) -c -o $@ $<

# sys/ — console (other system services later).
$(OBJDIR)/sys/%.o: $(TASKMAN_DIR)/sys/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# qsoe/ — taskman-private seL4 IPC primitives (the IN_TASKMAN flavour
# of the retired libqsoe; folded in 2026-06-01).  Same set of API
# entry points users see (ChannelCreate / MsgSend / ConnectAttach /
# ...), but the bodies dispatch straight into the local tm_* handlers
# instead of going out over an endpoint cap.
$(OBJDIR)/qsoe/%.o: $(TASKMAN_DIR)/qsoe/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# cpio.o: source is in ../common
$(OBJDIR)/cpio.o: $(LIBCPIO)/cpio.c
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO) -c -o $@ $<

# userland_archive.S -- .incbin shim that embeds the quser-built
# modpkg.cpio into taskman.elf at a known section.  The emitted
# globals _userland_cpio_start / _userland_cpio_end are consumed by
# main.c (see #ifndef TM_USE_INITRD_LOADER branch).  Regenerated
# whenever modpkg.cpio changes.  The .balign 4 is critical: libcpio's
# cpio_get_file does align_up(absolute_addr, 4) on file-data pointers,
# so an unaligned base would shift every returned data pointer by the
# misalignment and the spawn path would see garbage instead of ELF
# magic / shebang bytes.
$(OBJDIR)/userland_archive.S: $(MODPKG_CPIO)
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '.section .userland_cpio,"a",@progbits' \
	    '.balign 4' \
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
    $(OBJDIR)/tm_log.o \
    $(OBJDIR)/proc/process.o \
    $(OBJDIR)/proc/channel.o \
    $(OBJDIR)/proc/connect.o \
    $(OBJDIR)/proc/thread.o \
    $(OBJDIR)/proc/pulse.o \
    $(OBJDIR)/proc/spawn.o \
    $(OBJDIR)/proc/timer.o \
    $(OBJDIR)/mem/mmap.o \
    $(OBJDIR)/path/cpiofs.o \
    $(OBJDIR)/path/pmdir.o \
    $(OBJDIR)/path/sysfs.o \
    $(OBJDIR)/path/procfs.o \
    $(OBJDIR)/path/io.o \
    $(OBJDIR)/sys/console.o \
    $(OBJDIR)/sys/devnull.o \
    $(OBJDIR)/sys/devzero.o \
    $(OBJDIR)/sys/fdt.o \
    $(OBJDIR)/sys/irq.o \
    $(OBJDIR)/sys/rsrcdb.o \
    $(OBJDIR)/sys/sync.o \
    $(OBJDIR)/sys/syscfg.o \
    $(OBJDIR)/sys/sysmap.o \
    $(OBJDIR)/cpio.o \
    $(OBJDIR)/userland_archive.o \
    $(OBJDIR)/qsoe/state.o \
    $(OBJDIR)/qsoe/msg.o \
    $(OBJDIR)/qsoe/process.o \
    $(OBJDIR)/qsoe/thread.o \
    $(OBJDIR)/qsoe/channel.o \
    $(OBJDIR)/qsoe/connect.o \
    $(OBJDIR)/qsoe/io.o \
    $(OBJDIR)/qsoe/time.o

# Link.  All taskman objects + private qsoe/ trampolines + libtaskman.a
# (portable taskman body).  No libc -- taskman is a freestanding
# rootserver.  Plain archive semantics on libtaskman: only objects whose
# symbols are referenced get pulled in, so LQ-side overrides (e.g.
# sys/syscfg.o, kept while the API still differs) suppress libtaskman's
# matching module cleanly.
$(TASKMAN_ELF): $(TASKMAN_OBJS) $(LIBTASKMAN_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(TASKMAN_OBJS) $(LIBTASKMAN_A)

clean:
	rm -rf $(OBJDIR) $(TASKMAN_ELF)

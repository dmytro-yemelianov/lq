#
# QSOE — musl libc build.
#
# Compiles the vendored musl tree at $(TOP)/core/userland/libc/ into a
# single static archive at $(TOP)/build/libc.a, plus the three small
# headers musl generates from .in templates.  Top-level Makefile and
# component Makefiles invoke this via `make -C userland/libc`.
#
# Hand-run: cd userland/libc && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

# Cross-compiler.
CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc
AR         := $(CROSS)ar

# Source + build paths.  The musl tree itself lives under core/ (verbatim
# vendored upstream); our wrapper Makefile lives at userland/libc/ so a
# plain `make` in this directory drives the build.
MUSL_DIR     := $(TOP)/core/userland/libc
MUSL_PATCHES := $(MUSL_DIR)/patches
MUSL_GEN     := $(BUILD)/libc-gen
LIBC_BUILD   := $(BUILD)/libc-obj
LIBC_A       := $(BUILD)/libc.a

# Match upstream RISC-V flags + our nostdinc / freestanding policy.
ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mstrict-align

MUSL_CFLAGS := $(ARCH_CFLAGS) \
    -std=c99 -ffreestanding -nostdinc \
    -fno-pic -fno-pie -fno-common \
    -fno-stack-protector -fno-builtin \
    -fexcess-precision=standard -frounding-math \
    -D_XOPEN_SOURCE=700 \
    -Wa,--noexecstack \
    -Os

# Include order: patched headers (so syscall_arch.h indirect-call variant
# wins over the stock ecall one), then arch-specific, generic, generated
# (bits/alltypes.h, bits/syscall.h, version.h), and finally the public
# include tree.
MUSL_INCLUDES := \
    -I$(MUSL_PATCHES)/arch/riscv64 \
    -I$(MUSL_DIR)/arch/riscv64 \
    -I$(MUSL_DIR)/arch/generic \
    -I$(MUSL_GEN)/src/internal \
    -I$(MUSL_DIR)/src/include \
    -I$(MUSL_DIR)/src/internal \
    -I$(MUSL_GEN)/include \
    -I$(MUSL_DIR)/include

# Generated headers — two sed transforms replicating upstream musl's own
# Makefile, plus a static stand-in for the git-derived version string.
MUSL_GEN_HDRS := \
    $(MUSL_GEN)/include/bits/alltypes.h \
    $(MUSL_GEN)/include/bits/syscall.h \
    $(MUSL_GEN)/src/internal/version.h

$(MUSL_GEN)/include/bits/alltypes.h: $(MUSL_DIR)/tools/mkalltypes.sed \
                                      $(MUSL_DIR)/arch/riscv64/bits/alltypes.h.in \
                                      $(MUSL_DIR)/include/alltypes.h.in
	@mkdir -p $(@D)
	sed -f $(MUSL_DIR)/tools/mkalltypes.sed \
	    $(MUSL_DIR)/arch/riscv64/bits/alltypes.h.in \
	    $(MUSL_DIR)/include/alltypes.h.in > $@

$(MUSL_GEN)/include/bits/syscall.h: $(MUSL_DIR)/arch/riscv64/bits/syscall.h.in
	@mkdir -p $(@D)
	cp $< $@
	sed -n -e s/__NR_/SYS_/p < $< >> $@

$(MUSL_GEN)/src/internal/version.h:
	@mkdir -p $(@D)
	@printf '#define VERSION "qsoe-vendored"\n' > $@

# Enumerate musl sources.  scripts/placement.txt is the source of
# truth for what's IN our tree: it excludes non-RISC-V arch subdirs,
# Linux-only subtrees (aio/ipc/ldso/linux/mq/network/sched/thread), and
# specific Linux-syscall-dependent files in process/signal/misc/unistd.
# So a plain recursive find is now sufficient.
MUSL_SRCS_ALL := $(shell find $(MUSL_DIR)/src -name '*.c' -o -name '*.S' -o -name '*.s')

# v0.6.4: no brk anywhere in QSOE.  All three musl malloc backends
# (lite_malloc, oldmalloc, mallocng) call SYS_brk.  Strip them
# entirely — libqsoe's malloc.c provides malloc/free/realloc/calloc
# + the __libc_* aliases via mmap.  realloc.c / free.c / calloc.c /
# libc_calloc.c / aligned_alloc.c / memalign.c / posix_memalign.c /
# reallocarray.c are tiny wrappers; they stay (they just call
# __libc_*).
MUSL_SRCS_ALL := $(filter-out \
    $(MUSL_DIR)/src/malloc/lite_malloc.c \
    $(wildcard $(MUSL_DIR)/src/malloc/oldmalloc/*.c) \
    $(wildcard $(MUSL_DIR)/src/malloc/mallocng/*.c), \
    $(MUSL_SRCS_ALL))

# Split by extension for the patsubst rules below.
MUSL_C_SRCS   := $(filter %.c,$(MUSL_SRCS_ALL))
MUSL_ASM_SRCS := $(filter %.S %.s,$(MUSL_SRCS_ALL))

MUSL_OBJS := \
    $(patsubst $(MUSL_DIR)/%.c,$(LIBC_BUILD)/%.o,$(MUSL_C_SRCS)) \
    $(patsubst $(MUSL_DIR)/%.S,$(LIBC_BUILD)/%.o,$(filter %.S,$(MUSL_ASM_SRCS))) \
    $(patsubst $(MUSL_DIR)/%.s,$(LIBC_BUILD)/%.o,$(filter %.s,$(MUSL_ASM_SRCS)))

# Default + main targets.
.PHONY: all clean headers libc
all: $(LIBC_A)
libc: $(LIBC_A)
headers: $(MUSL_GEN_HDRS)

# Per-file compile rules.  Header deps are coarse — every .c depends on
# the three generated headers — but that keeps the build correct.
$(LIBC_BUILD)/%.o: $(MUSL_DIR)/%.c $(MUSL_GEN_HDRS)
	@mkdir -p $(@D)
	$(CC) $(MUSL_CFLAGS) $(MUSL_INCLUDES) -c -o $@ $<

$(LIBC_BUILD)/%.o: $(MUSL_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) $(MUSL_CFLAGS) $(MUSL_INCLUDES) -c -o $@ $<

$(LIBC_BUILD)/%.o: $(MUSL_DIR)/%.s
	@mkdir -p $(@D)
	$(CC) $(MUSL_CFLAGS) $(MUSL_INCLUDES) -c -o $@ $<

$(LIBC_A): $(MUSL_OBJS)
	@echo "  AR  $@ ($(words $(MUSL_OBJS)) objects)"
	@$(AR) rcs $@ $(MUSL_OBJS)

clean:
	rm -rf $(LIBC_BUILD) $(LIBC_A) $(MUSL_GEN)

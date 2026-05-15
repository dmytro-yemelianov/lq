#
# qsh — QSOE shell (derived from MirBSD mksh)
#
# Copyright (c) 19xx pdksh authors
# Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
#
# Adaptation for QSOE: Copyright (c) 2026 Yuri Zaporozhets <yuri.zaporozhets@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Standalone Makefile.  `make` in this directory builds qsh.elf,
# linking against musl (TOP/build/libc.a) and libqsoe (built fresh
# here from $(LIBQSOE)/src/*.c).  Run the top-level `make libc.a`
# first; that's libqsoe's only dependency.
#
# v0.6.3 status: 7-of-29 source files still have known issues; the
# remaining 22 compile cleanly.  Link will fail until those files
# build — useful as a progress signal while we rework qsh.
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build
QSH_DIR    := $(CURDIR)

# Cross-compiler (mirror top-level Makefile).
CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

# musl libc location + generated headers (built by top-level `make libc.a`).
# Note: musl is in the verbatim-from-upstream tree at $(TOP)/core/.
CORE       := $(TOP)/core
MUSL_DIR   := $(CORE)/userland/libc
MUSL_GEN   := $(BUILD)/libc-gen
LIBC_A     := $(BUILD)/libc.a

# libqsoe — we build its objects fresh into our own OBJDIR.
LIBQSOE_DIR := $(TOP)/userland/libqsoe
LIBQSOE_SRCS := state.c msg.c process.c thread.c channel.c connect.c \
                start_main.c io.c syscall_dispatch.c float128_stubs.c

# Build outputs.
OBJDIR     := $(BUILD)/qsh
QSH_ELF    := $(BUILD)/qsh.elf

# Flags — mirror top-level TM_CFLAGS for soft-float lp64, freestanding,
# musl-via-isystem.
ARCH_CFLAGS  := -march=rv64imafdc -mabi=lp64 -mno-relax \
                -nostdinc -static -fno-stack-protector -fno-pic -ffreestanding
QSH_CFLAGS   := $(ARCH_CFLAGS) -std=c11 -D_DEFAULT_SOURCE -O2 -g \
                -isystem $(MUSL_GEN)/include \
                -isystem $(MUSL_DIR)/include \
                -isystem $(MUSL_DIR)/arch/riscv64 \
                -isystem $(MUSL_DIR)/arch/generic \
                -I$(LIBQSOE_DIR)/include \
                -I$(QSH_DIR)/include \
                -I$(QSH_DIR)/gen \
                -Wno-error -w

# Sources: all .c files in this directory.
SRCS := $(notdir $(wildcard $(QSH_DIR)/*.c))
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

# Pre-built archives from sibling submakes.
LIBQSOE_A := $(BUILD)/libqsoe.a

all: $(QSH_ELF)

# Standard QSOE crt0: clear gp/tp, stage argc/argv/envp, jump to
# _qsoe_start_main.  Each userland program ships its own start.S.
$(OBJDIR)/start.o: $(QSH_DIR)/start.S | $(OBJDIR)
	$(CC) $(ARCH_CFLAGS) -c -o $@ $<

# Link: same shape as the other userland ELFs.
$(QSH_ELF): $(OBJDIR)/start.o $(OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(ARCH_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(OBJDIR)/start.o $(OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

# Compile each qsh .c into its .o, no abort on error so we surface all
# failures in a single pass.
$(OBJDIR)/%.o: $(QSH_DIR)/%.c | $(OBJDIR)
	$(CC) $(QSH_CFLAGS) -c -o $@ $< || (rm -f $@; false)

$(OBJDIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJDIR) $(QSH_ELF)

.PHONY: all clean

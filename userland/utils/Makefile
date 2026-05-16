#
# QSOE — userland/utils build.
#
# Single-file utilities — one .c source = one binary at /bin/<name>.
# Drop a new <foo>.c into this directory and a `make` builds
# $(BUILD)/utils/<foo>.elf; the top-level Makefile picks it up via
# wildcard and packs it into the CPIO as /bin/<foo>.
#
# Mirrors QRV's userland/utils/Makefile pattern.  No per-binary rule
# needed; the wildcard + pattern rule below handles every .c the
# same way (CFLAGS / link line are uniform).
#
# Hand-run: cd userland/utils && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

UTILS_DIR   := $(TOP)/userland/utils
LIBQSOE_DIR := $(TOP)/userland/libqsoe
TASKMAN_DIR := $(TOP)/userland/taskman
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/utils

LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

# Shared userland crt0.
CRT0_O      := $(BUILD)/crt0.o

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

# Same -nostdinc + musl-via-isystem layout as slogger / sloginfo /
# pci-server.  C11 strict per CLAUDE.md.
UTILS_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -D_XOPEN_SOURCE=700 \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include \
    -isystem $(MUSL_GEN)/include \
    -isystem $(MUSL_DIR)/include \
    -isystem $(MUSL_DIR)/arch/riscv64 \
    -isystem $(MUSL_DIR)/arch/generic

UTILS_HEADERS := \
    $(TASKMAN_DIR)/sel4_types.h \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(GEN)/qsoe/sys_version.h

# Enumerate sources from this directory; each .c becomes a .elf.
SRCS  := $(notdir $(wildcard $(UTILS_DIR)/*.c))
PROGS := $(SRCS:.c=.elf)

.PHONY: all clean
all: $(addprefix $(OBJDIR)/,$(PROGS))

# One .o per .c.
$(OBJDIR)/%.o: $(UTILS_DIR)/%.c $(UTILS_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(UTILS_CFLAGS) -c -o $@ $<

# One .elf per .o.  Same link line every binary.
$(OBJDIR)/%.elf: $(OBJDIR)/%.o $(CRT0_O) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(UTILS_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(CRT0_O) $< \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR)

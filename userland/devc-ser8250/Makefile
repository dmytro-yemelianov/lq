#
# QSOE — devc-ser8250 build.
#
# 16550 UART driver / resource manager.  QSOE's first userland resmgr.
# Spawned by init at boot, receives PLIC IRQs on a dedicated thread
# bound to a kernel-signaled Notification.  Registers /dev/ser1 with
# taskman's pathmgr.
#
# Hand-run: cd userland/devc-ser8250 && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

DSER_DIR    := $(TOP)/userland/devc-ser8250
TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/devc-ser8250
DSER_ELF    := $(BUILD)/devc-ser8250.elf

# Inputs from sibling submakes.
LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

TM_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include

# main.c needs musl headers (for stdio etc.); the smaller .c files
# don't pull anything from libc.
MAIN_CFLAGS := $(TM_CFLAGS) \
    -isystem $(MUSL_GEN)/include \
    -isystem $(MUSL_DIR)/include \
    -isystem $(MUSL_DIR)/arch/riscv64 \
    -isystem $(MUSL_DIR)/arch/generic

# Headers (subset of taskman's TM_HEADERS — devc-ser8250 doesn't need
# taskman-side server/spawn/pathmgr/etc.).
DSER_HEADERS := \
    $(TASKMAN_DIR)/sel4_syscalls.h \
    $(TASKMAN_DIR)/sel4_types.h \
    $(TASKMAN_DIR)/qsoe_invoke.h \
    $(LIBQSOE_DIR)/include/qsoe/qrv.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(DSER_ELF)

$(OBJDIR)/start.o: $(DSER_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/main.o: $(DSER_DIR)/main.c $(DSER_HEADERS) \
                  $(DSER_DIR)/uart.h $(DSER_DIR)/ring.h
	@mkdir -p $(@D)
	$(CC) $(MAIN_CFLAGS) -c -o $@ $<

$(OBJDIR)/uart.o: $(DSER_DIR)/uart.c $(DSER_DIR)/uart.h $(DSER_DIR)/ring.h
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(OBJDIR)/ring.o: $(DSER_DIR)/ring.c $(DSER_DIR)/ring.h
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

DSER_OBJS := \
    $(OBJDIR)/start.o \
    $(OBJDIR)/main.o \
    $(OBJDIR)/uart.o \
    $(OBJDIR)/ring.o

# Link.  --whole-archive on libqsoe.a so start_main / syscall_dispatch /
# float128_stubs land in even though they're referenced only via start.S
# / musl indirection / libgcc weak aliases.
$(DSER_ELF): $(DSER_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(DSER_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(DSER_ELF)

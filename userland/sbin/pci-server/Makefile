#
# QSOE — /sbin/pci-server build.
#
# PCI resource manager.  Scans the bus(es) at boot, owns the ECAM
# window, serves config-space reads/writes and BAR / IRQ queries to
# driver clients (devb-nvme, devnp-*) via /dev/pci.
#
# v0.8-rc2: single bus, INTx-only.  MSI / MSI-X land in v0.8-rc3
# alongside the DesignWare MSI bring-up for SiFive Unmatched.
#
# Hand-run: cd userland/sbin/pci-server && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc

PCI_DIR     := $(TOP)/userland/sbin/pci-server
TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
LIBPCI_DIR  := $(TOP)/userland/libpci
CORE        := $(TOP)/core
MUSL_DIR    := $(CORE)/userland/libc
MUSL_GEN    := $(BUILD)/libc-gen
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/sbin/pci-server
PCI_ELF     := $(BUILD)/sbin-pci-server.elf

LIBQSOE_A   := $(BUILD)/libqsoe.a
LIBC_A      := $(BUILD)/libc.a

# Shared userland crt0.
CRT0_O      := $(BUILD)/crt0.o

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

PCI_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include \
    -I$(LIBPCI_DIR)/include \
    -isystem $(MUSL_GEN)/include \
    -isystem $(MUSL_DIR)/include \
    -isystem $(MUSL_DIR)/arch/riscv64 \
    -isystem $(MUSL_DIR)/arch/generic

PCI_HEADERS := \
    $(TASKMAN_DIR)/sel4_types.h \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(LIBQSOE_DIR)/include/qsoe/hwinfo.h \
    $(LIBQSOE_DIR)/include/qsoe/syscfg.h \
    $(LIBQSOE_DIR)/include/sys/slog.h \
    $(LIBPCI_DIR)/include/pci/pci.h \
    $(GEN)/qsoe/sys_version.h

.PHONY: all clean
all: $(PCI_ELF)

$(OBJDIR)/main.o: $(PCI_DIR)/main.c $(PCI_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(PCI_CFLAGS) -c -o $@ $<

PCI_OBJS := \
    $(OBJDIR)/main.o

$(PCI_ELF): $(CRT0_O) $(PCI_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(PCI_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(CRT0_O) $(PCI_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

clean:
	rm -rf $(OBJDIR) $(PCI_ELF)

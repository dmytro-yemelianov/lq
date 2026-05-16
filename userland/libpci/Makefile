#
# QSOE — libpci build.
#
# Client wrappers for /dev/pci.  Built as a static archive linked
# into pci-aware programs (currently /bin/tester; future devb-*).
#
# Hand-run: cd userland/libpci && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc
AR         := $(CROSS)ar

LIBPCI_DIR  := $(TOP)/userland/libpci
LIBQSOE_DIR := $(TOP)/userland/libqsoe
TASKMAN_DIR := $(TOP)/userland/taskman
CORE        := $(TOP)/core
GEN         := $(BUILD)/gen

OBJDIR      := $(BUILD)/libpci-obj
LIBPCI_A    := $(BUILD)/libpci.a

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

# Same -nostdinc + libqsoe-internal include chain as rsrcdb.c — libpci
# reaches into qsoe_sys_call() (a static inline) and the internal coid
# table to label its own opcodes.
LIBPCI_CFLAGS := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include \
    -I$(LIBQSOE_DIR)/src \
    -I$(LIBPCI_DIR)/include \
    -I$(TASKMAN_DIR)

LIBPCI_HEADERS := \
    $(LIBQSOE_DIR)/include/qsoe-system.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBPCI_DIR)/include/pci/pci.h

LIBPCI_SRCS := libpci.c
LIBPCI_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(LIBPCI_SRCS))

.PHONY: all clean
all: $(LIBPCI_A)

$(OBJDIR)/%.o: $(LIBPCI_DIR)/src/%.c $(LIBPCI_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIBPCI_CFLAGS) -c -o $@ $<

$(LIBPCI_A): $(LIBPCI_OBJS)
	@echo "  AR  $@ ($(words $(LIBPCI_OBJS)) objects)"
	@$(AR) rcs $@ $(LIBPCI_OBJS)

clean:
	rm -rf $(OBJDIR) $(LIBPCI_A)

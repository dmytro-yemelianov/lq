#
# QSOE — libqsoe build.
#
# libqsoe is QSOE's userland C runtime — it sits between musl and
# seL4, providing channels/connections/IPC, syscall dispatch, process
# lifecycle, etc.  It is built in two flavours:
#
#   libqsoe.a     — normal: dispatch syscalls / process-mgmt via IPC
#                    to taskman.  Linked into init, tester,
#                    devc-ser8250, qsh, etc.
#
#   libqsoe-tm.a  — `-DQSOE_LIBQSOE_IN_TASKMAN`: same sources, but
#                    inside taskman the entrypoints call the tm_*
#                    handlers directly (no self-IPC).  Linked into
#                    taskman.
#
# Both archives land at $(TOP)/build/.
#
# Hand-run: cd userland/libqsoe && make clean all
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0
#

TOP        ?= $(abspath $(CURDIR)/../..)
BUILD      := $(TOP)/build

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc
AR         := $(CROSS)ar

LIBQSOE_DIR := $(TOP)/userland/libqsoe
TASKMAN_DIR := $(TOP)/userland/taskman
CORE        := $(TOP)/core
GEN         := $(BUILD)/gen

# Build sub-trees: one per flavour.
OBJDIR     := $(BUILD)/libqsoe-obj
NORMAL_OBJ := $(OBJDIR)/normal
TM_OBJ     := $(OBJDIR)/tm

LIBQSOE_A    := $(BUILD)/libqsoe.a
LIBQSOE_TM_A := $(BUILD)/libqsoe-tm.a

# Shared userland crt0.  Linked first by every spawned program
# (qsh, devc-ser8250, sbin/pipe, sbin/repath, tester, ...).
# taskman has its own start.S — different entry contract.
CRT0_O       := $(BUILD)/crt0.o

# CFLAGS — mirror the top-level TM_CFLAGS for consistency.
ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 \
               -mcmodel=medany -mstrict-align

TM_CFLAGS   := $(ARCH_CFLAGS) \
    -std=c11 -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include

NORMAL_CFLAGS := $(TM_CFLAGS) -I$(TASKMAN_DIR)
TM_VAR_CFLAGS := $(TM_CFLAGS) -DQSOE_LIBQSOE_IN_TASKMAN -I$(TASKMAN_DIR)

# Headers shared with taskman that affect ABI/behaviour of libqsoe.
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

# Auto-generated version header.  Pulls the latest git tag and emits
# numeric components + build date.  Duplicate of the rule in the top
# Makefile so this Makefile can run standalone.
$(GEN)/qsoe/sys_version.h: $(wildcard $(TOP)/.git/HEAD $(TOP)/.git/index)
	@mkdir -p $(@D)
	@cd $(TOP) && tag=$$(git describe --tags --abbrev=0 2>/dev/null || echo v0.0); \
	 ver=$${tag#v}; major=$${ver%%.*}; rest=$${ver#*.}; minor=$${rest%%.*}; \
	 patch=$${rest#*.}; [ "$$patch" = "$$rest" ] && patch=0; \
	 printf '/* auto-generated */\n#ifndef _QSOE_SYS_VERSION_H\n#define _QSOE_SYS_VERSION_H\n\n' > $@; \
	 printf '#define QSOE_VERSION_MAJOR %s\n' "$$major" >> $@; \
	 printf '#define QSOE_VERSION_MINOR %s\n' "$$minor" >> $@; \
	 printf '#define QSOE_VERSION_PATCH %s\n' "$$patch" >> $@; \
	 printf '#define QSOE_VERSION_STRING "%s"\n' "$$tag" >> $@; \
	 printf '#define QSOE_BUILD_DATE "%s"\n\n' "$$(date +%Y-%m-%d)" >> $@; \
	 printf '#endif\n' >> $@

# Source lists.
# Normal flavour: full surface (everything in src/).
NORMAL_SRCS := state.c msg.c process.c thread.c channel.c connect.c \
               start_main.c io.c mmap.c proc_info.c time.c \
               float128_stubs.c signal.c malloc.c ldisc.c irq.c \
               interrupt.c rsrcdb.c

# In-taskman flavour: subset — taskman doesn't need its own start_main,
# its own syscall dispatcher, or the float128 stubs.
TM_VAR_SRCS := state.c msg.c process.c thread.c channel.c connect.c io.c \
               time.c

NORMAL_OBJS := $(patsubst %.c,$(NORMAL_OBJ)/%.o,$(NORMAL_SRCS))
TM_VAR_OBJS := $(patsubst %.c,$(TM_OBJ)/%.o,$(TM_VAR_SRCS))

# Default + main targets.
.PHONY: all clean
all: $(LIBQSOE_A) $(LIBQSOE_TM_A) $(CRT0_O)

# Compile patterns.
$(NORMAL_OBJ)/%.o: $(LIBQSOE_DIR)/src/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(NORMAL_CFLAGS) -c -o $@ $<

$(TM_OBJ)/%.o: $(LIBQSOE_DIR)/src/%.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_VAR_CFLAGS) -c -o $@ $<

# Archive rules.
$(LIBQSOE_A): $(NORMAL_OBJS)
	@echo "  AR  $@ ($(words $(NORMAL_OBJS)) objects, normal flavour)"
	@$(AR) rcs $@ $(NORMAL_OBJS)

$(LIBQSOE_TM_A): $(TM_VAR_OBJS)
	@echo "  AR  $@ ($(words $(TM_VAR_OBJS)) objects, IN_TASKMAN flavour)"
	@$(AR) rcs $@ $(TM_VAR_OBJS)

# crt0 — built once, consumed by every userland program's Makefile.
$(CRT0_O): $(LIBQSOE_DIR)/src/crt0.S
	@mkdir -p $(@D)
	$(CC) $(ARCH_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(LIBQSOE_A) $(LIBQSOE_TM_A) $(CRT0_O)

# Makefile: build the QSOE C library for project LQ (the seL4 kernel).
#
# Running `make` here compiles the shared, OS-independent libc body (from the
# sibling libc/ repository) together with LQ's own kernel seam.  LQ never lives
# inside the libc repo; it drives that build and injects its seam — POSIX
# entry points that translate to taskman messages (TM_REQ_*) over seL4 IPC
# using libqsoe's `qsoe_invoke()` cap-invocation surface.
#
# Differences from NQ's seam:
#   * No `syscall.S`: the seL4 ABI is cap invocation (no RISC-V ecall slot
#     space the way Skimmer has).  The trampolines are plain C wrappers in
#     libqsoe, so the seam carries only POSIX entry-point .c files plus an
#     empty `syscall.h` shim so musl-internal headers (`stdio_impl.h`, ...)
#     that `#include "syscall.h"` resolve to something innocuous.
#   * Seam .c files reach into libqsoe (`<qsoe-system.h>`, `qsoe_invoke()`)
#     and taskman headers (`sel4_types.h`, `qsoe_invoke.h`) for the IPC
#     glue.  SEAM_INC plumbs those paths in.
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
#
# SPDX-License-Identifier: Apache-2.0

# LIBC: the shared OS-independent libc repository (sibling checkout).
# SEAM: LQ's kernel seam (POSIX entry points) lives right here.
# O:    output lives under the project-wide build/ tree so a single
#       `rm -rf build` from lq/ wipes every artifact.
LIBC := ../../libc
SEAM := $(CURDIR)
O    := $(CURDIR)/../build/libc

# seL4 kernel runs on rv64imac, soft-float lp64 ABI, medany code model;
# the libc body must be built with the same arch/ABI as the rest of LQ.
ARCHFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany

# Seam sources:
#   * Top-level lq/libc/*.c -- POSIX entry points (open/close/read/...).
#   * lq/libc/qsoe/*.c      -- LQ-specific seL4 IPC + QNX-shape API
#                               (formerly userland/libqsoe/src/, folded in
#                               2026-06-01 -- see project_libqsoe_folds_into_libc).
# crt0 is variant-neutral and lives at libc/crt/crt0.S; the shared libc
# Makefile builds it directly.
SEAM_SRC := $(wildcard $(SEAM)/*.c) $(wildcard $(SEAM)/qsoe/*.c)

# Seam includes:
#   $(SEAM)                      -- our own syscall.h stub
#   $(SEAM)/qsoe                 -- internal state.h alongside qsoe/*.c
#   taskman headers              -- sel4_types.h, qsoe_invoke.h (pre-libtaskman)
#   sel4 generated kernel ABI    -- 4 generated headers (arch/api/{invocation,
#                                   syscall,sel4_invocation}.h + api/invocation.h)
#                                   produced by the seL4 kernel build
#
# libqsoe/include/ is intentionally NOT here -- its public surface
# (qsoe-system.h, qsoe/{tls,slots,wire,...}.h, sys/{sync,slog,...}.h)
# is now sourced from the shared libc/include/ that NQ already uses.
LQ_TOP         := $(abspath $(CURDIR)/..)
SEL4_BOOTSTRAP := $(abspath $(LQ_TOP)/..)/sel4-bootstrap
SEL4_GEN_HDRS  := $(SEL4_BOOTSTRAP)/build-qsoe-riscv64/gen_headers
SEAM_INC := $(SEAM) \
            $(SEAM)/qsoe \
            $(SEAM)/include \
            $(LQ_TOP)/taskman \
            $(SEL4_GEN_HDRS)

.PHONY: all clean
# Build libc.a + libc.so + crt0.o.  crt0 is part of libc but sits as a
# standalone object (not in libc.so) so dynamically-linked user programs
# can put it on the command line ahead of -lc.
all:
	$(MAKE) -C $(LIBC) lib crt \
	    O="$(O)" \
	    SEAM_INC="$(SEAM_INC)" \
	    SEAM_SRC="$(SEAM_SRC)" \
	    ARCHFLAGS="$(ARCHFLAGS)"

clean:
	$(MAKE) -C $(LIBC) clean O="$(O)"

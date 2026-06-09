# QSOE — top-level Makefile (iteration v0.1)
#
# Produces build/qsoe.elf, a bootable image for qemu-system-riscv64.
# The image is the seL4 ELF-loader, with a CPIO archive of [kernel.elf,
# taskman] linked into its .rodata section.
#
#   OpenSBI  →  elfloader  →  seL4 kernel  →  taskman (rootserver)
#
# Build inputs:
#   ../sel4-bootstrap/seL4/        — upstream seL4 kernel; cloned by `make
#                                    prepare` (shallow); kernel.elf built
#                                    directly via cmake against it.
#   ../sel4-bootstrap/seL4_tools/  — upstream seL4 tooling; cloned by
#                                    `make prepare` (shallow).  We compile
#                                    elfloader-tool/src/ ourselves with
#                                    rules below; no CMake involvement.
#   ../common/                     — umbrella shared code.  Currently:
#                                    libcpio (vendored from seL4 util_libs;
#                                    see ../common/NOTICE-libcpio.md).  A
#                                    self-referencing symlink `cpio -> .`
#                                    makes both `<cpio.h>` and the
#                                    upstream-style `<cpio/cpio.h>`
#                                    resolve to ../common/cpio.h, so no
#                                    per-OS shim header is needed.
#   taskman/                       — hand-written taskman + vendored
#                                    sel4runtime (taskman/runenv/).
#
# Generated configuration headers (build/gen/) are written by this Makefile
# directly, with the minimum set of CONFIG_ defines the RISC-V elfloader
# actually references.
#
# Targets:
#   make            build build/qsoe.elf            (default)
#   ./emu.sh        boot it under qemu-system-riscv64
#   make kernel     build kernel.elf only
#   make clean      remove build/
#   make distclean  also remove ../sel4test-full/build-qsoe-riscv64/

# ----------------------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------------------

CROSS   := riscv64-linux-gnu-
CC      := $(CROSS)gcc

QEMU    := qemu-system-riscv64

# ----------------------------------------------------------------------------
# Paths
# ----------------------------------------------------------------------------

TOP         := $(CURDIR)
BUILD       := $(TOP)/build
GEN         := $(BUILD)/gen
ELFBUILD    := $(BUILD)/elfloader
TASKBUILD   := $(BUILD)/taskman

# Bootstrap clones live at the QSOE umbrella root (~/proj/QSOE/
# sel4-bootstrap/), one level UP from lq/.  `make prepare` populates
# them on first build; downstream rules reach into them by path.
SEL4_BOOTSTRAP := $(abspath $(TOP)/..)/sel4-bootstrap
SEL4_DIR       := $(SEL4_BOOTSTRAP)/seL4
SEL4_TOOLS_DIR := $(SEL4_BOOTSTRAP)/seL4_tools

# Elfloader sources live INSIDE seL4_tools' clone; we compile them
# ourselves with our own Make rules — no upstream CMake involvement
# for the elfloader.
ELFSRC      := $(SEL4_TOOLS_DIR)/elfloader-tool/src
ELFINCLUDE  := $(SEL4_TOOLS_DIR)/elfloader-tool/include

# libcpio lives in the umbrella's common/ tree (vendored from seL4
# util_libs, BSD-2-Clause; see ../common/NOTICE-libcpio.md).  A
# `cpio -> .` symlink at common/cpio lets the upstream-style include
# `<cpio/cpio.h>` resolve to common/./cpio.h via the same -I path
# that our flat `<cpio.h>` uses -- no per-OS shim header needed.
LIBCPIO     := $(TOP)/../common

TASKMAN_DIR := $(TOP)/taskman

# libc — shared OS-independent body lives in the sibling repo
# ~/proj/QSOE/libc/; LQ's seL4-specific seam (POSIX entry points that
# translate to TM_REQ_* over seL4 IPC) lives in lq/libc/.  The local
# Makefile under lq/libc/ drives the cross-tree build and lands the
# archives under $(BUILD)/libc/.  Replaced the older vendored musl
# tree at core/userland/libc/ on 2026-05-31.
LIBC_DIR     := $(TOP)/libc
LIBC_BUILD   := $(BUILD)/libc
LIBC_A       := $(LIBC_BUILD)/libc.a
LIBC_SO      := $(LIBC_BUILD)/libc.so
LIBC_CRT0    := $(LIBC_BUILD)/crt0.o
LIBC_INCLUDE := $(TOP)/../libc/include

# libtaskman — OS-independent body of every QSOE taskman.  Lives at the
# umbrella root, will graduate to its own gitlab repo (qsoe/libtaskman).
# Linked into taskman.elf; provides pathmgr / cred / syscfg / cpio /
# elf primitives plus init/seams.  See ~/proj/QSOE/libtaskman/CLAUDE.md
# (when written) and project_libtaskman memory.
LIBTASKMAN_DIR := $(abspath $(TOP)/..)/libtaskman
LIBTASKMAN_BUILD := $(BUILD)/libtaskman
LIBTASKMAN_A     := $(LIBTASKMAN_BUILD)/libtaskman.a

# rtld -- the QSOE dynamic linker (ld-qsoe.so.1).  Lives in the shared
# libc/rtld/ tree (OS-independent: walks libc.so's .dynsym at startup
# for POSIX entrypoints, no raw kernel calls).  Borrowed from FreeBSD,
# see project_borrow_rtld memory.
RTLD_DIR        := $(abspath $(TOP)/..)/libc/rtld
RTLD_BUILD      := $(BUILD)/rtld
RTLD_SO         := $(RTLD_BUILD)/ld-qsoe.so.1

# ----------------------------------------------------------------------------
# Target platform (PLAT) — selects the seL4 KernelPlatform and the few
# board-specific knobs that differ between QEMU and real hardware.
#
#   make                 build for qemu-riscv-virt (default; ./emu.sh)
#   make PLAT=hifive     build for the SiFive Unmatched / FU740
#                        (seL4's `hifive` platform); deploy via boot/.
#
# Only two things actually differ per platform at this layer:
#   * KernelPlatform passed to the seL4 cmake (+ QEMU_MEMORY, which is a
#     QEMU-only configure-time knob — the hifive memory map comes from
#     the board DTS, so it must NOT be forced there).
#   * CONFIG_FIRST_HART_ID baked into the elfloader's autoconf.h: the
#     FU740 boots the OS on its four U74 application cores (harts 1..4)
#     while hart 0 is the S7 monitor, so seL4's hifive platform sets
#     FirstHartID=1.  The elfloader uses CONFIG_FIRST_HART_ID to know
#     which secondary harts to release — getting it wrong on hifive
#     would start the monitor hart and skip an application core.
# Per-platform kernels build into separate dirs so switching PLAT
# doesn't reconfigure the other platform's kernel.
# ----------------------------------------------------------------------------
PLAT ?= qemu-riscv-virt

ifeq ($(PLAT),qemu-riscv-virt)
SEL4BUILD       := $(SEL4_BOOTSTRAP)/build-qsoe-riscv64
FIRST_HART_ID   := 0
KERNEL_QEMU_MEM := -DQEMU_MEMORY=512
else ifeq ($(PLAT),hifive)
SEL4BUILD       := $(SEL4_BOOTSTRAP)/build-qsoe-hifive
FIRST_HART_ID   := 1
KERNEL_QEMU_MEM :=
else
$(error unknown PLAT '$(PLAT)' — use qemu-riscv-virt or hifive)
endif

KERNEL_SRC  := $(SEL4BUILD)/kernel.elf
KERNEL_ELF  := $(BUILD)/kernel.elf

IMAGE         := $(BUILD)/qsoe.elf
TASKMAN_ELF   := $(BUILD)/taskman.elf

# Userland module package -- the spawnable-binary archive that taskman
# walks at runtime.  Lives in the sibling quser/ tree (one repo per QRV
# convention) and is built there by `make -C ../quser cpio`.  This LQ
# build does NOT produce its own userland.cpio (retired 2026-05-31).
# Taskman embeds the archive via .incbin (see taskman/Makefile and
# taskman/main.c #else branch); the bytes therefore travel inside
# taskman.elf, no QEMU `-initrd` plumbing.  A vestigial FDT-driven
# loader path is parked behind -DTM_USE_INITRD_LOADER pending
# elfloader work on reserved-memory delivery -- see
# taskman/sys/initrd.c.
QUSER         := $(abspath $(TOP)/..)/quser
MODPKG_CPIO   := $(QUSER)/build/modpkg.cpio
export MODPKG_CPIO

# ----------------------------------------------------------------------------
# Platform configuration (qemu-riscv-virt, RV64)
# ----------------------------------------------------------------------------

# Final address the elfloader relocates itself to. OpenSBI initially loads us
# at 0x80200000 (its default next-stage address), and the seL4 kernel.elf is
# linked to that same physical address. fixup_image_base() in crt0.S sees the
# mismatch and copies the elfloader image up here, clearing the kernel region.
IMAGE_START_ADDR := 0x84000000

# Memory region exposed to the elfloader. 512 MiB starting at 0x80000000.
# Must agree with the `-m` value passed to qemu in the `run` target below.
MEM_START := 0x80000000UL
MEM_END   := 0xA0000000UL

# ----------------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------------

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mstrict-align

EL_CFLAGS := $(ARCH_CFLAGS) \
    -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -D_XOPEN_SOURCE=700 -D__KERNEL_64__ \
    -Wall -Wextra

EL_INCLUDES := \
    -I$(GEN) \
    -I$(ELFINCLUDE) \
    -I$(ELFINCLUDE)/arch-riscv \
    -I$(LIBCPIO)

# Taskman's sel4_types.h / sel4_syscalls.h are our own minimal seL4
# surface, but they transitively #include <arch/api/invocation.h>
# and <arch/api/syscall.h> — generated by the kernel build, 4 files
# under $(SEL4BUILD)/gen_headers/.  Anyone using TM_CFLAGS therefore
# needs that include path.
TM_CFLAGS := $(ARCH_CFLAGS) \
    -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(SEL4BUILD)/gen_headers \
    -I$(LIBC_INCLUDE)

# ----------------------------------------------------------------------------
# Source lists
# ----------------------------------------------------------------------------

# Elfloader sources actually needed for RISC-V (drivers/ is ARM-only
# upstream).  Sources span two roots — upstream seL4_tools and our
# vendored libcpio — so the object-name mapping is done explicitly.
EL_UP_C_SRCS := \
    $(ELFSRC)/common.c \
    $(ELFSRC)/defaults.c \
    $(ELFSRC)/fdt.c \
    $(ELFSRC)/printf.c \
    $(ELFSRC)/string.c \
    $(ELFSRC)/arch-riscv/boot.c \
    $(ELFSRC)/arch-riscv/console.c \
    $(ELFSRC)/binaries/elf/elf.c \
    $(ELFSRC)/binaries/elf/elf32.c \
    $(ELFSRC)/binaries/elf/elf64.c

EL_UP_S_SRCS := $(ELFSRC)/arch-riscv/crt0.S

EL_CPIO_C_SRCS := $(LIBCPIO)/cpio.c

EL_OBJS := \
    $(patsubst $(ELFSRC)/%.c,$(ELFBUILD)/elfloader/%.o,$(EL_UP_C_SRCS)) \
    $(patsubst $(ELFSRC)/%.S,$(ELFBUILD)/elfloader/%.o,$(EL_UP_S_SRCS)) \
    $(patsubst $(LIBCPIO)/%.c,$(ELFBUILD)/cpio/%.o,$(EL_CPIO_C_SRCS))

# ----------------------------------------------------------------------------
# Default target
# ----------------------------------------------------------------------------

.PHONY: all image kernel clean distclean prepare
.DEFAULT_GOAL := all

# `all` triggers `prepare` first so a clean clone is auto-populated
# on the very first build.  `prepare` is idempotent: it checks for
# the bootstrap clones and only fetches what is missing.  modpkg is
# also folded in so a clean `make` produces both qsoe.elf AND the
# userland CPIO that emu.sh hands to QEMU as initrd.
all: prepare image modpkg
image: $(IMAGE)

# ----------------------------------------------------------------------------
# Prepare: shallow-clone seL4 + seL4_tools into ../sel4-bootstrap/.
#
# Two repos.  No `repo` tool, no manifest, no test apps, no musl, no
# CapDL, no nanopb — just the kernel and the elfloader source we
# actually compile.  Total footprint ~50 MB vs ~2 GB for the full
# sel4test-manifest checkout.
#
# Idempotent: existing clones are left alone.  To refresh, either
# `git pull` inside each clone, or `rm -rf ../sel4-bootstrap/` and
# re-run `make prepare`.
# ----------------------------------------------------------------------------

SEL4_URL       := https://github.com/seL4/seL4.git
SEL4_TOOLS_URL := https://github.com/seL4/seL4_tools.git

prepare:
	@if [ ! -d $(SEL4_DIR) ]; then \
	    echo "==> Cloning seL4 kernel into $(SEL4_DIR)..."; \
	    git clone --depth 1 $(SEL4_URL) $(SEL4_DIR); \
	else \
	    echo "==> seL4 already present at $(SEL4_DIR)"; \
	fi
	@if [ ! -d $(SEL4_TOOLS_DIR) ]; then \
	    echo "==> Cloning seL4_tools into $(SEL4_TOOLS_DIR)..."; \
	    git clone --depth 1 $(SEL4_TOOLS_URL) $(SEL4_TOOLS_DIR); \
	else \
	    echo "==> seL4_tools already present at $(SEL4_TOOLS_DIR)"; \
	fi

# ----------------------------------------------------------------------------
# Generated configuration headers
# ----------------------------------------------------------------------------

GEN_HEADERS := \
    $(GEN)/autoconf.h \
    $(GEN)/elfloader/gen_config.h \
    $(GEN)/image_start_addr.h \
    $(GEN)/platform_info.h

# Track the Makefile itself so that editing values (like IMAGE_START_ADDR)
# in the variable block above forces the generated headers to regenerate.
THIS_MAKEFILE := $(firstword $(MAKEFILE_LIST))

# build/gen/ is shared across platforms, but autoconf.h bakes in
# CONFIG_FIRST_HART_ID (PLAT-dependent).  A `make PLAT=hifive` after a
# qemu build doesn't touch the Makefile, so the gen headers wouldn't
# otherwise regenerate with the new hart-id.  This PLAT-named stamp is
# absent after a platform switch, re-firing the gen-header rules (and
# the downstream elfloader objects + qsoe.elf link).
PLAT_STAMP := $(GEN)/.plat-$(PLAT)
$(PLAT_STAMP):
	@mkdir -p $(@D) && rm -f $(GEN)/.plat-* && touch $@

$(GEN_HEADERS): $(THIS_MAKEFILE) $(PLAT_STAMP)

$(GEN)/autoconf.h:
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define CONFIG_PT_LEVELS         3' \
	    '#define CONFIG_FIRST_HART_ID     $(FIRST_HART_ID)' \
	    '#define CONFIG_MAX_NUM_NODES     4' \
	    '#define CONFIG_ENABLE_SMP_SUPPORT 1' \
	    > $@

$(GEN)/elfloader/gen_config.h:
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define CONFIG_HASH_NONE 1' \
	    > $@

$(GEN)/image_start_addr.h:
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define IMAGE_START_ADDR $(IMAGE_START_ADDR)' \
	    > $@

$(GEN)/platform_info.h:
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    'struct memory_region {' \
	    '    unsigned long start;' \
	    '    unsigned long end;' \
	    '};' \
	    'static const struct memory_region memory_region[] = {' \
	    '    { $(MEM_START), $(MEM_END) },' \
	    '};' \
	    'static const int num_memory_regions = 1;' \
	    > $@

# ----------------------------------------------------------------------------
# Elfloader: per-file compile rules
# ----------------------------------------------------------------------------

# Two source roots — upstream elfloader and our vendored libcpio —
# get separate rules so the object-tree under $(ELFBUILD) mirrors
# the source layout cleanly.
$(ELFBUILD)/elfloader/%.o: $(ELFSRC)/%.c $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/elfloader/%.o: $(ELFSRC)/%.S $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/cpio/%.o: $(LIBCPIO)/%.c $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

# ----------------------------------------------------------------------------
# Elfloader: preprocess linker script
# ----------------------------------------------------------------------------

$(ELFBUILD)/linker.lds_pp: $(ELFSRC)/linker.lds $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -P -E -x c -o $@ $<

# ----------------------------------------------------------------------------
# Taskman (rootserver — currently just spins)
# ----------------------------------------------------------------------------

# Headers shared across taskman/libqsoe TUs.
# v0.7: taskman was split into sys/ proc/ mem/ path/ subdirs; the
# headers now live under those.  Keep them all in TM_HEADERS as the
# coarse "if anything changed, rebuild" trigger.
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
    $(TASKMAN_DIR)/sys/platform.h \
    $(LIBC_INCLUDE)/qsoe-system.h \
    $(LIBC_INCLUDE)/qsoe/slots.h \
    $(LIBC_INCLUDE)/qsoe/tls.h \
    $(LIBC_INCLUDE)/qsoe/wire.h \
    $(GEN)/qsoe/sys_version.h

# Auto-generated version header. Pulls the latest git tag (vMAJOR.MINOR[.PATCH])
# and emits the numeric components + a build date. Regenerates whenever
# .git/HEAD or .git/index changes (new commit, branch switch, tag bump).
$(GEN)/qsoe/sys_version.h: $(wildcard .git/HEAD .git/index)
	@mkdir -p $(@D)
	@VERSION=$$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0"); \
	 VERSION="$${VERSION#v}"; \
	 MAJOR="$${VERSION%%.*}"; \
	 TEMP="$${VERSION#*.}"; \
	 MINOR="$${TEMP%%[!0-9]*}"; \
	 case "$$TEMP" in *.*) PATCH="$${TEMP#*.}";; *) PATCH=0;; esac; \
	 printf '/* Auto-generated from `git describe --tags --abbrev=0`. Do not edit. */\n' > $@; \
	 printf '#ifndef QSOE_SYS_VERSION_H\n#define QSOE_SYS_VERSION_H\n\n' >> $@; \
	 printf '#define QSOE_VERSION_STRING "%s"\n' "$$VERSION" >> $@; \
	 printf '#define QSOE_VERSION_MAJOR %s\n' "$$MAJOR" >> $@; \
	 printf '#define QSOE_VERSION_MINOR %s\n' "$$MINOR" >> $@; \
	 printf '#define QSOE_VERSION_PATCH %s\n' "$$PATCH" >> $@; \
	 printf '#define QSOE_BUILD_DATE "%s"\n\n' "$$(date +%Y-%m-%d)" >> $@; \
	 printf '#endif\n' >> $@

# ----------------------------------------------------------------------------
# libc — build delegated to lq/libc/Makefile, which drives the shared
# OS-independent body in ~/proj/QSOE/libc/ with LQ's seam.
#
# Run `make -C libc clean all` for a standalone rebuild.
# ----------------------------------------------------------------------------

AR := $(CROSS)ar

.PHONY: libc
libc:
	+$(MAKE) -C $(LIBC_DIR) all

# Order-only proxy: downstream rules listing $(LIBC_A) / $(LIBC_SO) /
# $(LIBC_CRT0) as a prerequisite trigger the libc submake.
$(LIBC_A) $(LIBC_SO) $(LIBC_CRT0): | libc
	@true

# libqsoe — retired 2026-06-01.  User-mode primitives folded into the
# libc seam at lq/libc/qsoe/; taskman absorbed its own private copies
# under taskman/qsoe/.  See project_libqsoe_folds_into_libc.

# taskman — build delegated to taskman/Makefile.  Embeds the quser-
# built modpkg.cpio via .incbin (the path retired 2026-05-31 then
# restored 2026-06-01; see taskman/Makefile top-of-file).
# Depends on $(MODPKG_CPIO) so changing a quser binary triggers a
# taskman re-link.
.PHONY: rtld
rtld: $(RTLD_SO)

# Build ld-qsoe.so.1 from the shared libc/rtld/ tree.  Pure userland
# shared object -- no taskman/seL4 dependency at build time.  LIBC_INC
# plumbs <qsoe-system.h>.
$(RTLD_SO):
	@mkdir -p $(RTLD_BUILD)
	+$(MAKE) -C $(RTLD_DIR) \
	    O=$(RTLD_BUILD) \
	    LIBC_INC=$(LIBC_INCLUDE) \
	    EXTRA_CPPFLAGS=-DQSOE_KERNEL_SEL4 \
	    ARCHFLAGS="-march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany" \
	    all

.PHONY: libtaskman
libtaskman: $(LIBTASKMAN_A)

# Build libtaskman.a from the umbrella-root tree.  Taskman is a freestanding
# static-link client, so override PICFLAG to -fno-pic to match the rest of
# taskman.  LIBC_INC plumbs <qsoe-system.h> in.
$(LIBTASKMAN_A):
	@mkdir -p $(LIBTASKMAN_BUILD)
	+$(MAKE) -C $(LIBTASKMAN_DIR) \
	    O=$(LIBTASKMAN_BUILD) \
	    LIBC_INC=$(LIBC_INCLUDE) \
	    PICFLAG=-fno-pic \
	    ARCHFLAGS="$(ARCH_CFLAGS)" \
	    all

.PHONY: taskman
taskman: $(MODPKG_CPIO) $(LIBTASKMAN_A) $(GEN)/qsoe/sys_version.h
	+$(MAKE) -C $(TOP)/taskman all LIBTASKMAN_A=$(LIBTASKMAN_A) LIBTASKMAN_INC=$(LIBTASKMAN_DIR)/include

$(TASKMAN_ELF): | taskman
	@true

# Standalone tester (lq/userland/tester) retired 2026-06-01.  The same
# exercises now live in the umbrella's quser/test/suite/ alongside
# QRV's syscall conformance suite, build into a single `suite` binary
# that travels via modpkg.cpio and runs on both NQ and LQ.

# ----------------------------------------------------------------------------
# Spawnable userland (init.sh, qsh, devc-ser8250, sbin/{pipe,repath,slogger,
# pci-server}, sloginfo, libpci, utils) lives in the umbrella-level
# quser/ tree.  Its CPIO archive (modpkg.cpio, QRV-style name) is built
# by `make -C ../quser cpio` and embedded into taskman.elf via .incbin
# (see taskman/Makefile -- restored 2026-06-01 from the brief
# FDT-initrd interlude).  See top-of-file MODPKG_CPIO note.
# ----------------------------------------------------------------------------

.PHONY: modpkg
modpkg: $(MODPKG_CPIO)

# Always re-enter quser's submake so per-component changes propagate;
# quser is responsible for its own incremental rebuild discipline.
# Pass LQ's own libc.so + rtld so they ship inside modpkg.cpio's /lib/
# tree -- needed at load time by every dynamically-linked binary
# (qsh + drivers + utils).  Regular deps on $(LIBC_SO) / $(RTLD_SO)
# (not order-only) so an updated libc.so triggers cpio rebuild + the
# downstream taskman .incbin re-link.
$(MODPKG_CPIO): $(LIBC_SO) $(LIBC_CRT0) $(RTLD_SO)
	+$(MAKE) -C $(QUSER) cpio \
	    LIBC_SO=$(LIBC_SO) \
	    RTLD_SO=$(RTLD_SO) \
	    DYNLIBC_SO=$(LIBC_SO)

# ----------------------------------------------------------------------------
# Kernel: built directly via cmake against ../sel4-bootstrap/seL4/.
#
# No sel4test layer, no init-build.sh, no settings.cmake from
# projects/sel4test/.  The seL4 kernel's own CMakeLists.txt declares
# `project(seL4 C ASM)` -- no CXX, so no g++-riscv64-linux-gnu host
# requirement and no sed hacks.  We feed it the Kernel* cache vars
# directly; the kernel build produces $(SEL4BUILD)/kernel.elf, which
# we copy into our build tree at $(BUILD)/kernel.elf.
# ----------------------------------------------------------------------------

kernel: $(KERNEL_ELF)

# $(BUILD)/kernel.elf is shared across platforms (it feeds archive.cpio).
# The PLAT_STAMP prerequisite forces this copy to re-run on a platform
# switch — otherwise, after `make PLAT=hifive` left the hifive kernel
# here, a plain `make` would see build/kernel.elf newer than the qemu
# $(KERNEL_SRC) and skip the copy, silently embedding the WRONG
# platform's kernel (a hifive kernel under qemu-virt faults with
# scause=7 on phantom RAM — the QEMU_MEMORY-mismatch symptom).
$(KERNEL_ELF): $(KERNEL_SRC) $(PLAT_STAMP)
	@mkdir -p $(@D)
	cp $(KERNEL_SRC) $@

$(KERNEL_SRC): | prepare
	@echo "==> Configuring seL4 kernel (one-time, ~1 min)..."
	@mkdir -p $(SEL4BUILD)
	@# Kernel cache vars (Kernel*):
	@#   KernelPlatform        — qemu-riscv-virt selects the virt machine
	@#   KernelSel4Arch        — riscv64 selects RV64
	@#   KernelMaxNumNodes=4   — SMP, 4 harts (must match -smp passed
	@#                            to qemu in ./emu.sh)
	@#   KernelIsMCS=ON        — MCS scheduler: time as a first-class
	@#                            citizen (sched contexts, reply objects,
	@#                            Wait-with-timeout).  Adopted for QSOE/L
	@#                            v0.10; see project_mcs memory.
	@#   KernelVerificationBuild=OFF — keep debug syscalls (printf etc.)
	@#                                  available; the verification mode
	@#                                  strips them.
	@#   QEMU_MEMORY=512       — MiB.  The kernel build runs qemu with
	@#                            `-m $QEMU_MEMORY` at CONFIGURE TIME
	@#                            to extract the DTS and bakes the
	@#                            resulting memory map into the kernel
	@#                            image.  MUST match the `-m 512M`
	@#                            passed by ./emu.sh at RUN TIME --
	@#                            otherwise the kernel maps phantom RAM
	@#                            beyond the qemu-allocated range and
	@#                            faults on first access (scause=7
	@#                            store/AMO access fault).
	@#                            qemu-riscv-virt's default is 3 GiB
	@#                            which exceeds our launch config.
	@# seL4's gcc.cmake is a template (configure_file expects @var@s to
	@# be expanded by the outer project); without that pre-pass the
	@# toolchain falls through to host gcc and fails with riscv flags.
	@# Bypass it by setting CROSS_COMPILER_PREFIX directly + a tiny
	@# CMAKE_TOOLCHAIN_FILE that only sets CMAKE_SYSTEM_NAME.
	cd $(SEL4BUILD) && cmake -G Ninja \
	    -DCMAKE_SYSTEM_NAME=Generic \
	    -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc \
	    -DCMAKE_ASM_COMPILER=riscv64-linux-gnu-gcc \
	    -DCROSS_COMPILER_PREFIX=riscv64-linux-gnu- \
	    -DKernelPlatform=$(PLAT) \
	    -DKernelSel4Arch=riscv64 \
	    -DKernelMaxNumNodes=4 \
	    -DKernelIsMCS=ON \
	    -DKernelVerificationBuild=OFF \
	    $(KERNEL_QEMU_MEM) \
	    $(SEL4_DIR)
	cd $(SEL4BUILD) && ninja kernel.elf

# ----------------------------------------------------------------------------
# CPIO archive: [kernel.elf, taskman] → archive.cpio → archive.o
# ----------------------------------------------------------------------------

$(BUILD)/archive.cpio: $(KERNEL_ELF) $(TASKMAN_ELF)
	@mkdir -p $(@D)
	@cd $(BUILD) && \
	    printf '%s\n' kernel.elf taskman.elf | \
	    cpio --quiet --create -H newc \
	         --owner=+0:+0 --reproducible \
	         --file=archive.cpio

$(ELFBUILD)/archive.S: $(BUILD)/archive.cpio $(firstword $(MAKEFILE_LIST))
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '.section ._archive_cpio,"aw"' \
	    '.globl _archive_start, _archive_start_end' \
	    '_archive_start:' \
	    '.incbin "$<"' \
	    '_archive_start_end:' \
	    > $@

$(ELFBUILD)/archive.o: $(ELFBUILD)/archive.S
	$(CC) $(EL_CFLAGS) -c -o $@ $<

# ----------------------------------------------------------------------------
# Final link
# ----------------------------------------------------------------------------

$(IMAGE): $(EL_OBJS) $(ELFBUILD)/archive.o $(ELFBUILD)/linker.lds_pp
	$(CC) $(EL_CFLAGS) -static -nostdlib \
	    -Wl,-T,$(ELFBUILD)/linker.lds_pp \
	    -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments \
	    -o $@ $(EL_OBJS) $(ELFBUILD)/archive.o

# ----------------------------------------------------------------------------
# Run under QEMU — moved out to ./emu.sh once the device list grew past
# a one-liner (NVMe drive image, gdb stub toggle, etc.).
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Cleanup
# ----------------------------------------------------------------------------

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SEL4BUILD)

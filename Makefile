# QSOE — top-level Makefile (iteration v0.1)
#
# Produces build/qsoe-l-<board>.elf for every board selected in Kconfig
# (qsoe-l-qemu.elf by default; qsoe-l-sifive.elf for the FU740).  Boards
# are chosen via `make menuconfig` (multi-select -- you can build one or
# both at once).  The image is the seL4 ELF-loader, with a CPIO archive
# of [kernel.elf, taskman] linked into its .rodata section.
#
#   OpenSBI  →  elfloader  →  seL4 kernel  →  taskman (rootserver)
#
# The userspace (libc, rtld, libtaskman, taskman, modpkg.cpio) is
# board-independent and built ONCE; only the seL4 kernel and the
# elfloader (which bakes in the board's FirstHartID) are per-board.
#
# Build inputs:
#   ../sel4-bootstrap/seL4/        — upstream seL4 kernel; cloned by `make
#                                    prepare` (shallow); kernel.elf built
#                                    directly via cmake against it.
#   ../sel4-bootstrap/seL4_tools/  — upstream seL4 tooling; cloned by
#                                    `make prepare` (shallow).  We compile
#                                    elfloader-tool/src/ ourselves with
#                                    rules below; no CMake involvement.
#   ../common/                     — umbrella shared code (libcpio).
#   taskman/                       — hand-written taskman + sel4runtime.
#
# Targets:
#   make                  build every selected board's qsoe-l-<board>.elf
#   make menuconfig       choose the target board(s) (QEMU virt / SiFive)
#   make qemu_defconfig   QEMU virt only (the default)
#   make sifive_defconfig SiFive Unmatched (FU740) only
#   make both_defconfig   build both boards
#   ./emu.sh              boot the qemu image under qemu-system-riscv64
#   make clean            remove build/  (.config is preserved)
#   make distclean        also remove the seL4 kernel build dirs

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
TASKBUILD   := $(BUILD)/taskman

# Bootstrap clones live at the QSOE umbrella root (~/proj/QSOE/
# sel4-bootstrap/), one level UP from lq/.  `make prepare` populates them.
SEL4_BOOTSTRAP := $(abspath $(TOP)/..)/sel4-bootstrap
SEL4_DIR       := $(SEL4_BOOTSTRAP)/seL4
SEL4_TOOLS_DIR := $(SEL4_BOOTSTRAP)/seL4_tools

# FU740 seL4 source patches (applied by the $(SEL4_PATCH_STAMP) rule below,
# before any seL4 cmake/ninja build).  Defined here so the kernel build rule's
# prerequisite resolves.
SEL4_HIFIVE_CMK  := $(SEL4_DIR)/src/plat/hifive/config.cmake
SEL4_PLIC_H      := $(SEL4_DIR)/include/drivers/irq/riscv_plic0.h
SEL4_PATCH_STAMP := $(SEL4_DIR)/.qsoe-fu740-patched

# Elfloader sources live INSIDE seL4_tools' clone; we compile them
# ourselves with our own Make rules — no upstream CMake involvement.
ELFSRC      := $(SEL4_TOOLS_DIR)/elfloader-tool/src
ELFINCLUDE  := $(SEL4_TOOLS_DIR)/elfloader-tool/include

# libcpio lives in the umbrella's common/ tree (vendored from seL4
# util_libs, BSD-2-Clause; see ../common/NOTICE-libcpio.md).
LIBCPIO     := $(TOP)/../common

TASKMAN_DIR := $(TOP)/taskman

# libc — shared OS-independent body lives in the sibling repo
# ~/proj/QSOE/libc/; LQ's seL4-specific seam lives in lq/libc/.
LIBC_DIR     := $(TOP)/libc
LIBC_BUILD   := $(BUILD)/libc
LIBC_A       := $(LIBC_BUILD)/libc.a
LIBC_SO      := $(LIBC_BUILD)/libc.so
LIBC_CRT0    := $(LIBC_BUILD)/crt0.o
LIBC_INCLUDE := $(TOP)/../libc/include

# libtaskman — OS-independent body of every QSOE taskman.
LIBTASKMAN_DIR   := $(abspath $(TOP)/..)/libtaskman
LIBTASKMAN_BUILD := $(BUILD)/libtaskman
LIBTASKMAN_A     := $(LIBTASKMAN_BUILD)/libtaskman.a

# rtld -- the QSOE dynamic linker (ld-qsoe.so.1).
RTLD_DIR        := $(abspath $(TOP)/..)/libc/rtld
RTLD_BUILD      := $(BUILD)/rtld
RTLD_SO         := $(RTLD_BUILD)/ld-qsoe.so.1

# Userland module package (board-independent), built in the quser/ tree.
QUSER         := $(abspath $(TOP)/..)/quser
MODPKG_CPIO   := $(QUSER)/build/modpkg.cpio
export MODPKG_CPIO

# taskman.elf is board-independent (the seL4 API it builds against is the
# same on every platform) and therefore built once and shared.
TASKMAN_ELF   := $(BUILD)/taskman.elf

# ----------------------------------------------------------------------------
# Board selection — Kconfig (lq/Kconfig -> lq/.config), multi-select.
# `make menuconfig`, or `make {qemu,sifive,both}_defconfig`.  .config lives
# at the repo root (survives `make clean`); -include pulls CONFIG_* in at
# parse time.  Absent (fresh checkout) the qemu default applies, and the
# `.config` rule bootstraps one via alldefconfig.
# ----------------------------------------------------------------------------
-include $(TOP)/.config

CONFIG_MAX_NUM_NODES ?= 4               # parse-time default for a fresh tree

BOARDS :=
ifeq ($(CONFIG_PLAT_QEMU_VIRT),y)
BOARDS += qemu
endif
ifeq ($(CONFIG_PLAT_SIFIVE),y)
BOARDS += sifive
endif

# ----------------------------------------------------------------------------
# Kconfig drive -- python3-kconfiglib, same as QSOE/N (nq/Makefile).
# ----------------------------------------------------------------------------
KCFG_LIB := /usr/lib/python3/dist-packages
KCFG_ENV := KCONFIG_CONFIG=$(TOP)/.config \
            KCONFIG_AUTOHEADER=$(GEN)/autoconf.h \
            srctree=$(TOP)

define kcfg_genheader
	@mkdir -p $(GEN)
	@$(KCFG_ENV) python3 $(KCFG_LIB)/genconfig.py --header-path $(GEN)/autoconf.h $(TOP)/Kconfig
endef

# ----------------------------------------------------------------------------
# Per-board values.  BOARD is set by the outer make when it recurses into
# a single board (board-% target); empty in the outer (dispatch) make.
# The seL4 kernel rules below derive KernelPlatform/QEMU_MEMORY from the
# build-dir name via target-specific variables, so they are independent of
# BOARD and work for both the per-board kernels and the shared taskman's
# (qemu) API headers.
# ----------------------------------------------------------------------------
ifeq ($(BOARD),sifive)
FIRST_HART_ID   := 1
SEL4BUILD       := $(SEL4_BOOTSTRAP)/build-qsoe-hifive
else
FIRST_HART_ID   := 0
SEL4BUILD       := $(SEL4_BOOTSTRAP)/build-qsoe-riscv64
endif

BBUILD      := $(BUILD)/$(BOARD)
ELFBUILD    := $(BBUILD)/elfloader
KERNEL_SRC  := $(SEL4BUILD)/kernel.elf
KERNEL_ELF  := $(BBUILD)/kernel.elf
IMAGE       := $(BUILD)/qsoe-l-$(BOARD).elf
BOOT_BIN    := $(BUILD)/qsoe-l-$(BOARD).bin

# taskman's seL4 ABI headers come from the qemu build dir (the seL4 API is
# board-independent, so this is correct for a sifive-only build too).
TM_SEL4BUILD := $(SEL4_BOOTSTRAP)/build-qsoe-riscv64

# ----------------------------------------------------------------------------
# Platform constants (board-independent today)
# ----------------------------------------------------------------------------

# Final address the elfloader relocates itself to (links + enters here).
IMAGE_START_ADDR := 0x84000000

# Memory region exposed to the elfloader. 512 MiB starting at 0x80000000.
MEM_START := 0x80000000UL
MEM_END   := 0xA0000000UL

# ----------------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------------

ARCH_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mstrict-align

# CONFIG_FIRST_HART_ID is injected here (per board) rather than via
# autoconf.h, since autoconf.h is shared and a single value could not be
# correct when both boards are built.
EL_CFLAGS := $(ARCH_CFLAGS) \
    -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -D_XOPEN_SOURCE=700 -D__KERNEL_64__ \
    -DCONFIG_FIRST_HART_ID=$(FIRST_HART_ID) \
    -Wall -Wextra

EL_INCLUDES := \
    -I$(GEN) \
    -I$(ELFINCLUDE) \
    -I$(ELFINCLUDE)/arch-riscv \
    -I$(LIBCPIO)

# ----------------------------------------------------------------------------
# Source lists
# ----------------------------------------------------------------------------

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

# Resolves under the per-board $(ELFBUILD) when BOARD is set (inner make).
EL_OBJS := \
    $(patsubst $(ELFSRC)/%.c,$(ELFBUILD)/elfloader/%.o,$(EL_UP_C_SRCS)) \
    $(patsubst $(ELFSRC)/%.S,$(ELFBUILD)/elfloader/%.o,$(EL_UP_S_SRCS)) \
    $(patsubst $(LIBCPIO)/%.c,$(ELFBUILD)/cpio/%.o,$(EL_CPIO_C_SRCS))

THIS_MAKEFILE := $(firstword $(MAKEFILE_LIST))

# ----------------------------------------------------------------------------
# Generated configuration headers (board-independent, shared in build/gen).
# ----------------------------------------------------------------------------

GEN_HEADERS := \
    $(GEN)/autoconf.h \
    $(GEN)/elfloader/gen_config.h \
    $(GEN)/image_start_addr.h \
    $(GEN)/platform_info.h

# autoconf.h is generated from Kconfig (lq/Kconfig + lq/.config) by
# kconfiglib's genconfig, mirroring QSOE/N.  Board-independent.
$(GEN)/autoconf.h: $(TOP)/.config $(TOP)/Kconfig
	@mkdir -p $(@D)
	@echo "  KCFG    $@"
	@$(KCFG_ENV) python3 $(KCFG_LIB)/genconfig.py --header-path $@ $(TOP)/Kconfig

$(GEN)/elfloader/gen_config.h: $(THIS_MAKEFILE)
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define CONFIG_HASH_NONE 1' \
	    > $@

$(GEN)/image_start_addr.h: $(THIS_MAKEFILE)
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define IMAGE_START_ADDR $(IMAGE_START_ADDR)' \
	    > $@

$(GEN)/platform_info.h: $(THIS_MAKEFILE)
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
# Elfloader: per-file compile rules (objects land under the per-board
# $(ELFBUILD); EL_CFLAGS carries that board's CONFIG_FIRST_HART_ID).
# ----------------------------------------------------------------------------

$(ELFBUILD)/elfloader/%.o: $(ELFSRC)/%.c $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/elfloader/%.o: $(ELFSRC)/%.S $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/cpio/%.o: $(LIBCPIO)/%.c $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/linker.lds_pp: $(ELFSRC)/linker.lds $(GEN_HEADERS) | prepare
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -P -E -x c -o $@ $<

# ----------------------------------------------------------------------------
# seL4 kernel: built via cmake against ../sel4-bootstrap/seL4/.  One build
# dir per board; KernelPlatform + QEMU_MEMORY come from target-specific
# variables keyed on the dir name, so these rules don't depend on $(BOARD)
# (the shared taskman triggers the qemu dir; each board triggers its own).
# ----------------------------------------------------------------------------

$(SEL4_BOOTSTRAP)/build-qsoe-riscv64/kernel.elf: KPLAT := qemu-riscv-virt
$(SEL4_BOOTSTRAP)/build-qsoe-riscv64/kernel.elf: KMEM  := -DQEMU_MEMORY=512
$(SEL4_BOOTSTRAP)/build-qsoe-hifive/kernel.elf:  KPLAT := hifive
$(SEL4_BOOTSTRAP)/build-qsoe-hifive/kernel.elf:  KMEM  :=

# QEMU_MEMORY=512 (MiB) is baked into the qemu kernel at CONFIGURE time and
# MUST match `-m 512M` in ./emu.sh, else the kernel maps phantom RAM and
# faults (scause=7).  The hifive memory map comes from the board DTS, so
# QEMU_MEMORY must NOT be forced there.
$(SEL4_BOOTSTRAP)/build-qsoe-%/kernel.elf: | prepare $(SEL4_PATCH_STAMP)
	@echo "==> Configuring + building seL4 kernel in $(@D) (one-time cmake ~1 min)..."
	@mkdir -p $(@D)
	cd $(@D) && cmake -G Ninja \
	    -DCMAKE_SYSTEM_NAME=Generic \
	    -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc \
	    -DCMAKE_ASM_COMPILER=riscv64-linux-gnu-gcc \
	    -DCROSS_COMPILER_PREFIX=riscv64-linux-gnu- \
	    -DKernelPlatform=$(KPLAT) \
	    -DKernelSel4Arch=riscv64 \
	    -DKernelMaxNumNodes=$(CONFIG_MAX_NUM_NODES) \
	    -DKernelIsMCS=ON \
	    -DKernelVerificationBuild=OFF \
	    $(KMEM) \
	    $(SEL4_DIR)
	cd $(@D) && ninja kernel.elf

# Per-board copy into the board's build subtree (feeds archive.cpio).
$(KERNEL_ELF): $(KERNEL_SRC)
	@mkdir -p $(@D)
	cp $(KERNEL_SRC) $@

# ----------------------------------------------------------------------------
# CPIO archive: [kernel.elf, taskman.elf] → archive.cpio → archive.o.
# Per board (its kernel + the shared taskman.elf).
# ----------------------------------------------------------------------------

$(BBUILD)/archive.cpio: $(KERNEL_ELF) $(TASKMAN_ELF)
	@mkdir -p $(@D)
	@cp $(TASKMAN_ELF) $(BBUILD)/taskman.elf
	@cd $(BBUILD) && \
	    printf '%s\n' kernel.elf taskman.elf | \
	    cpio --quiet --create -H newc \
	         --owner=+0:+0 --reproducible \
	         --file=archive.cpio

$(ELFBUILD)/archive.S: $(BBUILD)/archive.cpio $(THIS_MAKEFILE)
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
# Final link → build/qsoe-l-<board>.elf  (+ the raw .bin for SiFive).
# ----------------------------------------------------------------------------

$(IMAGE): $(EL_OBJS) $(ELFBUILD)/archive.o $(ELFBUILD)/linker.lds_pp
	$(CC) $(EL_CFLAGS) -static -nostdlib \
	    -Wl,-T,$(ELFBUILD)/linker.lds_pp \
	    -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments \
	    -o $@ $(EL_OBJS) $(ELFBUILD)/archive.o

$(BOOT_BIN): $(IMAGE)
	$(CROSS)objcopy -O binary $< $@
	@echo "  BIN     $@  (QSOE/L on FU740; U-Boot load + go @ $(IMAGE_START_ADDR))"

# ----------------------------------------------------------------------------
# prepare: shallow-clone seL4 + seL4_tools into ../sel4-bootstrap/.
# Idempotent: existing clones are left alone.
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
# FU740 patches for the vendored seL4 kernel.  seL4's "hifive" platform targets
# the FU540 (Unleashed); two things are wrong for the FU740 (Unmatched).  Per
# the vendoring convention these are sed'd in-place once per clone, recorded by
# a stamp inside the seL4 tree so a fresh `make prepare` re-clone re-applies
# them (the stamp dies with the tree).  The MAX_IRQ bump is in the hifive-only
# config.cmake; the PLIC fix touches the shared driver header but is benign on
# qemu-virt (whose claim auto-completes in-kernel).
#
#   1. MAX_IRQ 53 -> 128.  The FU540 PLIC stops at 53; the FU740 has more and
#      PCIe MSI lands above 53, so pci-server / NVMe never start.  seL4 derives
#      PLIC_MAX_IRQ, maxIRQ and the IRQ-cnode size from this one value.
#   2. PLIC complete-on-claiming-hart.  seL4 completes a PLIC claim on whichever
#      hart the userspace IRQHandler_Ack runs on; on SMP that may differ from
#      the hart that took the IRQ, so a level-triggered line never re-arms
#      (serial: one char, then silence).  Record the claiming hart per IRQ and
#      complete to it -- mirrors QRV/Skimmer.  NQ/Skimmer has no such bug.
# ----------------------------------------------------------------------------
$(SEL4_PATCH_STAMP): | prepare
	@echo "==> Patching vendored seL4 for FU740 (MAX_IRQ=128, PLIC complete-on-claiming-hart)..."
	sed -i 's/MAX_IRQ 53/MAX_IRQ 128/' $(SEL4_HIFIVE_CMK)
	sed -i '/^static inline irq_t plic_get_claim(void)/i static word_t plic_claim_hart[PLIC_MAX_IRQ + 1]; /* QSOE FU740: hart that claimed each IRQ */' $(SEL4_PLIC_H)
	sed -i 's|    return readl(PLIC_PPTR_BASE + plic_claim_offset(hart_id, PLIC_SVC_CONTEXT));|    irq_t claimed = readl(PLIC_PPTR_BASE + plic_claim_offset(hart_id, PLIC_SVC_CONTEXT));\n    if ((word_t)claimed <= (word_t)PLIC_MAX_IRQ) { plic_claim_hart[claimed] = hart_id; }\n    return claimed;|' $(SEL4_PLIC_H)
	sed -i 's|    writel(irq, PLIC_PPTR_BASE + plic_claim_offset(hart_id, PLIC_SVC_CONTEXT));|    if ((word_t)irq <= (word_t)PLIC_MAX_IRQ) { hart_id = plic_claim_hart[irq]; }\n    writel(irq, PLIC_PPTR_BASE + plic_claim_offset(hart_id, PLIC_SVC_CONTEXT));|' $(SEL4_PLIC_H)
	@touch $@

ifeq ($(BOARD),)
# ============================================================================
# OUTER make (BOARD unset): build the shared userspace once, then recurse
# into each selected board to build its kernel + elfloader + ELF.
# ============================================================================

# Goals that must run with NO board selected -- the Kconfig front-ends (they
# are how you GET a selection) and the clean targets.  $(error) is evaluated at
# parse time, so without this it would fire for `make qemu_defconfig` itself.
CONFIG_GOALS := menuconfig defconfig qemu_defconfig sifive_defconfig \
                both_defconfig clean distclean

# Fire only for a genuine misconfiguration: a .config that EXISTS but selects no
# board.  A fresh tree with no .config is not an error -- the $(TOP)/.config rule
# below bootstraps the qemu default and make re-reads.  And never block the
# config/clean goals above.
ifeq ($(strip $(BOARDS)),)
ifeq ($(filter $(CONFIG_GOALS),$(MAKECMDGOALS)),)
ifneq ($(wildcard $(TOP)/.config),)
$(error No target board selected — run `make menuconfig` and enable at least \
one board, or use `make qemu_defconfig` / `sifive_defconfig` / `both_defconfig`)
endif
endif
endif

.PHONY: all shared prepare clean distclean libc rtld libtaskman taskman modpkg \
        menuconfig defconfig qemu_defconfig sifive_defconfig both_defconfig \
        $(addprefix board-,$(BOARDS))
.DEFAULT_GOAL := all

BOARD_TARGETS := $(addprefix board-,$(BOARDS))

all: prepare shared $(BOARD_TARGETS)

# Everything board-independent: built once, before any board recurses.
shared: $(GEN_HEADERS) $(LIBC_A) $(LIBC_SO) $(LIBC_CRT0) $(RTLD_SO) \
        $(LIBTASKMAN_A) $(TASKMAN_ELF) $(MODPKG_CPIO)

# Static-pattern (not implicit) rule so it still fires for these .PHONY
# targets -- GNU make skips implicit/pattern rules for phony targets.
$(BOARD_TARGETS): board-%: shared
	+$(MAKE) BOARD=$* __image

# ---- Kconfig front-ends -----------------------------------------------------
menuconfig:
	@dpkg -s python3-kconfiglib >/dev/null 2>&1 || \
		{ echo "Error: python3-kconfiglib not installed.";       \
		  echo "  apt install python3-kconfiglib"; exit 1; }
	$(KCFG_ENV) python3 $(KCFG_LIB)/menuconfig.py $(TOP)/Kconfig
	$(call kcfg_genheader)

defconfig qemu_defconfig:
	$(KCFG_ENV) python3 $(KCFG_LIB)/alldefconfig.py $(TOP)/Kconfig
	$(call kcfg_genheader)

sifive_defconfig:
	$(KCFG_ENV) python3 $(KCFG_LIB)/alldefconfig.py $(TOP)/Kconfig
	$(KCFG_ENV) python3 $(KCFG_LIB)/setconfig.py --kconfig $(TOP)/Kconfig \
	    PLAT_QEMU_VIRT=n PLAT_SIFIVE=y
	$(call kcfg_genheader)

both_defconfig:
	$(KCFG_ENV) python3 $(KCFG_LIB)/alldefconfig.py $(TOP)/Kconfig
	$(KCFG_ENV) python3 $(KCFG_LIB)/setconfig.py --kconfig $(TOP)/Kconfig \
	    PLAT_SIFIVE=y
	$(call kcfg_genheader)

# Bootstrap: a fresh checkout with no .config gets the qemu defaults.
$(TOP)/.config:
	@echo "  KCFG    (bootstrap: alldefconfig -> .config)"
	@$(KCFG_ENV) python3 $(KCFG_LIB)/alldefconfig.py $(TOP)/Kconfig

# ---- shared userspace -------------------------------------------------------
AR := $(CROSS)ar

libc:
	+$(MAKE) -C $(LIBC_DIR) all
$(LIBC_A) $(LIBC_SO) $(LIBC_CRT0): | libc
	@true

rtld: $(RTLD_SO)
$(RTLD_SO):
	@mkdir -p $(RTLD_BUILD)
	+$(MAKE) -C $(RTLD_DIR) \
	    O=$(RTLD_BUILD) \
	    LIBC_INC=$(LIBC_INCLUDE) \
	    EXTRA_CPPFLAGS=-DQSOE_KERNEL_SEL4 \
	    ARCHFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany" \
	    all

libtaskman: $(LIBTASKMAN_A)
$(LIBTASKMAN_A):
	@mkdir -p $(LIBTASKMAN_BUILD)
	+$(MAKE) -C $(LIBTASKMAN_DIR) \
	    O=$(LIBTASKMAN_BUILD) \
	    LIBC_INC=$(LIBC_INCLUDE) \
	    PICFLAG=-fno-pic \
	    ARCHFLAGS="$(ARCH_CFLAGS)" \
	    all

# taskman is board-independent.  Its seL4 ABI headers come from the qemu
# kernel build dir (the API is the same on every platform), so we depend on
# that kernel.elf to guarantee the generated headers exist — even for a
# sifive-only build.  (A future optimization could configure-only.)
taskman: $(MODPKG_CPIO) $(LIBTASKMAN_A) $(GEN)/qsoe/sys_version.h \
         $(TM_SEL4BUILD)/kernel.elf
	+$(MAKE) -C $(TOP)/taskman all \
	    LIBTASKMAN_A=$(LIBTASKMAN_A) LIBTASKMAN_INC=$(LIBTASKMAN_DIR)/include
$(TASKMAN_ELF): | taskman
	@true

# Version header. Regenerates whenever .git/HEAD or .git/index changes.
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

# Always re-enter quser's submake; it owns its own incremental discipline.
$(MODPKG_CPIO): $(LIBC_SO) $(LIBC_CRT0) $(RTLD_SO)
	+$(MAKE) -C $(QUSER) cpio \
	    LIBC_SO=$(LIBC_SO) \
	    RTLD_SO=$(RTLD_SO) \
	    DYNLIBC_SO=$(LIBC_SO)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SEL4_BOOTSTRAP)/build-qsoe-riscv64 $(SEL4_BOOTSTRAP)/build-qsoe-hifive

else
# ============================================================================
# INNER make (BOARD set): build just this board's image.
# ============================================================================

.PHONY: __image
__image: $(IMAGE) $(if $(filter sifive,$(BOARD)),$(BOOT_BIN))

endif

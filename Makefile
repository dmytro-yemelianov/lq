# QSOE — top-level Makefile (iteration v0.1)
#
# Produces build/qsoe.elf, a bootable image for qemu-system-riscv64.
# The image is the seL4 ELF-loader, with a CPIO archive of [kernel.elf,
# taskman] linked into its .rodata section.
#
#   OpenSBI  →  elfloader  →  seL4 kernel  →  taskman (rootserver)
#
# Build inputs:
#   core/kernel/startup/  — elfloader sources (extracted from sel4test)
#   core/lib/cpio/        — libcpio (CPIO parser used by elfloader)
#   userland/taskman/     — hand-written taskman (currently a spin loop)
#   scripts/sel4test-full/ — upstream sel4test; we run its CMake build
#                            once to produce kernel.elf.
#
# Generated configuration headers (build/gen/) are written by this Makefile
# directly, with the minimum set of CONFIG_ defines the RISC-V elfloader
# actually references.
#
# Targets:
#   make            build build/qsoe.elf            (default)
#   make run        boot it under qemu-system-riscv64
#   make kernel     build kernel.elf only
#   make clean      remove build/
#   make distclean  also remove scripts/sel4test-full/build-qsoe-riscv64/

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

CORE        := $(TOP)/core
ELFSRC      := $(CORE)/kernel/startup
LIBCPIO     := $(CORE)/lib/cpio

TASKMAN_DIR := $(TOP)/userland/taskman
LIBQSOE_DIR := $(TOP)/userland/libqsoe
TESTER_DIR  := $(TOP)/userland/tester
TESTBUILD   := $(BUILD)/tester

# v0.5.1: musl libc. Vendored at core/userland/libc/; built into a
# static archive that gets linked into spawnable QSOE binaries.
MUSL_DIR     := $(CORE)/userland/libc
MUSL_PATCHES := $(MUSL_DIR)/patches
MUSL_GEN     := $(BUILD)/libc-gen
LIBC_BUILD   := $(BUILD)/libc-obj
LIBC_A       := $(BUILD)/libc.a

SEL4TEST    := $(TOP)/sel4test-full
SEL4BUILD   := $(SEL4TEST)/build-qsoe-riscv64
KERNEL_SRC  := $(SEL4BUILD)/kernel/kernel.elf
KERNEL_ELF  := $(BUILD)/kernel.elf

IMAGE         := $(BUILD)/qsoe.elf
TASKMAN_ELF   := $(BUILD)/taskman.elf
TESTER_ELF    := $(BUILD)/tester.elf
HELLO_ELF     := $(BUILD)/hello.elf
INIT_ELF      := $(BUILD)/init.elf
DSER_ELF      := $(BUILD)/devc-ser8250.elf
USERLAND_CPIO := $(BUILD)/userland.cpio

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
    -I$(ELFSRC)/include \
    -I$(ELFSRC)/include/arch-riscv \
    -I$(LIBCPIO)/include

TM_CFLAGS := $(ARCH_CFLAGS) \
    -ffreestanding -nostdlib -nostdinc \
    -fno-pic -fno-pie -fno-common -fno-stack-protector \
    -fno-builtin -Wall -Wextra \
    -I$(GEN) \
    -I$(CORE)/kernel/sel4_gen \
    -I$(LIBQSOE_DIR)/include

# ----------------------------------------------------------------------------
# Source lists
# ----------------------------------------------------------------------------

# Elfloader sources actually needed for RISC-V (drivers/ is ARM-only upstream).
EL_C_SRCS := \
    $(ELFSRC)/common.c \
    $(ELFSRC)/defaults.c \
    $(ELFSRC)/fdt.c \
    $(ELFSRC)/printf.c \
    $(ELFSRC)/string.c \
    $(ELFSRC)/arch-riscv/boot.c \
    $(ELFSRC)/arch-riscv/console.c \
    $(ELFSRC)/binaries/elf/elf.c \
    $(ELFSRC)/binaries/elf/elf32.c \
    $(ELFSRC)/binaries/elf/elf64.c \
    $(LIBCPIO)/src/cpio.c

EL_S_SRCS := $(ELFSRC)/arch-riscv/crt0.S

EL_OBJS := \
    $(patsubst $(CORE)/%.c,$(ELFBUILD)/%.o,$(EL_C_SRCS)) \
    $(patsubst $(CORE)/%.S,$(ELFBUILD)/%.o,$(EL_S_SRCS))

# ----------------------------------------------------------------------------
# Default target
# ----------------------------------------------------------------------------

.PHONY: all image kernel run clean distclean
.DEFAULT_GOAL := all

all: image
image: $(IMAGE)

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

$(GEN_HEADERS): $(THIS_MAKEFILE)

$(GEN)/autoconf.h:
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '/* Auto-generated by QSOE Makefile. Do not edit. */' \
	    '#pragma once' \
	    '#define CONFIG_PT_LEVELS         3' \
	    '#define CONFIG_FIRST_HART_ID     0' \
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

$(ELFBUILD)/%.o: $(CORE)/%.c $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

$(ELFBUILD)/%.o: $(CORE)/%.S $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(EL_CFLAGS) $(EL_INCLUDES) -c -o $@ $<

# ----------------------------------------------------------------------------
# Elfloader: preprocess linker script
# ----------------------------------------------------------------------------

$(ELFBUILD)/linker.lds_pp: $(ELFSRC)/linker.lds $(GEN_HEADERS)
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
    $(LIBQSOE_DIR)/include/qsoe/qrv.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
    $(LIBQSOE_DIR)/include/qsoe/tls.h \
    $(LIBQSOE_DIR)/include/qsoe/wire.h \
    $(LIBQSOE_DIR)/src/state.h \
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
# musl libc — build delegated to userland/libc/Makefile.
#
# Run `make -C userland/libc clean all` for a standalone libc.a rebuild;
# the top-level `libc` target below proxies the same submake invocation
# so existing dependencies keep working.
# ----------------------------------------------------------------------------

MUSL_GEN_HDRS := $(MUSL_GEN)/include/bits/alltypes.h \
                 $(MUSL_GEN)/include/bits/syscall.h \
                 $(MUSL_GEN)/src/internal/version.h

AR := $(CROSS)ar

.PHONY: libc
libc:
	+$(MAKE) -C $(TOP)/userland/libc all

# Order-only proxy targets — when a downstream rule has $(LIBC_A) or one
# of $(MUSL_GEN_HDRS) as a prerequisite, make sees libc as the producer
# and recurses into userland/libc.
$(LIBC_A) $(MUSL_GEN_HDRS): | libc
	@true

# libqsoe — build delegated to userland/libqsoe/Makefile.  Produces two
# archives:
#   $(LIBQSOE_A)    — normal flavour (hello/init/tester/devc-ser8250)
#   $(LIBQSOE_TM_A) — IN_TASKMAN flavour (taskman links this in)
LIBQSOE_A    := $(BUILD)/libqsoe.a
LIBQSOE_TM_A := $(BUILD)/libqsoe-tm.a

.PHONY: libqsoe
libqsoe:
	+$(MAKE) -C $(TOP)/userland/libqsoe all

$(LIBQSOE_A) $(LIBQSOE_TM_A): | libqsoe
	@true

# taskman — build delegated to userland/taskman/Makefile.  Embeds the
# userland CPIO via .incbin, so depends on $(USERLAND_CPIO) existing
# first (built by the cpio rule further down).
.PHONY: taskman
taskman: $(USERLAND_CPIO) $(LIBQSOE_TM_A)
	+$(MAKE) -C $(TOP)/userland/taskman all

$(TASKMAN_ELF): | taskman
	@true

# ----------------------------------------------------------------------------
# Tester — second user-space program, spawned by taskman.
# ----------------------------------------------------------------------------

$(TESTBUILD)/start.o: $(TESTER_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(TESTBUILD)/main.o: $(TESTER_DIR)/main.c $(TASKMAN_DIR)/sel4_syscalls.h \
                     $(TASKMAN_DIR)/sel4_types.h \
                     $(LIBQSOE_DIR)/include/qsoe/qrv.h \
                     $(LIBQSOE_DIR)/include/qsoe/slots.h
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# Tester links against $(LIBQSOE_A) (normal flavour: real-IPC path).
# --whole-archive ensures start_main / syscall_dispatch / float128_stubs
# are pulled in even when tester's own code doesn't reference them
# directly (start.S calls _qsoe_start_main; musl needs __sysinfo; etc.).
TESTER_OBJS := \
    $(TESTBUILD)/start.o \
    $(TESTBUILD)/main.o

$(TESTER_ELF): $(TESTER_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(TESTER_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

# ----------------------------------------------------------------------------
# hello — first non-taskman/non-tester userland program. Spawned by
# tester via posix_spawn(). Uses the same libqsoe build flags as tester.
# ----------------------------------------------------------------------------

HELLO_DIR  := $(TOP)/userland/hello
HELLOBUILD := $(BUILD)/hello

$(HELLOBUILD)/start.o: $(HELLO_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(HELLOBUILD)/main.o: $(HELLO_DIR)/main.c $(TM_HEADERS) $(MUSL_GEN_HDRS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) \
	    -isystem $(MUSL_GEN)/include \
	    -isystem $(MUSL_DIR)/include \
	    -isystem $(MUSL_DIR)/arch/riscv64 \
	    -isystem $(MUSL_DIR)/arch/generic \
	    -c -o $@ $<

HELLO_OBJS := \
    $(HELLOBUILD)/start.o \
    $(HELLOBUILD)/main.o

$(HELLO_ELF): $(HELLO_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(HELLO_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

# ----------------------------------------------------------------------------
# init — /sbin/init (v0.6.1+). Spawned by taskman at boot; orchestrates
# the rest of userland (resmgrs, getty, etc.). Same build shape as hello.
# ----------------------------------------------------------------------------

INIT_DIR   := $(TOP)/userland/init
INITBUILD  := $(BUILD)/init

$(INITBUILD)/start.o: $(INIT_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(INITBUILD)/main.o: $(INIT_DIR)/main.c $(TM_HEADERS) $(MUSL_GEN_HDRS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) \
	    -isystem $(MUSL_GEN)/include \
	    -isystem $(MUSL_DIR)/include \
	    -isystem $(MUSL_DIR)/arch/riscv64 \
	    -isystem $(MUSL_DIR)/arch/generic \
	    -c -o $@ $<

INIT_OBJS := \
    $(INITBUILD)/start.o \
    $(INITBUILD)/main.o

$(INIT_ELF): $(INIT_OBJS) $(LIBQSOE_A) $(LIBC_A)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $(INIT_OBJS) \
	    -Wl,--whole-archive $(LIBQSOE_A) -Wl,--no-whole-archive \
	    $(LIBC_A)

# ----------------------------------------------------------------------------
# devc-ser8250 — 16550 UART driver / resource manager (v0.6.1+).
# QSOE's first userland resmgr. Spawned by init. Receives PLIC IRQs
# on a dedicated thread bound to a kernel-signaled Notification.
# ----------------------------------------------------------------------------


# devc-ser8250 — build delegated to userland/devc-ser8250/Makefile.
.PHONY: devc-ser8250
devc-ser8250: $(LIBQSOE_A) $(LIBC_A)
	+$(MAKE) -C $(TOP)/userland/devc-ser8250 all

$(DSER_ELF): | devc-ser8250
	@true

# ----------------------------------------------------------------------------
# Userland CPIO — packs all spawnable binaries (init + tester + hello) and
# gets embedded in taskman.elf via .incbin so taskman can fetch them at
# runtime through libcpio. See plan §2.
# ----------------------------------------------------------------------------


QSH_ELF := $(BUILD)/qsh.elf

# qsh is built by userland/qsh/Makefile.
.PHONY: qsh.elf-build
qsh.elf-build: $(LIBQSOE_A) $(LIBC_A)
	+$(MAKE) -C $(TOP)/userland/qsh all

$(QSH_ELF): | qsh.elf-build
	@true

$(USERLAND_CPIO): $(INIT_ELF) $(TESTER_ELF) $(HELLO_ELF) $(DSER_ELF) $(QSH_ELF)
	@mkdir -p $(BUILD)/cpio-root/bin
	@cp $(INIT_ELF)   $(BUILD)/cpio-root/bin/init.elf
	@cp $(TESTER_ELF) $(BUILD)/cpio-root/bin/tester.elf
	@cp $(HELLO_ELF)  $(BUILD)/cpio-root/bin/hello.elf
	@cp $(DSER_ELF)   $(BUILD)/cpio-root/bin/devc-ser8250.elf
	@cp $(QSH_ELF)    $(BUILD)/cpio-root/bin/qsh.elf
	@cd $(BUILD)/cpio-root && \
	    printf '%s\n' bin/init.elf bin/tester.elf bin/hello.elf bin/devc-ser8250.elf bin/qsh.elf | \
	    cpio --quiet --create -H newc \
	         --owner=+0:+0 --reproducible \
	         --file=$(USERLAND_CPIO)

# userland_archive.S/.o (CPIO .incbin shim) is built by the taskman
# submake at $(BUILD)/taskman/userland_archive.{S,o}.

# ----------------------------------------------------------------------------
# Kernel: produced one-time by sel4test CMake build
# ----------------------------------------------------------------------------

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERNEL_SRC)
	@mkdir -p $(@D)
	cp $< $@

$(KERNEL_SRC):
	@echo "==> Bootstrapping seL4 kernel via sel4test (one-time, ~5 min)..."
	@# Drop CXX from sel4test's project() declarations: we only need kernel.elf,
	@# and the test suite's lone .cxx file is not in our build graph. Avoids
	@# needing g++-riscv64-linux-gnu just to pass CMake's language check.
	@sed -i 's/project(sel4test C CXX ASM)/project(sel4test C ASM)/' \
	    $(SEL4TEST)/projects/sel4test/CMakeLists.txt
	@sed -i 's/project(sel4test-tests C CXX)/project(sel4test-tests C)/' \
	    $(SEL4TEST)/projects/sel4test/apps/sel4test-tests/CMakeLists.txt
	@mkdir -p $(SEL4BUILD)
	@# QEMU_MEMORY (in MiB) is baked into the kernel's compile-time memory
	@# map (via the DTS extracted from QEMU at build time). It must match the
	@# `-m` value used in the `run` target; otherwise the kernel will try to
	@# touch RAM that doesn't exist and trap in S-mode.
	cd $(SEL4BUILD) && $(SEL4TEST)/init-build.sh \
	    -DPLATFORM=qemu-riscv-virt \
	    -DKernelSel4Arch=riscv64 \
	    -DQEMU_MEMORY=512 \
	    -DSMP=TRUE \
	    -DNUM_NODES=4 \
	    -DSIMULATION=TRUE
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
# Run under QEMU
# ----------------------------------------------------------------------------

run: $(IMAGE)
	$(QEMU) -machine virt -nographic -m 512M -smp 4 \
	    -bios default -kernel $(IMAGE)

# ----------------------------------------------------------------------------
# Cleanup
# ----------------------------------------------------------------------------

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SEL4BUILD)

# ----------------------------------------------------------------------------
# v0.6.2 — qsh compile-attempt (research-only target, not in `all`).
#
# Pull QRV's userland/sh source (= mksh, renamed qsh) and try to
# compile each .c against musl + libqsoe. The goal is the error log,
# not a working binary. Yuri analyses the output and decides v0.6.x
# next steps.
#
#   make qsh           — pull source if needed + compile each .c
#                        (build/qsh.log) + attempt partial link to
#                        capture undefined symbols (build/qsh.symbols.txt)
#
# Errors don't abort the loop — each file gets its own .errs sidecar.
# ----------------------------------------------------------------------------

QSH_DIR    := $(TOP)/userland/qsh
QSHBUILD   := $(BUILD)/qsh
QSH_LOG    := $(BUILD)/qsh.log
QSH_SYMS   := $(BUILD)/qsh.symbols.txt

QSH_CFLAGS := $(TM_CFLAGS) \
    -isystem $(MUSL_GEN)/include \
    -isystem $(MUSL_DIR)/include \
    -isystem $(MUSL_DIR)/arch/riscv64 \
    -isystem $(MUSL_DIR)/arch/generic \
    -I$(QSH_DIR)/include \
    -I$(QSH_DIR)/gen \
    -Wno-error -w

.PHONY: qsh qsh-pull
qsh-pull:
	@if [ ! -f $(QSH_DIR)/main.c ]; then \
	    echo "==> Pulling qsh source via scripts/pull-qsh.sh..."; \
	    $(TOP)/scripts/pull-qsh.sh; \
	fi

qsh: qsh-pull $(MUSL_GEN_HDRS)
	@mkdir -p $(QSHBUILD)
	@rm -f $(QSHBUILD)/*.o
	@echo "==> qsh compile-attempt (errors expected, not aborting on failure)"
	@echo "# qsh compile-attempt log generated $$(date)" > $(QSH_LOG)
	@: > $(QSH_SYMS)
	@for src in $(QSH_DIR)/*.c; do \
	    base=$$(basename $$src .c); \
	    echo "" >> $(QSH_LOG); \
	    echo "===== $$src =====" >> $(QSH_LOG); \
	    $(CC) $(QSH_CFLAGS) -c -o $(QSHBUILD)/$$base.o $$src \
	        >> $(QSH_LOG) 2>&1 || \
	        echo "  (compile failed for $$base.c)" >> $(QSH_LOG); \
	done
	@echo "==> compile log: $(QSH_LOG) ($$(grep -cE '(fatal )?error:' $(QSH_LOG)) error lines, $$(grep -c '(compile failed' $(QSH_LOG)) failed files)"
	@echo "==> capturing per-object undefined-symbol set..."
	@if ls $(QSHBUILD)/*.o >/dev/null 2>&1; then \
	    : > $(QSH_SYMS); \
	    for o in $(QSHBUILD)/*.o; do \
	        $(CROSS)nm -u $$o | awk '{print $$NF}' >> $(QSH_SYMS).raw; \
	    done; \
	    sort -u $(QSH_SYMS).raw > $(QSH_SYMS).objs.txt; \
	    rm -f $(QSH_SYMS).raw; \
	    echo "==> per-object undefined set: $(QSH_SYMS).objs.txt ($$(wc -l < $(QSH_SYMS).objs.txt) symbols)"; \
	    echo "==> attempting link against libc.a to see what's STILL unresolved..."; \
	    $(CC) $(TM_CFLAGS) -static -nostdlib \
	        -Wl,--warn-unresolved-symbols \
	        -o $(QSHBUILD)/qsh.elf.attempt \
	        $(QSHBUILD)/*.o $(LIBC_A) 2>$(QSH_SYMS) || true; \
	    echo "==> link-time undefined-after-libc: $(QSH_SYMS) ($$(grep -c 'undefined' $(QSH_SYMS)) warning lines)"; \
	else \
	    echo "==> no .o files produced; nothing to link"; \
	fi

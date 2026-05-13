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

SEL4TEST    := $(TOP)/sel4test-full
SEL4BUILD   := $(SEL4TEST)/build-qsoe-riscv64
KERNEL_SRC  := $(SEL4BUILD)/kernel/kernel.elf
KERNEL_ELF  := $(BUILD)/kernel.elf

IMAGE       := $(BUILD)/qsoe.elf
TASKMAN_ELF := $(BUILD)/taskman.elf
TESTER_ELF  := $(BUILD)/tester.elf

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
    -I$(GEN)

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
	    '#define CONFIG_PT_LEVELS     3' \
	    '#define CONFIG_FIRST_HART_ID 0' \
	    '#define CONFIG_MAX_NUM_NODES 1' \
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
TM_HEADERS := \
    $(TASKMAN_DIR)/sel4_syscalls.h \
    $(TASKMAN_DIR)/sel4_types.h \
    $(TASKMAN_DIR)/qsoe_invoke.h \
    $(TASKMAN_DIR)/server.h \
    $(TASKMAN_DIR)/spawn.h \
    $(LIBQSOE_DIR)/include/qsoe/qrv.h \
    $(LIBQSOE_DIR)/include/qsoe/slots.h \
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

# libqsoe is compiled into taskman with -DQSOE_LIBQSOE_IN_TASKMAN so its
# entrypoints call tm_* handlers directly instead of doing self-IPC.
LIBQSOE_CFLAGS := $(TM_CFLAGS) -DQSOE_LIBQSOE_IN_TASKMAN -I$(TASKMAN_DIR)

$(TASKBUILD)/start.o: $(TASKMAN_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(TASKBUILD)/main.o: $(TASKMAN_DIR)/main.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

$(TASKBUILD)/server.o: $(TASKMAN_DIR)/server.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(TASKBUILD)/spawn.o: $(TASKMAN_DIR)/spawn.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

# Pull libcpio (already extracted to core/lib/cpio for the elfloader) into
# taskman's build too, so the rootserver can locate tester.elf inside
# the embedded userland CPIO at runtime.
$(TASKBUILD)/cpio.o: $(LIBCPIO)/src/cpio.c
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -I$(LIBCPIO)/include -c -o $@ $<

$(TASKBUILD)/libqsoe/channel.o: $(LIBQSOE_DIR)/src/channel.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIBQSOE_CFLAGS) -c -o $@ $<

$(TASKBUILD)/libqsoe/connect.o: $(LIBQSOE_DIR)/src/connect.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIBQSOE_CFLAGS) -c -o $@ $<

$(TASKBUILD)/libqsoe/state.o: $(LIBQSOE_DIR)/src/state.c $(TM_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIBQSOE_CFLAGS) -c -o $@ $<

TASKMAN_OBJS := \
    $(TASKBUILD)/start.o \
    $(TASKBUILD)/main.o \
    $(TASKBUILD)/server.o \
    $(TASKBUILD)/spawn.o \
    $(TASKBUILD)/cpio.o \
    $(TASKBUILD)/userland_archive.o \
    $(TASKBUILD)/libqsoe/channel.o \
    $(TASKBUILD)/libqsoe/connect.o \
    $(TASKBUILD)/libqsoe/state.o

$(TASKMAN_ELF): $(TASKMAN_OBJS)
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $^

# ----------------------------------------------------------------------------
# Tester — second user-space program, spawned by taskman.
# ----------------------------------------------------------------------------

$(TESTBUILD)/start.o: $(TESTER_DIR)/start.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(TESTBUILD)/main.o: $(TESTER_DIR)/main.c $(TASKMAN_DIR)/sel4_syscalls.h
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

$(TESTER_ELF): $(TESTBUILD)/start.o $(TESTBUILD)/main.o
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -static -nostdlib \
	    -Wl,--build-id=none \
	    -Wl,-Ttext-segment=0x10000 \
	    -o $@ $^

# ----------------------------------------------------------------------------
# Userland CPIO — packs the spawned binaries (currently just tester.elf)
# and gets embedded in taskman.elf via .incbin so taskman can fetch them
# at runtime through libcpio. See plan §2.
# ----------------------------------------------------------------------------

USERLAND_CPIO := $(BUILD)/userland.cpio

$(USERLAND_CPIO): $(TESTER_ELF)
	@mkdir -p $(@D)
	@cd $(BUILD) && \
	    printf '%s\n' tester.elf | \
	    cpio --quiet --create -H newc \
	         --owner=+0:+0 --reproducible \
	         --file=userland.cpio

$(TASKBUILD)/userland_archive.S: $(USERLAND_CPIO) $(firstword $(MAKEFILE_LIST))
	@mkdir -p $(@D)
	@printf '%s\n' \
	    '.section .userland_cpio,"a"' \
	    '.balign 4096' \
	    '.globl _userland_cpio_start, _userland_cpio_end' \
	    '_userland_cpio_start:' \
	    '.incbin "$<"' \
	    '_userland_cpio_end:' \
	    > $@

$(TASKBUILD)/userland_archive.o: $(TASKBUILD)/userland_archive.S
	@mkdir -p $(@D)
	$(CC) $(TM_CFLAGS) -c -o $@ $<

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
	    -o $@ $(EL_OBJS) $(ELFBUILD)/archive.o

# ----------------------------------------------------------------------------
# Run under QEMU
# ----------------------------------------------------------------------------

run: $(IMAGE)
	$(QEMU) -machine virt -nographic -m 512M \
	    -bios default -kernel $(IMAGE)

# ----------------------------------------------------------------------------
# Cleanup
# ----------------------------------------------------------------------------

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SEL4BUILD)

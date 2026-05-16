# QSOE — Quick and Secure Operating Environment

A QNX Neutrino-style operating environment built on the [seL4](https://sel4.systems/) formally verified microkernel, targeting RISC-V 64-bit.

## Overview

QSOE brings the proven synchronous message-passing programming model of QNX Neutrino to the seL4 microkernel. It combines seL4's formally verified isolation and capability-based security with QNX's elegant Channel/Connection/Message IPC architecture.

```
┌───────────────────────────────────────────────┐
│         User applications / servers           │
├───────────────────────────────────────────────┤
│  QNX-compatible C library                     │
│  ChannelCreate · ConnectAttach · MsgSend/     │
│  Receive/Reply · MsgSendPulse · ThreadCreate  │
├───────────────────────────────────────────────┤
│  taskman — process/memory/path/system manager │
├───────────────────────────────────────────────┤
│  musl libc · sel4runtime                      │
├───────────────────────────────────────────────┤
│  seL4 microkernel (RISC-V 64-bit)             │
└───────────────────────────────────────────────┘
```

## Key ideas

- **QNX-compatible IPC** — `ChannelCreate`, `ConnectAttach`, `MsgSend`/`MsgReceive`/`MsgReply`, and pulses, mapped onto seL4 Endpoints, Capabilities, and Notifications
- **Formally verified kernel** — seL4 provides mathematical proof of correctness, capability isolation, and information-flow enforcement
- **taskman** — a single user-space process (analogous to QNX's `procnto`) that serves as process manager, memory manager, path manager, and system services provider
- **POSIX via musl** — standard C library support through musl libc, patched for seL4

## Target platform

- Architecture: RISC-V 64-bit (RV64)
- Board: `qemu-riscv-virt`

## Repository layout

```
core/                          vendored upstream (gitignored; populated
                                 by scripts/extract-sel4-riscv.sh)
  kernel/                      seL4 microkernel (generic + RISC-V arch)
  kernel/startup/              elfloader (boot/startup code)
  kernel/sel4_gen/             kernel-build-generated invocation/syscall enums
  lib/cpio/                    libcpio (used by elfloader and taskman)
  userland/libc/               musl libc (RISC-V 64-bit, seL4-patched)
  userland/taskman/runenv/     sel4runtime (C runtime for seL4 user-space)

userland/                      QSOE-native source (this is the work)
  taskman/                     central system server (sys/proc/mem/path/)
  libqsoe/                     QNX-compatible IPC, Sync*, hwinfo, line
                                 discipline (single header: <qsoe-system.h>)
  libpci/                      static libpci.a — PCI client API
  libc/qsoe/                   QSOE-native POSIX entry points, symlinked
                                 into musl's src/os_dependent/
  qsh/                         interactive shell (mksh-derived)
  init/                        /sbin/init — a shell script
  dev/ser8250/                 16550 UART driver / resmgr
  sbin/pipe/                   POSIX pipe / FIFO resmgr
  sbin/repath/                 pathmgr-rewire CLI helper
  sbin/slogger/                system log ring resmgr (/dev/slog)
  sbin/pci-server/             PCI bus resmgr (/dev/pci)
  utils/                       /bin tools (ls, cat) — one .c per binary
  sloginfo/                    /bin/sloginfo — drain the slog ring
  tester/                      end-to-end test program

scripts/                       source extraction and build helpers
doc/tex/Design/                design document (LaTeX)
sel4test-full/                 upstream seL4 + sel4test checkout (gitignored)
```

## Build & run

```
./scripts/extract-sel4-riscv.sh     # one-time: fetch upstream sources
make                                # builds kernel, elfloader, taskman,
                                    # libqsoe, libpci, libc, qsh, init,
                                    # tester, the resmgrs, and /bin tools
./emu.sh                            # boot under qemu-system-riscv64
                                    #   -gdb       attach gdb on :1234
                                    #   -no-nvme   omit the NVMe drive
                                    #   --         pass-through to qemu
```

Exit QEMU with `Ctrl-A x`. First-time builds take a few minutes because
the seL4 kernel is bootstrapped through sel4test's CMake; subsequent
incremental builds are seconds.

## Current status

See [CHANGELOG.md](CHANGELOG.md) for the full version log. Highlights as
of **v0.8**:

- QNX-style synchronous IPC — `ChannelCreate`/`Destroy`,
  `ConnectAttach`/`Detach`, `MsgSend`/`Receive`/`Reply`, plus
  `MsgSavereply` for non-MCS deferred replies (saves the implicit
  reply cap via `seL4_CNode_SaveCaller`).
- Pulses with bound-Notification wake (`MsgSendPulse`, 1-IPC idle
  `MsgReceive`).
- Threading (`ThreadCreate`/`Join`/`Detach`/`Destroy`/`Cancel`/`Ctl`)
  with per-thread TLS and SMP affinity across 4 harts.
- Processes (`ProcessCreate`/`Terminate`, `posix_spawn`, `_exit`,
  shebang `#!`) with per-child untyped budget, cap-leak hygiene, and
  argv/envp on the child's initial stack per RISC-V SysV ABI.
- **PCI bus support** — `/sbin/pci-server` enumerates qemu-virt's
  PCIe root complex over generic ECAM; on boot the QEMU Q35 host
  bridge plus an attached NVMe controller (1b36:0010) show up in
  `/dev/slog`.  Client API via `libpci.a` (`pci_device_find` /
  `_attach` / `_cfg_rd*` / `_read_ba` / `_read_irq`).  INTx routing
  via four PLIC vectors.  DesignWare MSI / iATU and SiFive Unmatched
  bring-up are v0.9.
- **`Sync*` primitives** — QNX-shape mutex / condvar / semaphore in
  libqsoe, fast-path CAS in userland, slow path through an
  address-keyed wait queue in taskman (`TM_REQ_SYNC_WAIT`/`_WAKE`).
  No futex layer; priority-inheritance deferred to the seL4/MCS
  port.
- **Resource Manager Database** — `rsrcdbmgr_create` / `_attach` /
  `_detach` / `_query` in libqsoe, table backed by taskman.  Used
  by pci-server to seed PCI windows and reserve INTx vectors.
- **System logger** — `/sbin/slogger` owns a 64 KiB ring at
  `/dev/slog`; `slogf(opcode, severity, fmt, ...)` from libqsoe
  drops records; `/bin/sloginfo` drains them.
- **FDT-driven syscfg** — taskman parses the device tree at boot
  into a tagged blob (`_MEMORY`, `_CPUS`, `_PLIC`, `_PCI_ECAM`,
  `_PCI_WINDOW`, `_PCI_IRQ`).  `TM_REQ_GET_SYSCFG` returns it;
  libqsoe's `<qsoe/hwinfo.h>` walks the tags.
- **`MAP_PHYS`** — physical-memory mapping via `qsoe_mmap`, with
  greedy power-of-2 skip-retypes when an aperture sits inside a
  larger untyped (PCI ECAM at `0x30000000` inside the 512 MiB UT
  at `0x20000000`).
- **`InterruptAttachThread` / `InterruptWait` / `InterruptUnmask`**
  in libqsoe — QNX/QRV-compatible IRQ surface, used by both
  `devc-ser8250` and `pci-server`.
- **Path manager** in taskman (prefix-tree namespace registry) with
  `register` / `repath` / `resolve` / `symlink` wire ops, plus
  the new `PATHMGR_HANDLER_TASKMAN_PMDIR` synthetic-directory
  handler that backs `/dev` (so `ls /` sees `dev` even though no
  single resmgr owns it).
- **/dev populated**: `null` (1, 3), `zero` (1, 5), `console`
  (5, 1), `tty` (symlink → console), `ser1` (4, 65 — Linux
  ttyS1), `slog` (10, 100), `pci` (10, 200).  `ls -la /dev`
  shows major / minor in the GNU `ls` shape via
  `<sys/sysmacros.h>`.
- POSIX surface — ~30 entry points in `userland/libc/qsoe/`,
  including the v0.8 additions of `getpwent` / `getgrent` /
  `getspent` (parsers ported from QRV) and a byte-oriented
  `getopt` replacement for musl's locale-heavy version.
- **musl libc** linked into spawned binaries — no `__sysinfo`
  indirection; QSOE-native POSIX entry points live in
  `userland/libc/qsoe/` and are symlinked into musl's
  `src/os_dependent/`.  An OS-owned `pthread_impl.h` shim
  (symlinked at parse time into musl's `src/internal/`) makes
  `putchar` / `putc` link.
- **mmap-only memory model** — no `brk` anywhere.  musl's three
  malloc backends are filtered out of `libc.a`; libqsoe provides
  `malloc`/`realloc`/`free` over `mmap`, which routes to taskman's
  Memory Manager (`TM_REQ_MMAP`).
- **cpiofs** — embedded `userland.cpio` mounted read-only at `/`,
  with one level of CPIO symlink resolution
  (`/bin/sh` → `/bin/qsh`).
- **BSD-style boot** — `/sbin/init` is a shell script.  Drivers
  detach via `procmgr_detach`; `wait` semantics + the shebang
  machinery in `tm_spawn` make this work.
- **`devc-ser8250`** — 16550 UART driver / resmgr, **only
  `libqsoe` + `libc`**.  Blocking reads park via `MsgSavereply`.
- **`/sbin/pipe`** — POSIX pipe / FIFO resmgr.  16-pipe pool,
  4 KiB ring each, QNX rcvid-park on full / empty.
- **`<qsoe-system.h>`** — single top-level header for the libqsoe
  public surface (analogous to QNX's `<sys/neutrino.h>`).
- **qsh** with working line discipline: backspace, VKILL, VEOF
  visible erase all work on the real UART via libqsoe's
  `qsoe_ldisc_*` line-discipline primitives, now with batched
  reads and UTF-8 codepoint-aware erase.  Arrow-key history
  remains a future-version item.
- **taskman logging discipline** — single `sel4_debug_*` callsite
  behind five severity macros (`tm_err` / `tm_warn` / `tm_info`
  / `tm_dbg` / `tm_trace`); `tm_crash()` halts with a loud banner
  in place of `for (;;) __asm__ volatile("nop");`.
- **`/bin/ls`** and **`/bin/cat`** — first two entries in the new
  `userland/utils/` framework (wildcard Makefile, one `.c` per
  binary).

## Documentation

The design document lives in `doc/tex/Design/` and can be built with:

```
cd doc/tex/Design
make
```

## License

TBD

## Acknowledgements

QSOE builds upon the work of:

- [seL4](https://sel4.systems/) — formally verified microkernel (seL4 Foundation)
- [musl libc](https://musl.libc.org/) — lightweight C standard library
- [QNX Neutrino](https://blackberry.qnx.com/) — the IPC model that inspired this project

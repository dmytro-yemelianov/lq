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
  libqsoe/                     QNX-compatible IPC + line discipline
                                 (single header: <qsoe-system.h>)
  libc/qsoe/                   QSOE-native POSIX entry points, symlinked
                                 into musl's src/os_dependent/
  qsh/                         interactive shell (mksh-derived)
  init/                        /sbin/init — a shell script
  dev/ser8250/                 16550 UART driver / resmgr
  sbin/pipe/                   POSIX pipe / FIFO resmgr
  sbin/repath/                 pathmgr-rewire CLI helper
  tester/                      end-to-end test program

scripts/                       source extraction and build helpers
doc/tex/Design/                design document (LaTeX)
sel4test-full/                 upstream seL4 + sel4test checkout (gitignored)
```

## Build & run

```
./scripts/extract-sel4-riscv.sh     # one-time: fetch upstream sources
make                                # builds kernel, elfloader, taskman,
                                    # libqsoe, libc, qsh, init, tester,
                                    # devc-ser8250, sbin/pipe, sbin/repath
./emu.sh                            # boot under qemu-system-riscv64
```

Exit QEMU with `Ctrl-A x`. First-time builds take a few minutes because
the seL4 kernel is bootstrapped through sel4test's CMake; subsequent
incremental builds are seconds.

## Current status

See [CHANGELOG.md](CHANGELOG.md) for the full version log. Highlights as
of **v0.7**:

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
- **Path manager** in taskman (prefix-tree namespace registry) with
  `register` / `repath` / `resolve` wire ops.
- **`/dev/console`** routed at boot to `/dev/ser1` (the real UART)
  by `/sbin/init`.
- POSIX surface — ~25 entry points in `userland/libc/qsoe/`:
  `open`/`close`/`read`/`write`/`writev`/`lseek`/`dup2`/`fcntl`,
  `chdir`/`getcwd`/`unlink`/`fstat`/`fstatat`/`readlink`/`access`,
  `opendir`/`readdir`, the `getpid`/`getppid`/`getuid`/... family,
  `clock_gettime`/`gettimeofday`/`time`/`times`,
  `nanosleep`/`setitimer`/`pause`, `umask`, `sysconf`, `isatty`,
  `pthread_sigmask`, `strerror`.
- **musl libc** linked into spawned binaries — no `__sysinfo`
  indirection; QSOE-native POSIX entry points live in
  `userland/libc/qsoe/` and are symlinked into musl's
  `src/os_dependent/`.
- **mmap-only memory model** — no `brk` anywhere.  musl's three
  malloc backends are filtered out of `libc.a`; libqsoe provides
  `malloc`/`realloc`/`free` over `mmap`, which routes to taskman's
  Memory Manager (`TM_REQ_MMAP`).
- **cpiofs** — embedded `userland.cpio` mounted read-only at `/`,
  with one level of CPIO symlink resolution
  (`/bin/sh` → `/bin/qsh`).
- **BSD-style boot** — `/sbin/init` is a shell script:
  ```sh
  #!/bin/sh
  /sbin/devc-ser8250
  /sbin/repath /dev/console /dev/ser1
  exec /bin/qsh -i
  ```
  Drivers detach via `procmgr_detach`; `wait` semantics + the
  shebang machinery in `tm_spawn` make this work.
- **`devc-ser8250`** — 16550 UART driver / resmgr, **rewritten in
  v0.7-rc3 to use only `libqsoe` + `libc`**.  No `seL4_*`, no
  `qsoe_sys_*`, no `taskman/` includes.  Three new libqsoe
  primitives (`qsoe_irq_set_notification` / `_wait` / `_ack`)
  encapsulate the IRQ-handler / Notification surface; blocking
  reads park via `MsgSavereply`.
- **`/sbin/pipe`** — POSIX pipe / FIFO resmgr.  16-pipe pool,
  4 KiB ring each, QNX rcvid-park on full / empty.  Same "libqsoe +
  libc only" discipline.
- **`<qsoe-system.h>`** — single top-level header for the libqsoe
  public surface (analogous to QNX's `<sys/neutrino.h>`).
- **qsh** with working line discipline: backspace, VKILL, VEOF
  visible erase all work on the real UART via libqsoe's
  `qsoe_ldisc_*` line-discipline primitives.  Arrow-key history
  remains v0.7+ work.

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

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
  taskman/                     central system server
  libqsoe/                     QNX-compatible IPC library
  tester/                      end-to-end test program
  hello/                       second user-space binary, exercises posix_spawn

scripts/                       source extraction and build helpers
doc/tex/Design/                design document (LaTeX)
sel4test-full/                 upstream seL4 + sel4test checkout (gitignored)
```

## Build & run

```
./scripts/extract-sel4-riscv.sh     # one-time: fetch upstream sources
make                                # builds kernel, elfloader, taskman, tester, hello
make run                            # boot under qemu-system-riscv64
```

Exit QEMU with `Ctrl-A x`. First-time builds take a few minutes because
the seL4 kernel is bootstrapped through sel4test's CMake; subsequent
incremental builds are seconds.

## Current status

See [CHANGELOG.md](CHANGELOG.md) for the full version log. Highlights as
of **v0.6.1**:

- QNX-style synchronous IPC: `ChannelCreate`/`Destroy`,
  `ConnectAttach`/`Detach`, `MsgSend`/`Receive`/`Reply`
- Pulses with `seL4` bound-Notification wake (`MsgSendPulse`, 1-IPC idle
  `MsgReceive`)
- Threading (`ThreadCreate`/`Join`/`Detach`/`Destroy`/`Cancel`/`Ctl`)
  with per-thread TLS and SMP affinity across 4 harts
- Processes (`ProcessCreate`/`Terminate`, `posix_spawn`, `_exit`) with
  per-child untyped budget and cap-leak hygiene
- argv/envp delivery on the child's initial stack per RISC-V SysV ABI
- Multi-server IPC end-to-end (a second process can act as a server
  and serve `MsgReceive` from other processes)
- **Path manager** in taskman (prefix-tree namespace registry)
- **`/dev/console`** as the first registered resource manager
- POSIX-style `open`/`close`/`read`/`write` via libqsoe; spawned
  processes inherit fds 0/1/2 bound to `/dev/console`
- **musl libc** linked into spawned binaries; `__sysinfo` indirection
  routes musl's "syscalls" into libqsoe; `printf` works end-to-end
- Per-process heap (2 MiB Mega_Page) backing musl's `lite_malloc`
  via `brk()`
- **cpiofs** — embedded `userland.cpio` mounted as a read-only
  filesystem at `/`; `open("/bin/hello.elf")` works from any program
- **`/sbin/init`** owns userland orchestration; taskman just
  bootstraps init
- **`procmgr_detach` + `waitpid`** — QNX/QRV-style daemon
  synchronisation; the parent blocks until the child says ready
- **`devc-ser8250`** — first real userland resmgr: drives the 16550
  UART via PLIC interrupts on a dedicated IRQ thread, registers at
  `/dev/ser1`, and init redirects `/dev/console` to it at boot

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

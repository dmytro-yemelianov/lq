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
kernel/                  seL4 microkernel (generic + RISC-V arch)
kernel/startup/          elfloader (boot/startup code)
userland/libc/           musl libc (RISC-V 64-bit, seL4-patched)
userland/taskman/        taskman — central system server
userland/taskman/runenv/ sel4runtime (C runtime for seL4 user-space)
scripts/                 source extraction and build helpers
doc/tex/Design/          design document (LaTeX)
```

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

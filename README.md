# QSOE/L

A QNX-style operating environment on the [seL4](https://sel4.systems/)
formally-verified microkernel, targeting RISC-V 64-bit.

**Codename:** *project LQ* — the L (for Jochen Liedtke, the L4 family's
creator) plus Q (for QNX).

## What this is

QSOE/L is one half of the **QSOE** umbrella project: a QNX-Neutrino-style
operating environment where the same userspace runs on either of two
interchangeable kernels:

- **QSOE/L** (this repository) — on the seL4 microkernel.
- **QSOE/N** — on *Skimmer*, a from-scratch LWKT-derived microkernel.

The userspace — shell, drivers, daemons, utilities — is **identical**
across both. The QNX-style API (`ChannelCreate`, `MsgSend`/`Receive`/`Reply`,
`ConnectAttach`, `ThreadCreate`, `Sync*`, signals-as-pulses, …) lives once
in the shared umbrella libc. Only a thin per-kernel seam differs.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  qsh + drivers + utils (dynamically linked)              │
├──────────────────────────────────────────────────────────┤
│  libc.so   (shared body; LQ-specific seam in lq/libc/)   │
│  rtld      (FreeBSD-derived; BSD-2-Clause)               │
├──────────────────────────────────────────────────────────┤
│  taskman   process / memory / path manager               │
│  libtaskman   portable taskman body (path / cred /       │
│               syscfg / sync / reloc / cpio / elf)        │
├──────────────────────────────────────────────────────────┤
│  seL4 microkernel  (RISC-V Sv39, formally verified)      │
│  elfloader         (loads kernel + taskman)              │
└──────────────────────────────────────────────────────────┘
```

`taskman` plays the role of QNX's `procnto`: every process / memory / path
operation is one `MsgSend` to taskman, encoded as a `TM_REQ_*` opcode
over a seL4 endpoint.

## Key ideas

- **One userspace, two kernels.** The seL4 port and the Skimmer port run
  the same binaries. The seam between OS-dependent and OS-independent
  code is small and explicit.
- **Synchronous QNX IPC over seL4 endpoints.** `Channel`s map to
  endpoints; `Connection`s carry a per-process badge; `MsgSend` is a
  seL4 `Call`; replies travel via the deferred-reply slot (`MsgSavereply`
  over `seL4_CNode_SaveCaller`).
- **Signals are pulses.** Every process has a system thread from t=0;
  signals arrive there as pulses, dispositions are a process-local
  `sigaction` table.
- **mmap-only memory.** No `brk`. `malloc` lives on top of
  `mmap`/`munmap`, which route to taskman's Memory Manager
  (`TM_REQ_MMAP` / `TM_REQ_MUNMAP`).
- **Spawn, not fork.** Process creation only via `posix_spawn(3)`.

## Current state — v0.14

**First login from real NVMe storage on the SiFive Unmatched.** v0.13
mounted a filesystem but wedged the instant taskman tried to spawn the
first program off it: the spawn-image read blocked taskman, while the NVMe
completion that would unblock it routed its wake *back through* taskman.
v0.14 takes taskman out of the wake path entirely — `Sync*` and device
pulses now go straight through the kernel — so QSOE/L boots NVMe → `getty`
→ `login` → an interactive `qsh` as a real user on the board. Everything
also runs under QEMU `virt` (polled virtio-mmio standing in for NVMe).

```
QSOE/L 0.14 on sifive,hifive-unmatched-a00
[init] mounting /dev/nvme0n1p8 at /usr...
fs-qrv: mounted qrvfs at /usr (dev=/dev/nvme0n1p8)
Sysinit: level1 running (on-disk init from /usr).

login: user
Password:
Welcome to QSOE, user.
[/home/user]$ ls /usr/bin
test_syncspace  test_msgpass  suite  time
[/home/user]$ ls -la /
lrwxrwxrwx  1  0  0  9 home -> /usr/home
dr-xr-xr-x  2  0  0  0 bin
drwxr-xr-x  6  0  0  1536 usr
...
[/home/user]$
```

What's working:

- **Dynamic linking end-to-end.** `rtld` (`ld-qsoe.so.1`, FreeBSD-derived)
  + `libc.so` + `qsh` run as a normal dyn-linked program. Taskman
  pre-applies relocations and jumps straight to the entry, then user-mode
  rtld stays available for future `dlopen()`.
- **`posix_spawn`.** Spawns over `TM_REQ_SPAWN` with a one-page mmap'd
  side channel for `(path, argv, envp)`. End-to-end from `init.sh` to
  the interactive `qsh`.
- **`mmap` / `munmap`** routed through taskman; per-process tracking of
  Mega_Page frames; munmap releases caps and recycles slots.
- **Path manager.** Prefix-tree namespace with `register` / `repath` /
  `resolve` / `symlink` wire ops; synthetic-directory handlers; an
  embedded `modpkg.cpio` mounted read-only at `/`.
- **PCI bus.** `/sbin/pci-server` enumerates both the qemu-virt root
  complex (generic ECAM, INTx via four PLIC vectors) and the FU740's
  Synopsys DesignWare host (config window + iATU + DW-MSI) — taskman
  recognizes `sifive,fu740-pcie` and the shared server drives both.
  Client surface via `libpci.a`.
- **Serial console.** `devc-ser8250` 16550 driver with IRQ-driven RX;
  `init` repaths `/dev/console` to `/dev/ser1` so qsh's blocking reads
  land on the real UART. Line discipline handles backspace, VKILL,
  VEOF visible erase, and UTF-8 codepoint-aware editing.
- **System logger.** `/sbin/slogger` owns a 64 KiB ring at `/dev/slog`;
  `slogf(opcode, severity, fmt, ...)` from libc drops records; `sloginfo`
  drains them.
- **`Sync*` primitives.** QNX-shape mutex / condvar / semaphore — fast
  path CAS in user-space, slow path through an in-process address-keyed
  wait/wake table backed by kernel Notifications (no taskman round-trip).
- **Pulse-based signals** with per-process system thread; no
  `TM_REQ_SIGACTION` opcode (libc-local dispositions).
- **Resource Manager Database.** `rsrcdbmgr_create` / `_attach` /
  `_detach` / `_query`, table-backed in taskman.
- **`/dev` populated**: `null`, `zero`, `console`, `tty` (→ console),
  `ser1`, `slog`, `pci`. `ls -la /dev` reports the GNU `ls`-shape
  `major,minor` for character devices.
- **FDT-driven syscfg.** Taskman parses the device tree at boot into a
  tagged blob (`_MEMORY`, `_CPUS`, `_PLIC`, `_PCI_ECAM`, …); user-space
  queries via `<qsoe/hwinfo.h>`.

New in v0.14:

- **Kernel-direct `Sync*`** — the slow path moved out of taskman into an
  in-process address-keyed wait/wake table backed by per-thread seL4
  Notifications (a Notification-as-mutex guards the table, safe under
  strict priority). Same credit-absorb / gen-check semantics; mutex /
  condvar / semaphore wakes never message taskman.
- **Kernel-direct device pulses** (`QSOE_CHF_PULSE_DIRECT`) — a device
  driver's MSI-pulse channel hands the connector a copy of its bound
  Notification at `ConnectAttach`; `MsgSendPulse` signals it straight and
  `MsgReceive` synthesizes an empty pulse. The NVMe completion wakes the
  driver with taskman uninvolved. Together with kernel-direct `Sync*`,
  this is what lets the spawn-image read off NVMe complete while taskman
  is blocked on it — the v0.13 wedge is gone.
- **First login from disk** — `getty` + `login` (crypt/shadow) chain off
  `/usr`; boots NVMe → login → `qsh` as a uid-1000 user on the FU740.
- **TM_REQ opcode honesty pass** — the shared "common" opcode buckets now
  hold only messages both kernels send; every LQ-only kernel primitive
  (sync, pulse, IRQ attach, syscfg / clock-freq, channels, connections,
  threads, process-create, `Sched*`) lives in LQ's variant-private space.
- **Shared-libc promotion + fd seam** — the OS-independent POSIX bodies
  live once in the shared libc tree; `ps -H` shows per-thread names
  (main / sigthread / irqN) via a `ThreadCtl(TCTL_NAME)` bridge.
- **Hard-float ABI**, unified credential setters with a `setuid(0)` gate,
  cpio cross-fs symlinks, and `MsgSendv`.

New in v0.13:

- **Storage stack** — real `Sched*` (POSIX→MCS-SchedContext adapter),
  `alloc_phys`, QNX global channels, and real `mprotect` together bring
  `devb-nvme` up over MSI-X on the FU740; `/dev/nvme0n1p*` appears.
- **First mounted filesystem** — external libressrv resource managers work
  on LQ (`open` sends `_IO_CONNECT`, `readdir` uses getdents framing); the
  `fs-qrv` qrvfs server mounts `/usr` over `devb-nvme` (board) or the new
  polled `devb-virtio` `/dev/vblk0` (QEMU).
- **spawn-from-fs** — taskman loads binaries and `#!`-scripts that aren't
  in the boot cpio off the mounted filesystem, so `/sbin/init` hands off to
  an editable on-disk `level1.sh`. Resmgr `dup` (`F_DUPFD`/`dup2`) refcounts
  shared handles, so a shell can relocate a script fd.
- **Diagnostics** — `--debug[=N]` boot cmdline; at TRACE, one `tm_msg
  0x<type>` line per incoming message. `ps` shows detached daemons as `d`.
- **Misc** — bulk IPC, `ConnectServerInfo`, zombie-reap fix, an FU740
  boot-stall fix (master-pool untyped cap), and `TimerCreate`/`Destroy`.

New in v0.12:

- **FU740 PCIe** — taskman parses the `sifive,fu740-pcie` DesignWare host
  (config/DBI windows + MSI source); a device-untyped high-water-mark fix
  in `MAP_PHYS` lets `pci-server` map windows that sit deep inside seL4's
  coarse gap-filled device-untypeds. Full topology enumerates on the
  board (NVMe + GK208).
- **Per-process signal thread** — main + system thread in every process;
  cross-process `kill()` runs handlers on the system thread (seL4
  Notification rebound to that TCB; per-thread reply objects).
- **Unified logger** — taskman uses the shared `libtaskman` logger;
  `--debug[=N]` from `/chosen/bootargs`; boot command line echoed.
- **Kconfig multi-board build** — multi-select QEMU-virt / SiFive,
  board-named ELFs, one recursive Makefile.
- **Build-time seL4 patches** (FU740) — `MAX_IRQ` 53 → 128 for PCIe MSI,
  and PLIC complete-on-claiming-hart, which fixed serial input on SMP
  hardware (was one char then silence).

New since v0.9 (the v0.10 MCS line and v0.11):

- **seL4 MCS kernel** — scheduling contexts + reply objects; the basis
  for native timers and `Wait`-with-timeout.
- **First FU740 boot** — `make PLAT=hifive` emits `qsoe-l-fu740.bin`
  (raw, for mr-bml) and a `bootm` uImage; `FirstHartID=1` (hart 0 is the
  S7 monitor). Booted to a shell on a real Unmatched.
- **Per-spawn resource reclamation** — RAM (per-process untyped +
  munmap'd-frame recycle), VA cursor, and CSpace slots (SCs, channel
  ntfn, page tables) are all reclaimed on exit; the `ps;sysinfo` loop
  runs unbounded (was OOM at ~13 spawns).
- **PCI ECAM as device MMIO** — `MAP_PHYS` fixed and unified across the
  two kernels; large device windows map as 2 MiB Mega_Pages; a device-
  frame registry lets pci-server and `sysinfo` share the same ECAM
  frames. `lspci` reports the host bridge.
- **Variant-private wire opcodes** — kernel-specific TM message codes
  live at `>= 0x10000`, out of the shared opcode space.

What's deliberately not implemented: `fork()`, `select()`, `brk()`.

Still ahead (v0.15+): the shared `quser/test/suite/` fully green on LQ; a
writable filesystem (qrvfs is read-only today); wall-clock
`CLOCK_REALTIME`; MCS-native timers; shell pipelines. The umbrella roadmap
(`ROADMAP.md` in the top-level `os` repo) sketches the path to the
unified 1.0.

## Build and run

LQ pulls seL4 and seL4_tools as shallow git clones into a sibling
`sel4-bootstrap/` directory (~30 MB, one-time, ~10 s). The first
`make` runs `make prepare` automatically.

```sh
make           # kernel.elf via cmake; elfloader + taskman + libc +
               # modpkg.cpio + qsoe.elf
./emu.sh       # boot under qemu-system-riscv64
               #   -gdb   attach gdb on :1234
               #   --     pass-through flags to QEMU
```

Exit QEMU with `Ctrl-A x`. First build takes ~1 minute; incremental
rebuilds are seconds.

## Layout

```
lq/
├── Makefile          top-level build
├── emu.sh            QEMU launcher
├── taskman/          the central system server
│   ├── main.c        dispatcher; TM_REQ_* opcodes
│   ├── proc/         processes / threads / channels / connections /
│   │                 pulses / spawn
│   ├── mem/          memory manager (mmap / munmap)
│   ├── path/         pathmgr / cpiofs / IO dispatch
│   ├── sys/          console / IRQ / FDT / syscfg / sync
│   └── qsoe/         taskman-private IPC primitives (was libqsoe)
├── libc/             LQ libc seam
│   ├── *.c           POSIX entries (open / close / read / mmap / ...)
│   └── qsoe/         QSOE/QNX-native API + LQ runtime
└── doc/
    └── tex/Design/   LaTeX design document
```

Sibling repositories in the umbrella:

- `quser/` — shared, OS-independent userspace (qsh + drivers + utilities)
- `libc/` — shared C library (OS-independent body, rtld)
- `libtaskman/` — portable taskman code linked by both LQ and NQ
- `common/` — small shared utilities (libcpio, …)
- `sel4-bootstrap/` — shallow clones of upstream seL4

## License

Apache-2.0. Vendored upstream files retain their original BSD-2-Clause
headers (libcpio from seL4 util_libs; rtld derived from FreeBSD). See
`LICENSE.md`.

## Contact

`yuriz@qsoe.net`

## Acknowledgements

- [seL4](https://sel4.systems/) — the formally verified microkernel
  underneath, and the source of `libcpio` (`util_libs/`) and the
  elfloader.
- FreeBSD — the dynamic linker (`libexec/rtld-elf`) was the starting
  point for `ld-qsoe.so.1`.
- musl — the body of the shared libc.
- QNX Neutrino — the IPC model that inspired the whole project.

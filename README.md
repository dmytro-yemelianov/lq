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

## Current state — v0.12

**PCIe enumeration on real silicon.** Building on the v0.11 first-boot,
QSOE/L's `pci-server` now walks the full SiFive Unmatched (FU740)
DesignWare PCIe topology under seL4 — 11 devices across buses 0..7,
including a Samsung NVMe SSD and an NVIDIA GK208 GPU with its HDMI-audio
function. Serial input works on the board, and every process runs a
per-process signal thread. Everything below also runs under QEMU `virt`.

```
QSOE/L Operating System version v0.12
[init] starting pci-server...
[pci-server] scan complete: 11 devices across buses 0..7
[init] starting devc-sersifive...
[devc-sersifive] /dev/ser1 registered (chid=2)
[/]# lspci
06:00.0 NVM controller [0108]: 144d:a80a (rev 00)
07:00.0 VGA compatible controller [0300]: 10de:128b (rev a1)
07:00.1 Multimedia controller [0403]: 10de:0e0f (rev a1)
[/]#
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
  path CAS in user-space, slow path through an address-keyed wait queue
  in taskman.
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

Still ahead: serial RX on the FU740 PLIC (the console accepts output but
hangs on input on real hardware); shell pipelines; a storage stack
(`devb-nvme`, `fs-qrv`); MCS-native timers. The shape of the work is
sketched in the umbrella's top-level documentation.

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

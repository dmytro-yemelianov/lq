# Changelog

All notable changes to QSOE. Format inspired by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning is
`vMAJOR.MINOR[.PATCH]` until v1.0, which is reserved for the first
release with full QNX libc compatibility.

## [v0.11] — 2026-06-13

**Milestone: first boot on real silicon.** QSOE/L came up on a SiFive
Unmatched (FU740) — one month after the project began — through
`bootm` → the seL4 MCS kernel → taskman → an interactive shell.
Built on the v0.10 MCS switch (scheduling contexts + reply objects);
this entry also folds in the per-spawn reclamation, PCI, and
wire-protocol work that made the system robust enough to take to
hardware.

### Boot on real hardware (FU740)
- **`make PLAT=hifive`** now also emits **`qsoe-l-fu740.bin`** — the raw
  elfloader image (objcopy of `qsoe.elf`), linked + entered at
  `0x84000000`, for mr-bml or a direct load.
- **`boot/`** ships a `bootm` path: the `.bin` is wrapped in a legacy
  U-Boot uImage (`qsoe-l-fu740.uImage`), so `bootm` enters it as
  `kernel(a0=hartid, a1=DTB)` — delivering the device tree the
  elfloader requires — without the RISC-V Linux Image header that
  `booti` checks.  `make -C boot deploy` + `run bootlq`.
- `FirstHartID=1` (hart 0 is the FU740's S7 monitor; the OS runs on the
  U74 harts 1-4); the elfloader and SMP bring-up come up on real
  silicon, and taskman builds its syscfg from the live FU740 FDT.

### Per-spawn resource reclamation
- **RAM** — the per-process untyped (pp_ut) allocator is bump-only;
  `munmap` now parks the freed Mega_Page on a per-process recycle list
  and re-maps + zeroes it, instead of leaking it until exit. qsh's
  per-`posix_spawn` 2 MiB args page no longer drains the RAM pool.
- **Address space** — `munmap` rewinds the `mmap_top` cursor on a
  contiguous-top free (the LIFO args case).
- **CSpace** — scheduling-context slots (main + worker), a channel's
  `ntfn_sig` slot, and the per-spawn page-table caps are now freed on
  exit (the page tables ride into the per-process objcnode). The
  `ps;sysinfo` loop runs unbounded; it previously OOM'd at ~13 spawns.

### PCI ECAM as device MMIO
- **`MAP_PHYS` fixed and unified.** The shared `qsoe_mmap()` helper
  framed the wire request with NQ's flag value (`0x10000`), which LQ
  taskman read as anonymous — so pci-server's ECAM window came back as
  zeroed RAM and `lspci` saw 32 phantom `0000:0000` devices. The flag
  (`TM_MMAP_FLAG_PHYS`) now lives once in `<qsoe/tm_msgs.h>`, equal to
  POSIX `MAP_PHYS`, with a `_Static_assert` guarding the equality.
- **2 MiB Mega_Page device mapping** — a 16 MiB ECAM window maps as 8
  Mega_Pages instead of 4096 4 KiB frames (which overflowed the
  4096-slot root CNode).
- **Device-frame registry** — a device region is carved from its
  device-UT once; every mapper gets `cnode_copy`'s mapped into its own
  VSpace, so pci-server and `sysinfo` share the same ECAM frames (a
  device-UT's free index only advances, so the region can't be
  re-carved). `lspci` reports the host bridge (`1b36:0008`).

### Wire protocol
- **Variant-private opcode space.** TM message codes `>= 0x10000`
  (`TM_REQ_VARIANT_BASE`) are kernel-private and defined only in the
  variant tree, never in the shared common code — keeping the shared
  opcode space collision-free. The LQ-only `TM_REQ_DUP_CAP` /
  `TM_REQ_DETACH_CAP` moved out of the shared enum's gaps into it.
- **Sysmap page** — taskman renders the syscfg blob into a read-only
  `PSYS` page mapped at `QSOE_SYSMAP_VA` in every process, so the shared
  libc `hwi_init()` works on LQ exactly as on NQ.

### Known issues
- Serial **input** hangs on the FU740 after the first keystroke
  (the SiFive UART receive-IRQ path on the real PLIC); output is fine.
- A multi-second pause before `/sbin/init` on large-RAM boards (scales
  with RAM size); under investigation.

## [v0.8] — 2026-05-17

**Milestone: PCI bus, system logger, real synchronisation, and a
populated `/dev`.**  qemu-virt's PCIe root complex enumerates, the NVMe
controller appears at BDF 0:1:0, every taskman log line carries a
severity, `Sync*` primitives back QNX-shape mutexes/condvars/semaphores
through an address-keyed wait queue (no futex layer), and `ls -la /dev`
prints every device node with Linux-compatible `major, minor` numbers.
Built up over rc1 → rc2 → rc3.

### PCI subsystem (rc2)
- **`/sbin/pci-server`** — first user-space driver port from QRV
  (~7 000 LoC).  v0.8-rc2 scope: single-bus generic ECAM
  (qemu-virt `pci-host-ecam-generic`), VID/DID/class scan, INTx vector
  capture, client-API switch (`QSOE_PCI_REQ_BIOS_PRESENT`,
  `FIND_DEV`, `CFG_RD`/`WR`, `ATTACH`/`DETACH`, `READ_BA`,
  `READ_IRQ`).  Boot transcript:
  `[pci-server] scan complete: 2 devices on bus 0` — the QEMU Q35
  host bridge (1b36:0008) plus the NVMe controller (1b36:0010).
- **`userland/libpci/`** — static `libpci.a`, ports QRV's
  `pci_client.c` + `pci_strerror.c` + public `<pci/pci.h>`.  Lazy
  `open("/dev/pci")` cached in a libqsoe `Sync*` mutex; each call
  marshals into the matching `IOM_PCI_*` wire struct and `MsgSend`s.
  MSI / MSI-X return `ENOSYS` in v0.8 (lands in v0.9 with the
  DesignWare-MSI controller for SiFive Unmatched).
- **hwinfo helpers in libqsoe** — `<qsoe/hwinfo.h>` /
  `libqsoe/src/hwinfo.c`.  Lazy `TM_REQ_GET_SYSCFG` fetch
  guarded by a `Sync*` mutex (singleton blob cached for process
  lifetime); cursor walks the tag stream
  (`TM_SYSCFG_TAG_PCI_ECAM`, `_WINDOW`, `_IRQ`).
- **taskman `syscfg` extended** — `userland/taskman/sys/syscfg.c`
  walks the FDT `/soc/pci@*` node, emitting one `_ECAM` tag, one
  `_WINDOW` tag per `ranges` record (IO / MEM32 / MEM64 + prefetch
  flags decoded from the child-addr high cell), and one `_IRQ` tag
  carrying the four INTx PLIC legs.

### Resource Manager Database (rc1)
- **`userland/libqsoe/src/rsrcdb.c`** — QNX-shape `rsrcdbmgr_create` /
  `_attach` / `_detach` / `_query`.  Each call wraps the new
  `TM_REQ_RSRCDB_*` ops.
- **taskman side** — `userland/taskman/sys/rsrcdb.c` carries a
  small static table of resource ranges (IO ports, memory windows,
  IRQ vectors).  Used by pci-server to seed PCI memory / I/O
  windows from the FDT-derived `_WINDOW` tags and to reserve the
  four INTx PLIC vectors so client drivers can't double-claim them.

### Interrupt API
- **`InterruptAttachThread` / `InterruptWait` / `InterruptUnmask`**
  in `userland/libqsoe/src/interrupt.c` — QNX/QRV-compatible IRQ
  surface.  Per-thread cap of 16 attaches.  Replaces the v0.7-era
  hand-rolled `seL4_IRQHandler_*` choreography in
  `dev/ser8250/src/irq.c`.

### MAP_PHYS for drivers
- **`QSOE_MAP_PHYS`** flag on `qsoe_mmap` — physical-memory mapping
  with greedy power-of-2 skip-retypes when the requested aperture
  sits inside a larger untyped (e.g. PCI ECAM at `0x30000000` inside
  the 512 MiB UT at `0x20000000`).  Taskman side does the retype
  walk; client gets back a virtual mapping.  Uses `Mega_Page` (2 MiB)
  for windows ≥ 2 MiB.

### FDT parser + syscfg blob
- **`userland/taskman/sys/fdt.c`** — minimal FDT walker (depth-first,
  no live-tree allocation) sufficient for the nodes QSOE cares about
  on qemu-virt and the Unmatched.  Path-keyed `tm_fdt_path()` returns
  a node cursor; property lookup is byte-oriented.
- **`syscfg.c`** — builds a self-describing tagged blob at boot.
  Tag list: `_END`, `_MEMORY`, `_BOOTHART`, `_CPUS`, `_PLIC`,
  `_PCI_ECAM`, `_PCI_WINDOW`, `_PCI_IRQ`.  Future tags
  (`_DW_MSI`, `_CLOCK_FREQ`) land in v0.9.
- **`TM_REQ_GET_SYSCFG`** — wire op returns the blob into the
  caller's IPC buffer.  Reply framing fixed in rc2:
  `reply_len = 4 + (want + 7) / 8` so the kernel actually
  transfers `msg[4..]` to the client.

### System logger
- **`/sbin/slogger`** — system log ring (64 KiB) backed by a
  resmgr at `/dev/slog`.  Wire op `TM_REQ_IO_WRITE` from `slogf()`
  drops records into the ring; `TM_REQ_IO_READ` drains them.
  Standard hand-built `MsgReceive` loop modelled on devc-ser8250.
- **`slogf(opcode, severity, fmt, ...)`** in libqsoe —
  `<sys/slog.h>` API.  Severities `_SLOG_*` and codes
  `_SLOGC_*` mirror QRV exactly.
- **`/bin/sloginfo`** — drain the slog ring to stdout.  Used at
  the qsh prompt to inspect what drivers logged during boot.

### Synchronisation: Sync* primitives, no futexes
- **`<sys/sync.h>`** in libqsoe — QNX-shape `SyncTypeCreate`,
  `SyncDestroy`, `SyncMutexLock`/`_Unlock`, `SyncCondvarWait`/
  `_Signal`/`_Broadcast`, `SyncSemPost`/`_Wait`.  `sync_t = {long
  count; unsigned long owner;}`; flag bits use the `QRV_` prefix
  (`QRV_SYNC_WAITERS = 0x80000000`).
- **Userspace fast path** — CAS on the `sync_t` word.  Uncontended
  lock / unlock never enters taskman.
- **Slow path: address-keyed wait/wake in taskman, NOT futex(2).**
  `userland/taskman/sys/sync.c` — 16-bucket wait list, 4 waiters
  per bucket.  Two modes: credit-absorb (for mutex) and gen-check
  (for condvar).  Reply parking via `qsoe_cnode_save_caller`.
  Wire ops `TM_REQ_SYNC_WAIT` / `_WAKE`.
- **Priority inheritance deferred** to the eventual seL4/MCS port —
  current scheduling model has no `sched_context_donate` analogue,
  so PI would be either a no-op or a custom hand-coded boost.  Both
  ugly; sidestep until MCS.

### POSIX surface: user database, getopt, pthread_impl
- **`userland/libc/qsoe/getpwent.c` / `getgrent.c` / `getspent.c`** —
  parsers ported from QRV; supersede musl's locale-heavy versions
  (which are now excluded in `placement.txt`).  In v0.8 there is no
  `/etc/passwd` on disk; the parsers will switch to reading
  `/usr/etc/passwd` once `fs-qrv` lands in v0.9.
- **`userland/libc/qsoe/getopt.c`** — byte-oriented replacement for
  musl's locale-dependent version (also excluded).
- **`userland/libc/include/pthread_impl.h`** — OS-owned shim,
  symlinked at parse-time into `core/userland/libc/src/internal/`.
  Defines `struct __pthread { int tid; }`, `__pthread_self()`
  reads the `tp` register, `__wake` / `__futexwait` stub out so
  musl's `putc.h` (which depends on `pthread_impl.h`) finally
  links — `putchar` works again.

### userland/utils framework
- **New top-level `userland/utils/`** — wildcard Makefile, one
  `.c` per binary, drops the result into `$(BUILD)/utils/<name>.elf`
  and packs it as `/bin/<name>` in the CPIO.
- **`/bin/ls`** — ported from QRV (xv6-based, MIT + Apache-2.0,
  164 LoC).  Flags `-l` and `-a`.  In v0.8-rc3, `-l` on a
  char/block device replaces the size column with `major, minor`
  (via `<sys/sysmacros.h>`) — matches GNU coreutils.
- **`/bin/cat`** — 71 LoC, reads a list of files into stdout.

### /dev and pathmgr improvements
- **/dev/null + /dev/zero** — pseudo-devices in taskman, mirror
  Linux `(1, 3)` / `(1, 5)`.
- **/dev/tty** — pathmgr symlink to `/dev/console`.
  `tm_pathmgr_symlink()` is a new node type that resolves at open
  time (one level; no recursion).
- **fstat in devc-ser8250** — `/dev/ser1` reports
  `S_IFCHR | 0666` with `(major, minor) = (4, 65)` (Linux ttyS1).
  `isatty(open("/dev/ser1"))` now returns 1 → qsh engages the
  interactive line editor without an explicit `-i`.
- **Synthetic `/dev` directory** (rc3) — new
  `PATHMGR_HANDLER_TASKMAN_PMDIR` backing path nodes that have
  registered children but no resmgr.  `path/pmdir.c` carries the
  per-open slot table; readdir iterates the pathmgr-tree
  children (so `ls /dev` lists `null zero console tty slog ser1
  pci` even though no single resmgr owns the directory).
- **cpiofs readdir** merges pathmgr-root children after the CPIO
  walk completes, so `ls /` sees `dev` alongside `bin` and `sbin`.
- **cpiofs readdir** subdirectory dedup widened from a single
  slot to a 16-entry list — fixes a double-listing of `bin/` and
  `sbin/` when CPIO interleaves entries from multiple directories.
- **fstat on external resmgrs** — slogger / devc-ser8250 / pci-server
  each now reply to `TM_REQ_FSTAT` with a char-device stat.  Without
  this, `lstat("/dev/pci")` returned `ENOSYS` and `ls /dev` printed
  "cannot stat /dev/pci".
- **Linux-compat (major, minor)** across the board:
  `/dev/null = (1, 3)`, `/dev/zero = (1, 5)`,
  `/dev/console = (5, 1)`, `/dev/ser1 = (4, 65)`,
  `/dev/slog = (10, 100)`, `/dev/pci = (10, 200)`.

### Logging & crash discipline in taskman
- **Single `sel4_debug_*` callsite** — `userland/taskman/tm_log.c`.
  Five severity macros — `tm_err` / `tm_warn` / `tm_info` /
  `tm_dbg` / `tm_trace` — expand to a printf-lite formatter
  (`%s` / `%c` / `%d` / `%u` / `%x` / `%p` / `%l` / `%0NX`).  All
  17 prior raw `sel4_debug_puts` sites converted; pci-server's
  two sites converted to `slogf`; tester converted to `printf`.
- **`tm_crash()`** — loud unmistakable banner + halt-in-WFI.
  Replaces 17 occurrences of
  `tm_err(...); for(;;) __asm__ volatile("nop");` across `main.c`.

### Path canonicalisation in open()
- **`userland/libc/qsoe/open.c`** — relative paths are prepended
  with the cwd, then `./` / `../` / `//` are collapsed in-place
  before the path goes on the wire.  Fixes `ls` in any subdirectory
  (e.g. `cd /bin; ls .` was previously
  `ls: cannot stat .` because pathmgr saw `/bin/.`).

### taskman scratch VA, future-proof
- **`TM_SCRATCH_VADDR = 0x40000000`** with lazy
  `ensure_scratch_pt()` allocating its own L1 + L0 page tables on
  first use.  The previous strategy (scratch immediately after
  the image) broke when taskman.elf grew past `0x1F8000` with
  `tm_log` + `sync.c` added; the new layout has 1 GiB of headroom
  below the image and is independent of image size.

### Build / boot
- **`emu.sh`** — replaces `make run`.  Default device set attaches
  an NVMe controller backed by a sparse 64 MiB file
  (`test/nvme.img`, auto-created), so pci-server's scan finds
  something interesting to enumerate.  Flags: `-gdb`, `-no-nvme`,
  `--` pass-through.
- **crt0 consolidation** — five identical `userland/*/start.S`
  files unified into one shared `crt0.S` in libqsoe; per-binary
  Makefiles pick it up from `$(LIBQSOE_DIR)/src/`.
- **Top-level `Makefile` shrunk** — each component owns its own
  Makefile; the top level delegates rather than spawning rules
  per-file.

### LDISC refactor (rc1)
- **Batch reads** — `qsoe_ldisc_readline` now consumes whatever
  bytes the driver has buffered in one `read()` instead of
  one-at-a-time.
- **UTF-8 codepoint erase** — backspace deletes a full codepoint
  (1–4 bytes) rather than a single byte.

### Known gaps / deferred to v0.9 / v0.10
- DesignWare MSI / iATU controller — needed for SiFive Unmatched
  boot; pci-server gains the runtime-detect path in v0.9.
- `fs-qrv` + `devb-nvme` — block-device resmgr and on-disk
  filesystem, mounted at `/usr`, with `/etc` symlinked into it.
  Once present, `/etc/passwd` / `group` / `shadow` go live.
- `poll()` is still a stub; real pulse-based per-fd readiness
  with timer integration is the v0.9 ticket.
- `TM_CLOCK_FREQ_HZ` is still hardcoded; v0.9 reads
  `/cpus/timebase-frequency` from the FDT (the parser exists now;
  just plumb the field through).
- Hotplug / surprise-remove on PCI, MSI-X capability programming,
  AER / DPC error handling, SR-IOV — all v0.9+.

## [v0.7] — 2026-05-15

**Milestone: full QNX-shape userland.**  Interactive shell with working
line editing on the real UART; BSD-style boot (`/sbin/init` is a shell
script); every resource manager uses only `libqsoe` + `libc` (no direct
seL4 surface anywhere outside the kernel-facing wrappers); a single
top-level system header.  Built up over rc1 → rc2 → rc3.

### Architecture: taskman split + wire protocol re-bucketing
- `userland/taskman/server.{c,h}` retired.  Code redistributed:
  - `proc/` — processes, threads, channels, connections, pulses, spawn,
    timer
  - `mem/`  — memory manager (mmap)
  - `path/` — pathmgr, cpiofs, IO dispatch, open/close
  - `sys/`  — console + platform
- `TM_REQ_*` wire labels renumbered into 0x100-wide buckets per
  subsystem (sysmgr 0x000, procmgr 0x100, memmgr 0x200, pathmgr 0x300);
  256 entries of headroom each.  Label rides in seL4's 52-bit
  `MessageInfo.label` field.
- `libqsoe/src/syscall_dispatch.c` and `musl_stubs.c` deleted — the
  `__sysinfo` indirection is gone.  Every POSIX entry point talks to
  taskman directly through libqsoe primitives.

### Single top-level header: `<qsoe-system.h>`
- `<qsoe/qrv.h>` retired; its declarations live in
  `userland/libqsoe/include/qsoe-system.h`, analogous to QNX's
  `<sys/neutrino.h>`.  60+ source files migrated in one sweep.
- `<qsoe/slots.h>`, `<qsoe/wire.h>`, `<qsoe/tls.h>` survive short-term
  — none are "qrv"-named.  Long-term consolidation deferred.
- New `qsoe_ipcbuf_t` typedef in `<qsoe/tls.h>` mirrors
  `seL4_IPCBuffer`'s layout under a QSOE-native name; resource managers
  no longer pull `<sel4_types.h>` just to reach `qsoe_ipcbuf->msg[]`.

### POSIX surface (rc1)
- `userland/libc/qsoe/` now hosts ~25 real entry points:
  - fd surface: `open` `close` `read` `write` `writev` `lseek` `dup2`
    `fcntl`
  - fs/path: `chdir` `getcwd` `unlink` `fstat` `fstatat` `readlink`
    `access` `opendir` `readdir`
  - cred/id: `getpid` `getppid` `getuid` `geteuid` `getgid` `getegid`
    `getpgrp` `setxid`
  - misc: `sysconf` `umask` `isatty` `pthread_sigmask` `strerror`
    `wctomb`, errno locks, stdio backend
  - clock: `clock_gettime` `gettimeofday` `time` `times` over QNX
    `Clock*`
  - stub: `poll()` — real wait/wake deferred to v0.8
- New `TM_REQ_*` ops for these: `CLOCK_FREQ`, `CHDIR`, `GETCWD`,
  `DUP_CAP`, `UMASK`, `PROC_SELF_INFO`, `SET_CRED`, `FSTAT`,
  `READLINK`, `LSEEK`, `READDIR`, `ACCESS`.

### QNX Clock*, timers, deferred replies
- QNX `Clock*` family in `libqsoe/src/time.c`, backed by RISC-V `rdtime`
  and a per-process cached frequency.  `TM_CLOCK_FREQ_HZ` hardcoded
  for qemu-riscv-virt (v0.8 reads `/cpus/timebase-frequency` from FDT).
- New `proc/timer.c`: 16-slot sleeper queue + per-process `ITIMER_REAL`
  state.  Hybrid lazy expiry: `tm_timer_sweep()` runs at every dispatch
  entry, replies to expired `nanosleep` callers, fires `SIGALRM` pulses
  on expired itimers.  No kernel patch in v0.7 — `option 3` hybrid is
  enough to ship.  libc shims: `nanosleep`, `setitimer` (backs
  `alarm()`), `pause`.
- New `MsgSavereply(rcvid)` + extended `MsgReply(rcvid, ...)`:
  - `MsgSavereply` copies the implicit reply cap into a fresh CSpace
    slot via `seL4_CNode_SaveCaller`, returns a stable rcvid with the
    `QSOE_RCVID_SAVED = 0x80000000` bit set.
  - `MsgReply` checks the bit: saved rcvids dispatch on the slot
    (`seL4_Send` + slot recycle); plain rcvids use the implicit cap.
  - **GOTCHA**: `QSOE_RCVID_SAVED` is the sign bit of `int`, so a
    *successful* save returns a negative signed integer.  Failure
    sentinel is `-1` exactly; callers must check `if (saved == -1)`,
    not `if (saved < 0)`.  Documented near the macro and in the
    design doc Chapter 4.
- `_msg_info` gained `unsigned label` — the seL4 message-info label
  surfaced so resmgrs dispatch on the wire-protocol tag without
  touching seL4 types directly.  `MsgReceive` populates it.

### Resource managers (the "no seL4 in resmgrs" rule, enforced)
- **`/sbin/pipe`** (rc2) — first non-driver System Program.  16-pipe
  pool, 4 KiB ring each, QNX rcvid-park for blocking read-on-empty
  and write-on-full.  Lives under `userland/sbin/pipe/`; uses ONLY
  libqsoe + libc.  `TM_REQ_PIPE_CREATE` mints two badged Send caps
  (read-end + write-end) on the caller's behalf.
- **`devc-ser8250` rewrite** (rc3) — directory moved to
  `userland/dev/ser8250/`.  Source now imports exactly:
  ```
  #include <stdio.h>
  #include <qsoe-system.h>
  #include <qsoe/slots.h>
  #include <qsoe/wire.h>
  ```
  All `qsoe_sys_*`, `qsoe_cnode_*`, `seL4_*`, and `../taskman/*`
  includes gone.  The old SaveCaller + slot-pool park pattern replaced
  by `MsgSavereply`; the explicit pulse-drain loop replaced by
  `MsgReceive`'s `QSOE_MI_PULSE` flag.  IRQ thread uses three new
  libqsoe wrappers — `qsoe_irq_set_notification`, `qsoe_irq_wait`,
  `qsoe_irq_ack` — in `src/irq.c`.
- **Two-step close** (rc2): `close(fd)` now does (1) `TM_REQ_CLOSE`
  on the fd's bound cap so the resmgr observes the close, (2)
  `TM_REQ_DETACH_CAP` for taskman-side cap delete + connection
  cleanup, (3) local fd unbind.  External resmgrs see closes
  uniformly with in-taskman ones.
- **`dup()` fix** (rc3): `tm_dup_cap` now clones the connection
  registry row alongside the cap copy, so each dup'd fd has its own
  row and closing one doesn't dangle the others.

### Line discipline (rc3)
- New `userland/libqsoe/src/ldisc.c` + declarations in
  `<qsoe-system.h>`:
  - Opaque `qsoe_ldisc_t`; `qsoe_ldisc_attr_t` with `icanon` /
    `echo` / `echoe` / `isig` / `icrnl` / `opost` / `onlcr` flags
    and `VINTR` / `VERASE` / `VKILL` / `VEOF` chars.
  - `qsoe_ldisc_open(fd_in, fd_out, attr)` — two fds, because in
    QSOE stdin and stdout are independently-minted caps; writing to
    fd 0 is a code smell.  qsh calls `qsoe_ldisc_open(0, 1, NULL)`.
  - `_readline` (canonical mode), `_readbyte` (single byte),
    `_write` (with optional NL → CR-NL translation).
  - Backspace accepts both 0x08 (BS) and 0x7F (DEL) regardless of
    `verase`.  `VINTR` aborts `readline` with `EINTR`.
- qsh's `lex.c` interactive prompt path now routes through
  `qsoe_ldisc_readline`.  **Backspace, VKILL, VEOF visible erase
  all work** on the real `/dev/ser1` UART.  Arrow-key history and
  Emacs-style raw editing remain a v0.7+ item (would need
  `qsh/edit.c` + raw-mode driver handshake).

### BSD-style boot (rc3)
- `/sbin/init` is now a **shell script** (`userland/init/init.sh`,
  mode 0755):
  ```sh
  #!/bin/sh
  /sbin/devc-ser8250
  /sbin/repath /dev/console /dev/ser1
  exec /bin/qsh -i
  ```
- **Shebang support** in `tm_spawn`: blobs starting with `#!` are
  recognised, the interpreter is looked up in cpiofs, argv is
  re-built per Linux convention (`[interp, optional_arg, script_path,
  original_argv[1..]]`), and the interpreter ELF is loaded.  Recursion
  limit 1 — no nested `#!`.
- **CPIO symlinks**: `bin/sh -> bin/qsh` ships in `userland.cpio`.
  `tm_cpio_lookup` in `path/cpiofs.c` resolves one level of symlinks
  (absolute *and* relative targets) and is used by `tm_cpiofs_open`,
  `tm_cpiofs_probe`, and the shebang interpreter lookup.
- **`/sbin/repath`** (new ~50-line helper) — CLI front-end for
  `qsoe_pathmgr_repath`.  Two forms:
  - `repath <target> <source>` — copies source's pathmgr binding to
    target.  Used by init.sh; no pids needed at the script level.
  - `repath <target> <pid> <chid>` — explicit form for tests.
- New `TM_REQ_PATHMGR_RESOLVE` + `qsoe_pathmgr_resolve()` library
  wrapper: longest-prefix lookup returning the bound
  `(server_pid, server_chid, handler_kind)`.  `/sbin/repath` uses it
  for the `<target> <source>` form.
- `tm_process_create_by_name` handles absolute paths
  (`/sbin/devc-ser8250`) directly; bare names try `bin/` then
  `sbin/` (minimal `PATH`-search until libc gains `execvp`).
- `spawn_name_eq` compares basenames so the devc-ser8250 cap-grant
  special case works regardless of how the program was looked up.

### Cleanup: `.elf` suffix dropped, `hello` retired
- CPIO entries lose the `.elf` suffix: `bin/init` (script), `bin/qsh`,
  `bin/tester`, `bin/sh` (→ qsh), `sbin/devc-ser8250`, `sbin/pipe`,
  `sbin/repath`.  Build outputs in `$(BUILD)/*.elf` keep the suffix
  for tooling clarity.
- `userland/hello/` retired entirely — its smoke-test role is
  covered by tester + qsh.
- `userland/init/main.c` and `init/start.S` deleted; only `init.sh`
  remains.

### Errno + caps
- New errno: `ENOEXEC = 8` in `<qsoe-system.h>` (used by shebang on
  nested-interpreter / malformed script).
- `cpiofs` stat returns mode `0555` (was `0444`) so qsh's
  `search_access` lets binaries run.  Per-entry CPIO mode bits
  threading deferred.

### Documentation
- Design doc Chapter 4 updated: new sections "Line discipline",
  "Driver-side IRQ surface", "Deferred replies (MsgSavereply)";
  obsolete `__sysinfo` bridge and brk/heap text rewritten;
  "Relationship to musl libc" reflects the current direct
  `os_dependent/` design and mmap-based malloc.  PDF rebuilds clean
  (30 pages).

### Verification
- `make` — green; all binaries produced.
- Boot in qemu — taskman banner, devc-ser8250 attaches, `/dev/ser1`
  registered, init repoints `/dev/console`, qsh prompt visible on
  the real UART.
- Interactive typing + backspace + VKILL exercise the full LDISC
  path on the rewritten driver.
- `make` in `doc/tex/Design/` rebuilds the design PDF clean.

### Known gaps / deferred to v0.7+
- Arrow-key history navigation (needs qsh's raw-mode `edit.c` wired
  in + `tcgetattr`/`tcsetattr` driver handshake).
- `pause()` blocks but nothing wakes it yet — real signal delivery
  to the in-process signal thread is v0.8.
- `poll()` ships as a stub; real wait/wake is v0.8.
- `TM_CLOCK_FREQ_HZ` hardcoded; v0.8 reads `/cpus/timebase-frequency`
  from the device tree.
- Cap-leak smoke test (tester) no longer runs at boot — init.sh
  doesn't spawn tester.  Run on demand from the shell.
- Mode-bit fidelity for cpiofs (currently every regular file is
  0555).

## [v0.6.4] — 2026-05-15

**Milestone: qsh runs and accepts input from the real interrupt-driven
console.**  Boot to `# ` prompt; commands typed at the QEMU terminal
flow through `devc-ser8250` (16550 UART driver) end-to-end into the
shell and back out.

### Memory model: no brk anywhere
- `lite_malloc.c`, `oldmalloc/`, `mallocng/` filtered out of `libc.a`
  in `userland/libc/Makefile` — all three musl backends call `SYS_brk`.
- New `userland/libqsoe/src/malloc.c` provides
  `malloc/realloc/free/calloc` plus the musl-internal aliases
  (`__libc_malloc`, `__libc_malloc_impl`, `__libc_free`,
  `__libc_realloc`, `__libc_calloc`) using only `mmap`.  Bump-pointer
  arena grown on demand by 2 MiB Mega_Pages; `free` is a no-op for
  v0.6.4 — proper freelist deferred.
- `SYS_brk` removed from `qsoe_syscall_dispatch`; `SYS_mmap` (riscv64
  #222) added.
- `qsoe_mmap(addr, length, prot, flags, fd, off)` in
  `syscall_dispatch.c` sends `TM_REQ_MMAP` to taskman's Memory
  Manager.

### Memory Manager in taskman
- New wire label `TM_REQ_MMAP = 0x2c`.
- New `tm_mmap_serve` in `userland/taskman/spawn.c` — allocates
  Mega_Pages from taskman's untyped pool on demand and maps them
  contiguously into the caller's vspace at its `mmap_top` cursor.
- New per-process `mmap_top` field on `tm_process_t`, initialised to
  `QSOE_MMAP_BASE = 0x2000000` (32 MiB).
- Pre-allocated heap region deleted from `tm_spawn` — memory is now
  fully on-demand.

### Interrupt-driven console input (read path)
- `devc-ser8250` refactored to the QRV two-thread design with the
  park/wake handoff implemented in QSOE primitives:
  - IRQ thread: `seL4_Wait(IRQ_NTFN)` → `uart_drain_rx` →
    `MsgSendPulse(self_coid, ...)` to wake the main thread →
    `irq_handler_ack`.
  - Main thread: single `seL4_Recv` loop, distinguishes IPC vs. wake
    by `badge & QSOE_NTFN_BADGE_BIT`.  On `TM_REQ_IO_READ` with empty
    ring: `seL4_CNode_SaveCaller` into a fresh slot, store as
    `g_pending_reader_slot`, re-Recv without replying.  On
    bound-Notification wake: drain pulse queue and, if a reader is
    parked + ring has data, `seL4_Send` the deferred reply on the
    saved slot.  Race re-check after the stash mirrors QRV.  Single
    blocking reader; a second concurrent reader gets `EBUSY`.
- Minimum line discipline in `uart_drain_rx`: inbound `\r → \n`
  (terminals send CR on Enter) + echo each printable byte back via
  `uart_tx_byte` so the user can see typing.  Full termios deferred.

### CSpace plumbing
- New well-known slot `QSOE_CAP_CNODE_SELF = 9` — cap to the
  process's own CNode, minted by `spawn.c` so user-space resmgrs can
  invoke `seL4_CNode_SaveCaller` from inside their own process.
  `QSOE_CAP_CNODE_DEPTH = 12` matches the freshly-retyped 12-bit
  CNode radix.
- New libqsoe slot allocator
  `qsoe_state_alloc_empty_slot/free_empty_slot` — bumps in
  `[0x800..0x1000)`, recycles freed slots through a small free list.
  Used as `SaveCaller` destinations.

### Signal API (compile-and-link scaffolding)
- `userland/libqsoe/src/signal.c` (new) — `signal`/`sigaction`/`kill`/
  `raise` plus signal-thread bootstrap that calls
  `TM_REQ_REGISTER_SIGNAL_CHID`.  Wire labels
  `TM_REQ_REGISTER_SIGNAL_CHID = 0x2a` and
  `TM_REQ_GET_SIGNAL_CHID = 0x2b` plus per-process `signal_chid`
  field on `tm_process_t`.
- `qsoe_signal_init` is stubbed in v0.6.4 — pulse delivery into the
  signal thread needs per-TCB notification binding which the kernel
  currently rejects (one bound notification per TCB).  Deferred to
  v0.6.5.  Stub prints a one-time warning and increments
  `qsoe_signal_init_entered` so it can't silently rot.

### musl-internal stubs
- `userland/libqsoe/src/musl_stubs.c` (new) — `__lsysinfo`, `__wait`,
  `__timedwait_cp` stubs needed at link time.  Each stub prints a
  loud "first-hit" warning and increments a counter
  (`qsoe_stub_hits_*`).

### init / qsh wiring
- `init` spawns qsh with `argv = { "qsh", "-i", 0 }` so `FTALKING`
  is set without depending on `isatty()`.  `PS1=#` exported via the
  child envp.

### Errors / wire-protocol
- New error `EBUSY = 16` in `qsoe/qrv.h`.
- New wire label `TM_REQ_MMAP = 0x2c`.

### Known gaps / deferred
- No line editor: backspace, arrow keys, history navigation don't
  work — typed input is taken as-is.  Full termios + edit.c port is
  v0.7+.
- `qsh pwd` errors out with "Destination address required" — no
  `getcwd`/filesystem yet.
- Signal delivery into the in-process signal thread waits on
  v0.6.5's per-TCB notification binding work.
- musl-stub real implementations tracked in TaskList #71.

## [v0.6.3] — 2026-05-14

**Stream A landing: QRV's mksh-derived shell ported into QSOE as
`userland/qsh/`, ready to compile clean and link against
libqsoe + musl.**

### Added
- Full qsh source tree in `userland/qsh/` (mksh-derived; license
  attribution preserved in `License-mksh-orig.txt` + per-header
  comments).
- `qsh_error.h` — designed `qsh_error_t` + `QSH_TRY()` for future
  setjmp/longjmp rip-out.

### Changed
- **Branding sweep** — all mksh/MKSH/Mksh/mksh_, ksh_/KSH_, mir/MIR
  vendor tags, mbsd*, mbcc* replaced with qsh/QSH equivalents.
- **mbsdint.h → qsh_intmath.h** (deep trim deferred).
- **Obsolete primitives stripped** — termios scaffolding deleted
  entirely (`x_mkraw`, `qsh_tcget/set`, `tty_state`, `struct termios`
  all gone; line discipline lives in `edit.c` against raw bytes from
  `devc-ser8250`).  TIOCGWINSZ branch out of `var_special.c`; magic-
  number table out of `exec.c`.
- **musl posix `setjmp`/`longjmp`** linked (BSD `_setjmp`/`_longjmp`
  pair upstream relied on doesn't exist in musl).  Full rip-out to
  explicit `qsh_error_t` propagation deferred to v0.6.4+.
- **File-size refactor** — `histrap` → `history + history_file +
  trap`; `tree` → `tree + tree_format`; `syn` → `syn + syn_compound
  + error`; `eval` → `eval + eval_expand`; `misc` → `misc + misc_str
  + misc_path`; `var` → `var + var_special`; `exec` → `exec + exec_io`.
- **Global unifdef purge** — 2826 lines removed by pinning
  `HAVE_*`/`QSH_*` gates from `qsh_config.h`.  Dead-platform sweep
  removed `__OS2__` and EBCDIC blocks across 6 files (~390 lines).

### Build modularisation
- `userland/libc/Makefile` — standalone musl build.
- `userland/libqsoe/Makefile` — produces `libqsoe.a` (normal) +
  `libqsoe-tm.a` (`-DQSOE_LIBQSOE_IN_TASKMAN`).
- `userland/taskman/Makefile` — `taskman.elf`, with embedded
  userland CPIO via `.incbin`.
- `userland/devc-ser8250/Makefile` — 16550 UART driver.
- Top-level `Makefile` shrunk ~250 lines; each component reachable
  via `cd userland/<name> && make clean all`.

### libc cleanup
- `scripts/extract-sel4-riscv.sh` gained recursive directory-
  exclusion pattern (`path/**/dirname/`).
- `placement.txt` expanded — non-RISC-V arch subdirs and
  Linux-specific subtrees / files (clone/exec/wait family in
  `src/process/`, futex/clone users in `src/thread/`, setresuid
  family, setdomainname) excluded.  190 files removed from
  `core/userland/libc/src/`.  Makefile's find collapsed to single
  recursive find.
- musl per-arch `.S`/`.s` files (`setjmp.S`, `longjmp.S`, ...) now
  compiled into `libc.a` — fixed a bug where their `.o` stubs were
  empty.

### Build state
- 0 compile failures across 5 user ELFs + taskman + qsoe.elf.
- `make qsh` target compiles all 30 `.c` files clean; 8 link
  warnings (posix_spawn/waitpid/`__wait` — satisfied by libqsoe at
  real-link time via `userland/qsh/Makefile`).

## [v0.6.2] — 2026-05-14

### Added
- **qsh compile-attempt target** (`make qsh`). Pulls QRV's
  `userland/sh` (mksh-derived QNX shell) via
  `scripts/pull-qsh.sh` into `userland/qsh/`, then compiles each
  `.c` independently against musl + libqsoe with errors captured
  per-file instead of aborting. Also performs a partial link with
  `--warn-unresolved-symbols` and collects the leftover symbol set.
- Three artefacts in `build/`:
  - `qsh.log` — full per-file compile log
  - `qsh.symbols.txt` — linker undefined-symbol warnings
  - `qsh.symbols.txt.objs.txt` — `nm -u` per-object set (sorted unique)
  - `qsh.summary.txt` — human-readable triage summary

### Results
- **22 of 29 `.c` files compile cleanly** against musl + libqsoe.
- **7 files fail**, in four categories:
  1. Missing QRV proprietary header (`sys/qrv_core.h`) in `edit.c`.
  2. Header-typedef breakage in `histrap.c`, `shf.c` — `sh.h` needs
     a typedef that lives behind the missing header.
  3. Missing platform constants (`SIGEMT`, `MKSH_DEFAULT_TMPDIR`)
     in `histrap.c`, `tempfile.c`.
  4. Inter-file forward-decl ordering in `eval.c`, `jobs.c`,
     `misc.c` (e.g. `j_change` referenced before its prototype).
- **84 link-time undefined symbols**; almost all are mksh-internal
  names defined IN the 7 fail-to-compile files. The actual missing
  libc primitives once those files build are roughly 5-10.

### Deliverable for Yuri's analysis
The compile log + symbol set is the raw material for deciding how
to proceed in v0.6.x or v0.7+: port what's needed of
`sys/qrv_core.h`, add the missing platform constants, untangle
forward-decl ordering, then evaluate which libc/system primitives
QSOE needs to add (pipes, dup2, termios, etc.).

## [v0.6.1] — 2026-05-14

### Added
- **`/sbin/init` takes over boot.** taskman now spawns init as the
  first userland process; init is responsible for spawning everything
  else (drivers, getty, etc.). Standard Unix-style boot chain.
- **`procmgr_detach(status)` + `waitpid(pid, *status, 0)`** —
  QNX/QRV-style "daemon stays resident" synchronisation. Child calls
  `procmgr_detach` when ready to serve; the parent's `waitpid`
  unblocks with the status; the child is reparented to pid 1 and
  keeps running. waitpid blocking uses `seL4_CNode_SaveCaller` to
  park the parent's reply cap. `_exit` also delivers status to any
  parked waiter (so non-daemon children work too).
- **`tm_process_t.parent_pid` + exit-state machine.** Tracks parent
  pid (set at spawn time from the calling pid), exit_state
  (alive / detached / exited), exit_status, and a per-child
  `waiter_reply_slot` for the parked SaveCaller cap.
- **IRQ invocation wrappers** in `qsoe_invoke.h`:
  `qsoe_irq_control_get` (RISC-V-specific Get-with-trigger variant),
  `qsoe_irq_handler_set_notification`, `qsoe_irq_handler_ack`.
- **Device-untyped scan** at boot — `find_device_untyped_for_paddr`
  walks `bi->untypedList[]` for the untyped covering a given paddr.
  taskman resolves the 16550 UART at `0x10000000` and stores its
  cap slot for later granting.
- **Driver-special-case in spawn.c.** When the ELF name is
  `devc-ser8250.elf`, also grant: IRQHandler for PLIC line 10
  (`QSOE_CAP_IRQ_HANDLER`), 4 KiB UART device frame mapped at
  `0xA00000` in the child's VSpace (`QSOE_CAP_UART_FRAME`), and a
  fresh Notification for IRQ-thread wakeups (`QSOE_CAP_IRQ_NTFN`).
- **`TM_REQ_PATHMGR_REGISTER` + `TM_REQ_PATHMGR_REPATH`** — runtime
  path-namespace mutation. Resmgrs announce themselves via
  REGISTER; init uses REPATH to swap `/dev/console` to the real
  UART driver once it's ready.
- **devc-ser8250** — QSOE's first userland-process resource manager.
  Split-thread architecture: main thread dispatches client I/O on
  its channel; dedicated IRQ thread (pinned to hart 1) blocks on
  the bound Notification and drains the UART RX FIFO into a shared
  ring buffer protected by a spinlock. Polled TX (the 16-byte UART
  FIFO absorbs writes). After init it calls `procmgr_detach(0)` to
  unblock its parent.
- **Stdio inheritance respects the current `/dev/console` binding.**
  `spawn.c` resolves the path manager at spawn time, so children
  spawned after `init`'s repath get stdio bound directly to
  devc-ser8250's channel — their `printf` bytes flow through the
  real driver's `uart_tx_byte`.

### Demo (boot transcript)
```
[init] alive, pid=2
[init] spawned devc-ser8250, pid=3
[devc-ser8250] 16550 initialised @ vaddr 0xA00000
[devc-ser8250] IRQ thread spawned, tid=2
[devc-ser8250] /dev/ser1 registered (chid=1)
[init] devc-ser8250 ready (detached with status 0)
[init] /dev/console now -> (3, 1) [devc-ser8250]
[init] spawned tester, pid=4
[tester] alive ... <full v0.6.0 test suite>  ← all via devc-ser8250
[init] tester exited, status=0
```

### Known limitations
- Cooked-mode line discipline (ICANON, echo, BS/erase) deferred.
  Raw 8N1 only — `read` returns whatever's in the ring buffer or 0.
- TX is polled. TX-via-interrupt for higher throughput is v0.7+.
- Driver cap granting is hardcoded (string-match on ELF name). A
  proper manifest-driven scheme is v0.7+.
- Kernel debug-putchar continues writing to the same UART via
  OpenSBI — output from before the console-switch (init's own
  early prints) interleaves with output via the real driver path.

## [v0.6.0] — 2026-05-14

### Added
- **cpiofs** — the embedded `userland.cpio` is now mounted at `/`
  via a third in-taskman handler kind
  (`PATHMGR_HANDLER_TASKMAN_CPIOFS`). `open("/bin/hello.elf")` works
  from any user program. Read-only; writes return `EROFS`.
  (`userland/taskman/cpiofs.{c,h}`)
- **Per-connection opaque context** (`unsigned long ctx[2]` on
  `tm_connection_t`). cpiofs uses it to stash the file's data
  pointer and the packed `(size << 32) | offset`. Generic — future
  stateful resmgrs can repurpose.
- **`tm_connection_badge_by_slot` / `tm_connection_set_ctx` /
  `tm_connection_get_ctx` helpers** in `server.c`.
- **Path manager root-match** — `tm_pathmgr_resolve` now seeds the
  deepest-match candidate from `g_root->has_obj` so a mount at `/`
  is reachable as a fallback even when no path components match.
- **CPIO layout migration**: entries are now `bin/tester.elf`,
  `bin/hello.elf`. `tm_process_create_by_name` internally prepends
  `bin/` so the wire protocol callers stay unchanged.

### Fixed
- **`TM_REQ_IO_READ` reply length** for variable payloads: the
  reply `MessageInfo.length` is now `4 + ceil(got/8)` so the kernel
  actually transfers `msg[4..]` to the client. Without this fix
  cpiofs reads returned stale bytes from the client's own IPC
  buffer (e.g. the path string from the previous `open` call).

## [v0.5.1] — 2026-05-14

### Added
- **musl libc linked into spawned binaries.** The vendored musl tree
  at `core/userland/libc/` is compiled into `build/libc.a` (~1260
  object files), then linked into tester and hello. The Makefile
  generates the `bits/alltypes.h` and `bits/syscall.h` headers from
  musl's own `.h.in` templates via the upstream `tools/mkalltypes.sed`
  script (extracted via placement.txt).
- **`__sysinfo` bridge** in `libqsoe/src/syscall_dispatch.c`. The
  patched `syscall_arch.h` at `core/userland/libc/patches/arch/riscv64/`
  routes every musl syscall through an indirect call on the global
  `__sysinfo` function pointer; `_qsoe_start_main` assigns it to
  `qsoe_syscall_dispatch` before user `main()` runs. Linux RISC-V
  syscall numbers (write=64, writev=66, read=63, openat=56, close=57,
  brk=214, exit=93, exit_group=94, plus a handful of no-op stubs)
  route to libqsoe's POSIX wrappers.
- **Heap region per process.** `spawn.c` reserves a 2 MiB
  `seL4_RISCV_Mega_Page` mapped at `[0x800000, 0xA00000)` in the
  child's VSpace. `qsoe_brk` tracks the current break pointer within
  this region; musl's `lite_malloc` uses it as a bump allocator.
- **Soft-float linker stubs** in `libqsoe/src/float128_stubs.c`.
  Empty aliases for `__addtf3`, `__multf3`, `__netf2`, etc.; the
  cross-compiler's libgcc.a is built for `lp64d` and won't link
  against our `lp64` (soft-float) objects, but musl's `vfprintf`
  references those symbols even on non-`%f` paths. The stubs satisfy
  the linker without dragging in a real soft-float runtime.
- **`__errno_location` override** — single static int rather than
  musl's per-thread TLS lookup (which needs full pthread init we
  don't run).
- **printf demo in hello.** Replaced the raw `sel4_debug_puts` calls
  with musl `printf` / `fprintf(stderr, ...)`. Output traverses the
  full stack: musl stdio → `writev` syscall → `__sysinfo` →
  `qsoe_syscall_dispatch` → `qsoe_writev` → `MsgSend(TM_REQ_IO_WRITE)`
  → taskman's `/dev/console` handler → `sel4_debug_putchar`.

### Known limitations
- No `%f` / `%Lf` printf — would need a real soft-float libgcc
  replacement or a switch to `lp64d` ABI.
- musl's `__libc_start_main` is bypassed entirely; the program
  doesn't get locale init, atexit handlers, stdio teardown on
  return. Programs must `fflush(stdout)` explicitly before `main`
  returns.
- pthread bootstrap is not run; `pthread_*` won't work yet.
- Heap is fixed at 2 MiB per process; growable heap via mmap is
  deferred.

## [v0.5.0] — 2026-05-14

### Added
- **Path manager in taskman.** First-class subsystem
  (`userland/taskman/pathmgr.{c,h}`): prefix-tree registry of named
  services, longest-prefix lookup, fixed pool of 64 nodes. Inspired
  by QNX's pathmgr tNode tree, written from scratch.
- **`/dev/console` as the first registered resource manager.** Lives
  inside taskman for v0.5.0 (`userland/taskman/console.{c,h}`).
  `write()` walks bytes through `sel4_debug_putchar` (the seL4
  debug-build SBI putchar). `read()` is stubbed to `EAGAIN` until a
  UART driver lands.
- **Wire labels for POSIX-shape I/O** (0x20-0x23): `TM_REQ_OPEN`,
  `TM_REQ_CLOSE`, `TM_REQ_IO_WRITE`, `TM_REQ_IO_READ`. The 0x20+
  range disambiguates the libc-facing wire from the taskman-internal
  management wire (0x01-0x1f).
- **Spawn-time stdio inheritance.** `spawn.c` now mints three badged
  Send-caps on `(TASKMAN_PID, TM_CONSOLE_CHID)` into every child's
  CSpace at `QSOE_CAP_STDIN/STDOUT/STDERR_CONNECT = 3/4/5`, and
  registers the three connections in taskman's connection table.
  `_qsoe_start_main` then calls `qsoe_state_force_bind_coid` to
  plant fds 0/1/2 in the child's coid table before `main()` runs ---
  matches POSIX's "fds 0/1/2 inherited across exec" contract.
- **libqsoe I/O wrappers** (`userland/libqsoe/src/io.c`):
  `qsoe_open`, `qsoe_close`, `qsoe_write`, `qsoe_read`. Each is the
  client-side half of one wire request; `qsoe_write` chunks payloads
  larger than ~900 bytes across multiple `TM_REQ_IO_WRITE` calls.
- **`tm_channel_by_badge` helper** in `server.c` — the dispatch loop
  uses it on every IO_WRITE/IO_READ to route the message to the
  right resmgr handler via (badge -> connection -> channel).
- **End-to-end smoke test in tester**: writes through fds 1 and 2,
  then `qsoe_open("/dev/console")` -> fd 3 -> `qsoe_write` -> close.
  All bytes appear on the QEMU console without any musl libc.

### Known limitations (target for v0.5.1)
- musl libc is extracted but not linked. `printf` doesn't work yet
  (use `qsoe_write` directly for now).
- `read()` returns 0 / `EAGAIN`. Real console input needs a 16550
  driver and is deferred.
- No heap; `qsoe_brk` not yet wired.

## [v0.4.4] — 2026-05-14

### Added
- **argv/envp delivery** on the child's initial stack per the RISC-V SysV
  ABI. taskman builds the standard layout (`argc`, argv pointers,
  argv NULL, envp pointers, envp NULL, auxv `AT_NULL`, strings) in
  the top page of a two-page spawn-allocated stack region at
  `[0x1FC000, 0x1FE000)`. sp is 16-byte aligned.
- Wire protocol for `TM_REQ_PROCESS_CREATE` extended to carry argc,
  envc, path length, and total-strings bytes in MR0-3; strings packed
  NUL-separated into `ipcbuf->msg[4..]`. Caps at 16 args, 16 envs,
  ~1 KiB total strings per spawn.
- New `_qsoe_start_main` C shim in `libqsoe/src/start_main.c`:
  performs `qsoe_libqsoe_init` and dispatches to a SysV-shaped
  `main(int argc, char **argv, char **envp)`.

### Changed
- crt0 contract: `start.S` no longer overrides sp. The kernel-supplied
  sp from `TCB_WriteRegisters` points at argc; crt0 loads argc/argv/envp
  into a1/a2/a3 and tail-calls `_qsoe_start_main`. The `.bss` static
  stack symbol is gone from both tester and hello.
- `posix_spawn` now actually forwards `argv[]` and `envp[]` to the
  child (the v0.4.1 stub ignored them).

## [v0.4.3] — 2026-05-14

### Added
- **libsel4 generated headers via placement.txt.** Pulls the four
  kernel-build-generated enums (`api/invocation.h`,
  `arch/api/invocation.h`, `arch/api/sel4_invocation.h`,
  `arch/api/syscall.h`) into `core/kernel/sel4_gen/`. taskman's
  `sel4_types.h` drops its hand-counted `INV_*`/`SYS_*` table and
  aliases the upstream enum members.
- **Bound-Notification pulse delivery.** Each `tm_channel_t` carries a
  Notification minted with `QSOE_NTFN_BADGE_BIT` (bit 63), bound to
  the owner's TCB via `seL4_TCB_BindNotification`. `MsgSendPulse`
  fires `seL4_Signal`; `MsgReceive` drops the unconditional pre-poll
  and instead checks the badge after a single `seL4_Recv`.
- **Multi-server IPC.** `hello.elf` now does a `ChannelCreate` +
  `MsgReceive`/`MsgReply` loop. tester `ConnectAttach`es to hello with
  retry, runs `MsgSend` round-trips, and `ConnectServerInfo` confirms
  the non-taskman server identity.
- Cross-hart wake test: a pulse-sender worker pinned to hart 1 fires
  while main blocks on hart 0, proving real parallelism through the
  bound-Notification path.

### Changed
- Pulse cost: idle `MsgReceive` from 3 IPCs → 1 IPC (the unconditional
  poll tax is gone); pulse delivery from 4 IPCs → 2 IPCs.

## [v0.4.2] — 2026-05-13

### Added
- **QNX-style pulses.** Async fixed-size queued messages: 8-bit code +
  32-bit value, layout matches QNX's `struct _pulse` (type, subtype,
  code, value, scoid). Per-channel 8-entry ring buffer in taskman;
  overflow returns `EAGAIN`.
- `MsgSendPulse(coid, prio, code, value)` and pulse delivery via
  `MsgReceive` (returns a `_pulse` struct; `_msg_info.flags |=
  QSOE_MI_PULSE`).
- Wire requests `TM_REQ_PULSE_SEND` and `TM_REQ_PULSE_FETCH`.

### Known limitations (addressed in v0.4.3)
- Receivers must poll `MsgReceive` to drain pulses; a blocked receiver
  doesn't wake on pulse arrival alone.

## [v0.4.1] — 2026-05-13

### Added
- **Process lifecycle (Tier 1).** Real pid allocator in taskman; pid 1
  reserved for taskman; child pids start at 2.
- `ProcessCreate(path)` + `posix_spawn(*pid, path, ...)` (argv/envp
  ignored until v0.4.4) — looks up an ELF in the embedded userland
  CPIO and spawns it.
- `ProcessTerminate(pid, status)` with self-terminate (`pid==0`); `exit`
  / `_exit` route through this.
- Per-child **untyped budget** (256 KiB) retyped at spawn and copied
  into the child's CSpace at `QSOE_CAP_OWN_UNTYPED`.
- **Cap-leak hygiene**: taskman's CSpace slot free-list. Verified
  delta=0 across 100 `ChannelCreate`/`Destroy` cycles.
- `hello` binary: second non-taskman process, exercises
  `posix_spawn` end-to-end.

## [v0.4] — 2026-05-13

### Added
- **QNX-compatible threading API on SMP.** `ThreadCreate`,
  `ThreadJoin`, `ThreadDetach`, `ThreadDestroy`, `ThreadCancel`,
  `ThreadCtl` (NAME, RUNMASK).
- Per-thread state via the RISC-V `tp` register; libqsoe's TLS macros
  (`qsoe_curthr()`, `qsoe_self_pid`, `qsoe_ipcbuf`, `qsoe_errno`).
- Trampoline + Notification-based join sync.
- Deferred cancellation: each libqsoe IPC entrypoint hits a cancel
  point and self-terminates the thread if `cancel_pending` is set.

## [v0.3.4] — 2026-05-13

### Added
- **SMP boot.** seL4 kernel rebuilt with `SMP=TRUE NUM_NODES=4`.
  Elfloader's autoconf bumped to `CONFIG_MAX_NUM_NODES=4`.
- `qsoe_tcb_set_affinity` invocation wrapper. `_thread_attr.runmask`
  pins threads to specific harts.
- QEMU runs with `-smp 4`.

### Changed
- All invocation labels recounted for SMP (TCBSetAffinity inserted at
  position 15; arch labels shifted +1).

## [v0.3.3] — 2026-05-13

### Added
- **Side-channel coid namespace.** Bit 30 marks side-channel
  identifiers (matches QNX's `_NTO_SIDE_CHANNEL`); keeps system
  connections from aliasing FD-numbered coids 0/1/2.
- `SYSMGR_PID`, `SYSMGR_CHID`, `SYSMGR_COID` constants; the taskman
  connection is pre-bound at libqsoe init.
- `ConnectServerInfo`, `ConnectClientInfo`, `ConnectFlags` introspection.

## [v0.3.2] — 2026-05-13

### Added
- Real cross-process IPC: tester ↔ taskman round-trips through the
  wire protocol (no more `QSOE_LIBQSOE_IN_TASKMAN` shortcut for
  cross-process calls).
- Pid-as-badge convention on every endpoint cap minted by taskman.

## [v0.3.1] — 2026-05-13

### Added
- `MsgSend`, `MsgReceive`, `MsgReply` in libqsoe.
- Byte ↔ word marshalling: MR0-3 register-passed, longer messages
  spill into `ipcbuf->msg[4..]`.
- `_msg_info` struct (minimal subset of QNX's `struct _msg_info`).

## [v0.3.0] — 2026-05-13

### Added
- **Second user-space process.** taskman hand-rolls a complete spawner
  (`tm_spawn`): ELF walker, page-table builder, frame allocator,
  CSpace minter, TCB configure + WriteRegisters + Resume.
- tester binary spawned from taskman, runs on its own TCB.
- Userland CPIO embedded in taskman.elf via `.incbin`.
- Seven new seL4 invocations wrapped in `qsoe_invoke.h`:
  `TCB_Configure`, `TCB_WriteRegisters`, `TCB_SetPriority`,
  `TCB_Resume`, `RISCV_Page_Map`, `RISCV_PageTable_Map`,
  `RISCV_ASIDPool_Assign`.

## [v0.2] — 2026-05-13

### Added
- **Four core IPC lifecycle calls** in libqsoe + taskman:
  `ChannelCreate`, `ChannelDestroy`, `ConnectAttach`, `ConnectDetach`.
- taskman's channel/connection registries (`tm_channel_t`,
  `tm_connection_t`).
- `QSOE_LIBQSOE_IN_TASKMAN` build flag — libqsoe entrypoints call
  `tm_*` handlers directly when compiled into taskman (no self-IPC).

## [v0.1] — 2026-05-13

### Added
- First bootable QSOE image. Top-level `Makefile` builds elfloader,
  seL4 kernel, and a seed `taskman.elf` (initially a spin loop, then
  a banner-printer).
- OpenSBI → elfloader → seL4 kernel → taskman boot chain on
  qemu-riscv-virt.


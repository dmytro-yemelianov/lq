# Changelog

All notable changes to QSOE. Format inspired by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning is
`vMAJOR.MINOR[.PATCH]` until v1.0, which is reserved for the first
release with full QNX libc compatibility.

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


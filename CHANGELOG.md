# Changelog

All notable changes to QSOE. Format inspired by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning is
`vMAJOR.MINOR[.PATCH]` until v1.0, which is reserved for the first
release with full QNX libc compatibility.

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


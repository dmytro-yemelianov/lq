# Changelog

All notable changes to QSOE. Format inspired by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning is
`vMAJOR.MINOR[.PATCH]` until v1.0, which is reserved for the first
release with full QNX libc compatibility.

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


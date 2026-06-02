/*
 * sys/platform.h — board-specific constants.
 *
 * v0.7 carries the single value taskman needs to publish through
 * TM_REQ_CLOCK_FREQ: the frequency of RISC-V's `time` CSR (the one
 * `rdtime` reads).  Every spawned process queries this at startup
 * and caches it; ClockTime / nanosleep / times all read rdtime
 * directly and convert via the cached value.
 *
 * v0.8 MUST replace this hardcode with a runtime FDT parse —
 * /cpus/timebase-frequency is in every RISC-V DTB and arrives via
 * OpenSBI's handoff.  See [[project_clock_freq_fdt_v08.md]].
 *
 * Current value is for qemu-riscv-virt (10 MHz).  Other platforms
 * have different frequencies (U740: 1 MHz; K3: ~24 MHz).
 */
#ifndef QSOE_TASKMAN_PLATFORM_H
#define QSOE_TASKMAN_PLATFORM_H

#define TM_CLOCK_FREQ_HZ  10000000UL    /* qemu-riscv-virt: 10 MHz */

#endif /* QSOE_TASKMAN_PLATFORM_H */

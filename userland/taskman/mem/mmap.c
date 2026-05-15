/*
 * mem/mmap.c — Memory Manager.
 *
 * tm_mmap_serve allocates Mega_Pages out of taskman's untyped pool,
 * maps them contiguously into the caller's VSpace at its mmap_top
 * cursor, advances mmap_top, and returns the base vaddr.
 *
 * v0.6.4 limitations (carried forward from spawn.c, where this used
 * to live):
 *   - Granularity is 2 MiB; callers asking for less get a 2 MiB
 *     mapping anyway.
 *   - No munmap, so addresses are bump-allocated and never reclaimed.
 *   - No L0 PT lazy-allocation: each Mega_Page goes straight into the
 *     L1 it shares with neighbours, which works because Sv39 has one
 *     L1 entry per 1 GiB and we never grow past 1 GiB per process.
 *     v0.7+ will lift that.
 */

#include "mem.h"
#include "../proc/proc.h"
#include "../sel4_syscalls.h"
#include "../qsoe_invoke.h"

int tm_mmap_serve(pid_t caller, unsigned long len, unsigned long *out_vaddr)
{
    tm_process_t *proc = tm_process_lookup(caller);
    if (!proc) return -ESRCH;
    if (len == 0) return -EINVAL;

    /* Round up to a multiple of QSOE_MEGA_PAGE. */
    unsigned long bytes = (len + QSOE_MEGA_PAGE - 1) & ~(QSOE_MEGA_PAGE - 1);
    unsigned long base  = proc->mmap_top;
    unsigned long pages = bytes / QSOE_MEGA_PAGE;

    for (unsigned long i = 0; i < pages; ++i) {
        seL4_CPtr frame = taskman_alloc_and_retype(seL4_RISCV_Mega_Page, 0);
        if (!frame) {
            sel4_debug_puts("tm_mmap_serve: Mega_Page alloc failed\n");
            return -ENOMEM;
        }
        seL4_Word err = qsoe_riscv_page_map(frame, proc->vspace,
                                            base + i * QSOE_MEGA_PAGE,
                                            QSOE_RIGHTS_ALL,
                                            QSOE_VM_ATTR_DEFAULT);
        if (err) {
            sel4_debug_puts("tm_mmap_serve: Page_Map failed\n");
            return -ENOMEM;
        }
    }

    proc->mmap_top = base + bytes;
    *out_vaddr = base;
    return 0;
}

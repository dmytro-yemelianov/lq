/*
 * sys/sysmap.h -- LQ taskman's builder for the QSOE sysmap page.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TM_SYS_SYSMAP_H
#define TM_SYS_SYSMAP_H

/*
 * Build the 'PSYS' sysmap TLV page from the already-parsed syscfg blob
 * (tm_syscfg_build must have run first).  Returns 0 on success, -1 if
 * the syscfg blob isn't ready.  Result is cached in a static page that
 * proc/spawn.c copies into every child and maps read-only at
 * QSOE_SYSMAP_VA.
 */
int tm_sysmap_build(void);

/*
 * Hand back the cached sysmap page (always TM_SYSMAP_PAGE_BYTES, the
 * tail zero-padded) plus the populated length.  Returns 0 on success,
 * -1 if tm_sysmap_build() hasn't run.
 */
int tm_sysmap_get(const void **out_page, unsigned *out_len);

#endif /* TM_SYS_SYSMAP_H */

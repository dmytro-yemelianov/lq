/*
 * sys/syscfg.h — taskman's system-configuration builder + query.
 *
 * Built once at boot from the FDT, queried by clients via the
 * TM_REQ_GET_SYSCFG message.  Wire format is defined in
 * <qsoe/syscfg.h> (shared with clients).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_SYSCFG_H
#define QSOE_TASKMAN_SYSCFG_H

#include <qsoe/syscfg.h>
#include "../sel4_types.h"

/* Build the syscfg blob from the FDT pointed to by `fdt_blob`.
 * Returns 0 on success, -1 on error (invalid FDT / buffer overflow
 * during emit).  After a successful call, tm_syscfg_*() return the
 * built blob. */
int tm_syscfg_build(const void *fdt_blob);

/* Get a pointer to (and length of) the built blob.  Returns 0 on
 * success, -1 if tm_syscfg_build() hasn't run / failed. */
int tm_syscfg_get(const void **out_blob, unsigned *out_len);

/* Convenience — read a u64 tag.  Returns 0 on success. */
int tm_syscfg_find_u64(unsigned tag_id, uint64_t *out);

/* Convenience — read a u32 tag.  Returns 0 on success. */
int tm_syscfg_find_u32(unsigned tag_id, uint32_t *out);

/* Find the first occurrence of `tag_id` and return its payload via
 * *out_ptr/*out_len.  Returns 0 on success, -1 on not-found. */
int tm_syscfg_find(unsigned tag_id, const void **out_ptr,
                   unsigned *out_len);

#endif /* QSOE_TASKMAN_SYSCFG_H */

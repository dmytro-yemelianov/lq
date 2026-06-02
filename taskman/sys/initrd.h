/*
 * sys/initrd.h -- LQ taskman initrd loader.  Vestigial; activate via
 *                 -DTM_USE_INITRD_LOADER.  See sys/initrd.c.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <r_tty@yahoo.co.uk>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TM_SYS_INITRD_H
#define TM_SYS_INITRD_H

#include "../sel4_types.h"

/* Discover the userland CPIO archive that QEMU passed via `-initrd`
 * (FDT /chosen/linux,initrd-{start,end}) and map it into taskman's
 * VSpace at a fixed VA range.  On success returns 0, *out_data is a
 * read-only pointer to the archive, *out_size its length.  On any
 * failure (no FDT, no initrd-* nodes, no covering RAM untyped, ...)
 * returns -1 with a tm_err line explaining which step failed. */
int tm_initrd_load(seL4_BootInfo *bi, const void *fdt,
                   const void **out_data, unsigned long *out_size);

#endif /* TM_SYS_INITRD_H */

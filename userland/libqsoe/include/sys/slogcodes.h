/*
 * sys/slogcodes.h — system-wide catalogue of slog major codes.
 *
 * Each subsystem owns a major code; minor codes are subsystem-local.
 * QRV-compatible names kept where they exist so ported callers
 * (pci-server's _SLOGC_PCI, etc.) need no patching.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SYS_SLOGCODES_H
#define QSOE_SYS_SLOGCODES_H

/* Kernel / early boot. */
#define _SLOGC_KERNEL       1

/* taskman (procnto-equivalent). */
#define _SLOGC_TASKMAN      2

/* Drivers — character devices. */
#define _SLOGC_CHAR         100
#define _SLOGC_CONSOLE      101

/* PCI subsystem (matches QRV). */
#define _SLOGC_PCI          110

/* Block devices. */
#define _SLOGC_BLOCK        120

/* Filesystem servers. */
#define _SLOGC_FSYS         140

/* Network stack (reserved). */
#define _SLOGC_NET          160

/* Generic / application. */
#define _SLOGC_TEST         9000
#define _SLOGC_USER         12000

#endif /* QSOE_SYS_SLOGCODES_H */

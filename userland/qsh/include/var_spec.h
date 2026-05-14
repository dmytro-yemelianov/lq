/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(VARSPEC_DEFNS)
#define FN(name) /* nothing */
#elif defined(VARSPEC_ENUMS)
#define FN(name) V_##name,
#define F0(name) V_##name = 0,
#elif defined(VARSPEC_ITEMS)
#define F0(name) /* nothing */
#define FN(name) #name,
#endif

#ifndef F0
#define F0 FN
#endif

/* NOTE: F0 are skipped for the ITEMS array, only FN generate names */

/* 0 is always V_NONE */
F0(NONE)

/* 1 and up are special variables */
FN(BASHPID)
FN(COLUMNS)
FN(EPOCHREALTIME)
FN(HISTSIZE)
FN(IFS)
FN(LINENO)
FN(LINES)
FN(OPTIND)
FN(PATH)
FN(RANDOM)
FN(SECONDS)
FN(TERM)
FN(TMOUT)
FN(TMPDIR)

#undef FN
#undef F0
#undef VARSPEC_DEFNS
#undef VARSPEC_ENUMS
#undef VARSPEC_ITEMS

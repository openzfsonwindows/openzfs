// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

#ifndef _SPL_ERR_H
#define	_SPL_ERR_H

#include <sys/debug.h>

#ifdef _MSC_VER
#define	_Noreturn
#else
#define	_Noreturn	__attribute__((__noreturn__))
#endif

void err(int, const char *, ...) _Noreturn __printf0like(2, 3);
void errx(int, const char *, ...) _Noreturn __printf0like(2, 3);
void warnx(const char *, ...) __printflike(1, 2);

inline static void
err(int x, const char *f, ...)
{
}

inline static void
errx(int x, const char *f, ...)
{
	exit(x);
}

inline static void
warnx(const char *f, ...)
{
	exit(1);
}

#endif

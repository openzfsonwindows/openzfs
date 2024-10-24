/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 *
 * Copyright (C) 2017 Jorgen Lundman <lundman@lundman.net>
 *
 */

#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <spl-debug.h>

#include <Trace.h>

void
vcmn_err(int ce, const char *fmt, va_list ap)
{
	char msg[MAXMSGLEN];

	_vsnprintf(msg, MAXMSGLEN - 1, fmt, ap);

	switch (ce) {
		case CE_IGNORE:
			break;
		case CE_CONT:
			dprintf("%s", msg);
			break;
		case CE_NOTE:
			dprintf("SPL: Notice: %s\n", msg);
			break;
		case CE_WARN:
			TraceEvent(TRACE_WARNING, "SPL: Warning: %s\n", msg);
			break;
		case CE_PANIC:
			PANIC("%s", msg);
			break;
	}
} /* vcmn_err() */

void
cmn_err(int ce, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcmn_err(ce, fmt, ap);
	va_end(ap);
} /* cmn_err() */

void
spl_panic(const char *file, const char *func, int line, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	do {

		// console
		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, fmt, ap));

		// cbuf
		printBuffer(fmt, ap);
		printBuffer("OpenZFS version %s\n", ZFS_META_GITREV);	\

		DbgBreakPoint();
		windows_delay(hz * 100);
	} while (1);
	va_end(ap);
}

// Backward compatible, loses FILE/FUNCTION/LINE
// but no longer used much in ZFS.
void
panic(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	spl_panic(__FILE__, __FUNCTION__, __LINE__, fmt, ap);
	va_end(ap);
}

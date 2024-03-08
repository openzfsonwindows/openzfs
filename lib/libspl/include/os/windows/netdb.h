/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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

#ifndef _LIBSPL_NETDB_H
#define _LIBSPL_NETDB_H

#include <sys/cdefs.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#pragma comment (lib, "Ws2_32.lib")

#define	EAI_ADDRFAMILY	1 /* address family for hostname not supported */
#define	EAI_AGAIN	2 /* name could not be resolved at this time */
#define	EAI_BADFLAGS	3 /* flags parameter had an invalid value */
#define	EAI_FAIL	4 /* non-recoverable failure in name resolution */
#define	EAI_FAMILY	5 /* address family not recognized */
#define	EAI_MEMORY	6 /* memory allocation failure */
#define	EAI_NODATA	7 /* no address associated with hostname */
#define	EAI_NONAME	8 /* name does not resolve */
#define	EAI_SERVICE	9 /* service not recognized for socket type */
#define	EAI_SOCKTYPE	10 /* intended socket type was not recognized */
#define	EAI_SYSTEM	11 /* system error returned in errno */
#define	EAI_BADHINTS	12 /* invalid value for hints */
#define	EAI_PROTOCOL	13 /* resolved protocol is unknown */
#define	EAI_OVERFLOW	14 /* argument buffer overflow */
#define	EAI_MAX		15

#define	poll WSAPoll
#define	INFTIM		(-1)

static ssize_t writev(int fd, struct iovec *iov, unsigned iov_cnt);

#endif

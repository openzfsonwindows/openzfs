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

#ifndef _LIBSPL_NETINET_TCP_H
#define _LIBSPL_NETINET_TCP_H

#include <sys/cdefs.h>
#include <winsock2.h>

#define	TCP_NODELAY	1	/* don't delay send to coalesce packets */
#define	TCP_MAXSEG	2	/* set maximum segment size */
#define	TCP_NOPUSH	4	/* don't push last block of write */
#define	TCP_NOOPT	8	/* don't use TCP options */
#define	TCP_MD5SIG	16	/* use MD5 digests (RFC2385) */
#define	TCP_INFO	32	/* retrieve tcp_info structure */
#define	TCP_STATS	33	/* retrieve stats blob structure */
#define	TCP_LOG		34	/* configure event logging for connection */
#define	TCP_LOGBUF	35	/* retrieve event log for connection */

#endif

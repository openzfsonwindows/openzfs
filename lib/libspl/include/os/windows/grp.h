/*
 * CDDL HEADER START
 *
 *  The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or  http://www.opensolaris.org/os/licensing.
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
 * Copyright (c) 2017 Jorgen Lundman <lundman@lundman.net>
 */

#ifndef _SPL_GRP_H
#define	_SPL_GRP_H

struct  group { /* see getgrent(3C) */
	char    *gr_name;
	char    *gr_passwd;
	gid_t   gr_gid;
	char    **gr_mem;
};

extern struct group *getgrnam(const char *);    /* MT-unsafe */

int getgrnam_r(const char *name, struct group *grp,
    char *buf, size_t buflen, struct group **result);

#endif

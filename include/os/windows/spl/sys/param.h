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

#ifndef _SPL_PARAM_H
#define	_SPL_PARAM_H

/* Pages to bytes and back */
#define	ptob(pages)			(pages << PAGE_SHIFT)
#define	btop(bytes)			(bytes >> PAGE_SHIFT)
#ifndef howmany
#define	howmany(x, y)   ((((x) % (y)) == 0) ? ((x) / (y)) : (((x) / (y)) + 1))
#endif

#define	MAXUID				UINT32_MAX

#define	PAGESHIFT	PAGE_SHIFT

#endif /* SPL_PARAM_H */

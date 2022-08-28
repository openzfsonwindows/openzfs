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

#ifndef _SYS_VDEV_FILE_H
#define	_SYS_VDEV_FILE_H

#include <sys/vdev.h>

#ifdef	__cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef struct vdev_file {
	zfs_file_t	*vf_file;
	uint64_t	vdev_win_offset; /* soft partition start */
	uint64_t	vdev_win_length; /* soft partition length */
} vdev_file_t;
#endif

extern void vdev_file_init(void);
extern void vdev_file_fini(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VDEV_FILE_H */

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
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>
 */

#ifndef _ZFS_VSS_H
#define	_ZFS_VSS_H

#include <sys/spa.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>

/*
 * VSS snapshot device lifecycle.
 */
extern void	zfs_vss_init(void);
extern void	zfs_vss_fini(void);

extern int	zfs_vss_snapshot_add(uint64_t guid, const char *snapname,
    uint64_t creation);
extern void	zfs_vss_snapshot_remove(uint64_t guid);

/*
 * Find the GUID of the closest snapshot of 'dataset' at or before
 * unix_time.  Returns 0 if no snapshot qualifies.
 */
extern uint64_t	zfs_vss_find_by_time(uint64_t unix_time,
    const char *dataset);

/* Name-based wrappers - called from the ioctl layer after create/destroy */
extern void	zfs_vss_snapshot_add_by_name(const char *snapname);
extern void	zfs_vss_snapshot_remove_by_name(const char *snapname);

extern void	zfs_vss_pool_add(spa_t *spa);
extern void	zfs_vss_pool_remove(spa_t *spa);

/* dmu_objset_find_dp callback - not for external callers */
extern int	zfs_vss_pool_add_cb(dsl_pool_t *dp, dsl_dataset_t *ds,
    void *arg);

/* Returns B_TRUE if a live kernel device object exists for this GUID */
extern boolean_t zfs_vss_has_device(uint64_t guid);

/* IRP dispatcher for \Device\ZfsSnapshot<hex> device objects */
extern NTSTATUS	zfs_vss_dispatcher(PDEVICE_OBJECT DeviceObject, PIRP *pIrp,
    PIO_STACK_LOCATION IrpSp);

#endif /* _ZFS_VSS_H */

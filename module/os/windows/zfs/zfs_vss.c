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
 * fields enclosed by brackets "[]" replaced with your own applying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>
 */

/*
 * VSS (Volume Shadow Copy Service) snapshot device support.
 *
 * Each ZFS snapshot that exists on an imported pool gets a kernel device
 * object named:
 *
 *   \Device\ZfsSnapshot<hex16>
 *
 * where <hex16> is the 16-character lowercase hex encoding of the ZFS
 * dataset GUID (dsl_dataset_phys_t::ds_guid, a uint64_t).  The name is
 * deterministic and bidirectional: given the device name you can recover
 * the GUID, and given the GUID you can find the snapshot via the MOS.
 * No extra ZAP attribute or registry entry is needed to maintain the
 * kernel-side mapping.
 *
 * Lifecycle:
 *   zfs_vss_snapshot_add()    - called after snapshot creation
 *   zfs_vss_snapshot_remove() - called before snapshot destruction
 *   zfs_vss_pool_add()        - called after pool import; iterates all
 *                               snapshots and calls zfs_vss_snapshot_add()
 *   zfs_vss_pool_remove()     - called before pool export; tears down all
 *                               snapshot devices for the pool
 *   zfs_vss_init() / fini()   - module lifetime
 *
 * The userland VSS Provider COM service is responsible for:
 *   - responding to VSS requestor calls (BeginPrepareSnapshot etc.)
 *   - calling "zfs snapshot" and "zfs destroy" via ioctls
 *   - maintaining the small VSS-GUID -> ZFS-GUID mapping in the registry
 *   - returning \Device\ZfsSnapshot<hex16> as the shadow copy device path
 */

#include <Ntifs.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntddvol.h>
#include <mountmgr.h>
#include <Mountdev.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zfs_context.h>
#include <sys/zfs_znode.h>
#include <sys/vnode.h>
#include <sys/mount.h>

#include <sys/zfs_vss.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_windows.h>
#include <sys/zfs_mount.h>

/* Prefix for all snapshot device objects. */
#define	ZFS_VSS_DEVICE_PREFIX	"\\Device\\ZfsSnapshot"
#define	ZFS_VSS_DEVICE_PREFIXW	L"\\Device\\ZfsSnapshot"

/*
 * Per-snapshot device tracking entry.
 * Stored in a global AVL tree keyed by ZFS dataset GUID.
 */
typedef struct zfs_vss_snap {
	avl_node_t	zvs_node;
	uint64_t	zvs_guid;	/* ZFS dataset GUID (key) */
	uint64_t	zvs_creation;	/* snapshot creation (Unix seconds) */
	PDEVICE_OBJECT	zvs_devobj;	/* \Device\ZfsSnapshot<hex> */
} zfs_vss_snap_t;

static avl_tree_t	zfs_vss_snaps;
static kmutex_t		zfs_vss_lock;

static int
zfs_vss_snap_compare(const void *a, const void *b)
{
	const zfs_vss_snap_t *sa = a;
	const zfs_vss_snap_t *sb = b;
	return (TREE_CMP(sa->zvs_guid, sb->zvs_guid));
}

extern PDRIVER_OBJECT WIN_DriverObject;

static void zfs_vss_notify_mountmgr(PDEVICE_OBJECT devobj);

/*
 * Create \Device\ZfsSnapshot<hex16> for the given ZFS dataset GUID.
 * poolname is stored in the device extension so IOCTL handlers can
 * open the SPA and look up the snapshot for size queries.
 */
int
zfs_vss_snapshot_add(uint64_t guid, const char *snapname, uint64_t creation)
{
	NTSTATUS status;
	char devname[64];
	ANSI_STRING astr;
	UNICODE_STRING ustr;
	PDEVICE_OBJECT devobj = NULL;

	mutex_enter(&zfs_vss_lock);

	/* Already tracked? */
	zfs_vss_snap_t key = { .zvs_guid = guid };
	if (avl_find(&zfs_vss_snaps, &key, NULL) != NULL) {
		mutex_exit(&zfs_vss_lock);
		return (0);
	}

	mutex_exit(&zfs_vss_lock);

	snprintf(devname, sizeof (devname),
	    ZFS_VSS_DEVICE_PREFIX "%016llx", (unsigned long long)guid);

	astr.Buffer = devname;
	astr.Length = (USHORT)strlen(devname);
	astr.MaximumLength = sizeof (devname);

	status = RtlAnsiStringToUnicodeString(&ustr, &astr, TRUE);
	if (!NT_SUCCESS(status)) {
		dprintf("%s: RtlAnsiStringToUnicodeString failed 0x%x\n",
		    __func__, status);
		return (EIO);
	}

	/*
	 * FILE_DEVICE_DISK_FILE_SYSTEM: required so that the IO Manager routes
	 * IRP_MJ_CREATE with a path component to this device.  With any disk-
	 * style device type (FILE_DEVICE_VIRTUAL_DISK, FILE_DEVICE_DISK, …)
	 * the IO Manager rejects "\\?\GLOBALROOT\Device\ZfsSnapshot...\path"
	 * with ERROR_INVALID_NAME before sending a single IRP — it requires
	 * either a filesystem device type or a VPB to locate the FS.
	 * FILE_READ_ONLY_DEVICE: snapshot is always read-only.
	 * Use sizeof(mount_t) so the device extension IS the mount object;
	 * fsDispatcher can handle file IRPs once the snapshot is lazily mounted.
	 */
	/*
	 * Do NOT use FILE_DEVICE_SECURE_OPEN: that flag makes the IO Manager
	 * enforce the device-object security descriptor for every file/directory
	 * open within the filesystem, not just raw device opens.  The default
	 * device SD (created by IoCreateDevice) grants access only to SYSTEM
	 * and Administrators, so Explorer (running with the user's limited token)
	 * would get STATUS_ACCESS_DENIED on every path open.  Standard filesystem
	 * volumes (NTFS, ZFS live mounts) omit this flag and let the filesystem
	 * driver handle per-file security via its own ACL checks.
	 * FILE_READ_ONLY_DEVICE is still set to prevent write IRPs at the device
	 * level — snapshots are always read-only.
	 */
	status = IoCreateDevice(WIN_DriverObject,
	    sizeof (mount_t),
	    &ustr,
	    FILE_DEVICE_DISK_FILE_SYSTEM,
	    FILE_READ_ONLY_DEVICE,
	    FALSE,
	    &devobj);

	RtlFreeUnicodeString(&ustr);

	if (!NT_SUCCESS(status)) {
		dprintf("%s: IoCreateDevice '%s' failed 0x%x\n",
		    __func__, devname, status);
		return (EIO);
	}

	mount_t *zmo = devobj->DeviceExtension;
	RtlZeroMemory(zmo, sizeof (mount_t));
	zmo->type = MOUNT_TYPE_VSS;
	zmo->size = sizeof (mount_t);
	zmo->vss_guid     = guid;
	zmo->vss_creation = creation;
	strlcpy(zmo->vss_snapname, snapname ? snapname : "",
	    sizeof (zmo->vss_snapname));
	/* ascii_name points into vss_snapname ("pool/ds@snap") */
	zmo->ascii_name = (const unsigned char *)zmo->vss_snapname;
	zmo->mountflags = MNT_RDONLY;
	KeInitializeMutex(&zmo->vss_mount_lock, 0);
	InitializeListHead(&zmo->DirNotifyList);
	FsRtlNotifyInitializeSync(&zmo->NotifySync);
	devobj->SectorSize = 512;

	/*
	 * Clear DO_DEVICE_INITIALIZING so the I/O Manager will accept IRPs
	 * sent to this device.  Must be done before notifying the Mount Manager,
	 * otherwise its probe IRPs are silently rejected.
	 */
	devobj->Flags &= ~DO_DEVICE_INITIALIZING;

	zfs_vss_snap_t *zvs = kmem_alloc(sizeof (*zvs), KM_SLEEP);
	zvs->zvs_guid     = guid;
	zvs->zvs_creation = creation;
	zvs->zvs_devobj   = devobj;

	mutex_enter(&zfs_vss_lock);
	avl_add(&zfs_vss_snaps, zvs);
	mutex_exit(&zfs_vss_lock);

	dprintf("%s: created %s\n", __func__, devname);

	/*
	 * Notify MountMgr so the snapshot device obtains a stable Volume GUID
	 * (\\?\Volume{...}).  The VSS coordinator requires this GUID to write
	 * the snapshot to its persistent shadow catalog — without it,
	 * "vssadmin list shadows" returns nothing even though the snapshot
	 * exists.  The previous concern about "IOCTL Unpack overflow" from
	 * swprv/ichannel probing is resolved: IOCTL_VOLSNAP_QUERY_DIFF_AREA_*
	 * now returns STATUS_NOT_SUPPORTED, so the System Provider skips our
	 * device as a diff-area candidate without overflowing its buffer.
	 */
	zfs_vss_notify_mountmgr(devobj);

	return (0);
}

/*
 * Tell the Mount Manager that a new volume has arrived.
 * It will respond by issuing IOCTL_MOUNTDEV_QUERY_DEVICE_NAME and
 * IOCTL_MOUNTDEV_QUERY_UNIQUE_ID to our device, exercising the handlers.
 */
static void
zfs_vss_notify_mountmgr(PDEVICE_OBJECT devobj)
{
	UNICODE_STRING mmgrName;
	PFILE_OBJECT   mmgrFileObj = NULL;
	PDEVICE_OBJECT mmgrDevObj  = NULL;
	NTSTATUS       status;

	RtlInitUnicodeString(&mmgrName, L"\\Device\\MountPointManager");

	status = IoGetDeviceObjectPointer(&mmgrName, FILE_READ_ATTRIBUTES,
	    &mmgrFileObj, &mmgrDevObj);
	if (!NT_SUCCESS(status)) {
		dprintf("%s: IoGetDeviceObjectPointer failed 0x%x\n",
		    __func__, status);
		return;
	}

	/* Query the kernel name of the device we just created. */
	ULONG nbytes = 0;
	/* First call with size=0 returns STATUS_INFO_LENGTH_MISMATCH + size */
	(void) ObQueryNameString(devobj, NULL, 0, &nbytes);
	if (nbytes == 0) {
		ObDereferenceObject(mmgrFileObj);
		return;
	}

	POBJECT_NAME_INFORMATION oni = kmem_alloc(nbytes, KM_SLEEP);
	status = ObQueryNameString(devobj, oni, nbytes, &nbytes);
	if (!NT_SUCCESS(status) || oni->Name.Length == 0) {
		kmem_free(oni, nbytes);
		ObDereferenceObject(mmgrFileObj);
		return;
	}

	USHORT nameLen = oni->Name.Length;
	ULONG insize = sizeof (MOUNTMGR_TARGET_NAME) + nameLen;
	MOUNTMGR_TARGET_NAME *mtn = kmem_alloc(insize, KM_SLEEP);
	mtn->DeviceNameLength = nameLen;
	RtlCopyMemory(mtn->DeviceName, oni->Name.Buffer, nameLen);
	kmem_free(oni, nbytes);

	KEVENT event;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IO_STATUS_BLOCK iosb = { 0 };

	PIRP irp = IoBuildDeviceIoControlRequest(
	    IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION,
	    mmgrDevObj, mtn, insize, NULL, 0,
	    FALSE, &event, &iosb);

	if (irp == NULL) {
		kmem_free(mtn, insize);
		ObDereferenceObject(mmgrFileObj);
		return;
	}

	status = IoCallDriver(mmgrDevObj, irp);
	if (status == STATUS_PENDING)
		KeWaitForSingleObject(&event, Executive, KernelMode,
		    FALSE, NULL);

	dprintf("%s: MountMgr arrival status 0x%lx\n", __func__,
	    iosb.Status);

	kmem_free(mtn, insize);
	ObDereferenceObject(mmgrFileObj);
}

/*
 * Remove the device object for the given GUID.
 */
void
zfs_vss_snapshot_remove(uint64_t guid)
{
	zfs_vss_snap_t key = { .zvs_guid = guid };

	mutex_enter(&zfs_vss_lock);
	zfs_vss_snap_t *zvs = avl_find(&zfs_vss_snaps, &key, NULL);
	if (zvs == NULL) {
		mutex_exit(&zfs_vss_lock);
		return;
	}
	avl_remove(&zfs_vss_snaps, zvs);
	mutex_exit(&zfs_vss_lock);

	dprintf("%s: removing " ZFS_VSS_DEVICE_PREFIX "%016llx\n",
	    __func__, (unsigned long long)guid);

	mount_t *zmo = zvs->zvs_devobj->DeviceExtension;

	/* Unmount the snapshot filesystem if it was lazily mounted. */
	if (vfs_fsprivate(zmo) != NULL) {
		dprintf("%s: unmounting snapshot %016llx\n",
		    __func__, (unsigned long long)guid);
		(void) zfs_vfs_unmount(zmo, MNT_FORCE, NULL);
		vfs_mount_remove(zmo);
	}
	FsRtlNotifyUninitializeSync(&zmo->NotifySync);

	IoDeleteDevice(zvs->zvs_devobj);
	kmem_free(zvs, sizeof (*zvs));
}

/*
 * Resolve a snapshot dataset name to its GUID, then create the device.
 * Called after snapshot creation completes.
 */
void
zfs_vss_snapshot_add_by_name(const char *snapname)
{
	spa_t *spa;
	dsl_pool_t *dp;
	dsl_dataset_t *ds;

	if (spa_open(snapname, &spa, FTAG) != 0)
		return;

	dp = spa_get_dsl(spa);
	if (dp == NULL) {
		spa_close(spa, FTAG);
		return;
	}

	dsl_pool_config_enter(dp, FTAG);
	if (dsl_dataset_hold(dp, snapname, FTAG, &ds) == 0) {
		if (dsl_dataset_is_snapshot(ds))
			(void) zfs_vss_snapshot_add(dsl_dataset_phys(ds)->ds_guid,
			    snapname, dsl_get_creation(ds));
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);

	spa_close(spa, FTAG);
}

/*
 * Resolve a snapshot dataset name to its GUID, then remove the device.
 * Called before snapshot destruction.
 */
void
zfs_vss_snapshot_remove_by_name(const char *snapname)
{
	spa_t *spa;
	dsl_pool_t *dp;
	dsl_dataset_t *ds;

	if (spa_open(snapname, &spa, FTAG) != 0)
		return;

	dp = spa_get_dsl(spa);
	if (dp == NULL) {
		spa_close(spa, FTAG);
		return;
	}

	dsl_pool_config_enter(dp, FTAG);
	if (dsl_dataset_hold(dp, snapname, FTAG, &ds) == 0) {
		if (dsl_dataset_is_snapshot(ds))
			zfs_vss_snapshot_remove(dsl_dataset_phys(ds)->ds_guid);
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);

	spa_close(spa, FTAG);
}

/*
 * Called after a pool is imported.  Walk every snapshot in every
 * mounted dataset and create the corresponding device object.
 */
void
zfs_vss_pool_add(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	if (dp == NULL)
		return;

	dsl_pool_config_enter(dp, FTAG);

	/*
	 * Iterate all datasets in the pool looking for snapshots.
	 * dmu_objset_find_dp visits every objset; we filter for snapshots.
	 */
	dmu_objset_find_dp(dp, dp->dp_root_dir_obj,
	    zfs_vss_pool_add_cb, NULL, DS_FIND_SNAPSHOTS | DS_FIND_CHILDREN);

	dsl_pool_config_exit(dp, FTAG);
}

/* Callback for dmu_objset_find_dp - create device for each snapshot. */
int
zfs_vss_pool_add_cb(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	(void) arg;

	if (!dsl_dataset_is_snapshot(ds))
		return (0);

	char dsname[ZFS_MAX_DATASET_NAME_LEN];
	dsl_dataset_name(ds, dsname);
	uint64_t guid = dsl_dataset_phys(ds)->ds_guid;
	uint64_t ctime = dsl_get_creation(ds);
	(void) zfs_vss_snapshot_add(guid, dsname, ctime);
	return (0);
}

/*
 * Called before a pool is exported.  Remove all snapshot devices
 * belonging to this pool.
 *
 * We walk the AVL tree and delete any entry whose device object
 * name matches the pool's snapshots by re-checking via the dataset GUID.
 * For simplicity we remove everything and let re-import recreate them;
 * if multiple pools are active, only the exported pool's snapshots are
 * affected because GUIDs are pool-unique.
 */
void
zfs_vss_pool_remove(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	if (dp == NULL)
		return;

	/*
	 * Collect GUIDs to remove (can't remove while iterating AVL, and
	 * can't sleep while holding the mutex).  Pre-allocate based on the
	 * current tree count, then fill under the lock.
	 */
	uint_t alloc = 0;
	mutex_enter(&zfs_vss_lock);
	alloc = (uint_t)avl_numnodes(&zfs_vss_snaps);
	mutex_exit(&zfs_vss_lock);

	if (alloc == 0)
		return;

	uint64_t *guids = kmem_alloc(alloc * sizeof (uint64_t), KM_SLEEP);
	uint_t nsnaps = 0;

	mutex_enter(&zfs_vss_lock);
	zfs_vss_snap_t *zvs;
	for (zvs = avl_first(&zfs_vss_snaps); zvs != NULL &&
	    nsnaps < alloc; zvs = AVL_NEXT(&zfs_vss_snaps, zvs))
		guids[nsnaps++] = zvs->zvs_guid;
	mutex_exit(&zfs_vss_lock);

	/*
	 * Second pass: filter to only GUIDs belonging to this pool.
	 * dsl_dataset_hold_obj may sleep so it must run outside the mutex.
	 */
	dsl_pool_config_enter(dp, FTAG);
	uint_t npool = 0;
	for (uint_t i = 0; i < nsnaps; i++) {
		dsl_dataset_t *ds = NULL;
		if (dsl_dataset_hold_obj(dp, guids[i], FTAG, &ds) == 0) {
			dsl_dataset_rele(ds, FTAG);
			guids[npool++] = guids[i];
		}
	}
	dsl_pool_config_exit(dp, FTAG);
	nsnaps = npool;

	for (uint_t i = 0; i < nsnaps; i++)
		zfs_vss_snapshot_remove(guids[i]);

	if (guids != NULL)
		kmem_free(guids, alloc * sizeof (uint64_t));
}

/*
 * Find the GUID of the snapshot of 'dataset' whose creation time is
 * the largest value that is <= unix_time.  Used by the @GMT time-warp
 * path handler: given a UTC timestamp from a Previous Versions path
 * (\@GMT-YYYY.MM.DD-HH.MM.SS\...), find the ZFS snapshot to open.
 *
 * Returns the dataset GUID, or 0 if no matching snapshot was found.
 *
 * 'dataset' is the live dataset name (e.g. "pool/data"); only snapshots
 * of the form "pool/data@..." are considered.
 */
uint64_t
zfs_vss_find_by_time(uint64_t unix_time, const char *dataset)
{
	size_t dslen = dataset ? strlen(dataset) : 0;
	uint64_t best_guid = 0;
	uint64_t best_ctime = 0;
	zfs_vss_snap_t *zvs;

	mutex_enter(&zfs_vss_lock);
	for (zvs = avl_first(&zfs_vss_snaps);
	    zvs != NULL;
	    zvs = AVL_NEXT(&zfs_vss_snaps, zvs)) {
		/* Filter by dataset: snapname must be "<dataset>@..." */
		if (dslen > 0) {
			mount_t *snzmo =
			    (mount_t *)zvs->zvs_devobj->DeviceExtension;
			const char *sn = snzmo->vss_snapname;
			if (strncmp(sn, dataset, dslen) != 0 ||
			    sn[dslen] != '@')
				continue;
		}
		if (zvs->zvs_creation <= unix_time &&
		    zvs->zvs_creation > best_ctime) {
			best_ctime = zvs->zvs_creation;
			best_guid  = zvs->zvs_guid;
		}
	}
	mutex_exit(&zfs_vss_lock);
	return (best_guid);
}

void
zfs_vss_init(void)
{
	mutex_init(&zfs_vss_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&zfs_vss_snaps, zfs_vss_snap_compare,
	    sizeof (zfs_vss_snap_t), offsetof(zfs_vss_snap_t, zvs_node));
	dprintf("%s: VSS snapshot device support initialised\n", __func__);
}

void
zfs_vss_fini(void)
{
	/* Remove any remaining snapshot devices. */
	mutex_enter(&zfs_vss_lock);
	zfs_vss_snap_t *zvs;
	void *cookie = NULL;
	while ((zvs = avl_destroy_nodes(&zfs_vss_snaps, &cookie)) != NULL) {
		IoDeleteDevice(zvs->zvs_devobj);
		kmem_free(zvs, sizeof (*zvs));
	}
	avl_destroy(&zfs_vss_snaps);
	mutex_exit(&zfs_vss_lock);
	mutex_destroy(&zfs_vss_lock);
}

/*
 * Get the number of bytes referenced by the snapshot (its logical size).
 * Returns 0 if the lookup fails.
 */
static uint64_t
zfs_vss_snap_refbytes(mount_t *zmo)
{
	spa_t *spa;
	dsl_pool_t *dp;
	dsl_dataset_t *ds;
	uint64_t refbytes = 0;

	/* Extract pool name from "pool/ds@snap" or "pool" */
	char poolname[256];
	const char *snapname = zmo->vss_snapname;
	const char *sep = strpbrk(snapname, "/@");
	size_t plen = sep ? (size_t)(sep - snapname) : strlen(snapname);
	if (plen == 0 || plen >= sizeof (poolname))
		return (0);
	strlcpy(poolname, snapname, plen + 1);

	if (spa_open(poolname, &spa, FTAG) != 0)
		return (0);

	dp = spa_get_dsl(spa);
	if (dp == NULL) {
		spa_close(spa, FTAG);
		return (0);
	}

	dsl_pool_config_enter(dp, FTAG);
	if (dsl_dataset_hold_obj(dp, zmo->vss_guid, FTAG, &ds) == 0) {
		refbytes = dsl_dataset_phys(ds)->ds_referenced_bytes;
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);
	spa_close(spa, FTAG);

	return (refbytes);
}

/*
 * Mount the ZFS snapshot named by zmo->vss_snapname onto zmo (which is the
 * device extension of the \Device\ZfsSnapshot<guid> device object).
 * After this returns 0, vfs_fsprivate(zmo) is a live zfsvfs_t and
 * fsDispatcher can handle file IRPs on the snapshot device.
 */
static int
zfs_vss_do_mount(PDEVICE_OBJECT DeviceObject, mount_t *zmo)
{
	struct zfs_mount_args mnt_args;
	int err;

	ASSERT(vfs_fsprivate(zmo) == NULL);

	mnt_args.struct_size = sizeof (mnt_args);
	mnt_args.optlen = 0;
	mnt_args.mflag = MNT_RDONLY;
	mnt_args.fspec = (char *)zmo->vss_snapname;

	dprintf("%s: mounting snapshot '%s'\n", __func__, zmo->vss_snapname);
	err = zfs_vfs_mount(zmo, NULL, (user_addr_t)&mnt_args, NULL);
	if (err != 0) {
		dprintf("%s: zfs_vfs_mount failed %d\n", __func__, err);
		return (err);
	}

	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	if (zfsvfs != NULL) {
		zfsvfs->z_vfs = zmo;
		zfsvfs->z_mimic = ZFS_MIMIC_NTFS;
	}

	zmo->VolumeDeviceObject = DeviceObject;
	zmo->FunctionalDeviceObject = DeviceObject;

	/*
	 * Do NOT install a VPB on this device.  A self-referential VPB
	 * (DeviceObject == RealDevice == our device) confuses the IO Manager:
	 * for FILE_DIRECTORY_FILE opens with a non-root FileName (e.g.
	 * \HelloWorld) the IO Manager attempts a namespace sub-device lookup
	 * for the path component, fails to find one, and the IRP never reaches
	 * our dispatcher.  Without a VPB, FILE_DEVICE_DISK_FILE_SYSTEM devices
	 * receive all IRPs — including sub-directory opens — directly.
	 *
	 * The mount-check in zfs_vnop_lookup_impl is updated to use
	 * vfs_fsprivate(zmo) instead of the VPB_MOUNTED flag.
	 */

	vfs_mount_add(zmo);
	dprintf("%s: snapshot '%s' mounted\n", __func__, zmo->vss_snapname);
	return (0);
}

/*
 * Work item context for lazy snapshot mount.
 * Carries the pending IRP so it can be completed after the mount finishes.
 */
typedef struct {
	PIO_WORKITEM	wi_item;
	PDEVICE_OBJECT	wi_devobj;
	PIRP		wi_irp;
} zfs_vss_mount_wi_t;

static IO_WORKITEM_ROUTINE zfs_vss_mount_workitem;
static VOID
zfs_vss_mount_workitem(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
	zfs_vss_mount_wi_t *wi = Context;
	PIRP Irp = wi->wi_irp;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	mount_t *zmo = DeviceObject->DeviceExtension;

	/*
	 * Serialise: only one thread mounts; others wait on the mutex and
	 * then fall through to fsDispatcher with the snapshot already live.
	 */
	KeWaitForMutexObject(&zmo->vss_mount_lock, Executive,
	    KernelMode, FALSE, NULL);

	if (vfs_fsprivate(zmo) == NULL)
		(void) zfs_vss_do_mount(DeviceObject, zmo);

	KeReleaseMutex(&zmo->vss_mount_lock, FALSE);

	if (vfs_fsprivate(zmo) == NULL) {
		/* Mount failed; reject the create. */
		Irp->IoStatus.Status = STATUS_UNRECOGNIZED_VOLUME;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	} else {
		/*
		 * Snapshot is live; let fsDispatcher handle the create.
		 * fsDispatcher returns without completing the IRP (the main
		 * dispatcher() loop normally does that); we must complete it.
		 */
		NTSTATUS st = fsDispatcher(DeviceObject, &Irp, IrpSp);
		if (Irp != NULL) {
			Irp->IoStatus.Status = st;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
		}
	}

	IoFreeWorkItem(wi->wi_item);
	kmem_free(wi, sizeof (*wi));
}

/*
 * Helper for IOCTL_VOLSNAP_QUERY_ORIGINAL_VOLUME_NAME.
 * Walks the mount list for the DCB whose ascii_name matches the parent
 * dataset (vss_snapname with the "@snapshot" suffix stripped).
 */
typedef struct {
	const char	*dataset;
	USHORT		namelen;	/* bytes copied into namebuf */
	WCHAR		namebuf[PATH_MAX];
	boolean_t	found;
} vqovn_ctx_t;

static int
vqovn_find_cb(void *mp, void *arg)
{
	mount_t *m = mp;
	vqovn_ctx_t *ctx = arg;

	if (m->type != MOUNT_TYPE_DCB ||
	    m->ascii_name == NULL ||
	    strcmp(m->ascii_name, ctx->dataset) != 0)
		return (0);

	/*
	 * srv2.sys compares ORIGINAL_VOLUME_NAME against the filesystem
	 * volume device (the VCB), not the disk device.  The DCB's fs_name
	 * (\Device\ZFS{uuid}) is the name IoCreateDevice used for the VCB,
	 * so it is what IoGetRelatedDeviceObject returns for a file on the
	 * share.  Return fs_name; fall back to device_name if fs_name is
	 * unset (should not happen for a mounted volume).
	 */
	const UNICODE_STRING *src = (m->fs_name.Length > 0 &&
	    m->fs_name.Buffer != NULL) ? &m->fs_name : &m->device_name;
	if (src->Length == 0 || src->Buffer == NULL)
		return (0);

	USHORT copy_len = min(src->Length,
	    (USHORT)(sizeof (ctx->namebuf) - sizeof (WCHAR)));
	RtlCopyMemory(ctx->namebuf, src->Buffer, copy_len);
	ctx->namelen = copy_len;
	ctx->found = B_TRUE;
	dprintf("%s: returning '%.*S' for dataset '%s'\n",
	    __func__, (int)(copy_len / sizeof (WCHAR)), ctx->namebuf,
	    ctx->dataset);
	return (1);
}

/*
 * IRP_MJ_DEVICE_CONTROL handler for VSS snapshot devices.
 *
 * Handles the storage/volume/mountdev IOCTLs that VSS and the
 * Mount Manager issue when discovering and characterising shadow copies.
 *
 * From WireShark:
 *  {0x530018, "IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS" },
 *  {0x530050, "IOCTL_VOLSNAP_QUERY_EPIC" },
 *  {0x530190, "IOCTL_VOLSNAP_QUERY_ORIGINAL_VOLUME_NAME" },
 *  {0x530194, "IOCTL_VOLSNAP_QUERY_SNAPSHOT_TIMESTAMP" },
 *  {0x53019c, "IOCTL_VOLSNAP_QUERY_CONFIG_INFO" }, // (FILE_ANY_ACCESS) IOCTL_VOLSNAP_QUERY_APPLICATION_INFO
 *  {0x5301e0, "IOCTL_VOLSNAP_QUERY_REVERT_UID" },
 *  {0x534058, "IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE" },
 *  {0x534070, "IOCTL_VOLSNAP_QUERY_APPLICATION_FLAGS" },
 *  {0x534194, "IOCTL_VOLSNAP_QUERY_SNAPSHOT_TIMESTAMP_RO" },
 *  {0x53419C, "IOCTL_VOLSNAP_QUERY_APPLICATION_INFO" }, //  (FILE_READ_ACCESS)
 *  {0x53c004, "IOCTL_VOLSNAP_RELEASE_WRITES" },
 *  {0x53c198, "IOCTL_VOLSNAP_SET_APPLICATION_INFO" },
 *
 *  {0x530024, "IOCTL_VOLSNAP_QUERY_DIFF_AREA" },
 * 
 *
 * 
 * IOCTL_VOLSNAP_QUERY_APPLICATION_INFO
struct ApplicationInformation
{
    uint8_t length[4];                 // ULONG: Total length of this struct payload (0x44 bytes)
    GUID    guid;                      // GUID: VOLSNAP_APPINFO_GUID_CLIENT_ACCESSIBLE 
                                       // ({e5de7d45-49f2-40a4-817c-7dc82b72587f})
    GUID    shadowCopyGuid;            // GUID: The unique VSS Snapshot ID
    GUID    shadowCopySetGuid;         // GUID: The unique VSS Snapshot Set ID (the group ID)
    uint8_t snapshotContext[4];        // LONG: The VSS Context flags (e.g., VSS_CTX_NAS_ROLLBACK)
    uint8_t unknown1[4];               // ULONG: Internal alignment / Provider version stamp
    VSS_VOLUME_SNAPSHOT_ATTRIBUTES attributes; // ULONG: The critical bitmask flags!
    uint8_t unknown2[4];               // ULONG: Reserved trailing packing space
};

 * IOCTL_VOLSNAP_QUERY_CONFIG_INFO
typedef struct _VOLSNAP_CONFIG_INFO {
    ULONG         InstanceCount;          // How many snapshots exist on the volume
    ULONG         AllocatedSpaceInBytes;  // COW diff space size (can be 0 for ZFS)
    ULONG         TotalSpaceInBytes;      // Max diff space size limit (can be 0 for ZFS)
    LARGE_INTEGER SnapshotCreationTime;   // The holy grail 1601 64-bit timestamp
}
 */

#ifndef FILE_DEVICE_VOLUME
#define FILE_DEVICE_VOLUME 0x00000056 // 'S'
#endif

#ifndef VOLSNAP_DEVICE_TYPE
#define VOLSNAP_DEVICE_TYPE   0x00000053
#endif

#ifndef IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS
#define IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS CTL_CODE(VOLSNAP_DEVICE_TYPE, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_EPIC
#define IOCTL_VOLSNAP_QUERY_EPIC CTL_CODE(VOLSNAP_DEVICE_TYPE, 20, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_DIFF_AREA
#define IOCTL_VOLSNAP_QUERY_DIFF_AREA CTL_CODE(VOLSNAP_DEVICE_TYPE, 9, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_ORIGINAL_VOLUME_NAME
#define IOCTL_VOLSNAP_QUERY_ORIGINAL_VOLUME_NAME CTL_CODE(VOLSNAP_DEVICE_TYPE, 100, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

// IOCTL_VOLSNAP_QUERY_SNAPSHOT_TIMESTAMP
#ifndef IOCTL_VOLSNAP_QUERY_CONFIG_INFO
#define IOCTL_VOLSNAP_QUERY_CONFIG_INFO CTL_CODE(VOLSNAP_DEVICE_TYPE, 101, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_CONFIG_INFO_RO
#define IOCTL_VOLSNAP_QUERY_CONFIG_INFO_RO CTL_CODE(VOLSNAP_DEVICE_TYPE, 101, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE
#define IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE CTL_CODE(VOLSNAP_DEVICE_TYPE, 22, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_APPLICATION_FLAGS
#define IOCTL_VOLSNAP_QUERY_APPLICATION_FLAGS CTL_CODE(VOLSNAP_DEVICE_TYPE, 28, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

 // Function 103 with ANY access matches your disassembly (0x53019C)
#ifndef IOCTL_VOLSNAP_QUERY_APPLICATION_INFO
#define IOCTL_VOLSNAP_QUERY_APPLICATION_INFO CTL_CODE(VOLSNAP_DEVICE_TYPE, 103, METHOD_BUFFERED, FILE_ANY_ACCESS) // 0x53019C
#endif

// Function 103 with READ access (0x53419C) if other modern tools probe for it
#ifndef IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_RO
#define IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_RO CTL_CODE(VOLSNAP_DEVICE_TYPE, 103, METHOD_BUFFERED, FILE_READ_ACCESS) // 0x53419C
#endif

#ifndef IOCTL_VOLSNAP_RELEASE_WRITES
#define IOCTL_VOLSNAP_RELEASE_WRITES CTL_CODE(VOLSNAP_DEVICE_TYPE, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_SET_APPLICATION_INFO
#define IOCTL_VOLSNAP_SET_APPLICATION_INFO CTL_CODE(VOLSNAP_DEVICE_TYPE, 102, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_REVERT_UID
#define IOCTL_VOLSNAP_QUERY_REVERT_UID CTL_CODE(VOLSNAP_DEVICE_TYPE, 120, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_PROPERTIES
#define IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_PROPERTIES CTL_CODE(VOLSNAP_DEVICE_TYPE, 27, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

#ifndef IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS_RO
#define IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS_RO CTL_CODE(VOLSNAP_DEVICE_TYPE, 5, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

#ifndef VSS_VOLSNAP_ATTR_PERSISTENT
#define VSS_VOLSNAP_ATTR_PERSISTENT 0x00000001
#endif
#ifndef VSS_VOLSNAP_ATTR_NO_AUTORECOVERY
#define VSS_VOLSNAP_ATTR_NO_AUTORECOVERY 0x00000002
#endif
#ifndef VSS_VOLSNAP_ATTR_CLIENT_ACCESSIBLE
#define VSS_VOLSNAP_ATTR_CLIENT_ACCESSIBLE 0x00000004
#endif
#ifndef VSS_VOLSNAP_ATTR_NO_AUTO_RELEASE
#define VSS_VOLSNAP_ATTR_NO_AUTO_RELEASE 0x00000008
#endif
#ifndef VSS_VOLSNAP_ATTR_NO_WRITERS
#define VSS_VOLSNAP_ATTR_NO_WRITERS 0x00000010
#endif


static NTSTATUS
zfs_vss_device_control(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	mount_t *zmo = DeviceObject->DeviceExtension;
	ulong_t cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
	ulong_t outlen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	void *buf = Irp->AssociatedIrp.SystemBuffer;
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

	switch (cmd) {

	case IOCTL_VOLUME_IS_CLUSTERED: /* 0x560030 */
		/*
		 * srv2.sys sends this to every volume device while scanning for
		 * shadow copies.  STATUS_UNSUCCESSFUL = not clustered (correct
		 * for snapshot devices).  STATUS_INVALID_DEVICE_REQUEST would
		 * cause srv2.sys to skip this device as an invalid disk type.
		 */
		dprintf("VSS %s: IOCTL_VOLUME_IS_CLUSTERED -> not clustered\n",
		    __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_UNSUCCESSFUL;
		break;

	case IOCTL_DISK_IS_WRITABLE:
		/* Snapshots are always read-only */
		dprintf("VSS %s: IOCTL_DISK_IS_WRITABLE\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_MEDIA_WRITE_PROTECTED;
		break;

	case IOCTL_DISK_CHECK_VERIFY:
	case IOCTL_STORAGE_CHECK_VERIFY:
	case IOCTL_STORAGE_CHECK_VERIFY2:
	case IOCTL_DISK_MEDIA_REMOVAL:
	case IOCTL_STORAGE_MEDIA_REMOVAL:
	case IOCTL_VOLUME_ONLINE:
#ifdef IOCTL_VOLUME_POST_ONLINE
	case IOCTL_VOLUME_POST_ONLINE:
#endif
	case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED:
	case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED:
	case IOCTL_MOUNTDEV_LINK_CREATED:
	case IOCTL_MOUNTDEV_LINK_DELETED:
		dprintf("VSS %s: simple-ok ioctl 0x%lx\n", __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
		/*
		 * Tell the Mount Manager we have no suggested link name so it
		 * does not auto-assign a drive letter to snapshot devices.
		 */
		dprintf("VSS %s: IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME"
		    " (suppressed)\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_FOUND;
		break;

	case IOCTL_STORAGE_GET_DEVICE_NUMBER:
	{
		dprintf("VSS %s: IOCTL_STORAGE_GET_DEVICE_NUMBER\n", __func__);
		if (outlen < sizeof (STORAGE_DEVICE_NUMBER)) {
			Irp->IoStatus.Information =
			    sizeof (STORAGE_DEVICE_NUMBER);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		STORAGE_DEVICE_NUMBER *sdn = buf;
		sdn->DeviceType    = FILE_DEVICE_VIRTUAL_DISK;
		sdn->DeviceNumber  = (ULONG)(zmo->vss_guid & 0xffffffff);
		sdn->PartitionNumber = (ULONG)-1;
		Irp->IoStatus.Information = sizeof (STORAGE_DEVICE_NUMBER);
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_DISK_GET_LENGTH_INFO:
	{
		dprintf("VSS %s: IOCTL_DISK_GET_LENGTH_INFO\n", __func__);
		if (outlen < sizeof (GET_LENGTH_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (GET_LENGTH_INFORMATION);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		GET_LENGTH_INFORMATION *gli = buf;
		gli->Length.QuadPart = (int64_t)zfs_vss_snap_refbytes(zmo);
		Irp->IoStatus.Information = sizeof (GET_LENGTH_INFORMATION);
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_DISK_GET_DRIVE_GEOMETRY:
	{
		dprintf("VSS %s: IOCTL_DISK_GET_DRIVE_GEOMETRY\n", __func__);
		if (outlen < sizeof (DISK_GEOMETRY)) {
			Irp->IoStatus.Information = sizeof (DISK_GEOMETRY);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		uint64_t total = zfs_vss_snap_refbytes(zmo);
		const ULONG bps = 512;
		const ULONG spt = 63;
		const ULONG tpc = 255;
		ULONG spc = spt * tpc;
		LONGLONG cyls = (total / bps) / spc;
		if (cyls < 1)
			cyls = 1;
		DISK_GEOMETRY *dg = buf;
		dg->Cylinders.QuadPart = cyls;
		dg->TracksPerCylinder  = tpc;
		dg->SectorsPerTrack    = spt;
		dg->BytesPerSector     = bps;
		dg->MediaType          = FixedMedia;
		Irp->IoStatus.Information = sizeof (DISK_GEOMETRY);
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
	{
		dprintf("VSS %s: IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n",
		    __func__);
		/* Build wide device name from the prefix + hex GUID */
		WCHAR wname[64];
		int nch = _snwprintf(wname, sizeof (wname) / sizeof (wname[0]),
		    ZFS_VSS_DEVICE_PREFIXW L"%016llx",
		    (unsigned long long)zmo->vss_guid);
		USHORT namelen = (USHORT)(nch * sizeof (WCHAR));
		ULONG needed = FIELD_OFFSET(MOUNTDEV_NAME, Name) + namelen;
		if (outlen < sizeof (MOUNTDEV_NAME)) {
			Irp->IoStatus.Information = needed;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		MOUNTDEV_NAME *mdn = buf;
		mdn->NameLength = namelen;
		if (outlen < needed) {
			Irp->IoStatus.Information = needed;
			status = STATUS_BUFFER_OVERFLOW;
			break;
		}
		RtlCopyMemory(mdn->Name, wname, namelen);
		Irp->IoStatus.Information = needed;
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
	{
		dprintf("VSS %s: IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n", __func__);
		/* Use the hex GUID string as the unique ID (ASCII bytes) */
		char uid[32];
		int uidlen = snprintf(uid, sizeof (uid),
		    "%016llx", (unsigned long long)zmo->vss_guid);
		ULONG needed = FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) +
		    uidlen;
		if (outlen < sizeof (MOUNTDEV_UNIQUE_ID)) {
			Irp->IoStatus.Information = needed;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		MOUNTDEV_UNIQUE_ID *uid_out = buf;
		uid_out->UniqueIdLength = (USHORT)uidlen;
		if (outlen < needed) {
			Irp->IoStatus.Information = needed;
			status = STATUS_BUFFER_OVERFLOW;
			break;
		}
		RtlCopyMemory(uid_out->UniqueId, uid, uidlen);
		Irp->IoStatus.Information = needed;
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
	{
		dprintf("VSS %s: IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\n",
		    __func__);
		if (outlen < sizeof (VOLUME_DISK_EXTENTS)) {
			Irp->IoStatus.Information =
			    sizeof (VOLUME_DISK_EXTENTS);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		VOLUME_DISK_EXTENTS *vde = buf;
		vde->NumberOfDiskExtents = 1;
		vde->Extents[0].DiskNumber = (ULONG)(zmo->vss_guid & 0xffffffff);
		vde->Extents[0].StartingOffset.QuadPart = 0;
		vde->Extents[0].ExtentLength.QuadPart =
		    (int64_t)zfs_vss_snap_refbytes(zmo);
		Irp->IoStatus.Information = sizeof (VOLUME_DISK_EXTENTS);
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
	{
		dprintf("VSS %s: IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n", __func__);
		if (outlen < sizeof (VOLUME_GET_GPT_ATTRIBUTES_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		VOLUME_GET_GPT_ATTRIBUTES_INFORMATION *ga = buf;
		/* GPT_ATTRIBUTE_PLATFORM_REQUIRED | no-automount */
		ga->GptAttributes = 0x8000000000000001ULL;
		Irp->IoStatus.Information =
		    sizeof (VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE:
		/*
		 * Snapshot devices cannot host a volsnap diff-area.
		 * Decline so the System Provider does not try to use
		 * ZFS snapshot devices as diff-area storage targets.
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE"
		    " -> NOT_SUPPORTED\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED;
		break;

	case IOCTL_VOLSNAP_FLUSH_AND_HOLD_WRITES:
	case IOCTL_VOLSNAP_RELEASE_WRITES:
		/*
		 * Snapshot devices are read-only; no write hold/release needed.
		 */
		dprintf("VSS %s: VOLSNAP flush/hold/release no-op 0x%lx\n",
		    __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IOCTL_VOLSNAP_QUERY_APPLICATION_FLAGS:
	{
		/*
		 * Returns ApplicationInformation (68 bytes):
		 *   ULONG length, GUID×3, ULONG×3, LONG attributes, ULONG.
		 * All zeros = no VSS application has set flags.
		 */
		const ULONG appsz = 4 + 16 + 16 + 16 + 4 + 4 + 4 + 4;
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_APPLICATION_FLAGS"
		    " -> 68 zero bytes\n", __func__);
		if (outlen < appsz) {
			Irp->IoStatus.Information = appsz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		RtlZeroMemory(buf, appsz);
		Irp->IoStatus.Information = appsz;
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_PROPERTIES: 
		/*
		 * Snapshot devices cannot host a diff-area; decline.
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_APPLICATION_INFO_PROPERTIES"
		    " -> NOT_SUPPORTED\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED;
		break;

	case IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS:
	case IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS_RO:
	{
		/*
		 * The snapshot device itself has no child snapshots.
		 * Return an empty VOLSNAP_NAMES: MultiSzLength=0, Names[1]=0.
		 *
		 * VOLSNAP_NAMES layout: { ULONG MultiSzLength; WCHAR Names[1]; }
		 * sizeof() = 8 (ULONG=4 + WCHAR=2 + 2 bytes pad to ULONG align).
		 * ichannel Unpack<VOLSNAP_NAMES> requires at least 8 bytes;
		 * returning only sizeof(ULONG)=4 causes "IOCTL Unpack overflow".
		 */
		const ULONG sz = sizeof (ULONG) + sizeof (WCHAR) +
		    2 * sizeof (BYTE); /* = 8, matching sizeof(VOLSNAP_NAMES) */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS\n",
		    __func__);
		if (outlen < sz) {
			Irp->IoStatus.Information = sz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		RtlZeroMemory(buf, sz); /* MultiSzLength=0, Names=L"" */
		Irp->IoStatus.Information = sz;
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_VOLSNAP_QUERY_DIFF_AREA: /* IOCTL_VOLSNAP_QUERY_DIFF_AREA */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_DIFF_AREA\n",
		    __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED; // STATUS_TARGET_SET_NOT_VOLUME
		break;

	case 12345: // not used, we do not return GMT- names?
	{
		/*
		 * srv2.sys (Smb2SnapCheckAppInfoForTimeWarp) calls this after
		 * opening the snapshot device to get the @GMT timestamp it will
		 * advertise in SRV_SNAPSHOT_ARRAY.
		 *
		 * Layout: { ULONG Type; ULONG Length; WCHAR bValue[Length/2]; }
		 * We put Type=1 (time-warp capable) and bValue = the 24-WCHAR
		 * @GMT-YYYY.MM.DD-HH.MM.SS string (no NUL in bValue itself).
		 * 
		 */
		const ULONG gmt_bytes = 24 * sizeof (WCHAR);
		const ULONG appsz = 2 * sizeof (ULONG) + gmt_bytes;
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_APPLICATION_INFO\n",
		    __func__);
		if (outlen < 2 * sizeof (ULONG)) {
			Irp->IoStatus.Information = appsz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		ULONG *hdr = (ULONG *)buf;
		hdr[0] = 1;          /* Type = 1 (time-warp) */
		hdr[1] = gmt_bytes;  /* Length of bValue */
		Irp->IoStatus.Information = 2 * sizeof (ULONG);

		if (outlen >= appsz) {
			LARGE_INTEGER vqai_li;
			TIME_UNIX_TO_WINDOWS_EX(zmo->vss_creation, 0,
			    vqai_li.QuadPart);
			TIME_FIELDS vqai_tf;
			RtlTimeToTimeFields(&vqai_li, &vqai_tf);

			WCHAR *wp = (WCHAR *)(hdr + 2);
			wp[0]  = L'@'; wp[1]  = L'G'; wp[2]  = L'M';
			wp[3]  = L'T'; wp[4]  = L'-';
			wp[5]  = L'0' + (WCHAR)(vqai_tf.Year / 1000);
			wp[6]  = L'0' + (WCHAR)((vqai_tf.Year / 100) % 10);
			wp[7]  = L'0' + (WCHAR)((vqai_tf.Year / 10) % 10);
			wp[8]  = L'0' + (WCHAR)(vqai_tf.Year % 10);
			wp[9]  = L'.';
			wp[10] = L'0' + (WCHAR)(vqai_tf.Month / 10);
			wp[11] = L'0' + (WCHAR)(vqai_tf.Month % 10);
			wp[12] = L'.';
			wp[13] = L'0' + (WCHAR)(vqai_tf.Day / 10);
			wp[14] = L'0' + (WCHAR)(vqai_tf.Day % 10);
			wp[15] = L'-';
			wp[16] = L'0' + (WCHAR)(vqai_tf.Hour / 10);
			wp[17] = L'0' + (WCHAR)(vqai_tf.Hour % 10);
			wp[18] = L'.';
			wp[19] = L'0' + (WCHAR)(vqai_tf.Minute / 10);
			wp[20] = L'0' + (WCHAR)(vqai_tf.Minute % 10);
			wp[21] = L'.';
			wp[22] = L'0' + (WCHAR)(vqai_tf.Second / 10);
			wp[23] = L'0' + (WCHAR)(vqai_tf.Second % 10);
			Irp->IoStatus.Information = appsz;
			dprintf("VSS %s: APPLICATION_INFO "
			    "@GMT-%04d.%02d.%02d-%02d.%02d.%02d\n",
			    __func__, vqai_tf.Year, vqai_tf.Month, vqai_tf.Day,
			    vqai_tf.Hour, vqai_tf.Minute, vqai_tf.Second);
		} else {
			status = STATUS_BUFFER_OVERFLOW;
			break;
		}
		status = STATUS_SUCCESS;
		break;
	}
#if 0
	case IOCTL_VOLSNAP_QUERY_SNAPSHOT_TIMESTAMP:
	{
		/*
		 * Smb2ShareQuerySnapShotTimestamp calls this on the snapshot
		 * device to obtain the snapshot creation time.  Returns a
		 * LARGE_INTEGER (Windows file time: 100-ns intervals since
		 * Jan 1, 1601).  srv2.sys converts it to the @GMT string used
		 * in the SRV_SNAPSHOT_ARRAY / Previous Versions response.
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_SNAPSHOT_TIMESTAMP\n",
		    __func__);
		if (outlen < sizeof (LARGE_INTEGER)) {
			Irp->IoStatus.Information = sizeof (LARGE_INTEGER);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		LARGE_INTEGER *vqst_li = (LARGE_INTEGER *)buf;
		TIME_UNIX_TO_WINDOWS_EX(zmo->vss_creation, 0,
		    vqst_li->QuadPart);
		LARGE_INTEGER vqst_tf_li = *vqst_li;
		TIME_FIELDS vqst_tf;
		RtlTimeToTimeFields(&vqst_tf_li, &vqst_tf);
		dprintf("VSS %s: SNAPSHOT_TIMESTAMP unix=%llu "
		    "@GMT-%04d.%02d.%02d-%02d.%02d.%02d (0x%llx)\n",
		    __func__, (unsigned long long)zmo->vss_creation,
		    vqst_tf.Year, vqst_tf.Month, vqst_tf.Day,
		    vqst_tf.Hour, vqst_tf.Minute, vqst_tf.Second,
		    (unsigned long long)vqst_li->QuadPart);
		Irp->IoStatus.Information = sizeof (LARGE_INTEGER);
		status = STATUS_SUCCESS;
		break;
	}
#endif

	case IOCTL_VOLSNAP_QUERY_REVERT_UID:
		/*
		 * srv2.sys calls this after QUERY_APPLICATION_INFO.
		 * Returns a 16-byte UID (GUID-sized) identifying this snapshot
		 * for revert purposes.  A zero GUID is acceptable here.
		 */
	    _Static_assert(IOCTL_VOLSNAP_QUERY_REVERT_UID == 0x5301e0, "Macro did not match expected hex!");
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_REVERT_UID\n", __func__);
		if (outlen >= 16)
			RtlZeroMemory(buf, 16);
		Irp->IoStatus.Information = (outlen >= 16) ? 16 : 0;
		status = STATUS_SUCCESS;
		break;

	case IOCTL_VOLSNAP_QUERY_CONFIG_INFO:
	case IOCTL_VOLSNAP_QUERY_CONFIG_INFO_RO:
	{

	    typedef struct _VOLSNAP_CONFIG_INFO {
		ULONG           Attributes;
		ULONG           Reserved;
		LARGE_INTEGER   SnapshotCreationTime;
	    } VOLSNAP_CONFIG_INFO, *PVOLSNAP_CONFIG_INFO;

	    dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_CONFIG_INFO\n",
		__func__);

	    ULONG need = sizeof(VOLSNAP_CONFIG_INFO);

	    if (outlen < need) {
		Irp->IoStatus.Information = need;
		status = STATUS_BUFFER_OVERFLOW;
		break;
	    }

	    VOLSNAP_CONFIG_INFO *config = (VOLSNAP_CONFIG_INFO *)buf;
	    RtlZeroMemory(config, sizeof(*config));

	    config->Attributes =
		VSS_VOLSNAP_ATTR_PERSISTENT |
		VSS_VOLSNAP_ATTR_CLIENT_ACCESSIBLE |
		VSS_VOLSNAP_ATTR_NO_AUTO_RELEASE;

	    /*
	     * SnapshotCreationTime must be a 64-bit Windows FILETIME
	     * (100-nanosecond intervals since January 1, 1601).
	     * Ensure zmo->creation_time (Unix epoch) is converted cleanly.
	     */

	    LARGE_INTEGER ct;
	    TIME_UNIX_TO_WINDOWS_EX(zmo->vss_creation, 0,
		ct.QuadPart);
	    config->SnapshotCreationTime.QuadPart = ct.QuadPart;

	    Irp->IoStatus.Information = sizeof(VOLSNAP_CONFIG_INFO);

	    status = STATUS_SUCCESS;
	    break;
	}

	case IOCTL_VOLSNAP_QUERY_ORIGINAL_VOLUME_NAME:
	{
	    // 2. Put your target string lookup logic here!
	    // This is where srv2.sys / I/O Manager asks for the parent volume link string.
	    char vqovn_ds[256];
	    strlcpy(vqovn_ds, zmo->vss_snapname, sizeof(vqovn_ds));
	    char *vqovn_at = strchr(vqovn_ds, '@');
	    if (vqovn_at)
		*vqovn_at = '\0';

	    vqovn_ctx_t vqovn_ctx = { .dataset = vqovn_ds };
	    vfs_mount_iterate(vqovn_find_cb, &vqovn_ctx);

	    if (!vqovn_ctx.found) {
		status = STATUS_NOT_FOUND;
		break;
	    }

	    // Standard volume name structures follow a short length prefix then WCHAR array
	    ULONG string_need = sizeof(USHORT) + vqovn_ctx.namelen;
	    if (outlen < string_need) {
		Irp->IoStatus.Information = string_need;
		status = STATUS_BUFFER_OVERFLOW;
		break;
	    }

	    USHORT *out_len_prefix = (USHORT *)buf;
	    *out_len_prefix = vqovn_ctx.namelen;
	    RtlCopyMemory(out_len_prefix + 1, vqovn_ctx.namebuf, vqovn_ctx.namelen);

	    Irp->IoStatus.Information = string_need;
	    status = STATUS_SUCCESS;
	    break;
	}

	case IOCTL_VOLSNAP_QUERY_APPLICATION_INFO:
	{
		/*
		 * typedef struct _VOLSNAP_APPLICATION_INFO {
		 * ULONG   InformationLength;
		 * UCHAR   Information[1];
		 * //
		 * // It is highly recommended that the first 16 bytes from the 
		 * // Application Info should be a unique GUID indentifying the 
		 * // unique structure layout of the subsequent Application Info.
		 * //
	         * } VOLSNAP_APPLICATION_INFO, *PVOLSNAP_APPLICATION_INFO;
		 * 
		 * srv2.sys probes with a small buffer (BUFFER_OVERFLOW),
		 * reads [probe_buf+0] as DWORD, adds 8, allocates, retries.
		 * After the real call it checks:
		 *   [rdi+0] DWORD > 0x10              (total size sanity)
		 *   [rdi+4] == VOLSNAP_APPINFO_GUID_CLIENT_ACCESSIBLE (16 B)
		 *
		 * Wire format (inferred from srv2.sys disassembly):
		 *   ULONG  cb;       // +0:  total struct size, must be > 16
		 *   GUID   guid;     // +4:  VOLSNAP_APPINFO_GUID_CLIENT_ACCESSIBLE
		 *   USHORT NameLen;  // +20: byte count of VolumeName
		 *   WCHAR  Name[];   // +22: NT device name of original volume
		 *
		 * Reply structure should be:
		 *
		 */
#pragma pack(push, 4)
typedef struct _VSS_APPLICATION_INFO_PAYLOAD {
    /* The outer VOLSNAP_APPLICATION_INFO wrapper header */
	    ULONG   InformationLength;   // Total size of subsequent fields (0x38 or 56 bytes)

	    /* The serialized stream fields packed by VSS channel */
	    GUID    AppInfoLayoutId;     // VOLSNAP_APPINFO_GUID_CLIENT_ACCESSIBLE ({e5de7d45-49f2-40a4-817c-7dc82b72587f})
	    GUID    SnapshotId;          // The unique VSS Snapshot ID tracking this dataset snapshot
	    GUID    SnapshotSetId;       // The unique VSS Snapshot Set/Group ID
	    ULONG   SnapshotContext;     // The VSS Context flags (e.g., VSS_CTX_NAS_ROLLBACK / VSS_CTX_CLIENT_ACCESSIBLE)
	    ULONG   SnapshotCount;       // Number of snapshots included in this frozen set
	} VSS_APPLICATION_INFO_PAYLOAD, *PVSS_APPLICATION_INFO_PAYLOAD;
#pragma pack(pop)

		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_APPLICATION_INFO (outlen=%lu)\n",
		    __func__, outlen);

	    /* Entire payload structure footprint is exactly 60 bytes (0x3C) */
	    ULONG need = sizeof(VSS_APPLICATION_INFO_PAYLOAD);

	    if (outlen < need) {
		/* Stage 1 Probe: Pass required allocation payload footprint size back to srv2 */
		if (outlen >= sizeof(ULONG)) {
		    *(ULONG *)buf = need;
		}
		Irp->IoStatus.Information = sizeof(ULONG);
		status = STATUS_BUFFER_OVERFLOW; // Trigger the memory allocation loop
		break;
	    }

	    /* Stage 2: Populate the target buffer memory space explicitly */
	    VSS_APPLICATION_INFO_PAYLOAD *app_info = (VSS_APPLICATION_INFO_PAYLOAD *)buf;
	    RtlZeroMemory(app_info, need);

	    /* InformationLength is the size of everything *after* the ULONG length tracker field itself */
	    app_info->InformationLength = need - sizeof(ULONG); // 56 bytes (0x38)

	    /* Set structural verification layout tokens */
	    static const GUID client_accessible_guid = {
		    0xe5de7d45, 0x49f2, 0x40a4,
		    {0x81, 0x7c, 0x7d, 0xc8, 0x2b, 0x72, 0x58, 0x7f}
	    };
	    app_info->AppInfoLayoutId = client_accessible_guid;

	    /* Track individual volume snapshot definitions */
	    // app_info->SnapshotId = zmo->vss_snapshot_guid;
	    GUID *g = (GUID *)&(app_info->SnapshotId);
	    g->Data1 = (ULONG)(zmo->vss_guid & 0xffffffff);
	    g->Data2 = (USHORT)((zmo->vss_guid >> 32) & 0xffff);
	    g->Data3 = (USHORT)((zmo->vss_guid >> 48) & 0xffff);

	    // app_info->SnapshotSetId = zmo->vss_set_guid;
	    g = (GUID *)&(app_info->SnapshotSetId);
	    g->Data1 = (ULONG)(zmo->vss_guid & 0xffffffff);
	    g->Data2 = (USHORT)((zmo->vss_guid >> 32) & 0xffff);
	    g->Data3 = (USHORT)((zmo->vss_guid >> 48) & 0xffff);

	    /* 0x00000009 standard value for VSS_CTX_CLIENT_ACCESSIBLE context flags */
	    app_info->SnapshotContext =
		VSS_VOLSNAP_ATTR_CLIENT_ACCESSIBLE |
		VSS_VOLSNAP_ATTR_NO_AUTO_RELEASE |
		VSS_VOLSNAP_ATTR_NO_WRITERS;
	    app_info->SnapshotCount = 1;

	    Irp->IoStatus.Information = need;
	    status = STATUS_SUCCESS;
	    break;
	}

#if 0
		/*
		 * {E5DE7D45-49F2-40A4-817C-7DC82B72587F}
		 * Extracted from srv2!VOLSNAP_APPINFO_GUID_CLIENT_ACCESSIBLE.
		 */
		static const GUID vqovn_guid = {
		    0xe5de7d45, 0x49f2, 0x40a4,
		    {0x81, 0x7c, 0x7d, 0xc8, 0x2b, 0x72, 0x58, 0x7f}
		};

		/* Strip @snap suffix from vss_snapname to get dataset name. */
		char vqovn_ds[256];
		strlcpy(vqovn_ds, zmo->vss_snapname, sizeof (vqovn_ds));
		char *vqovn_at = strchr(vqovn_ds, '@');
		if (vqovn_at)
			*vqovn_at = '\0';

		vqovn_ctx_t vqovn_ctx = { .dataset = vqovn_ds };
		vfs_mount_iterate(vqovn_find_cb, &vqovn_ctx);

		if (!vqovn_ctx.found) {
			dprintf("VSS %s: parent DCB for '%s' not found\n",
			    __func__, vqovn_ds);
			status = STATUS_NOT_FOUND;
			break;
		}

		/* cb + GUID + NameLen + Name */
		ULONG vqovn_need = (ULONG)(sizeof (ULONG) + sizeof (GUID) +
		    sizeof (USHORT) + vqovn_ctx.namelen);

		if (outlen < vqovn_need) {
			/*
			 * Probe: write cb so srv2 can compute correct alloc
			 * size ("alloc = [probe_buf_dword] + 8").
			 */
			if (outlen >= sizeof (ULONG))
				*(ULONG *)buf = vqovn_need;
			Irp->IoStatus.Information = vqovn_need;
			status = STATUS_BUFFER_OVERFLOW;
			break;
		}

		ULONG  *vqovn_cb  = (ULONG *)buf;
		GUID   *vqovn_g   = (GUID *)(vqovn_cb + 1);
		USHORT *vqovn_nl  = (USHORT *)(vqovn_g + 1);
		*vqovn_cb = vqovn_need;
		*vqovn_g  = vqovn_guid;
		*vqovn_nl = vqovn_ctx.namelen;
		RtlCopyMemory(vqovn_nl + 1, vqovn_ctx.namebuf,
		    vqovn_ctx.namelen);
		Irp->IoStatus.Information = vqovn_need;
		dprintf("VSS %s: returning device name len=%u total=%u\n",
		    __func__, vqovn_ctx.namelen, vqovn_need);
		status = STATUS_SUCCESS;
		break;
#endif

	case IOCTL_VOLSNAP_QUERY_EPIC: /* IOCTL_VOLSNAP function 0x14 - control, no output */
		/*
		 * VSS sends this to the snapshot device with a 0-byte (or tiny)
		 * output buffer; it is a fire-and-forget control IOCTL with no
		 * data returned.  STATUS_SUCCESS + Information=0 is correct.
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_EPIC no-op\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IOCTL_STORAGE_QUERY_PROPERTY:
	{
		/*
		 * Return a minimal STORAGE_DEVICE_DESCRIPTOR so that VSS and
		 * storage subsystems can identify this as a virtual disk device.
		 */
		dprintf("VSS %s: IOCTL_STORAGE_QUERY_PROPERTY\n", __func__);

		if (IrpSp->Parameters.DeviceIoControl.InputBufferLength <
		    sizeof (STORAGE_PROPERTY_QUERY)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		STORAGE_PROPERTY_QUERY *q = (STORAGE_PROPERTY_QUERY *)buf;
		STORAGE_PROPERTY_ID propid = q->PropertyId;
		STORAGE_QUERY_TYPE qtype = q->QueryType;

		if (qtype == PropertyExistsQuery) {
			/* Exists query: just confirm StorageDeviceProperty exists */
			Irp->IoStatus.Information = 0;
			status = (propid == StorageDeviceProperty) ?
			    STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
			break;
		}

		if (propid != StorageDeviceProperty) {
			status = STATUS_NOT_SUPPORTED;
			break;
		}

		/* PropertyStandardQuery for StorageDeviceProperty */
		ULONG needed = sizeof (STORAGE_DEVICE_DESCRIPTOR);
		if (outlen < sizeof (STORAGE_DESCRIPTOR_HEADER)) {
			Irp->IoStatus.Information = needed;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		if (outlen < needed) {
			/* Return just the header */
			STORAGE_DESCRIPTOR_HEADER *h =
			    (STORAGE_DESCRIPTOR_HEADER *)buf;
			h->Version = sizeof (STORAGE_DEVICE_DESCRIPTOR);
			h->Size    = needed;
			Irp->IoStatus.Information =
			    sizeof (STORAGE_DESCRIPTOR_HEADER);
			status = STATUS_SUCCESS;
			break;
		}
		STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
		RtlZeroMemory(d, needed);
		d->Version            = sizeof (STORAGE_DEVICE_DESCRIPTOR);
		d->Size               = needed;
		d->DeviceType         = FILE_DEVICE_VIRTUAL_DISK;
		d->DeviceTypeModifier = 0;
		d->RemovableMedia     = FALSE;
		d->CommandQueueing    = FALSE;
		d->VendorIdOffset     = 0;
		d->ProductIdOffset    = 0;
		d->ProductRevisionOffset = 0;
		d->SerialNumberOffset = 0;
		/* BusTypeVirtual = 0x12 (Windows 8+) */
		d->BusType            = (STORAGE_BUS_TYPE)0x12;
		d->RawPropertiesLength = 0;
		Irp->IoStatus.Information = needed;
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_DISK_GET_PARTITION_INFO_EX: /* 0x70048 */
	{
		/*
		 * Snapshot devices are raw (no partition table).
		 * Return PARTITION_STYLE_RAW with the snapshot's logical size.
		 */
		dprintf("VSS %s: IOCTL_DISK_GET_PARTITION_INFO_EX\n", __func__);
		if (outlen < sizeof (PARTITION_INFORMATION_EX)) {
			Irp->IoStatus.Information =
			    sizeof (PARTITION_INFORMATION_EX);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		PARTITION_INFORMATION_EX *pi = (PARTITION_INFORMATION_EX *)buf;
		RtlZeroMemory(pi, sizeof (*pi));
		pi->PartitionStyle = PARTITION_STYLE_RAW;
		pi->StartingOffset.QuadPart = 0;
		pi->PartitionLength.QuadPart =
		    (LONGLONG)zfs_vss_snap_refbytes(zmo);
		pi->PartitionNumber = 0;
		Irp->IoStatus.Information = sizeof (PARTITION_INFORMATION_EX);
		status = STATUS_SUCCESS;
		break;
	}

	case 0x2d1084: /* IOCTL_STORAGE_GET_DEVICE_NUMBER_EX (Win10 1709+) */
	{
		/*
		 * STORAGE_DEVICE_NUMBER_EX:
		 *   ULONG Version, Size, Flags, DeviceType, DeviceNumber;
		 *   GUID  DeviceGuid;
		 *   ULONG PartitionNumber;
		 * Total: 6 * ULONG + GUID = 24 + 16 = 40 bytes.
		 */
		dprintf("VSS %s: IOCTL_STORAGE_GET_DEVICE_NUMBER_EX\n",
		    __func__);
		const ULONG sz = 6 * sizeof (ULONG) + sizeof (GUID);
		if (outlen < sz) {
			Irp->IoStatus.Information = sz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		ULONG *p = (ULONG *)buf;
		RtlZeroMemory(p, sz);
		p[0] = sz;	/* Version */
		p[1] = sz;	/* Size */
		p[2] = 0;	/* Flags */
		p[3] = FILE_DEVICE_VIRTUAL_DISK; /* DeviceType */
		p[4] = (ULONG)(zmo->vss_guid & 0xffffffff); /* DeviceNumber */
		/* DeviceGuid: derive from vss_guid */
		GUID *g = (GUID *)(p + 5);
		g->Data1 = (ULONG)(zmo->vss_guid & 0xffffffff);
		g->Data2 = (USHORT)((zmo->vss_guid >> 32) & 0xffff);
		g->Data3 = (USHORT)((zmo->vss_guid >> 48) & 0xffff);
		/* Data4 already zeroed */
		p[9] = (ULONG)-1;	/* PartitionNumber */
		Irp->IoStatus.Information = sz;
		status = STATUS_SUCCESS;
		break;
	}

	case 0x2d0c14: /* IOCTL_STORAGE_GET_HOTPLUG_INFO (function 0x305) */
	{
		/*
		 * STORAGE_HOTPLUG_INFO: { ULONG Size; BOOLEAN x4; }
		 * Snapshot devices are fixed, non-removable virtual disks.
		 */
		dprintf("VSS %s: IOCTL_STORAGE_GET_HOTPLUG_INFO\n", __func__);
		const ULONG sz = sizeof (ULONG) + 4 * sizeof (BOOLEAN);
		if (outlen < sz) {
			Irp->IoStatus.Information = sz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		ULONG *shi = (ULONG *)buf;
		RtlZeroMemory(shi, sz);
		shi[0] = sz; /* Size field */
		/* MediaRemovable, MediaHotplug, DeviceHotplug, Override = FALSE */
		Irp->IoStatus.Information = sz;
		status = STATUS_SUCCESS;
		break;
	}

	case 0x56c064: /* VOLUME function 0x19 r/w - not applicable to snaps */
	case 0x2d118c: /* STORAGE function 0x463 - not applicable to snaps */
	case 0x2d1190: /* STORAGE function 0x464 - not applicable to snaps */
		dprintf("VSS %s: optional ioctl 0x%lx not supported\n",
		    __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED;
		break;

	case 0x4d0010: /* VOLMGR function 4 - not applicable to snapshots */
	case 0x4d0018: /* VOLMGR function 6 - not applicable to snapshots */
		dprintf("VSS %s: volmgr 0x%lx not supported\n", __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED;
		break;

	default:
		dprintf("VSS %s: unhandled ioctl 0x%lx\n", __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	dprintf("VSS %s: ioctl 0x%lx -> ntstatus 0x%lx info %lu\n",
	    __func__, cmd, (ULONG)status,
	    (ULONG)Irp->IoStatus.Information);

	return (status);
}

/*
 * IRP dispatcher for \Device\ZfsSnapshot<hex> devices.
 */
NTSTATUS
zfs_vss_dispatcher(PDEVICE_OBJECT DeviceObject, PIRP *pIrp,
    PIO_STACK_LOCATION IrpSp)
{
	PIRP Irp = *pIrp;
	NTSTATUS status;

	mount_t *zmo = DeviceObject->DeviceExtension;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
	{
		if (vfs_fsprivate(zmo) != NULL) {
			/* Snapshot already mounted — fsDispatcher handles all. */
			dprintf("VSS %s: IRP_MJ_CREATE -> fsDispatcher"
			    " (mounted)\n", __func__);
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}

		/*
		 * Snapshot not yet mounted.
		 *
		 * If this is a pure device open (srv2.sys probing for AppInfo
		 * via NtOpenFile + IOCTL_VOLSNAP_QUERY_APPLICATION_INFO), the
		 * FileName is empty or just "\".  Succeed immediately without
		 * triggering a full ZFS mount.
		 */
		{
			PUNICODE_STRING fn = &IrpSp->FileObject->FileName;
			boolean_t is_dev_open = (fn->Length == 0 ||
			    (fn->Length == sizeof (WCHAR) &&
			    fn->Buffer[0] == L'\\'));
			if (is_dev_open) {
				dprintf("VSS %s: IRP_MJ_CREATE device open"
				    " (no mount)\n", __func__);
				Irp->IoStatus.Status = STATUS_SUCCESS;
				Irp->IoStatus.Information = FILE_OPENED;
				IoCompleteRequest(Irp, IO_NO_INCREMENT);
				*pIrp = NULL;
				return (STATUS_SUCCESS);
			}
		}

		/*
		 * File-path open — pend and schedule a work item to mount,
		 * then replay via fsDispatcher.
		 */
		dprintf("VSS %s: IRP_MJ_CREATE -> lazy mount\n", __func__);
		zfs_vss_mount_wi_t *wi =
		    kmem_alloc(sizeof (*wi), KM_NOSLEEP);
		if (wi == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		wi->wi_item = IoAllocateWorkItem(DeviceObject);
		if (wi->wi_item == NULL) {
			kmem_free(wi, sizeof (*wi));
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		wi->wi_devobj = DeviceObject;
		wi->wi_irp    = Irp;
		IoMarkIrpPending(Irp);
		IoQueueWorkItem(wi->wi_item, zfs_vss_mount_workitem,
		    DelayedWorkQueue, wi);
		*pIrp = NULL; /* work item owns the IRP now */
		return (STATUS_PENDING);
	}

	case IRP_MJ_CLEANUP:
		if (vfs_fsprivate(zmo) != NULL &&
		    IrpSp->FileObject != NULL &&
		    IrpSp->FileObject->FsContext != NULL) {
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}
		dprintf("VSS %s: IRP_MJ_CLEANUP\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IRP_MJ_CLOSE:
		if (vfs_fsprivate(zmo) != NULL &&
		    IrpSp->FileObject != NULL &&
		    IrpSp->FileObject->FsContext != NULL) {
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}
		dprintf("VSS %s: IRP_MJ_CLOSE\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IRP_MJ_DEVICE_CONTROL:
		status = zfs_vss_device_control(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_READ:
		/*
		 * If a file is open (FsContext set), delegate to fsDispatcher.
		 * Otherwise this is a raw sector read from VSS probing the
		 * device; return zeroed data.
		 */
		if (vfs_fsprivate(zmo) != NULL &&
		    IrpSp->FileObject != NULL &&
		    IrpSp->FileObject->FsContext != NULL) {
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}
		{
			ULONG rdlen = IrpSp->Parameters.Read.Length;
			dprintf("VSS %s: IRP_MJ_READ (raw) off=%lld len=%lu\n",
			    __func__,
			    IrpSp->Parameters.Read.ByteOffset.QuadPart, rdlen);
			if (rdlen > 0 && Irp->MdlAddress != NULL) {
				void *kbuf = MmGetSystemAddressForMdlSafe(
				    Irp->MdlAddress, NormalPagePriority);
				if (kbuf != NULL) {
					RtlZeroMemory(kbuf, rdlen);
					Irp->IoStatus.Information = rdlen;
					status = STATUS_SUCCESS;
					break;
				}
			}
			Irp->IoStatus.Information = 0;
			status = STATUS_SUCCESS;
		}
		break;

	case IRP_MJ_WRITE:
	case IRP_MJ_SET_INFORMATION:
	case IRP_MJ_QUERY_EA:
	case IRP_MJ_SET_EA:
	case IRP_MJ_FLUSH_BUFFERS:
	case IRP_MJ_QUERY_VOLUME_INFORMATION:
	case IRP_MJ_SET_VOLUME_INFORMATION:
	case IRP_MJ_DIRECTORY_CONTROL:
	case IRP_MJ_FILE_SYSTEM_CONTROL:
	case IRP_MJ_LOCK_CONTROL:
	case IRP_MJ_QUERY_SECURITY:
	case IRP_MJ_SET_SECURITY:
		if (vfs_fsprivate(zmo) != NULL) {
			dprintf("VSS %s: major 0x%x -> fsDispatcher\n",
			    __func__, IrpSp->MajorFunction);
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}
		dprintf("VSS %s: major 0x%x unmounted\n",
		    __func__, IrpSp->MajorFunction);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;

	case IRP_MJ_QUERY_INFORMATION:
		if (vfs_fsprivate(zmo) != NULL) {
			dprintf("VSS %s: IRP_MJ_QUERY_INFORMATION -> fsDispatcher\n",
			    __func__);
			return (fsDispatcher(DeviceObject, pIrp, IrpSp));
		}
		/*
		 * Unmounted: answer creation-time queries used by srv2.sys
		 * when it probes the snapshot device for AppInfo.
		 */
		{
			ULONG cls =
			    IrpSp->Parameters.QueryFile.FileInformationClass;
			void *ibuf = Irp->AssociatedIrp.SystemBuffer;
			LARGE_INTEGER ct;
			TIME_UNIX_TO_WINDOWS_EX(zmo->vss_creation, 0,
			    ct.QuadPart);

			if (cls == FileBasicInformation) {
				if (IrpSp->Parameters.QueryFile.Length <
				    sizeof (FILE_BASIC_INFORMATION)) {
					Irp->IoStatus.Information =
					    sizeof (FILE_BASIC_INFORMATION);
					status = STATUS_BUFFER_TOO_SMALL;
				} else {
					FILE_BASIC_INFORMATION *fbi = ibuf;
					RtlZeroMemory(fbi, sizeof (*fbi));
					fbi->CreationTime = ct;
					fbi->LastAccessTime = ct;
					fbi->LastWriteTime  = ct;
					fbi->ChangeTime     = ct;
					fbi->FileAttributes =
					    FILE_ATTRIBUTE_READONLY;
					Irp->IoStatus.Information =
					    sizeof (FILE_BASIC_INFORMATION);
					status = STATUS_SUCCESS;
				}
			} else if (cls == FileNetworkOpenInformation) {
				if (IrpSp->Parameters.QueryFile.Length <
				    sizeof (FILE_NETWORK_OPEN_INFORMATION)) {
					Irp->IoStatus.Information =
					    sizeof (FILE_NETWORK_OPEN_INFORMATION);
					status = STATUS_BUFFER_TOO_SMALL;
				} else {
					FILE_NETWORK_OPEN_INFORMATION *fnoi = ibuf;
					RtlZeroMemory(fnoi, sizeof (*fnoi));
					fnoi->CreationTime = ct;
					fnoi->LastAccessTime = ct;
					fnoi->LastWriteTime  = ct;
					fnoi->ChangeTime     = ct;
					fnoi->AllocationSize.QuadPart =
					    (LONGLONG)zfs_vss_snap_refbytes(zmo);
					fnoi->EndOfFile =
					    fnoi->AllocationSize;
					fnoi->FileAttributes =
					    FILE_ATTRIBUTE_READONLY;
					Irp->IoStatus.Information =
					    sizeof (FILE_NETWORK_OPEN_INFORMATION);
					status = STATUS_SUCCESS;
				}
			} else {
				dprintf("VSS %s: QUERY_INFO unmounted"
				    " cls=%lu\n", __func__, cls);
				Irp->IoStatus.Information = 0;
				status = STATUS_INVALID_DEVICE_REQUEST;
			}
		}
		break;

	default:
		dprintf("VSS %s: unhandled major 0x%x\n",
		    __func__, IrpSp->MajorFunction);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	*pIrp = NULL;
	return (status);
}

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

/* Prefix for all snapshot device objects. */
#define	ZFS_VSS_DEVICE_PREFIX	"\\Device\\ZfsSnapshot"
#define	ZFS_VSS_DEVICE_PREFIXW	L"\\Device\\ZfsSnapshot"

/*
 * Device extension stored in each \Device\ZfsSnapshot<hex> device object.
 * Must begin with FSD_IDENTIFIER_TYPE so the main dispatcher can route it.
 */
typedef struct zfs_vss_ext {
	FSD_IDENTIFIER_TYPE	zve_type;	/* == MOUNT_TYPE_VSS */
	uint64_t		zve_guid;	/* ZFS dataset GUID */
	char			zve_poolname[MAXPATHLEN]; /* pool name for DSL */
} zfs_vss_ext_t;

/*
 * Per-snapshot device tracking entry.
 * Stored in a global AVL tree keyed by ZFS dataset GUID.
 */
typedef struct zfs_vss_snap {
	avl_node_t	zvs_node;
	uint64_t	zvs_guid;	/* ZFS dataset GUID (key) */
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

/*
 * Create \Device\ZfsSnapshot<hex16> for the given ZFS dataset GUID.
 * poolname is stored in the device extension so IOCTL handlers can
 * open the SPA and look up the snapshot for size queries.
 */
int
zfs_vss_snapshot_add(uint64_t guid, const char *poolname)
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
	 * FILE_DEVICE_VIRTUAL_DISK: no "new device" popup.
	 * FILE_READ_ONLY_DEVICE: snapshot is always read-only.
	 * Allocate sizeof(zfs_vss_ext_t) extension so the dispatcher
	 * can identify this as a VSS device via DeviceExtension->type.
	 */
	status = IoCreateDevice(WIN_DriverObject,
	    sizeof (zfs_vss_ext_t),
	    &ustr,
	    FILE_DEVICE_VIRTUAL_DISK,
	    FILE_DEVICE_SECURE_OPEN | FILE_READ_ONLY_DEVICE,
	    FALSE,
	    &devobj);

	RtlFreeUnicodeString(&ustr);

	if (!NT_SUCCESS(status)) {
		dprintf("%s: IoCreateDevice '%s' failed 0x%x\n",
		    __func__, devname, status);
		return (EIO);
	}

	zfs_vss_ext_t *ext = devobj->DeviceExtension;
	ext->zve_type = MOUNT_TYPE_VSS;
	ext->zve_guid = guid;
	strlcpy(ext->zve_poolname, poolname ? poolname : "",
	    sizeof (ext->zve_poolname));

	zfs_vss_snap_t *zvs = kmem_alloc(sizeof (*zvs), KM_SLEEP);
	zvs->zvs_guid   = guid;
	zvs->zvs_devobj = devobj;

	mutex_enter(&zfs_vss_lock);
	avl_add(&zfs_vss_snaps, zvs);
	mutex_exit(&zfs_vss_lock);

	dprintf("%s: created %s\n", __func__, devname);
	return (0);
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
			    spa_name(spa));
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

	uint64_t guid = dsl_dataset_phys(ds)->ds_guid;
	(void) zfs_vss_snapshot_add(guid, spa_name(dp->dp_spa));
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
zfs_vss_snap_refbytes(zfs_vss_ext_t *ext)
{
	spa_t *spa;
	dsl_pool_t *dp;
	dsl_dataset_t *ds;
	uint64_t refbytes = 0;

	if (ext->zve_poolname[0] == '\0')
		return (0);

	if (spa_open(ext->zve_poolname, &spa, FTAG) != 0)
		return (0);

	dp = spa_get_dsl(spa);
	if (dp == NULL) {
		spa_close(spa, FTAG);
		return (0);
	}

	dsl_pool_config_enter(dp, FTAG);
	if (dsl_dataset_hold_obj(dp, ext->zve_guid, FTAG, &ds) == 0) {
		refbytes = dsl_dataset_phys(ds)->ds_referenced_bytes;
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);
	spa_close(spa, FTAG);

	return (refbytes);
}

/*
 * IRP_MJ_DEVICE_CONTROL handler for VSS snapshot devices.
 *
 * Handles the storage/volume/mountdev IOCTLs that VSS and the
 * Mount Manager issue when discovering and characterising shadow copies.
 */
static NTSTATUS
zfs_vss_device_control(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	zfs_vss_ext_t *ext = DeviceObject->DeviceExtension;
	ulong_t cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
	ulong_t outlen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	void *buf = Irp->AssociatedIrp.SystemBuffer;
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

	switch (cmd) {

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
		sdn->DeviceNumber  = (ULONG)(ext->zve_guid & 0xffffffff);
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
		gli->Length.QuadPart = (int64_t)zfs_vss_snap_refbytes(ext);
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
		uint64_t total = zfs_vss_snap_refbytes(ext);
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
		    (unsigned long long)ext->zve_guid);
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
		    "%016llx", (unsigned long long)ext->zve_guid);
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
		vde->Extents[0].DiskNumber = (ULONG)(ext->zve_guid & 0xffffffff);
		vde->Extents[0].StartingOffset.QuadPart = 0;
		vde->Extents[0].ExtentLength.QuadPart =
		    (int64_t)zfs_vss_snap_refbytes(ext);
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

	default:
		dprintf("VSS %s: unhandled ioctl 0x%lx\n", __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

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

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("VSS %s: IRP_MJ_CREATE\n", __func__);
		Irp->IoStatus.Information = FILE_OPENED;
		status = STATUS_SUCCESS;
		break;

	case IRP_MJ_CLEANUP:
		dprintf("VSS %s: IRP_MJ_CLEANUP\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IRP_MJ_CLOSE:
		dprintf("VSS %s: IRP_MJ_CLOSE\n", __func__);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case IRP_MJ_DEVICE_CONTROL:
		status = zfs_vss_device_control(DeviceObject, Irp, IrpSp);
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

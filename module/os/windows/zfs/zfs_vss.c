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

static void zfs_vss_notify_mountmgr(PDEVICE_OBJECT devobj);

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

	/*
	 * Clear DO_DEVICE_INITIALIZING so the I/O Manager will accept IRPs
	 * sent to this device.  Must be done before notifying the Mount Manager,
	 * otherwise its probe IRPs are silently rejected.
	 */
	devobj->Flags &= ~DO_DEVICE_INITIALIZING;

	zfs_vss_snap_t *zvs = kmem_alloc(sizeof (*zvs), KM_SLEEP);
	zvs->zvs_guid   = guid;
	zvs->zvs_devobj = devobj;

	mutex_enter(&zfs_vss_lock);
	avl_add(&zfs_vss_snaps, zvs);
	mutex_exit(&zfs_vss_lock);

	dprintf("%s: created %s\n", __func__, devname);

	/*
	 * Intentionally skip MountMgr volume-arrival notification for
	 * snapshot devices.  Notifying MountMgr causes the Windows System
	 * Provider to load and probe the device with VOLSNAP IOCTLs whose
	 * responses it passes through ichannel — producing "IOCTL Unpack
	 * overflow" errors before our software provider can respond.
	 * Snapshot devices do not need drive letters; the VSS software
	 * provider already knows the full device path.
	 */
	(void) devobj;

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

	case 0x534058: /* IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE */
		/*
		 * VSS coordinator uses ichannel.hxx to Unpack<ULONGLONG> from
		 * this response.  ZFS snapshots are CoW; no diff area storage
		 * is used.  Return 0 to satisfy the Unpack size check.
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_DIFF_AREA_MINIMUM_SIZE"
		    " (snapshot device)\n", __func__);
		if (outlen < sizeof (ULONGLONG)) {
			Irp->IoStatus.Information = sizeof (ULONGLONG);
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		*(ULONGLONG *)buf = 0;
		Irp->IoStatus.Information = sizeof (ULONGLONG);
		status = STATUS_SUCCESS;
		break;

	case IOCTL_VOLSNAP_FLUSH_AND_HOLD_WRITES:
	case 0x0053C004: /* IOCTL_VOLSNAP_RELEASE_WRITES */
		/*
		 * Snapshot devices are read-only; no write hold/release needed.
		 */
		dprintf("VSS %s: VOLSNAP flush/hold/release no-op 0x%lx\n",
		    __func__, cmd);
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;

	case 0x53406C: /* IOCTL_VOLSNAP_QUERY_DIFF_AREA_INFORMATION */
	{
		/*
		 * Return a zeroed structure; no diff area is maintained for
		 * ZFS CoW snapshots.  Structure layout:
		 *   ULONGLONG UsedSpace
		 *   ULONGLONG AllocatedSpace
		 *   ULONGLONG MaximumSpace
		 */
		dprintf("VSS %s: IOCTL_VOLSNAP_QUERY_DIFF_AREA_INFORMATION\n",
		    __func__);
		const ULONG sz = 3 * sizeof (ULONGLONG);
		if (outlen < sz) {
			Irp->IoStatus.Information = sz;
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		RtlZeroMemory(buf, sz);
		Irp->IoStatus.Information = sz;
		status = STATUS_SUCCESS;
		break;
	}

	case 0x530018: /* IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS (primary) */
	case 0x534014: /* IOCTL_VOLSNAP_QUERY_NAMES_OF_SNAPSHOTS (alt) */
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

	case 0x530050: /* IOCTL_VOLSNAP function 0x14 - control, no output */
		/*
		 * VSS sends this to the snapshot device with a 0-byte (or tiny)
		 * output buffer; it is a fire-and-forget control IOCTL with no
		 * data returned.  STATUS_SUCCESS + Information=0 is correct.
		 */
		dprintf("VSS %s: VOLSNAP 0x530050 no-op\n", __func__);
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
		    (LONGLONG)zfs_vss_snap_refbytes(ext);
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
		p[4] = (ULONG)(ext->zve_guid & 0xffffffff); /* DeviceNumber */
		/* DeviceGuid: derive from zve_guid */
		GUID *g = (GUID *)(p + 5);
		g->Data1 = (ULONG)(ext->zve_guid & 0xffffffff);
		g->Data2 = (USHORT)((ext->zve_guid >> 32) & 0xffff);
		g->Data3 = (USHORT)((ext->zve_guid >> 48) & 0xffff);
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
		status = STATUS_INVALID_DEVICE_REQUEST;
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

	case IRP_MJ_READ:
	{
		/*
		 * VSS reads the first sector of the snapshot device to verify
		 * it is accessible.  The snapshot device is a virtual read-only
		 * representation; return zeroed data so the read succeeds.
		 * The buffer is mapped via MDL for disk-style reads.
		 */
		ULONG rdlen = IrpSp->Parameters.Read.Length;
		dprintf("VSS %s: IRP_MJ_READ off=%lld len=%lu\n", __func__,
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
		/* No MDL (e.g. buffered read with 0 length) */
		Irp->IoStatus.Information = 0;
		status = STATUS_SUCCESS;
		break;
	}

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		/*
		 * FSCTLs to the snapshot device: reject gracefully so that
		 * VSS does not interpret the error as a fatal provider veto.
		 */
		dprintf("VSS %s: IRP_MJ_FILE_SYSTEM_CONTROL fsctl=0x%lx"
		    " (not supported)\n", __func__,
		    IrpSp->Parameters.FileSystemControl.FsControlCode);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;

	case IRP_MJ_QUERY_VOLUME_INFORMATION:
		/*
		 * Volume information queries: return STATUS_INVALID_DEVICE_REQUEST
		 * for now; snapshot devices are not full filesystem volumes.
		 */
		dprintf("VSS %s: IRP_MJ_QUERY_VOLUME_INFORMATION class=%d\n",
		    __func__,
		    IrpSp->Parameters.QueryVolume.FsInformationClass);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_DEVICE_REQUEST;
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

// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * In-kernel root pool import, for Windows booting from ZFS.
 *
 * When the kernel asks us to mount the boot partition there is no zpool
 * and no zed, so we read the vdev label from the partition device we were
 * handed, build the pool config from it, and register the pool the same
 * way spa_import_rootpool() does on FreeBSD. The pool is then opened
 * normally and bootfs gives the dataset to mount.
 *
 * Only single top-level vdev pools are supported for now.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <sys/dsl_dataset.h>

#include <ntdddisk.h>
#include <ntddstor.h>

#define	RP_TAG	'tRpZ'

static NTSTATUS
rp_wait(PDEVICE_OBJECT dev, PIRP irp, PKEVENT ev, PIO_STATUS_BLOCK iosb)
{
	NTSTATUS st = IoCallDriver(dev, irp);

	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(ev, Executive, KernelMode, FALSE, NULL);
		st = iosb->Status;
	}
	return (st);
}

static NTSTATUS
rp_ioctl(PDEVICE_OBJECT dev, ULONG code, void *out, ULONG outlen)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb = { 0 };
	PIRP irp;

	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(code, dev, NULL, 0, out, outlen,
	    FALSE, &ev, &iosb);
	if (irp == NULL)
		return (STATUS_INSUFFICIENT_RESOURCES);
	return (rp_wait(dev, irp, &ev, &iosb));
}

static NTSTATUS
rp_read(PDEVICE_OBJECT dev, uint64_t offset, void *buf, ULONG len)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb = { 0 };
	LARGE_INTEGER off;
	PIRP irp;

	off.QuadPart = offset;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, dev, buf, len, &off,
	    &ev, &iosb);
	if (irp == NULL)
		return (STATUS_INSUFFICIENT_RESOURCES);
	return (rp_wait(dev, irp, &ev, &iosb));
}

/*
 * Read the four labels and return the config with the highest txg.
 * ENOENT if none of them hold a usable ZFS label.
 */
static int
rp_read_label(PDEVICE_OBJECT dev, uint64_t psize, nvlist_t **configp)
{
	vdev_phys_t *vp;
	nvlist_t *best = NULL;
	uint64_t best_txg = 0;

	vp = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof (vdev_phys_t),
	    RP_TAG);
	if (vp == NULL)
		return (ENOMEM);

	psize = P2ALIGN_TYPED(psize, sizeof (vdev_label_t), uint64_t);

	for (int l = 0; l < VDEV_LABELS; l++) {
		nvlist_t *config = NULL;
		uint64_t txg = 0, state = 0;

		if (!NT_SUCCESS(rp_read(dev, vdev_label_offset(psize, l,
		    offsetof(vdev_label_t, vl_vdev_phys)), vp,
		    sizeof (vdev_phys_t))))
			continue;

		if (nvlist_unpack(vp->vp_nvlist, sizeof (vp->vp_nvlist),
		    &config, KM_SLEEP) != 0)
			continue;

		if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_STATE,
		    &state) != 0 || state == POOL_STATE_DESTROYED ||
		    nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG,
		    &txg) != 0) {
			nvlist_free(config);
			continue;
		}

		if (best == NULL || txg > best_txg) {
			if (best != NULL)
				nvlist_free(best);
			best = config;
			best_txg = txg;
		} else {
			nvlist_free(config);
		}
	}

	ExFreePoolWithTag(vp, RP_TAG);

	if (best == NULL)
		return (ENOENT);
	*configp = best;
	return (0);
}

/*
 * The paths stored in the label are whatever userland used at create
 * time; at boot we know exactly which device this is, so point the leaf
 * vdev with this guid at it.
 */
static boolean_t
rp_set_path(nvlist_t *nv, uint64_t guid, const char *path)
{
	nvlist_t **child;
	uint_t n;
	uint64_t g;

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN, &child,
	    &n) == 0) {
		for (uint_t i = 0; i < n; i++) {
			if (rp_set_path(child[i], guid, path))
				return (B_TRUE);
		}
		return (B_FALSE);
	}

	if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &g) == 0 &&
	    g == guid) {
		fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
		(void) nvlist_remove_all(nv, ZPOOL_CONFIG_PHYS_PATH);
		(void) nvlist_remove_all(nv, ZPOOL_CONFIG_DEVID);
		return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Turn a single device's label config into a full pool config, as
 * spa_generate_rootconf() does on FreeBSD.
 */
static nvlist_t *
rp_generate_rootconf(nvlist_t *label, const char *path)
{
	nvlist_t *config, *top, *nvroot;
	uint64_t pgid, vguid, nchildren = 1;

	(void) nvlist_lookup_uint64(label, ZPOOL_CONFIG_VDEV_CHILDREN,
	    &nchildren);
	if (nchildren != 1) {
		cmn_err(CE_NOTE, "%s: root pool has %llu top-level vdevs, "
		    "only 1 supported", __func__, nchildren);
		return (NULL);
	}

	vguid = fnvlist_lookup_uint64(label, ZPOOL_CONFIG_GUID);
	top = fnvlist_dup(fnvlist_lookup_nvlist(label,
	    ZPOOL_CONFIG_VDEV_TREE));

	if (!rp_set_path(top, vguid, path)) {
		cmn_err(CE_NOTE, "%s: label guid not in its own vdev tree",
		    __func__);
		fnvlist_free(top);
		return (NULL);
	}

	config = fnvlist_dup(label);

	pgid = fnvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID);
	nvroot = fnvlist_alloc();
	fnvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_uint64(nvroot, ZPOOL_CONFIG_ID, 0ULL);
	fnvlist_add_uint64(nvroot, ZPOOL_CONFIG_GUID, pgid);
	fnvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&top, 1);

	fnvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, nvroot);

	/* Drop vdev-level elements that don't belong in a pool config */
	fnvlist_remove(config, ZPOOL_CONFIG_GUID);
	fnvlist_remove(config, ZPOOL_CONFIG_TOP_GUID);

	fnvlist_free(nvroot);
	fnvlist_free(top);
	return (config);
}

/*
 * Register the pool as the root pool; the same dance as FreeBSD's
 * spa_import_rootpool(). The pool is not loaded until it is opened.
 */
static int
rp_register_rootpool(nvlist_t *config, char *namep, size_t namelen)
{
	spa_t *spa;
	vdev_t *rvd;
	nvlist_t *nvtop;
	const char *pname;
	int error;

	pname = fnvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME);
	strlcpy(namep, pname, namelen);

	spa_namespace_enter(FTAG);

	if ((spa = spa_lookup(pname)) != NULL) {
		/* Already imported (e.g. by a previous attempt) */
		if (spa->spa_state == POOL_STATE_ACTIVE) {
			spa_namespace_exit(FTAG);
			return (0);
		}
		spa_remove(spa);
	}
	spa = spa_add(pname, config, NULL);

	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_VERSION,
	    &spa->spa_ubsync.ub_version) != 0)
		spa->spa_ubsync.ub_version = SPA_VERSION_INITIAL;

	spa->spa_is_root = B_TRUE;
	spa->spa_import_flags = ZFS_IMPORT_VERBATIM;

	nvtop = fnvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	error = spa_config_parse(spa, &rvd, nvtop, NULL, 0,
	    VDEV_ALLOC_ROOTPOOL);
	spa_config_exit(spa, SCL_ALL, FTAG);
	if (error) {
		spa_namespace_exit(FTAG);
		cmn_err(CE_NOTE, "%s: can not parse config for pool '%s'",
		    __func__, pname);
		return (error);
	}

	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	vdev_free(rvd);
	spa_config_exit(spa, SCL_ALL, FTAG);
	spa_namespace_exit(FTAG);

	return (0);
}

/*
 * Which partition did the loader boot from?
 *
 * The loader hands the kernel the ARC name of the boot device (for us the
 * ordinary multi(0)disk(0)rdisk(N)partition(M)), and until the boot volume
 * has been mounted \SystemRoot is the symlink \ArcName\<ARC name>\<NtBootPath>.
 * The kernel makes \ArcName\<ARC name> point at \Device\HarddiskN\PartitionM.
 * Afterwards it rewrites \SystemRoot to a plain NT device name, so we have to
 * follow the links once, at driver start.
 */
static boolean_t rp_boot_valid = B_FALSE;
static ULONG rp_boot_disk = 0;
static ULONG rp_boot_part = 0;

extern uint64_t zfs_boot_import;

/* Read a run of decimal digits at *sp, and step over them */
static boolean_t
rp_parse_ulong(const WCHAR **sp, const WCHAR *end, ULONG *out)
{
	const WCHAR *s = *sp;
	ULONG v = 0;
	unsigned int ndigits = 0;

	while (s < end && *s >= L'0' && *s <= L'9') {
		if (++ndigits > 9)
			return (B_FALSE);
		v = v * 10 + (*s - L'0');
		s++;
	}

	if (ndigits == 0)
		return (B_FALSE);

	*sp = s;
	*out = v;
	return (B_TRUE);
}

/* "\Device\Harddisk<d>\Partition<p>", optionally followed by "\..." */
static boolean_t
rp_parse_partition_path(PUNICODE_STRING path, ULONG *disk, ULONG *part)
{
	UNICODE_STRING p1 = RTL_CONSTANT_STRING(L"\\Device\\Harddisk");
	UNICODE_STRING p2 = RTL_CONSTANT_STRING(L"\\Partition");
	UNICODE_STRING rest;
	const WCHAR *s, *end;

	if (!RtlPrefixUnicodeString(&p1, path, TRUE))
		return (B_FALSE);

	s = path->Buffer + p1.Length / sizeof (WCHAR);
	end = path->Buffer + path->Length / sizeof (WCHAR);

	if (!rp_parse_ulong(&s, end, disk))
		return (B_FALSE);

	rest.Buffer = (PWCH)s;
	rest.Length = rest.MaximumLength = (USHORT)((end - s) * sizeof (WCHAR));
	if (!RtlPrefixUnicodeString(&p2, &rest, TRUE))
		return (B_FALSE);

	s += p2.Length / sizeof (WCHAR);
	if (!rp_parse_ulong(&s, end, part))
		return (B_FALSE);

	return (s == end || *s == L'\\');
}

/* "\ArcName\<name>\..." -> the link "\ArcName\<name>" */
static boolean_t
rp_arc_link(PUNICODE_STRING path, PUNICODE_STRING link)
{
	UNICODE_STRING prefix = RTL_CONSTANT_STRING(L"\\ArcName\\");
	size_t n = path->Length / sizeof (WCHAR);
	size_t plen = prefix.Length / sizeof (WCHAR);
	size_t i;

	if (!RtlPrefixUnicodeString(&prefix, path, TRUE) || n <= plen)
		return (B_FALSE);

	for (i = plen; i < n && path->Buffer[i] != L'\\'; i++)
		;

	link->Buffer = path->Buffer;
	link->Length = link->MaximumLength = (USHORT)(i * sizeof (WCHAR));
	return (B_TRUE);
}

static NTSTATUS
rp_query_link(PUNICODE_STRING name, PUNICODE_STRING target)
{
	OBJECT_ATTRIBUTES oa;
	HANDLE h;
	ULONG retlen = 0;
	NTSTATUS status;

	InitializeObjectAttributes(&oa, name,
	    OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

	status = ZwOpenSymbolicLinkObject(&h, GENERIC_READ, &oa);
	if (!NT_SUCCESS(status))
		return (status);

	target->Length = 0;
	status = ZwQuerySymbolicLinkObject(h, target, &retlen);
	ZwClose(h);

	return (status);
}

/* Call once, from DriverEntry, before any boot volume is mounted. */
void
zfs_rootpool_boot_init(void)
{
	UNICODE_STRING sysroot = RTL_CONSTANT_STRING(L"\\SystemRoot");
	UNICODE_STRING t1, t2, link;
	WCHAR b1[256], b2[256];

	t1.Buffer = b1;
	t1.Length = 0;
	t1.MaximumLength = sizeof (b1);
	t2.Buffer = b2;
	t2.Length = 0;
	t2.MaximumLength = sizeof (b2);

	if (!NT_SUCCESS(rp_query_link(&sysroot, &t1)))
		return;
	dprintf("%s: SystemRoot is %wZ\n", __func__, &t1);

	/* Not ARC based: the boot volume is already mounted, or not ours */
	if (!rp_arc_link(&t1, &link))
		return;

	if (!NT_SUCCESS(rp_query_link(&link, &t2)))
		return;
	dprintf("%s: %wZ is %wZ\n", __func__, &link, &t2);

	if (!rp_parse_partition_path(&t2, &rp_boot_disk, &rp_boot_part))
		return;

	rp_boot_valid = B_TRUE;
	cmn_err(CE_NOTE, "%s: booted from Harddisk%lu Partition%lu", __func__,
	    rp_boot_disk, rp_boot_part);
}

/*
 * Given the partition device the kernel wants mounted, import the root
 * pool held on it and return the name of its bootfs dataset in dsname.
 *
 * Returns ENOENT if the device carries no ZFS label (not ours), so the
 * caller can decline the mount; any other error means it is ZFS but the
 * import failed.
 */
extern PDEVICE_OBJECT zfs_boot_devobj;
extern char zfs_boot_path[64];
extern uint64_t zfs_boot_size;

int
zfs_rootpool_import(PDEVICE_OBJECT dev, char *dsname, size_t dslen)
{
	STORAGE_DEVICE_NUMBER sdn = { 0 };
	PARTITION_INFORMATION_EX pix = { 0 };
	GET_LENGTH_INFORMATION gli = { 0 };
	nvlist_t *label = NULL, *config = NULL;
	spa_t *spa;
	uint64_t psize = 0;
	uint64_t bootfs;
	char path[64];
	char poolname[ZFS_MAX_DATASET_NAME_LEN];
	int error;

	/* dsl_dsobj_to_dsname() writes up to a full dataset name */
	if (dslen < ZFS_MAX_DATASET_NAME_LEN)
		return (EINVAL);

	if (!NT_SUCCESS(rp_ioctl(dev, IOCTL_STORAGE_GET_DEVICE_NUMBER,
	    &sdn, sizeof (sdn))))
		return (ENOENT);

	/*
	 * boot_import=2: only the partition the loader booted from. Anything
	 * else is just a data disk, so decline the mount without even reading
	 * its labels, and let the normal import path deal with it.
	 */
	if (zfs_boot_import >= 2 &&
	    (!rp_boot_valid || sdn.DeviceNumber != rp_boot_disk ||
	    sdn.PartitionNumber != rp_boot_part))
		return (ENOENT);

	if (NT_SUCCESS(rp_ioctl(dev, IOCTL_DISK_GET_PARTITION_INFO_EX,
	    &pix, sizeof (pix))))
		psize = pix.PartitionLength.QuadPart;
	if (psize == 0 && NT_SUCCESS(rp_ioctl(dev, IOCTL_DISK_GET_LENGTH_INFO,
	    &gli, sizeof (gli))))
		psize = gli.Length.QuadPart;
	if (psize == 0)
		return (ENOENT);

	if ((error = rp_read_label(dev, psize, &label)) != 0)
		return (error);

	/*
	 * Open through the partition device itself, not the volume: by the
	 * time the pool is loaded the volume will be ours.
	 */
	snprintf(path, sizeof (path), "\\Device\\Harddisk%lu\\Partition%lu",
	    sdn.DeviceNumber, sdn.PartitionNumber);

	config = rp_generate_rootconf(label, path);
	nvlist_free(label);
	if (config == NULL)
		return (ENOTSUP);

	error = rp_register_rootpool(config, poolname, sizeof (poolname));
	fnvlist_free(config);
	if (error != 0)
		return (error);

	/*
	 * We are inside the mount IRP for this partition, so vdev_disk_open
	 * must not open it by name (mount lock); hand it the device.
	 */
	strlcpy(zfs_boot_path, path, sizeof (zfs_boot_path));
	zfs_boot_size = psize;
	zfs_boot_devobj = dev;

	/* Load the pool and find out which dataset is the boot one */
	error = spa_open(poolname, &spa, FTAG);
	zfs_boot_devobj = NULL;
	if (error != 0) {
		cmn_err(CE_NOTE, "%s: spa_open('%s') failed %d", __func__,
		    poolname, error);
		return (error);
	}

	bootfs = spa_bootfs(spa);
	if (bootfs == 0) {
		cmn_err(CE_NOTE, "%s: pool '%s' has no bootfs property set",
		    __func__, poolname);
		error = ENOENT;
	} else {
		/* Takes its own dsl_pool_hold() */
		error = dsl_dsobj_to_dsname(poolname, bootfs, dsname);
	}
	spa_close(spa, FTAG);

	return (error);
}

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
/*
 * Copyright (c) 2017 Jorgen Lundman <lundman@lundman.net>
 * Portions Copyright 2022 Andrew Innes <andrew.c12@gmail.com>
 */

#undef _NTDDK_
#include <ntifs.h>
#include <ntddk.h>
#include <ntddscsi.h>
#include <scsi.h>
#include <ntddcdrm.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntintsafe.h>
#include <mountmgr.h>
#include <Mountdev.h>
#include <ntddvol.h>
#include <os/windows/zfs/sys/zfs_ioctl_compat.h>
#include <sys/fs/zfsdi.h>
#include <sys/driver_extension.h>

// I have no idea what black magic is needed to get ntifs.h to define these

#ifndef FsRtlEnterFileSystem
#define	FsRtlEnterFileSystem() { \
	KeEnterCriticalRegion();     \
}
#endif
#ifndef FsRtlExitFileSystem
#define	FsRtlExitFileSystem() { \
    KeLeaveCriticalRegion();     \
}
#endif

#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ioctl_compat.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_vnops_os.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_ctldir.h>

#include <sys/callb.h>
#include <sys/unistd.h>
#include <sys/zfs_windows.h>
#include <sys/kstat.h>

#ifdef DEBUG_IOCOUNT
static kmutex_t GIANT_SERIAL_LOCK;
#endif

#ifdef _KERNEL

#ifndef STATUS_VOLUME_NOT_MOUNTED
#define	STATUS_VOLUME_NOT_MOUNTED 0xC000001A
#endif

DRIVER_INITIALIZE DriverEntry;

unsigned int debug_vnop_osx_printf = 0;
unsigned int zfs_vnop_ignore_negatives = 0;
unsigned int zfs_vnop_ignore_positives = 0;
unsigned int zfs_vnop_create_negatives = 1;

#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

#define	DECLARE_CRED(ap) \
	cred_t *cr;
#define	DECLARE_CONTEXT(ap) \
	caller_context_t *ct
#define	DECLARE_CRED_AND_CONTEXT(ap)	\
	DECLARE_CRED(ap);		\
	DECLARE_CONTEXT(ap)


#ifdef _KERNEL
uint64_t vnop_num_reclaims = 0;
uint64_t vnop_num_vnodes = 0;
uint64_t zfs_disable_wincache = 0;

ZFS_MODULE_RAW(zfs, disable_wincache, zfs_disable_wincache,
    U64, ZMOD_RW, 0, "Disable OS caching.");
#endif

extern void UnlockAndFreeMdl(PMDL);

BOOLEAN
zfs_AcquireForLazyWrite(void *Context, BOOLEAN Wait)
{
	FILE_OBJECT *fo = Context;
	BOOLEAN result = FALSE;

	dprintf("%s:fo %p\n", __func__, fo);

	if (fo == NULL)
		return (FALSE);

	mount_t *zmo = fo->DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	struct vnode *vp = fo->FsContext;

	if (unlikely(zfsvfs == NULL)) {
		dprintf("%s: fo %p already freed zfsvfs\n", __func__, fo);
		return (FALSE);
	}

	/* Confirm we are mounted, and stop unmounting */
	if (vfs_busy(zfsvfs->z_vfs, 0) != 0)
		return (FALSE);

	if (zfsvfs->z_unmounted ||
	    zfs_enter(zfsvfs, FTAG) != 0) {
		vfs_unbusy(zfsvfs->z_vfs);
		return (FALSE);
	}

	vfs_unbusy(zfsvfs->z_vfs);

	if (vp == NULL ||
	    VTOZ(vp) == NULL ||
	    VN_HOLD(vp) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (FALSE);
	}
	zfs_exit(zfsvfs, FTAG);

	if (!ExAcquireResourceExclusiveLite(
	    vp->FileHeader.Resource, Wait)) {
		dprintf("Failed\n");
		goto out;
	}

	vnode_ref(vp);
	result = TRUE;
	IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

out:
	VN_RELE(vp);

	return (result);
}

void
zfs_ReleaseFromLazyWrite(void *Context)
{
	FILE_OBJECT *fo = Context;

	dprintf("%s:\n", __func__);
	if (fo == NULL)
		return;

	mount_t *zmo = fo->DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	struct vnode *vp = fo->FsContext;

	if (vp != NULL && VN_HOLD(vp) == 0) {
		ExReleaseResourceLite(vp->FileHeader.Resource);
		vnode_rele(vp);
		VN_RELE(vp);
		if (IoGetTopLevelIrp() ==
		    (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP)
			IoSetTopLevelIrp(NULL);

		return;
	}
	dprintf("%s WARNING FAILED\n", __func__);
}

BOOLEAN
zfs_AcquireForReadAhead(void *Context, BOOLEAN Wait)
{
	FILE_OBJECT *fo = Context;
	BOOLEAN result = FALSE;

	dprintf("%s:\n", __func__);

	if (fo == NULL)
		return (FALSE);

	mount_t *zmo = fo->DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	struct vnode *vp = fo->FsContext;

	if (unlikely(zfsvfs == NULL)) {
		dprintf("%s: fo %p already freed zfsvfs\n", __func__, fo);
		return (FALSE);
	}

	if (vfs_busy(zfsvfs->z_vfs, 0) != 0)
		return (FALSE);

	if (zfsvfs->z_unmounted ||
	    zfs_enter(zfsvfs, FTAG) != 0) {
		vfs_unbusy(zfsvfs->z_vfs);
		return (FALSE);
	}

	vfs_unbusy(zfsvfs->z_vfs);

	if (vp == NULL ||
	    VTOZ(vp) == NULL ||
	    VN_HOLD(vp) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (FALSE);
	}
	zfs_exit(zfsvfs, FTAG);

	if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource,
	    Wait)) {
		dprintf("Failed\n");
		goto out;
	}

	vnode_ref(vp);
	IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
	result = TRUE;

out:
	VN_RELE(vp);

	return (result);
}

void
zfs_ReleaseFromReadAhead(void *Context)
{
	FILE_OBJECT *fo = Context;

	dprintf("%s:\n", __func__);
	if (fo == NULL)
		return;

	mount_t *zmo = fo->DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);

	struct vnode *vp = fo->FsContext;

	if (vp != NULL && VN_HOLD(vp) == 0) {
		ExReleaseResourceLite(vp->FileHeader.Resource);
		vnode_rele(vp);
		VN_RELE(vp);
		if (IoGetTopLevelIrp() ==
		    (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP)
			IoSetTopLevelIrp(NULL);

		return;
	}
	dprintf("%s WARNING FAILED\n", __func__);
}

CACHE_MANAGER_CALLBACKS CacheManagerCallbacks =
{
	.AcquireForLazyWrite = zfs_AcquireForLazyWrite,
	.ReleaseFromLazyWrite = zfs_ReleaseFromLazyWrite,
	.AcquireForReadAhead = zfs_AcquireForReadAhead,
	.ReleaseFromReadAhead = zfs_ReleaseFromReadAhead
};

int
zfs_init_cache(FILE_OBJECT *fo, struct vnode *vp, CC_FILE_SIZES *ccfs)
{
	zfs_ccb_t *zccb = fo->FsContext2;

	try {
		if (fo->PrivateCacheMap == NULL) {

			VERIFY3U(zccb->cacheinit, ==, 0);
			atomic_inc_64(&zccb->cacheinit);

			CcInitializeCacheMap(fo,
			    ccfs,
			    FALSE,
			    &CacheManagerCallbacks, fo);
			dprintf("CcInitializeCacheMap called on vp %p\n", vp);
			// CcSetAdditionalCacheAttributes(fo, FALSE, FALSE);
			// must be FALSE (Disk IO only)
			// CcSetReadAheadGranularity(fo, READ_AHEAD_GRAN);
			// fo->Flags |= FO_CACHE_SUPPORTED;
			dprintf("%s: CcInitializeCacheMap\n", __func__);
		}
	} except(EXCEPTION_EXECUTE_HANDLER) {
		return (GetExceptionCode());
	}

	return (0);
}


/*
 * zfs vfs operations.
 */

/*
 * FileObject->FsContext will point to vnode, many FileObjects can point
 * to same vnode.
 * FileObject->FsContext2 will point to own "zfs_ccb_t" and be unique
 * to each FileObject.
 * - which could also be done with TSD data, but this appears to be
 * the Windows norm.
 */
static void
zfs_couplefileobject(vnode_t *vp, vnode_t *dvp, FILE_OBJECT *fileobject,
    uint64_t size, zfs_ccb_t **ccb, uint64_t alloc, ACCESS_MASK access,
    char *stream)
{
	zfs_ccb_t *zccb;

	if (fileobject->FsContext2 == NULL) {
		zccb = kmem_zalloc(sizeof (zfs_ccb_t), KM_SLEEP);
		zccb->magic = ZFS_CCB_MAGIC;
		fileobject->FsContext2 = zccb;
	} else {
		zccb = fileobject->FsContext2;
	}
	zccb->access = access;

	if (ccb != NULL)
		*ccb = zccb;

	vnode_ref(vp);
	vnode_couplefileobject(vp, fileobject, size);

	if (dvp)
		vnode_setparent(vp, dvp);

	uint64_t s = 0ULL;
	uint64_t a = 0ULL;
	if (VTOZ(vp) != NULL) {
		znode_t *zp = VTOZ(vp);
		s = zp->z_size;
		a = P2ROUNDUP(zp->z_size, zp->z_blksz);
	}

	vp->FileHeader.AllocationSize.QuadPart = alloc ? alloc : a;
	vp->FileHeader.FileSize.QuadPart = s;
	vp->FileHeader.ValidDataLength.QuadPart = s;

#ifdef ZFS_HAVE_FASTIO
	vp->FileHeader.IsFastIoPossible = fast_io_possible(vp);
#endif

	zfs_build_path_stream(VTOZ(vp), dvp ? VTOZ(dvp) : NULL,
	    &zccb->z_name_cache,
	    &zccb->z_name_len,
	    &zccb->z_name_offset,
	    stream);

}

static void
zfs_decouplefileobject(vnode_t *vp, FILE_OBJECT *fileobject,
    boolean_t SkipCache)
{
	// We release FsContext2 at CLEANUP, but fastfat releases it in
	// CLOSE. Does this matter?
	zfs_ccb_t *zccb = fileobject->FsContext2;

	if (zccb != NULL) {

		if (zccb->searchname.Buffer != NULL) {
			kmem_free(zccb->searchname.Buffer,
			    zccb->searchname.MaximumLength);
			zccb->searchname.Buffer = NULL;
			zccb->searchname.MaximumLength = 0;
		}

		if (zccb->z_name_cache != NULL)
			kmem_free(zccb->z_name_cache, zccb->z_name_len);
		zccb->z_name_cache = NULL;
		zccb->z_name_len = 0;
		kmem_free(zccb, sizeof (zfs_ccb_t));
		fileobject->FsContext2 = NULL;
	}

	if (!SkipCache)
		CcUninitializeCacheMap(fileobject, NULL, NULL);
	vnode_decouplefileobject(vp, fileobject);

}

static void
allocate_reparse(struct vnode *vp, char *finalname, PIRP Irp)
{
	znode_t *zp;
	REPARSE_DATA_BUFFER *rpb;
	size_t size;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PFILE_OBJECT FileObject = IrpSp->FileObject;

	zp = VTOZ(vp);
	// fix me, direct vp access
	size = zfsctl_is_node(zp) ? vp->v_reparse_size :
	    zp->z_size;
	rpb = ExAllocatePoolWithTag(PagedPool,
	    size, '!FSZ');
	get_reparse_point_impl(zp, (char *)rpb, size, NULL);

	/*
	 * Length, in bytes, of the unparsed portion of the
	 * file name pointed to by the FileName member of the
	 * associated file object.
	 * Should include the leading "/", when finalname
	 * here would be "lower".
	 */
	ULONG len = 0;
	if (finalname && *finalname) {
		RtlUTF8ToUnicodeN(NULL, 0, &len,
		    finalname, strlen(finalname));
		len += sizeof (WCHAR);
	}
	rpb->Reserved = len;

	dprintf("%s: returning REPARSE (remainder %d)\n",
	    __func__, rpb->Reserved);
	Irp->IoStatus.Information = rpb->ReparseTag;
	Irp->Tail.Overlay.AuxiliaryBuffer = (void *)rpb;

#if 0
	/* Unknown why, but btrfs does this */
	if (FileObject) {
		UNICODE_STRING *fn = &FileObject->FileName;
		if (fn->Buffer[(fn->Length / sizeof (WCHAR)) - 1] == '\\')
			rpb->Reserved = sizeof (WCHAR);
	}
#endif
}

void
check_and_set_stream_parent(char *stream_name, PFILE_OBJECT FileObject,
    uint64_t id)
{
	if (stream_name != NULL && FileObject != NULL &&
	    FileObject->FsContext2 != NULL) {
		zfs_ccb_t *zccb = FileObject->FsContext2;

		zccb->real_file_id = id;

		if (FileObject->FsContext != NULL) {
			struct vnode *vp = FileObject->FsContext;
			znode_t *dzp;
			znode_t *zp = VTOZ(vp);
			if (zp != NULL && zp->z_zfsvfs != NULL) {
				// Fetch gparent (one above xattr dir)
				int error = zfs_zget(zp->z_zfsvfs, id, &dzp);
				if (!error) {
					zfs_build_path_stream(zp, dzp,
					    &zccb->z_name_cache,
					    &zccb->z_name_len,
					    &zccb->z_name_offset, stream_name);
					zrele(dzp);
				}
			}
		}
	}
}

/*
 * Take filename, look for colons ":".
 * No colon, return OK.
 * if ends with "::$DATA". Terminate on colon, return OK (regular file open).
 * if ends with anything not ":$DATA", return error.
 * (we don't handle other types)
 * if colon, parse name up until next colon. Assign colonname to
 * point to stream name.
 */
int
stream_parse(char *filename, char **streamname)
{
	char *colon, *second;

	// Just a filename, no streams.
	colon = strchr(filename, ':');
	if (colon == NULL)
		return (0);

	// Regular file, with "::$DATA" end?
	if (strcasecmp(colon, "::$DATA") == 0) {
		*colon = 0; // Terminate before colon
		return (0);
	}

	// Look for second colon
	second = strchr(&colon[1], ':');

	// No second colon, just stream name. Validity check?
	if (second == NULL) {
		*streamname = &colon[1];
		*colon = 0; // Cut off streamname from filename

		// We now ADD ":$DATA" to the stream name.
		strcat(*streamname, ":$DATA");

		goto checkname;
	}

	// Have second colon, better be ":$DATA".
	if (strcasecmp(second, ":$DATA") == 0) {

		// Terminate at second colon, set streamname
		// We now keep the ":$DATA" extension in the xattr name
		// *second = 0;

		*streamname = &colon[1];
		*colon = 0; // Cut of streamname from filename

		goto checkname;
	}

	// Not $DATA
	dprintf("%s: Not handling StreamType '%s'\n", __func__, second);
	return (EINVAL);

checkname:
	if (strlen(*streamname) >= 512)
		return (STATUS_OBJECT_NAME_INVALID);

	if (strchr(*streamname, '/') ||
	    /* strchr(&colon[2], ':') || there is one at ":$DATA" */
	    !strcasecmp("DOSATTRIB:$DATA", *streamname) ||
	    !strcasecmp("EA:$DATA", *streamname) ||
	    !strcasecmp("reparse:$DATA", *streamname) ||
	    !strcasecmp("casesensitive:$DATA", *streamname))
		return (STATUS_OBJECT_NAME_INVALID);

	return (0);
}

/*
 * Attempt to parse 'filename', descending into filesystem.
 * If start "dvp" is passed in, it is expected to have a HOLD
 * If successful, function will return with:
 * - HOLD on dvp
 * - HOLD on vp
 * - final parsed filename part in 'lastname' (in the case of creating an entry)
 *
 * IRP_MJ_CREATE calls
 *
 * zfsvfs  Filename
 * --------------------------------------------------------
 * IRP_MJ_CREATE(pool, "/lower/today.txt")
 *     : lookup "lower", return STATUS_REPARSE
 *     : Set unparsed length rdp->Reserved = 10 ("/today.txt") * 2
 * --------------------------------------------------------
 * IRP_MJ_CREATE(lower, "/today.txt")
 *     : lookup "today.txt", return SUCCESS
 */

int
zfs_find_dvp_vp(zfsvfs_t *zfsvfs, char *filename, int finalpartmaynotexist,
    int finalpartmustnotexist, char **lastname, struct vnode **dvpp,
    struct vnode **vpp, int flags, ULONG options)
{
	int error = ENOENT;
	znode_t *zp;
	struct vnode *dvp = NULL;
	struct vnode *vp = NULL;
	char *word = NULL;
	char *brkt = NULL;
	struct componentname cn;
	int fullstrlen;
	char namebuffer[MAXNAMELEN];
	BOOLEAN FileOpenReparsePoint;

	FileOpenReparsePoint =
	    BooleanFlagOn(options, FILE_OPEN_REPARSE_POINT);

	// Iterate from dvp if given, otherwise root
	dvp = *dvpp;

	if (dvp == NULL) {
		// Grab a HOLD
		error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);
		if (error != 0)
			return (ESRCH);  // No such dir
		dvp = ZTOV(zp);
	} else {
		// Passed in dvp is already HELD, but grab one now
		// since we release dirs as we descend
		dprintf("%s: passed in dvp\n", __func__);
		if (VN_HOLD(dvp) != 0)
			return (ESRCH);
	}

	fullstrlen = strlen(filename);

	// Sometimes we are given a path like "\Directory\directory\"
	// with the final separator, we want to eat that final character.
	if ((fullstrlen > 2) &&
	    (filename[fullstrlen - 1] == '\\'))
		filename[--fullstrlen] = 0;

	for (word = strtok_r(filename, "/\\", &brkt);
	    word;
	    word = strtok_r(NULL, "/\\", &brkt)) {
		int direntflags = 0;
		// dprintf("..'%s'..", word);

		// If a component part name is too long
		if (strlen(word) > MAXNAMELEN - 1) {
			VN_RELE(dvp);
			return (STATUS_OBJECT_NAME_INVALID);
		}
		strlcpy(namebuffer, word, sizeof (namebuffer));
		// Dont forget zfs_lookup() modifies
		// "cn" here, so size needs to be max, if
		// formD is in effect.
		cn.cn_nameiop = LOOKUP;
		cn.cn_flags = ISLASTCN;
		cn.cn_namelen = strlen(namebuffer);
		cn.cn_nameptr = namebuffer;
		cn.cn_pnlen = MAXNAMELEN;
		cn.cn_pnbuf = namebuffer;

		error = zfs_lookup(VTOZ(dvp), namebuffer,
		    &zp, flags, NULL, &direntflags, &cn);

		// If snapshot dir and we are pretending it is deleted...
		if (error == 0 && zp->z_vnode != NULL && ZTOV(zp)->v_unlink) {
			VN_RELE(ZTOV(zp));
			error = ENOENT;
		}
		if (error != 0) {
			// If we are creating a file, or looking up parent,
			// allow it not to exist
			if (finalpartmaynotexist)
				break;
			dprintf("failing out here\n");
			// since we weren't successful, release dvp here
			VN_RELE(dvp);
			dvp = NULL;
			break;
		}

		// If last lookup hit a non-directory type, we stop
		vp = ZTOV(zp);
		ASSERT(zp != NULL);

		/*
		 * If we come across a REPARSE, we stop processing here
		 * and pass the "zp" back for caller to do more processing,
		 * which might include returning "zp" (FILE_OPEN_REPARSE_POINT)
		 * and ReparseTag.
		 * If they requested FileOpenReparsePoint, AND we are at the
		 * final-part, we open it normally.
		 * Other cases we need to ask for redriving the query
		 */

		if (zp->z_pflags & ZFS_REPARSE) {
			/*
			 * Indicate if reparse was final part,
			 * caller will handle this case
			 */
			if (lastname)
				*lastname = brkt;

			if (dvpp != NULL)
				*dvpp = dvp;
			if (vpp != NULL)
				*vpp = vp;

			return (STATUS_REPARSE);
		}

		if (vp->v_type == VDIR) {
			// Not reparse
			VN_RELE(dvp);
			dvp = vp;
			vp = NULL;
		} else {
			// If we aren't the final component, descending dirs,
			// and it's a file?
			if (brkt != NULL && *brkt != 0) {
				dprintf("%s: not a DIR triggered '%s'\n",
				    __func__, word);
				if (vp)
					VN_RELE(vp);
				VN_RELE(dvp);
				return (ENOTDIR);
			}
			break;
		} // is dir or not

	} // for word
	// dprintf("\n");

	if (dvp != NULL) {
		// We return with dvp HELD
		// VN_RELE(dvp);
	} else {
		dprintf("%s: failed to find dvp for '%s' word '%s' err %d\n",
		    __func__, filename, word?word:"(null)", error);
		return (error);
	}

	if (error != 0 && !vp && !finalpartmaynotexist) {
		VN_RELE(dvp);
		return (ENOENT);
	}

	/* finalpartmustnotexist and we got a vp? */
	if (!word && finalpartmustnotexist && dvp && vp) {
		dprintf("CREATE with existing dir exit?\n");

		VN_RELE(vp);
		VN_RELE(dvp);

		if (zp && !S_ISDIR(zp->z_mode))
			return (ENOTDIR);
		return (EEXIST);
	}

	// If finalpartmaynotexist is TRUE, make sure we are looking at
	// the finalpart, and not in the middle of descending
	if (finalpartmaynotexist && brkt != NULL && *brkt != 0) {
		dprintf("finalpartmaynotexist, but not at finalpart: %s\n",
		    brkt);
		VN_RELE(dvp);
		return (ESRCH);
	}

	if (lastname) {

		*lastname = word /* ? word : filename */;

		// Skip any leading "\"
		while (*lastname != NULL &&
		    (**lastname == '\\' || **lastname == '/'))
			(*lastname)++;

	}

	if (dvpp != NULL)
		*dvpp = dvp;
	if (vpp != NULL)
		*vpp = vp;

	return (0);
}

/*
 * In POSIX, the vnop_lookup() would return with iocount still held
 * for the caller to issue VN_RELE() on when done.
 * The above zfs_find_dvp_vp() behaves a little like that, in that
 * if a successful "vp" is returned, it has a iocount lock, and
 * is released here when finished.
 * zfs_vnop_lookup serves as the bridge between Windows and Unix
 * and will assign FileObject->FsContext as appropriate, with usecount set
 * when required, but it will not hold iocount.
 */
int
zfs_vnop_lookup_impl(PIRP Irp, PIO_STACK_LOCATION IrpSp, mount_t *zmo,
    char *filename, xvattr_t *xvap)
{
	int error;
	cred_t *cr = NULL;
	char *finalname = NULL;
	PFILE_OBJECT FileObject;
	ULONG outlen;
	struct vnode *dvp = NULL;
	struct vnode *vp = NULL;
	znode_t *zp = NULL;
	znode_t *dzp = NULL;
	ULONG Options;
	BOOLEAN CreateDirectory;
	BOOLEAN NoIntermediateBuffering;
	BOOLEAN OpenDirectory;
	BOOLEAN IsPagingFile;
	BOOLEAN OpenTargetDirectory;
	BOOLEAN DirectoryFile;
	BOOLEAN NonDirectoryFile;
	BOOLEAN NoEaKnowledge;
	BOOLEAN DeleteOnClose;
	BOOLEAN OpenRequiringOplock;
	BOOLEAN TemporaryFile;
	BOOLEAN OpenRoot;
	BOOLEAN CreateFile;
	BOOLEAN FileOpenByFileId;
	BOOLEAN FileOpenReparsePoint;
	ULONG CreateDisposition;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	int flags = 0;
	int dvp_no_rele = 0;
	char *stream_name = NULL;
	boolean_t UndoShareAccess = FALSE;
	NTSTATUS Status = STATUS_SUCCESS;
	ACCESS_MASK granted_access = 0;
	ACCESS_MASK DesiredAccess =
	    IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
	zfs_ccb_t *zccb = NULL;
	vattr_t *vap = &xvap->xva_vattr;

	if (zfsvfs == NULL)
		return (STATUS_OBJECT_PATH_NOT_FOUND);

	if (vfs_isunmount(zmo))
		return (STATUS_DEVICE_NOT_READY);

	FileObject = IrpSp->FileObject;
	Options = IrpSp->Parameters.Create.Options;

	dprintf("%s: enter\n", __func__);

	if (FileObject->RelatedFileObject != NULL) {
		FileObject->Vpb = FileObject->RelatedFileObject->Vpb;
		//  A relative open must be via a relative path.
		if (FileObject->FileName.Length != 0 &&
		    FileObject->FileName.Buffer[0] == L'\\') {
			return (STATUS_INVALID_PARAMETER);
		}
	} else {
		FileObject->Vpb = zmo->vpb;
	}

	DirectoryFile =
	    BooleanFlagOn(Options, FILE_DIRECTORY_FILE);
	NonDirectoryFile =
	    BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE);
	NoIntermediateBuffering =
	    BooleanFlagOn(Options, FILE_NO_INTERMEDIATE_BUFFERING);
	NoEaKnowledge =
	    BooleanFlagOn(Options, FILE_NO_EA_KNOWLEDGE);
	DeleteOnClose =
	    BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE);
	FileOpenByFileId =
	    BooleanFlagOn(Options, FILE_OPEN_BY_FILE_ID);
	FileOpenReparsePoint =
	    BooleanFlagOn(Options, FILE_OPEN_REPARSE_POINT);


	// Should be passed an 8/16 byte FileId instead.
	if (FileOpenByFileId) {
		if (FileObject->FileName.Length !=
		    sizeof (ULONGLONG) &&
		    FileObject->FileName.Length !=
		    sizeof (FILE_ID_128))
			return (STATUS_INVALID_PARAMETER);
	}

	TemporaryFile = BooleanFlagOn(IrpSp->Parameters.Create.FileAttributes,
	    FILE_ATTRIBUTE_TEMPORARY);

	CreateDisposition = (Options >> 24) & 0x000000ff;

	IsPagingFile = BooleanFlagOn(IrpSp->Flags, SL_OPEN_PAGING_FILE);
	// ASSERT(!IsPagingFile);
	// ASSERT(!OpenRequiringOplock);
	// Open the directory instead of the file
	OpenTargetDirectory = BooleanFlagOn(IrpSp->Flags,
	    SL_OPEN_TARGET_DIRECTORY);
/*
 *	CreateDisposition value	Action if file exists
 *	Action if file does not exist  UNIX Perms
 *		FILE_SUPERSEDE		Replace the file.
 *		    Create the file.        *        Unlink + O_CREAT | O_TRUNC
 *		FILE_CREATE		    Return an error.
 *		    Create the file.        *        O_CREAT | O_EXCL
 *		FILE_OPEN		    Open the file.
 *		        Return an error.        *        0
 *		FILE_OPEN_IF		Open the file.
 *		        Create the file.        *        O_CREAT
 *		FILE_OVERWRITE		Open the file, overwrite it.
 *	Return an error.    *        O_TRUNC
 *		FILE_OVERWRITE_IF	Open the file, overwrite it.
 *	Create the file.    *        O_CREAT | O_TRUNC
 *
 *	Apparently SUPERSEDE is more or less Unlink entry before recreate,
 * so it loses ACLs, XATTRs and NamedStreams.
 *
 *		IoStatus return codes:
 *		FILE_CREATED
 *		FILE_OPENED
 *		FILE_OVERWRITTEN
 *		FILE_SUPERSEDED
 *		FILE_EXISTS
 *		FILE_DOES_NOT_EXIST
 *
 */

	// Dir create/open is straight forward, do that here
	// Files are harder, do that once we know if it exists.
	CreateDirectory = (BOOLEAN)(DirectoryFile &&
	    ((CreateDisposition == FILE_CREATE) ||
	    (CreateDisposition == FILE_OPEN_IF)));

	OpenDirectory = (BOOLEAN)(DirectoryFile &&
	    ((CreateDisposition == FILE_OPEN) ||
	    (CreateDisposition == FILE_OPEN_IF)));

	CreateFile = (BOOLEAN)(
	    ((CreateDisposition == FILE_CREATE) ||
	    (CreateDisposition == FILE_OPEN_IF) ||
	    (CreateDisposition == FILE_SUPERSEDE) ||
	    (CreateDisposition == FILE_OVERWRITE_IF)));

	// If it is a volumeopen, we just grab rootvp so that directory
	// listings work - most Options are ignored with VolumeOpens
	if (FileObject->FileName.Length == 0 &&
	    FileObject->RelatedFileObject == NULL) {

		// don't allow root to be opened on unmounted FS
		if (!(zmo->vpb->Flags & VPB_MOUNTED))
			return (STATUS_DEVICE_NOT_READY);

		dprintf("Started NULL open, returning root of mount\n");
		error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);
		if (error != 0)
			return (FILE_DOES_NOT_EXIST);  // No root dir?!

		dvp = ZTOV(zp);

		zfs_couplefileobject(dvp, NULL, FileObject, 0ULL, &zccb,
		    Irp->Overlay.AllocationSize.QuadPart, DesiredAccess,
		    stream_name);

		VN_RELE(dvp);

		Irp->IoStatus.Information = FILE_OPENED;
		return (STATUS_SUCCESS);
	}

	// No name conversion with FileID

	if (!FileOpenByFileId) {

		if (FileObject->FileName.Buffer != NULL &&
		    FileObject->FileName.Length > 0) {
			// Convert incoming filename to utf8
			error = RtlUnicodeToUTF8N(filename, PATH_MAX - 1,
			    &outlen,
			    FileObject->FileName.Buffer,
			    FileObject->FileName.Length);

			if (error != STATUS_SUCCESS &&
			    error != STATUS_SOME_NOT_MAPPED) {
				dprintf("RtlUnicodeToUTF8N returned 0x%x "
				    "input len %d\n",
				    error, FileObject->FileName.Length);
				return (STATUS_OBJECT_NAME_INVALID);
			}
			// ASSERT(error != STATUS_SOME_NOT_MAPPED);
			// Output string is only null terminated if input is,
			// so do so now.
			filename[outlen] = 0;
			dprintf("%s: converted name is '%s' input len bytes %d "
			    "(err %d) %s %s\n", __func__, filename,
			    FileObject->FileName.Length, error,
			    DeleteOnClose ? "DeleteOnClose" : "",
			    IrpSp->Flags&SL_CASE_SENSITIVE ? "CaseSensitive" :
			    "CaseInsensitive");

			if (!(IrpSp->Flags & SL_CASE_SENSITIVE) &&
			    (zfsvfs->z_case != ZFS_CASE_SENSITIVE))
				flags |= FIGNORECASE;

#if 0
			if (strcmp(
			    "\\System Volume Information\\WPSettings.dat",
			    filename) == 0)
				return (STATUS_OBJECT_NAME_INVALID);
#endif

			if (Irp->Overlay.AllocationSize.QuadPart > 0)
				dprintf("AllocationSize requested %llu\n",
				    Irp->Overlay.AllocationSize.QuadPart);

			// Check if we are called as VFS_ROOT();
			OpenRoot = (strncmp("\\", filename, PATH_MAX) == 0 ||
			    strncmp("\\*", filename, PATH_MAX) == 0);

			if (OpenRoot) {

				if (NonDirectoryFile)
					return (STATUS_FILE_IS_A_DIRECTORY);

				error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);

				if (error == 0) {
					vp = ZTOV(zp);
					zfs_couplefileobject(vp, NULL,
					    FileObject, zp->z_size, &zccb,
					    Irp->
					    Overlay.AllocationSize.QuadPart,
					    DesiredAccess,
					    stream_name);
					VN_RELE(vp);

					Irp->IoStatus.Information = FILE_OPENED;
					return (STATUS_SUCCESS);
				}

				Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
				return (STATUS_OBJECT_PATH_NOT_FOUND);
			} // OpenRoot

		} else { // If got filename

			// If no filename, we should fail,
			// unless related is set.
			if (FileObject->RelatedFileObject == NULL) {
				// Fail
				return (STATUS_OBJECT_NAME_INVALID);
			}
			// Related set, return it as opened.
			dvp = FileObject->RelatedFileObject->FsContext;
			zp = VTOZ(dvp);
			dprintf("%s: Relative null-name open\n",
			    __func__);
			// Check types
			if (NonDirectoryFile && vnode_isdir(dvp)) {
				Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
				return (STATUS_FILE_IS_A_DIRECTORY);
			}
			if (DirectoryFile && !vnode_isdir(dvp)) {
				Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
				return (STATUS_NOT_A_DIRECTORY);
			}
			// Grab vnode to ref
			if (VN_HOLD(dvp) == 0) {
				zfs_couplefileobject(dvp, NULL, FileObject,
				    0ULL, &zccb,
				    Irp->Overlay.AllocationSize.QuadPart,
				    DesiredAccess,
				    stream_name);
				VN_RELE(dvp);
			} else {
				Irp->IoStatus.Information = 0;
				return (STATUS_OBJECT_PATH_NOT_FOUND);
			}
			Irp->IoStatus.Information = FILE_OPENED;
			return (STATUS_SUCCESS);
		}

		// We have converted the filename, continue..
		if (FileObject->RelatedFileObject &&
		    FileObject->RelatedFileObject->FsContext) {
			dvp = FileObject->RelatedFileObject->FsContext;

			// This branch here, if failure, should not release dvp
			dvp_no_rele = 1;
		}

/*
 * Here, we want to check for Streams, which come in the syntax
 * filename.ext:Stream:Type
 *  Type: appears optional, or we handle ":DATA". All others will be rejected.
 *  Stream: name of the stream, we convert this into XATTR named Stream
 * It is valid to create a filename containing colons, so who knows what will
 * happen here.
 */
		error = stream_parse(filename, &stream_name);
		if (error) {
			Irp->IoStatus.Information = 0;
			return (error);
		}
		if (stream_name != NULL)
			dprintf("%s: Parsed out streamname '%s'\n",
			    __func__, stream_name);

		// There is a special case, where name is just the stream
		// ":ZoneIdentifier:$DATA", and
		// RelatedFileObject is set to the object.
		if (stream_name != NULL &&
		    FileObject->RelatedFileObject &&
		    FileObject->RelatedFileObject->FsContext &&
		    strlen(filename) == 0) {

			// The RelatedFileObject conditional above will
			// assign "dvp" - but
			// the stream_name check below will expect it in "vp".
			// dvp_no_rele is already set.
			dprintf("special case Zone.Identifier\n");
			dvp_no_rele = 1;
			vp = FileObject->RelatedFileObject->FsContext;
			zp = VTOZ(vp);
			dvp = NULL;
			VERIFY0(VN_HOLD(vp));

		} else {

			// If we have dvp, it is HELD
			error = zfs_find_dvp_vp(zfsvfs, filename,
			    (CreateFile || OpenTargetDirectory),
			    (CreateDisposition == FILE_CREATE),
			    &finalname, &dvp, &vp, flags, Options);

		}

	} else {  // Open By File ID FileOpenByFileId

		// Filename.Length is 16 (ObjectID) should we
		// verify the VolumeID matches? Lookup VolumeID?
		// or can we rely on zfsvfs being correct?
		error = zfs_zget(zfsvfs,
		    *((uint64_t *)IrpSp->FileObject->FileName.Buffer), &zp);
		// Code below assumed dvp is also , so we need to
		// open parent. We can not trust vnode_parent() here since
		// links can have different parents. Possibly speed this up
		// in future with a z_links > 1 test?
		if (error == 0) {
			uint64_t parent;
			error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
			    &parent, sizeof (parent));
			if (error == 0) {
				error = zfs_zget(zfsvfs, parent, &dzp);
			}
			vp = ZTOV(zp);
			if (error != 0) {
				VN_RELE(vp);
				dprintf("Missing parent error\n");
				return (error);
			} // failed to get parentid, or find parent
			// Copy over the vp info for below, both are held.
			// dzp/dvp held by zget()
			dvp = ZTOV(dzp);
			dprintf("getid start %d\n", vp->v_iocount);
		}
	}

	// If successful:
	// - vp is HELD
	// - dvp is HELD
	// we need dvp from here on down.

	// If asked to open reparse point instead of following it, and
	// it was the final part of the path, then just open it.
	if (error == STATUS_REPARSE && FileOpenReparsePoint &&
	    (!finalname || !*finalname))
		error = STATUS_SUCCESS;

	if (error) {

		/*
		 * With REPARSE, we are given "zp" to read the ReparseTag, and
		 * if they asked for it returned, do so, or free it.
		 */
		if (error == STATUS_REPARSE) {
			/*
			 * How reparse points work from the point of
			 * view of the filesystem appears to undocumented.
			 * When returning STATUS_REPARSE, MSDN encourages
			 * us to return IO_REPARSE in
			 * Irp->IoStatus.Information, but that means we
			 * have to do our own translation. If we instead
			 * return the reparse tag in Information, and
			 * store a pointer to the reparse data buffer in
			 * Irp->Tail.Overlay.AuxiliaryBuffer,
			 * IopSymlinkProcessReparse will do the
			 * translation for us.
			 * - maharmstone
			 */
			Irp->IoStatus.Information = 0;
			Irp->IoStatus.Status = 0;
			allocate_reparse(vp, finalname, Irp);

			// should this only work on the final component?
#if 0
			if (Options & FILE_OPEN_REPARSE_POINT) {
				// Hold open reference, until CLOSE
				error = STATUS_SUCCESS;
				zfs_couplefileobject(vp, NULL, FileObject,
				    zp ? zp->z_size : 0ULL,
				    &zccb,
				    Irp->Overlay.AllocationSize.QuadPart,
				    DesiredAccess,
				    stream_name);
			}
#endif
			VN_RELE(vp);
			if (dvp)
				VN_RELE(dvp);

			return (error); // STATUS_REPARSE
		}

		if (dvp && !dvp_no_rele) VN_RELE(dvp);
		if (vp) VN_RELE(vp);

		if (!dvp && error == ESRCH) {
			dprintf("%s: failed to find dvp for '%s' \n",
			    __func__, filename);
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			return (STATUS_OBJECT_PATH_NOT_FOUND);
		}
		if (error == STATUS_OBJECT_NAME_INVALID) {
			dprintf("%s: filename component too long\n", __func__);
			return (error);
		}
		if (error == STATUS_IO_REPARSE_TAG_NOT_HANDLED) {
			dprintf("%s: reparse but asked not to handle\n",
			    __func__);
			return (error);
		}
		// Open dir with FILE_CREATE but it exists
		if (error == EEXIST) {
			dprintf("%s: dir exists, wont create\n", __func__);
			Irp->IoStatus.Information = FILE_EXISTS;
			if (OpenTargetDirectory)
				return (STATUS_NOT_A_DIRECTORY);
			return (STATUS_FILE_IS_A_DIRECTORY); // 2
		}
		if (error == ENOTDIR) {
			dprintf("%s: file exists, wont create\n", __func__);
			Irp->IoStatus.Information = FILE_EXISTS;
			return (STATUS_OBJECT_NAME_COLLISION); // 3
		}
		// A directory component did not exist, or was a file
		if ((dvp == NULL) || (error == ENOTDIR)) {
			dprintf("%s: failed to find dvp - or dvp is a file\n",
			    __func__);
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			return (STATUS_OBJECT_NAME_NOT_FOUND);
		}
		dprintf("%s: failed to find vp in dvp\n", __func__);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return (STATUS_OBJECT_NAME_NOT_FOUND);
	}

	// Streams
	// If we opened vp, grab its xattrdir, and try to to locate stream
	if (stream_name != NULL && vp != NULL) {
		// Here, we will release dvp, and attempt to open the xattr dir.
		// xattr dir will be the new dvp. Then we will look for
		// streamname in xattrdir, and assign vp.

		VERIFY3P(dvp, !=, vp);

		// Create the xattrdir only if we are to create a new entry
		zp = VTOZ(vp);
		if ((error = zfs_get_xattrdir(zp, &dzp, cr,
		    CreateFile ? CREATE_XATTR_DIR : 0))) {
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			VN_RELE(vp);
			return (STATUS_OBJECT_NAME_NOT_FOUND);
		}
		VN_RELE(vp);
		if (dvp && !dvp_no_rele)
			VN_RELE(dvp);
		vp = NULL;
		zp = NULL;
		dvp = ZTOV(dzp);
		int direntflags = 0; // To detect ED_CASE_CONFLICT
		error = zfs_dirlook(dzp, stream_name, &zp, FIGNORECASE,
		    &direntflags, NULL);
		if (!CreateFile && error) {
			zrele(dzp);
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			return (STATUS_OBJECT_NAME_NOT_FOUND);
		}
		// Here, it may not exist, as we are to create it.
		// If it exists, keep vp, otherwise, it is NULL
		if (!error) {
			vp = ZTOV(zp);
		} // else vp is NULL from above

		finalname = stream_name;
	}

	if (OpenTargetDirectory) {
		if (dvp) {

#if 0
			// If we asked for PARENT of a non-existing file,
			// do we return error?
			if (vp == NULL) {
				dprintf("%s: opening PARENT dir, is ENOENT\n",
				    __func__);
				VN_RELE(dvp);
				Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
				return (STATUS_OBJECT_NAME_NOT_FOUND);
			}
#endif

			dprintf("%s: opening PARENT directory\n", __func__);
			zfs_couplefileobject(dvp, NULL, FileObject, 0ULL,
			    &zccb,
			    Irp->Overlay.AllocationSize.QuadPart,
			    DesiredAccess,
			    stream_name);
			if (DeleteOnClose)
				Status = zfs_setunlink_masked(FileObject, NULL);
			if (Status == STATUS_SUCCESS)
				Irp->IoStatus.Information = FILE_OPENED;

			if (vp) VN_RELE(vp);
			VN_RELE(dvp);
			return (Status);
		}
		ASSERT(vp == NULL);
		ASSERT(dvp == NULL);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return (STATUS_OBJECT_NAME_NOT_FOUND);
	}

	// Here we have "dvp" of the directory.
	// "vp" if the final part was a file.

	// Don't create if FILE_OPEN_IF (open existing)
	if ((CreateDisposition == FILE_OPEN_IF) && (vp != NULL))
		CreateDirectory = 0;

	// Fail if FILE_CREATE but target exist
	if ((CreateDisposition == FILE_CREATE) && (vp != NULL)) {
		VN_RELE(vp);
		if (dvp)
			VN_RELE(dvp);
		Irp->IoStatus.Information = FILE_EXISTS;
		if (CreateDirectory && !vnode_isdir(vp))
			return (STATUS_NOT_A_DIRECTORY);
		return (STATUS_OBJECT_NAME_COLLISION); // create file error
	}

	if (CreateDirectory && finalname) {

		if (TemporaryFile)
			return (STATUS_INVALID_PARAMETER);

		if (zfsvfs->z_rdonly || vfs_isrdonly(zfsvfs->z_vfs) ||
		    !spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
			VN_RELE(dvp);
			Irp->IoStatus.Information = 0; // ?
			return (STATUS_MEDIA_WRITE_PROTECTED);
		}

		zfs_setwinflags_xva(NULL,
		    IrpSp->Parameters.Create.FileAttributes, vap);
		vap->va_type = VDIR;
		// Set default 777 if something else wasn't passed in
		if (!(vap->va_mask & ATTR_MODE))
			vap->va_mode = 0777;
		vap->va_mode |= S_IFDIR;
		vap->va_mask |= (ATTR_MODE | ATTR_TYPE);

		/* If parent is CaseSensitive, sub-Dir should be too */
		if (VTOZ(dvp)->z_pflags & ZFS_CASESENSITIVEDIR) {
			xoptattr_t *xoap;
			xoap = xva_getxoptattr(xvap);
			xoap->xoa_case_sensitive_dir = 1;
			XVA_SET_REQ(xvap, XAT_CASESENSITIVEDIR);
		}
		ASSERT(strchr(finalname, '\\') == NULL);
		error = zfs_mkdir(VTOZ(dvp), finalname, vap, &zp, NULL,
		    flags, NULL, NULL);
		if (error == 0) {
			vp = ZTOV(zp);
			zfs_couplefileobject(vp, NULL, FileObject, 0ULL,
			    &zccb,
			    Irp->Overlay.AllocationSize.QuadPart,
			    DesiredAccess,
			    stream_name);

			if (DeleteOnClose)
				Status = zfs_setunlink_masked(FileObject, dvp);

			if (Status == STATUS_SUCCESS) {

				Irp->IoStatus.Information = FILE_CREATED;

				IoSetShareAccess(
				    DesiredAccess,
				    IrpSp->Parameters.Create.ShareAccess,
				    FileObject,
				    &vp->share_access);

#define	SC IrpSp->Parameters.Create.SecurityContext
				if (SC != NULL &&
				    SC->AccessState &&
				    SC->AccessState->
				    SecurityDescriptor != NULL) {
					merge_security(vp,
					    SC->AccessState);
				}

				zfs_send_notify(zfsvfs, zccb->z_name_cache,
				    zccb->z_name_offset,
				    FILE_NOTIFY_CHANGE_DIR_NAME,
				    FILE_ACTION_ADDED);
			}
			VN_RELE(vp);
			VN_RELE(dvp);
			return (Status);
		}
		VN_RELE(dvp);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return (STATUS_OBJECT_PATH_NOT_FOUND);
	}

	// If they requested just directory, fail non directories
	if (DirectoryFile && vp != NULL && !vnode_isdir(vp)) {
		dprintf("%s: asked for directory but found file\n", __func__);
		VN_RELE(vp);
		if (dvp)
			VN_RELE(dvp);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return (STATUS_FILE_IS_A_DIRECTORY);
	}

	// Asked for non-directory, but we got directory
	if (NonDirectoryFile && !CreateFile && vp == NULL) {
		dprintf("%s: asked for file but found directory\n", __func__);
		if (dvp)
			VN_RELE(dvp);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return (STATUS_FILE_IS_A_DIRECTORY);
	}

	if (vp) {
		zp = VTOZ(vp);
	}

	// If HIDDEN and SYSTEM are set, then the open of file must also have
	// HIDDEN and SYSTEM set.
	if ((zp != NULL) &&
	    ((CreateDisposition == FILE_SUPERSEDE) ||
	    (CreateDisposition == FILE_OVERWRITE) ||
	    (CreateDisposition == FILE_OVERWRITE_IF))) {
		if (((zp->z_pflags&ZFS_HIDDEN) &&
		    !FlagOn(IrpSp->Parameters.Create.FileAttributes,
		    FILE_ATTRIBUTE_HIDDEN)) ||
		    ((zp->z_pflags&ZFS_SYSTEM) &&
		    !FlagOn(IrpSp->Parameters.Create.FileAttributes,
		    FILE_ATTRIBUTE_SYSTEM))) {
			VN_RELE(vp);
			if (dvp)
				VN_RELE(dvp);
			dprintf("%s: denied due to hidden+system combo\n",
			    __func__);
			return (STATUS_ACCESS_DENIED);
		}
	}

	// If overwrite, and tagged readonly, fail
	// (note, supersede should succeed)
	if ((zp != NULL) &&
	    ((CreateDisposition == FILE_OVERWRITE) ||
	    (CreateDisposition == FILE_OVERWRITE_IF))) {
		if (zp->z_pflags&ZFS_READONLY) {
			VN_RELE(vp);
			VN_RELE(dvp);
			dprintf("%s: denied due to ZFS_READONLY + OVERWRITE\n",
			    __func__);
			return (STATUS_ACCESS_DENIED);
		}
	}

	// If flags are readonly, and tries to open with write, fail
	if ((zp != NULL) &&
	    (DesiredAccess&(FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
	    (zp->z_pflags&ZFS_READONLY)) {
		VN_RELE(vp);
		VN_RELE(dvp);
		dprintf("%s: denied due to ZFS_READONLY + WRITE_DATA\n",
		    __func__);
		return (STATUS_ACCESS_DENIED);
	}


	if (DeleteOnClose &&
	    vp && zp &&
	    dvp && VTOZ(dvp) &&
	    !zfsctl_is_node(VTOZ(dvp)) &&
	    zfs_zaccess_delete(VTOZ(dvp), zp, 0, NULL) > 0) {
			VN_RELE(vp);
			if (dvp)
				VN_RELE(dvp);

			dprintf("%s: denied due to IMMUTABLE+NOUNLINK\n",
			    __func__);
			return (STATUS_ACCESS_DENIED);
	}


	// Some cases we always create the file, and sometimes only if
	// it is not there. If the file exists and we are only to create
	// the file if it is not there:
	if ((CreateDisposition == FILE_OPEN_IF) && (vp != NULL))
		CreateFile = 0;


	if (vp || CreateFile == 0) {
//		NTSTATUS Status;

		// Streams do not call SeAccessCheck?
		if (stream_name != NULL && vp != NULL) {
			IoSetShareAccess(
			    DesiredAccess,
			    IrpSp->Parameters.Create.ShareAccess,
			    FileObject, vp ? &vp->share_access :
			    &dvp->share_access);

		} else if (
		    DesiredAccess != 0 && vp) {

			SeLockSubjectContext(
			    &IrpSp->Parameters.Create.SecurityContext->
			    AccessState->SubjectSecurityContext);
#if 1
			if (!FileOpenReparsePoint &&
			    !SeAccessCheck(vnode_security(vp ? vp : dvp),
			    &IrpSp->Parameters.Create.SecurityContext->
			    AccessState->SubjectSecurityContext,
			    TRUE,
			    DesiredAccess,
			    0, NULL,
			    IoGetFileObjectGenericMapping(),
			    IrpSp->Flags & SL_FORCE_ACCESS_CHECK ? UserMode :
			    Irp->RequestorMode,
			    &granted_access, &Status)) {
				SeUnlockSubjectContext(
				    &IrpSp->Parameters.Create.SecurityContext->
				    AccessState->SubjectSecurityContext);
				if (vp) VN_RELE(vp);
				VN_RELE(dvp);
				dprintf("%s: denied due to SeAccessCheck()\n",
				    __func__);
				return (Status);
			}
#endif
			SeUnlockSubjectContext(
			    &IrpSp->Parameters.Create.SecurityContext->
			    AccessState->SubjectSecurityContext);
		} else {
			granted_access = 0;
		}

		// Io*ShareAccess(): X is not an atomic operation. Therefore,
		// drivers calling this routine must protect the shared
		// file object
		vnode_lock(vp ? vp : dvp);
		if (vnode_isinuse(vp ? vp : dvp, 0)) {
// 0 is we are the only (usecount added below), 1+ if already open.
			Status = IoCheckShareAccess(granted_access,
			    IrpSp->Parameters.Create.ShareAccess, FileObject,
			    vp ? &vp->share_access : &dvp->share_access, FALSE);
			if (!NT_SUCCESS(Status)) {
				vnode_unlock(vp ? vp : dvp);
				if (vp) VN_RELE(vp);
				VN_RELE(dvp);
				dprintf("%s: denied IoCheckShareAccess\n",
				    __func__);
				return (Status);
			}
			IoUpdateShareAccess(FileObject,
			    vp ? &vp->share_access : &dvp->share_access);
		} else {
			IoSetShareAccess(granted_access,
			    IrpSp->Parameters.Create.ShareAccess,
			    FileObject,
			    vp ? &vp->share_access : &dvp->share_access);
		}
		// Since we've updated ShareAccess here, if we cancel
		// the open we need to undo it.
		UndoShareAccess = TRUE;
		vnode_unlock(vp ? vp : dvp);
	}

#define	UNDO_SHARE_ACCESS(vp) \
	if ((vp) && UndoShareAccess) {    \
		vnode_lock((vp));     \
		IoRemoveShareAccess(FileObject, &(vp)->share_access); \
		vnode_unlock((vp));   \
	}


	// We can not DeleteOnClose if readonly filesystem
	if (DeleteOnClose) {
		if (zfsvfs->z_rdonly || vfs_isrdonly(zfsvfs->z_vfs) ||
		    !spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
			UNDO_SHARE_ACCESS(vp);
			if (vp) VN_RELE(vp);
			VN_RELE(dvp);
			Irp->IoStatus.Information = 0; // ?
			return (STATUS_MEDIA_WRITE_PROTECTED);
		}
	}

	if (CreateFile && finalname) {
		int replacing = 0;

		if (zfsvfs->z_rdonly || vfs_isrdonly(zfsvfs->z_vfs) ||
		    !spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
			UNDO_SHARE_ACCESS(vp);
			if (vp) VN_RELE(vp);
			VN_RELE(dvp);
			Irp->IoStatus.Information = 0; // ?
			return (STATUS_MEDIA_WRITE_PROTECTED);
		}

		// Would we replace file?
		if (vp) {
			VN_RELE(vp);
			vp = NULL;
			replacing = 1;
		}

		zfs_setwinflags_xva(NULL,
		    IrpSp->Parameters.Create.FileAttributes, vap);
		vap->va_type = VREG;
		if (!(vap->va_mask & ATTR_MODE))
			vap->va_mode = 0777 | S_IFREG;
		vap->va_mask |= (ATTR_MODE | ATTR_TYPE);

		// If O_TRUNC:
		switch (CreateDisposition) {
		case FILE_SUPERSEDE:
		case FILE_OVERWRITE_IF:
		case FILE_OVERWRITE:
			vap->va_mask |= ATTR_SIZE;
			vap->va_size = 0;
			break;
		}

		// O_EXCL only if FILE_CREATE
		error = zfs_create(VTOZ(dvp), finalname, vap,
		    CreateDisposition == FILE_CREATE, vap->va_mode,
		    &zp, NULL, flags, NULL, NULL);
		if (error == 0) {
			boolean_t reenter_for_xattr = B_FALSE;

			// Creating two things? Don't attach until 2nd item.
			if (!(zp->z_pflags & ZFS_XATTR) && stream_name != NULL)
				reenter_for_xattr = B_TRUE;

			vp = ZTOV(zp);

			if (!reenter_for_xattr) {
				zfs_couplefileobject(vp, dvp, FileObject,
				    zp ? zp->z_size : 0ULL, &zccb,
				    Irp->Overlay.AllocationSize.QuadPart,
				    granted_access ?
				    granted_access : DesiredAccess,
				    stream_name);

				if (DeleteOnClose)
					Status =
					    zfs_setunlink_masked(FileObject,
					    dvp);

				Irp->IoStatus.Information = replacing ?
				    CreateDisposition == FILE_SUPERSEDE ?
				    FILE_SUPERSEDED : FILE_OVERWRITTEN :
				    FILE_CREATED;

				vnode_lock(vp);
				IoSetShareAccess(
				    DesiredAccess,
				    IrpSp->Parameters.Create.ShareAccess,
				    FileObject,
				    &vp->share_access);
				vnode_unlock(vp);

				// Did we create file, or stream?
				if (!(zp->z_pflags & ZFS_XATTR)) {

					if (SC != NULL &&
					    SC->AccessState &&
					    SC->AccessState->
					    SecurityDescriptor != NULL) {
						merge_security(vp,
						    SC->AccessState);
					}

					zfs_send_notify(zfsvfs,
					    zccb->z_name_cache,
					    zccb->z_name_offset,
					    FILE_NOTIFY_CHANGE_FILE_NAME,
					    FILE_ACTION_ADDED);
				} else {
					check_and_set_stream_parent(stream_name,
					    FileObject,
					    VTOZ(dvp)->z_xattr_parent);

					zfs_send_notify_stream(zfsvfs, // WOOT
					    zccb->z_name_cache,
					    zccb->z_name_offset,
					    FILE_NOTIFY_CHANGE_STREAM_NAME,
					    FILE_ACTION_ADDED_STREAM,
					    NULL);
				}
			}
		/* Windows lets you create a file, and stream, in one. */
		/* Call this function again, lets hope, only once */
			if (NT_SUCCESS(Status) && reenter_for_xattr) {
				Status = EAGAIN;
			}

			VN_RELE(vp);
			VN_RELE(dvp);

			return (Status);
		}
		if (error == EEXIST)
			Irp->IoStatus.Information = FILE_EXISTS;
		else
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;

		UNDO_SHARE_ACCESS(dvp);
		VN_RELE(dvp);
		switch (error) {
		case ENOSPC:
			return (STATUS_DISK_FULL);
		case EDQUOT:
			return (STATUS_DISK_FULL);
			// return (STATUS_DISK_QUOTA_EXCEEDED);
		default:
			// create file error
			return (STATUS_OBJECT_NAME_COLLISION);
		}
	}


	// Just open it, if the open was to a directory, add ccb
	ASSERT(IrpSp->FileObject->FsContext == NULL);
	if (vp == NULL) {
		zfs_couplefileobject(dvp, NULL, FileObject, 0ULL,
		    &zccb,
		    Irp->Overlay.AllocationSize.QuadPart,
		    granted_access ? granted_access : DesiredAccess,
		    stream_name);

		if (DeleteOnClose)
			Status = zfs_setunlink_masked(FileObject, NULL);

		if (Status == STATUS_SUCCESS) {
			if (UndoShareAccess == FALSE) {
				vnode_lock(dvp);
				IoSetShareAccess(
				    DesiredAccess,
				    IrpSp->Parameters.Create.ShareAccess,
				    FileObject,
				    &dvp->share_access);
				vnode_unlock(dvp);
			}
		} else {
			UNDO_SHARE_ACCESS(dvp);
		}
		VN_RELE(dvp);
	} else {
		// Technically, this should call zfs_open() -
		// but zfs_open is mostly empty

		zfs_couplefileobject(vp, dvp, FileObject, zp->z_size,
		    &zccb,
		    Irp->Overlay.AllocationSize.QuadPart,
		    granted_access ? granted_access : DesiredAccess,
		    stream_name);

		// Now that vp is set, check delete
		if (DeleteOnClose)
			Status = zfs_setunlink_masked(FileObject, dvp);

		if (Status == STATUS_SUCCESS) {

			Irp->IoStatus.Information = FILE_OPENED;

			// If we are to truncate the file:
			if (CreateDisposition == FILE_OVERWRITE) {
				Irp->IoStatus.Information = FILE_OVERWRITTEN;
				zp->z_pflags |= ZFS_ARCHIVE;
				// zfs_freesp() path uses vnode_pager_setsize()
				// so we need to make sure fileobject is set.
				zfs_freesp(zp, 0, 0, FWRITE, B_TRUE);
				// Did they ask for an AllocationSize
			}

			// If we created something new, add this permission
			if (UndoShareAccess == FALSE) {
				vnode_lock(vp);
				IoSetShareAccess(
				    DesiredAccess,
				    IrpSp->Parameters.Create.ShareAccess,
				    FileObject,
				    &vp->share_access);
				vnode_unlock(vp);
			}
		} else {
			UNDO_SHARE_ACCESS(vp);
		}
		VN_RELE(vp);
		VN_RELE(dvp);
	}

	IrpSp->Parameters.Create.SecurityContext->AccessState->
	    PreviouslyGrantedAccess |= granted_access;
	IrpSp->Parameters.Create.SecurityContext->AccessState->
	    RemainingDesiredAccess &= ~(granted_access | MAXIMUM_ALLOWED);

	return (Status);
}

int
zfs_vnop_lookup(PIRP Irp, PIO_STACK_LOCATION IrpSp, mount_t *zmo)
{
	int status;
	char *filename = NULL;
	xvattr_t xva = { 0 };
	vattr_t *vap = &xva.xva_vattr;

	// Check the EA buffer is good, if supplied.
	if (Irp->AssociatedIrp.SystemBuffer != NULL &&
	    IrpSp->Parameters.Create.EaLength > 0) {
		ULONG offset;
		status = IoCheckEaBufferValidity(
		    Irp->AssociatedIrp.SystemBuffer,
		    IrpSp->Parameters.Create.EaLength, &offset);
		if (!NT_SUCCESS(status)) {
			dprintf("IoCheckEaBufferValidity returned %08x "
			    "(error at offset %lu)\n", status, offset);
			return (status);
		}
	}

	// Allocate space to hold name, must be freed from here on
	filename = kmem_alloc(PATH_MAX, KM_SLEEP);

	// Deal with ExtraCreateParameters
#if defined(NTDDI_WIN10_RS5) && (NTDDI_VERSION >= NTDDI_WIN10_RS5)
	/* Check for ExtraCreateParameters */
	PECP_LIST ecp = NULL;
	ATOMIC_CREATE_ECP_CONTECT *acec = NULL;
	PQUERY_ON_CREATE_ECP_CONTEXT qocContext = NULL;
	FsRtlGetEcpListFromIrp(Irp, &ecp);
	if (ecp) {
		GUID ecpType;
		void *ecpContext = NULL;
		ULONG ecpContextSize;
		while (NT_SUCCESS(FsRtlGetNextExtraCreateParameter(ecp,
		    ecpContext, &ecpType, &ecpContext, &ecpContextSize))) {
			if (IsEqualGUID(&ecpType, &GUID_ECP_ATOMIC_CREATE)) {
				dprintf("GUID_ECP_ATOMIC_CREATE\n");
				// More code to come here:
				acec = ecpContext;
			} else if (IsEqualGUID(&ecpType,
			    &GUID_ECP_QUERY_ON_CREATE)) {
				dprintf("GUID_ECP_QUERY_ON_CREATE\n");
				// It wants a getattr call on success,
				// before we finish up
				qocContext =
				    (PQUERY_ON_CREATE_ECP_CONTEXT)ecpContext;
			} else if (IsEqualGUID(&ecpType,
			    &GUID_ECP_CREATE_REDIRECTION)) {
				dprintf("GUID_ECP_CREATE_REDIRECTION\n");
				// We get this one a lot.
			} else {
				dprintf("Other GUID_ECP type\n");
// IopSymlinkECPGuid "73d5118a-88ba-439f-92f4-46d38952d250"
			}
		}// while
	} // if ecp
#endif

	// The associated buffer on a CreateFile is an EA buffer.
	// Already Verified above - do a quickscan of any EAs we
	// handle in a special way, before we call zfs_vnop_lookup_impl().
	// We handle the regular EAs afterward.
	if (Irp->AssociatedIrp.SystemBuffer != NULL &&
	    IrpSp->Parameters.Create.EaLength > 0) {
		PFILE_FULL_EA_INFORMATION ea;
		for (ea =
		    (PFILE_FULL_EA_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
		    /* empty */;
		    ea = (PFILE_FULL_EA_INFORMATION)((uint8_t *)ea +
		    ea->NextEntryOffset)) {
			// only parse $LX attrs right now -- things we can store
			// before the file gets created.
			if (vattr_apply_lx_ea(vap, ea)) {
				dprintf("encountered special attrs EA '%.*s'\n",
				    ea->EaNameLength, ea->EaName);
			}
			if (ea->NextEntryOffset == 0)
				break;
		}
	}

	do {

		// Call ZFS
		status = zfs_vnop_lookup_impl(Irp, IrpSp, zmo, filename, &xva);

	} while (status == EAGAIN);

#if defined(NTDDI_WIN10_RS5) && (NTDDI_VERSION >= NTDDI_WIN10_RS5)
	// Did ECP ask for getattr to be returned? None, one or both can be set.
	// This requires vnode_couplefileobject() was called
	if (NT_SUCCESS(status) && qocContext && IrpSp->FileObject->FsContext) {

		ULONG classes = 0;

		// Handle RS5 >= version < 19H1 when the struct had "Flags".
#if defined(NTDDI_WIN10_19H1) && (NTDDI_VERSION >= NTDDI_WIN10_19H1)
		classes = qocContext->RequestedClasses;
#else
		classes = qocContext->Flags;
#endif

		if (BooleanFlagOn(classes, QoCFileStatInformation)) {
			file_stat_information(IrpSp->DeviceObject, Irp, IrpSp,
			    &qocContext->StatInformation);
		}
		if (BooleanFlagOn(classes, QoCFileLxInformation)) {
			file_stat_lx_information(IrpSp->DeviceObject, Irp,
			    IrpSp, &qocContext->LxInformation);
		}
		if (BooleanFlagOn(classes, QoCFileEaInformation)) {
			dprintf("%s: unsupported QoC: QoCFileEaInformation\n");
		}
#if defined(NTDDI_WIN10_19H1) && (NTDDI_VERSION >= NTDDI_WIN10_19H1)
		// We should fill this in, right? Only set those we understand.
		qocContext->ClassesProcessed =
		    classes & (QoCFileStatInformation|QoCFileLxInformation);
		qocContext->ClassesWithErrors = 0;
		qocContext->ClassesWithNoData = 0;
#endif

		FsRtlAcknowledgeEcp(qocContext);
	}

	if (NT_SUCCESS(status) && acec && acec->
	    InFlags & ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED) {
		panic("Implement me: atomic reparse point");
		// acec->OutFlags |=
		// 	ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET;
	}
#endif

	// Now handle proper EAs properly
	if (NT_SUCCESS(status)) {
		if (Irp->AssociatedIrp.SystemBuffer &&
		    IrpSp->FileObject->FsContext) {
			// Second pass: this will apply all EAs that are
			// not only LX EAs
			vnode_apply_eas(IrpSp->FileObject->FsContext,
			    IrpSp->FileObject->FsContext2,
			    (PFILE_FULL_EA_INFORMATION)
			    Irp->AssociatedIrp.SystemBuffer,
			    IrpSp->Parameters.Create.EaLength, NULL);
		}

		if (!BooleanFlagOn(IrpSp->Parameters.Create.Options,
		    FILE_NO_INTERMEDIATE_BUFFERING)) {
			IrpSp->FileObject->Flags |= FO_CACHE_SUPPORTED;
		}

	}

	// Free filename
	kmem_free(filename, PATH_MAX);

	dprintf("%s: %s with %s\n", __func__,
	    common_status_str(status),
	    create_reply(status, Irp->IoStatus.Information));

	return (status);
}


/*
 * reclaim is called when a vnode is to be terminated,
 * VFS (spl-vnode.c) will hold iocount == 1, usecount == 0
 * so release associated ZFS node, and free everything
 */
int
zfs_vnop_reclaim(struct vnode *vp)
{
	znode_t *zp = VTOZ(vp);
	if (zp == NULL) {
		ASSERT("NULL zp in reclaim?");
		return (0);
	}

	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	dprintf("  zfs_vnop_recycle: releasing zp %p and vp %p\n", zp, vp);

	// Decouple the nodes
	ASSERT(ZTOV(zp) != (vnode_t *)0xdeadbeefdeadbeef);

	mutex_enter(&zp->z_lock);
	// lost the race?
	if (VTOZ(vp) == NULL) {
		mutex_exit(&zp->z_lock);
		return (0);
	}
	ZTOV(zp) = NULL;
	vnode_clearfsnode(vp); /* vp->v_data = NULL */
	mutex_exit(&zp->z_lock);
	// vnode_removefsref(vp); /* ADDREF from vnode_create */

	void *sd = vnode_security(vp);
	if (sd != NULL)
		ExFreePool(sd);
	vnode_setsecurity(vp, NULL);

	vp = NULL;

	// Release znode
	/*
	 * This will release as much as it can, based on reclaim_reentry,
	 * if we are from fastpath, we do not call free here, as zfs_remove
	 * calls zfs_znode_delete() directly.
	 * zfs_zinactive() will leave earlier if z_reclaim_reentry is true.
	 */
	rw_enter(&zfsvfs->z_teardown_inactive_lock, RW_READER);
	if (zp->z_sa_hdl == NULL) {
		zfs_znode_free(zp);
	} else {
		zfs_zinactive(zp);
		zfs_znode_free(zp);
	}
	rw_exit(&zfsvfs->z_teardown_inactive_lock);

	atomic_dec_64(&vnop_num_vnodes);
	atomic_inc_64(&vnop_num_reclaims);

	if (vnop_num_vnodes % 1000 == 0)
		dprintf("%s: num_vnodes %llu\n", __func__, vnop_num_vnodes);

	return (0);
}

/*
 */
void
getnewvnode_reserve(int num)
{
}

void
getnewvnode_drop_reserve()
{
}

/*
 * Get new vnode for znode.
 *
 * This function uses zp->z_zfsvfs, zp->z_mode, zp->z_flags, zp->z_id
 * and sets zp->z_vnode and zp->z_vid.
 * If given parent, dzp, we can save some hassles. If not, looks it
 * up internally.
 */
int
zfs_znode_getvnode(znode_t *zp, znode_t *dzp, zfsvfs_t *zfsvfs)
{
	struct vnode *vp = NULL;
	int flags = 0;
	struct vnode *parentvp = NULL;
	// dprintf("getvnode zp %p with vp %p zfsvfs %p vfs %p\n", zp, vp,
	//    zfsvfs, zfsvfs->z_vfs);

	if (zp->z_vnode)
		panic("zp %p vnode already set\n", zp->z_vnode);

	// "root" / mountpoint holds long term ref
	if (zp->z_id == zfsvfs->z_root) {
		flags |= VNODE_MARKROOT;
	} else {

		/*
		 * To maintain a well-defined vnode tree,
		 * we need the parent here.
		 * This could cascade?
		 */
		if (dzp != NULL)
			parentvp = ZTOV(dzp);
		if (parentvp != NULL) {
			VERIFY0(VN_HOLD(parentvp));
		} else {
			uint64_t parent;
			znode_t *parentzp;
			VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
			    &parent, sizeof (parent)) == 0);
			if (zfs_zget(zfsvfs, parent, &parentzp) != 0) {
				return (NULL);
			}
			parentvp = ZTOV(parentzp);
		}
	}

	/*
	 * vnode_create() has a habit of calling both vnop_reclaim() and
	 * vnop_fsync(), which can create havok as we are already holding locks.
	 */
	vnode_create(zfsvfs->z_vfs, parentvp,
	    zp, IFTOVT((mode_t)zp->z_mode), flags, &vp);

	/* We also get here with xdvp on the file, can be NULL */
	if (parentvp != NULL) {

		boolean_t isanyxattr = B_FALSE;

		znode_t *dzp = VTOZ(parentvp);
		if (dzp && (dzp->z_pflags & ZFS_XATTR))
			isanyxattr = B_TRUE;

		if (zp->z_pflags & ZFS_XATTR)
			isanyxattr = B_TRUE;

		if (vnode_isdir(parentvp) &&
		    !isanyxattr)
			vnode_setparent(vp, parentvp);

		VN_RELE(parentvp);
	}

	atomic_inc_64(&vnop_num_vnodes);

	// dprintf("Assigned zp %p with vp %p\n", zp, vp);
	zp->z_vid = vnode_vid(vp);
	zp->z_vnode = vp;

	// Assign security here. But, if we are XATTR, we do not? In Windows,
	// it refers to Streams and they do not have Security?
	if (zp->z_pflags & ZFS_XATTR)
		;
	else {
		NTSTATUS Status;
		Status = zfs_attach_security(vp, dzp && ZTOV(dzp) ?
		    ZTOV(dzp) : NULL);
		if (!NT_SUCCESS(Status))
			dprintf("zfs_attach_security failed: 0x%lx\n", Status);
	}
	return (0);
}


NTSTATUS
dev_ioctl(PDEVICE_OBJECT DeviceObject, ULONG ControlCode, PVOID InputBuffer,
    ULONG InputBufferSize, PVOID OutputBuffer, ULONG OutputBufferSize,
    BOOLEAN Override, IO_STATUS_BLOCK* iosb)
{
	PIRP Irp;
	KEVENT Event;
	NTSTATUS Status;
	PIO_STACK_LOCATION Stack;
	IO_STATUS_BLOCK IoStatus;

	KeInitializeEvent(&Event, NotificationEvent, FALSE);

	Irp = IoBuildDeviceIoControlRequest(ControlCode,
	    DeviceObject,
	    InputBuffer,
	    InputBufferSize,
	    OutputBuffer,
	    OutputBufferSize,
	    FALSE,
	    &Event,
	    &IoStatus);

	if (!Irp)
		return (STATUS_INSUFFICIENT_RESOURCES);

	if (Override) {
		Stack = IoGetNextIrpStackLocation(Irp);
		Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
	}

	Status = IoCallDriver(DeviceObject, Irp);

	if (Status == STATUS_PENDING) {
		KeWaitForSingleObject(&Event, Executive, KernelMode,
		    FALSE, NULL);
		Status = IoStatus.Status;
	}

	if (iosb)
		*iosb = IoStatus;

	return (Status);
}

static WCHAR
hex_digit(uint8_t u)
{
	if (u >= 0xa && u <= 0xf)
		return ((uint8_t)(u - 0xa + 'a'));
	else
		return ((uint8_t)(u + '0'));
}

// THIS IS THE PNP DEVICE ID
NTSTATUS
pnp_query_id(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	mount_t *zmo;
	WCHAR *idString = NULL;
	int idLen = 0;
	NTSTATUS Status = STATUS_SUCCESS;
	DECLARE_UNICODE_STRING_SIZE(mpt, 100);

	dprintf("%s: query id type %d\n", __func__,
	    IrpSp->Parameters.QueryId.IdType);

	Irp->IoStatus.Information = NULL;

	zmo = (mount_t *)DeviceObject->DeviceExtension;
	/*
	 * Hark. So BusQueryHardwareIDs and BusQueryCompatibleIDs do not
	 * take a single string, but a MULTI_SZ - list of strings.
	 * Each string is null-terminated, and the last string is
	 * double null-terminated. Oh the fun we had figuring that out.
	 */
	switch (IrpSp->Parameters.QueryId.IdType) {
	case BusQueryDeviceID:
		RtlUnicodeStringPrintf(&mpt,
		    L"OpenZFS\\%wZ%lc", &zmo->uuid, 0);
		idString = mpt.Buffer;
		idLen = mpt.Length;
		break;
	case BusQueryHardwareIDs: // IDs, plural
		RtlUnicodeStringPrintf(&mpt,
		    L"ROOT\\OpenZFS%lc%lc", 0, 0); // double nulls
		idString = mpt.Buffer;
		idLen = mpt.Length;
		break;
	case BusQueryContainerID:
		RtlUnicodeStringPrintf(&mpt,
		    L"{00000001-0002-0003-0004-000000000088}%lc", 0);
		idString = mpt.Buffer;
		idLen = mpt.Length;
		break;
#if 0
	// If these are included, AddDevice() does not get called.
	case BusQueryCompatibleIDs:
		RtlUnicodeStringPrintf(&mpt,
		    L"OpenZFS\\Generic%lc%lc", 0, 0);
		idString = mpt.Buffer;
		idLen = mpt.Length;
		break;
	case BusQueryInstanceID: // Needs to be unique.
		idString = zmo->uuid.Buffer;
		idLen = zmo->uuid.Length;
		break;
#endif
	default:
		// Status = Irp->IoStatus.Status;
		Status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	if (idLen > 0) {
		WCHAR *str;
		str = (WCHAR *)ExAllocatePoolWithTag(PagedPool,
		    idLen /* + sizeof(WCHAR) */, '!OIZ');
		if (str == NULL)
			return (STATUS_INSUFFICIENT_RESOURCES);

		RtlCopyMemory((void *)str, idString,
		    idLen);
		// str[idLen / sizeof(WCHAR)] = UNICODE_NULL;

		Irp->IoStatus.Information = (ULONG_PTR)str;

		dprintf("replying with '%.*S'\n",
		    (int)(idLen/sizeof (WCHAR)),
		    (WCHAR *)str);

	}

	return (Status);
}

NTSTATUS
pnp_device_state(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	dprintf("%s:\n", __func__);
	PPNP_DEVICE_STATE pDeviceState =
	    (PPNP_DEVICE_STATE)&Irp->IoStatus.Information;

	pDeviceState = 0;

	if (vfs_isunmount(DeviceObject->DeviceExtension))
		Irp->IoStatus.Information |= PNP_DEVICE_REMOVED;
	// Irp->IoStatus.Information |= PNP_DEVICE_NOT_DISABLEABLE;

	return (STATUS_SUCCESS);
}

NTSTATUS
query_volume_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;
	Status = STATUS_NOT_IMPLEMENTED;
	int space;
	int error = 0;
	uint64_t refdbytes, availbytes, usedobjs, availobjs;

	mount_t *zmo = DeviceObject->DeviceExtension;
	if (!zmo ||
	    (zmo->type != MOUNT_TYPE_VCB &&
	    zmo->type != MOUNT_TYPE_DCB)) {
		return (STATUS_INVALID_PARAMETER);
	}

	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	if (zfsvfs == NULL)
		return (STATUS_INVALID_PARAMETER);

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);  // This returns EIO if fail

	uint64_t sectorsz = 512ULL;
	if (zfsvfs->z_os && zfsvfs->z_os->os_spa)
		sectorsz = zfsvfs->z_os->os_spa->spa_min_alloc;

	switch (IrpSp->Parameters.QueryVolume.FsInformationClass) {

	case FileFsAttributeInformation:
		//
		// If overflow, set Information to input_size and NameLength
		// to what we fit.
		//
		dprintf("* %s: FileFsAttributeInformation\n", __func__);
		if (IrpSp->Parameters.QueryVolume.Length <
		    sizeof (FILE_FS_ATTRIBUTE_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_FS_ATTRIBUTE_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

/* Do not enable until we have implemented FileRenameInformationEx method. */
#if (NTDDI_VERSION >= NTDDI_WIN10_RS1)
#define	ZFS_FS_ATTRIBUTE_POSIX
#endif

#define	ZFS_FS_ATTRIBUTE_CLEANUP_INFO

		FILE_FS_ATTRIBUTE_INFORMATION *ffai =
		    Irp->AssociatedIrp.SystemBuffer;
		ffai->FileSystemAttributes =
		    FILE_CASE_PRESERVED_NAMES | FILE_NAMED_STREAMS |
		    FILE_PERSISTENT_ACLS | FILE_SUPPORTS_OBJECT_IDS |
		    FILE_SUPPORTS_SPARSE_FILES | FILE_VOLUME_QUOTAS |
		    FILE_SUPPORTS_REPARSE_POINTS | FILE_UNICODE_ON_DISK |
		    FILE_SUPPORTS_HARD_LINKS | FILE_SUPPORTS_OPEN_BY_FILE_ID |
		    FILE_SUPPORTS_EXTENDED_ATTRIBUTES |
		    FILE_CASE_SENSITIVE_SEARCH;
#if defined(ZFS_FS_ATTRIBUTE_POSIX)
		ffai->FileSystemAttributes |= FILE_SUPPORTS_POSIX_UNLINK_RENAME;
#endif
#if defined(ZFS_FS_ATTRIBUTE_CLEANUP_INFO)
		ffai->FileSystemAttributes |= FILE_RETURNS_CLEANUP_RESULT_INFO;
#endif
#if defined(FILE_SUPPORTS_BLOCK_REFCOUNTING)
		/* Block-cloning, from FSCTL_DUPLICATE_EXTENTS */
		ffai->FileSystemAttributes |= FILE_SUPPORTS_BLOCK_REFCOUNTING;
#endif

		ffai->FileSystemAttributes |= FILE_FILE_COMPRESSION |
		    FILE_VOLUME_QUOTAS | FILE_SUPPORTS_SPARSE_VDL;


		/*
		 * NTFS has these:
		 * FILE_CASE_SENSITIVE_SEARCH | FILE_FILE_COMPRESSION |
		 * FILE_RETURNS_CLEANUP_RESULT_INFO |
		 * FILE_SUPPORTS_POSIX_UNLINK_RENAME |
		 * FILE_SUPPORTS_ENCRYPTION | FILE_SUPPORTS_TRANSACTIONS |
		 * FILE_SUPPORTS_USN_JOURNAL;
		 */

		if (zfsvfs->z_case == ZFS_CASE_SENSITIVE)
			ffai->FileSystemAttributes |=
			    FILE_CASE_SENSITIVE_SEARCH;

		if (zfsvfs->z_rdonly) {
			SetFlag(ffai->FileSystemAttributes,
			    FILE_READ_ONLY_VOLUME);
		}
		ffai->MaximumComponentNameLength = MAXNAMELEN - 1;
		// ffai->FileSystemAttributes = 0x3E706FF; // ntfs 2023

		// There is room for one char in the struct
		// Alas, many things compare string to "NTFS".
		space = IrpSp->Parameters.QueryVolume.Length -
		    FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);

		UNICODE_STRING name;
		if (zfsvfs->z_mimic == ZFS_MIMIC_OFF)
			RtlInitUnicodeString(&name, L"ZFS");
		else
			RtlInitUnicodeString(&name, L"NTFS");
		dprintf("Replying as %wZ\n", &name);

		space = MIN(space, name.Length);
		ffai->FileSystemNameLength = name.Length;
		RtlCopyMemory(ffai->FileSystemName, name.Buffer, space);
		Irp->IoStatus.Information =
		    FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION,
		    FileSystemName) + space;

		if (space < name.Length)
			Status = STATUS_BUFFER_OVERFLOW;
		else
			Status = STATUS_SUCCESS;

		ASSERT(Irp->IoStatus.Information <=
		    IrpSp->Parameters.QueryVolume.Length);
		break;

	case FileFsControlInformation:
		dprintf("* %s: FileFsControlInformation NOT IMPLEMENTED\n",
		    __func__);
		break;

	case FileFsDeviceInformation:
		dprintf("* %s: FileFsDeviceInformation\n",
		    __func__);
		FILE_FS_DEVICE_INFORMATION *ffdi;
		ffdi = Irp->AssociatedIrp.SystemBuffer;
		ffdi->DeviceType = FILE_DEVICE_DISK;
		ffdi->Characteristics = 0; // FILE_REMOVABLE_MEDIA |
		    // FILE_DEVICE_IS_MOUNTED /* | FILE_READ_ONLY_DEVICE */;
		Irp->IoStatus.Information = sizeof (FILE_FS_DEVICE_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	case FileFsDriverPathInformation:
		dprintf("* %s: FileFsDriverPathInformation NOT IMPLEMENTED\n",
		    __func__);
		break;

	case FileFsFullSizeInformation:
		dprintf("* %s: FileFsFullSizeInformation\n", __func__);
		if (IrpSp->Parameters.QueryVolume.Length <
		    sizeof (FILE_FS_FULL_SIZE_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_FS_FULL_SIZE_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		dmu_objset_space(zfsvfs->z_os,
		    &refdbytes, &availbytes, &usedobjs, &availobjs);

		FILE_FS_FULL_SIZE_INFORMATION *fffsi =
		    Irp->AssociatedIrp.SystemBuffer;
		fffsi->TotalAllocationUnits.QuadPart =
		    (refdbytes + availbytes) / sectorsz;
		fffsi->ActualAvailableAllocationUnits.QuadPart =
		    availbytes / sectorsz;
		fffsi->CallerAvailableAllocationUnits.QuadPart =
		    availbytes / sectorsz;
		fffsi->BytesPerSector = sectorsz;
		fffsi->SectorsPerAllocationUnit = 1;
		Irp->IoStatus.Information =
		    sizeof (FILE_FS_FULL_SIZE_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	case FileFsObjectIdInformation:
		dprintf("* %s: FileFsObjectIdInformation\n", __func__);
		FILE_FS_OBJECTID_INFORMATION *ffoi =
		    Irp->AssociatedIrp.SystemBuffer;
		RtlZeroMemory(ffoi->ObjectId, sizeof (ffoi->ObjectId));
		uint64_t guid = dmu_objset_fsid_guid(zfsvfs->z_os);
		RtlCopyMemory(ffoi->ObjectId, &guid, sizeof (ffoi->ObjectId));
		RtlZeroMemory(ffoi->ExtendedInfo, sizeof (ffoi->ExtendedInfo));
		Irp->IoStatus.Information =
		    sizeof (FILE_FS_OBJECTID_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	case FileFsVolumeInformation:
		// Confirmed this call beha
		dprintf("* %s: FileFsVolumeInformation\n", __func__);

		if (IrpSp->Parameters.QueryVolume.Length <
		    sizeof (FILE_FS_VOLUME_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_FS_VOLUME_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_FS_VOLUME_INFORMATION *ffvi =
		    Irp->AssociatedIrp.SystemBuffer;
		TIME_UNIX_TO_WINDOWS_EX(zfsvfs->z_last_unmount_time, 0,
		    ffvi->VolumeCreationTime.QuadPart);
		ffvi->SupportsObjects = TRUE;
		PVPB Vpb = zmo->vpb;
		WCHAR *wstr;

		ffvi->VolumeSerialNumber = 0x19831116;
#if 0
		ffvi->VolumeLabelLength =
		    sizeof (VOLUME_LABEL) - sizeof (WCHAR);
		wstr = VOLUME_LABEL;
#else
		ffvi->VolumeLabelLength =
		    zmo->name.Length;
		wstr = zmo->name.Buffer;
#endif
		int space =
		    IrpSp->Parameters.QueryFile.Length -
		    FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
		space = MIN(space, ffvi->VolumeLabelLength);

		/*
		 * This becomes the name displayed in Explorer, so we return the
		 * dataset name here, as much as we can
		 */
		RtlCopyMemory(ffvi->VolumeLabel, wstr, space);

		Irp->IoStatus.Information =
		    FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION,
		    VolumeLabel) + space;

		if (space < ffvi->VolumeLabelLength)
			Status = STATUS_BUFFER_OVERFLOW;
		else
			Status = STATUS_SUCCESS;

		break;

	case FileFsSizeInformation:
		dprintf("* %s: FileFsSizeInformation\n", __func__);
		if (IrpSp->Parameters.QueryVolume.Length <
		    sizeof (FILE_FS_SIZE_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_FS_SIZE_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		dmu_objset_space(zfsvfs->z_os,
		    &refdbytes, &availbytes, &usedobjs, &availobjs);

		FILE_FS_SIZE_INFORMATION *ffsi =
		    Irp->AssociatedIrp.SystemBuffer;
		ffsi->TotalAllocationUnits.QuadPart =
		    (refdbytes + availbytes) / sectorsz;
		ffsi->AvailableAllocationUnits.QuadPart =
		    availbytes / sectorsz;
		ffsi->SectorsPerAllocationUnit = 1;
		ffsi->BytesPerSector = sectorsz;
		Irp->IoStatus.Information = sizeof (FILE_FS_SIZE_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	case FileFsSectorSizeInformation:
		dprintf("* %s: FileFsSectorSizeInformation\n", __func__);
		if (IrpSp->Parameters.QueryVolume.Length <
		    sizeof (FILE_FS_SECTOR_SIZE_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_FS_SECTOR_SIZE_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_FS_SECTOR_SIZE_INFORMATION *ffssi =
		    Irp->AssociatedIrp.SystemBuffer;
		ffssi->LogicalBytesPerSector = sectorsz;
		ffssi->PhysicalBytesPerSectorForAtomicity = sectorsz;
		ffssi->PhysicalBytesPerSectorForPerformance = sectorsz;
		ffssi->FileSystemEffectivePhysicalBytesPerSectorForAtomicity =
		    sectorsz;
		ffssi->Flags = SSINFO_FLAGS_NO_SEEK_PENALTY;
		ffssi->ByteOffsetForSectorAlignment = SSINFO_OFFSET_UNKNOWN;
		ffssi->ByteOffsetForPartitionAlignment = SSINFO_OFFSET_UNKNOWN;
		Irp->IoStatus.Information =
		    sizeof (FILE_FS_SECTOR_SIZE_INFORMATION);
		Status = STATUS_SUCCESS;
		break;

	default:
		dprintf("* %s: unknown class 0x%x\n", __func__,
		    IrpSp->Parameters.QueryVolume.FsInformationClass);
		Status = STATUS_NOT_IMPLEMENTED;
		break;
	}
	zfs_exit(zfsvfs, FTAG);
	return (Status);
}


NTSTATUS
lock_control(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_SUCCESS;

	dprintf("%s: FileObject %p flags 0x%x %s %s\n", __func__,
	    IrpSp->FileObject, IrpSp->Flags,
	    IrpSp->Flags & SL_EXCLUSIVE_LOCK ? "Exclusive" : "Shared",
	    IrpSp->Flags & SL_FAIL_IMMEDIATELY ? "Nowait" : "Wait");

	return (Status);
}

NTSTATUS
query_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
	ULONG usedspace = 0;
	struct vnode *vp = NULL;
	int normalize = 0;

	if (IrpSp->FileObject && IrpSp->FileObject->FsContext) {
		vp = IrpSp->FileObject->FsContext;
		if (VN_HOLD(vp) != 0)
			return (STATUS_INVALID_PARAMETER);
	}

	switch (IrpSp->Parameters.QueryFile.FileInformationClass) {

	case FileAllInformation:
		dprintf("%s: FileAllInformation: buffer 0x%lx\n", __func__,
		    IrpSp->Parameters.QueryFile.Length);

		if (IrpSp->Parameters.QueryFile.Length <
		    sizeof (FILE_ALL_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_ALL_INFORMATION);
// We should send Plus Filename here, to be nice, but this doesnt happen
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_ALL_INFORMATION *all = Irp->AssociatedIrp.SystemBuffer;

		// Even if the name does not fit, the other information
		// should be correct
		Status = file_basic_information(DeviceObject, Irp, IrpSp,
		    &all->BasicInformation);
		if (Status != STATUS_SUCCESS)
			break;
		Status = file_standard_information(DeviceObject, Irp, IrpSp,
		    &all->StandardInformation);
		if (Status != STATUS_SUCCESS)
			break;
		Status = file_position_information(DeviceObject, Irp, IrpSp,
		    &all->PositionInformation);
		if (Status != STATUS_SUCCESS)
			break;
		Status = file_ea_information(DeviceObject, Irp, IrpSp,
		    &all->EaInformation);
		if (Status != STATUS_SUCCESS)
			break;
#if 1
		zfs_ccb_t *zccb = IrpSp->FileObject->FsContext2;
		all->AccessInformation.AccessFlags =
		    GENERIC_ALL | GENERIC_EXECUTE |
		    GENERIC_READ | GENERIC_WRITE;
		all->ModeInformation.Mode =
		    zccb && zccb->deleteonclose ? FILE_DELETE_ON_CLOSE : 0;
#endif
		Status = file_alignment_information(DeviceObject, Irp, IrpSp,
		    &all->AlignmentInformation);
		if (Status != STATUS_SUCCESS)
			break;

		Status = file_internal_information(DeviceObject, Irp, IrpSp,
		    &all->InternalInformation);
		if (Status != STATUS_SUCCESS)
			break;

		// First get the Name, to make sure we have room
		IrpSp->Parameters.QueryFile.Length -=
		    offsetof(FILE_ALL_INFORMATION, NameInformation);
		Status = file_name_information(DeviceObject, Irp, IrpSp,
		    &all->NameInformation, &usedspace, 0);
		IrpSp->Parameters.QueryFile.Length +=
		    offsetof(FILE_ALL_INFORMATION, NameInformation);

		// file_name_information sets FileNameLength, so update size
		// to be ALL struct not NAME struct
		// However, there is room for one char in the struct,
		// so subtract that from total.
		Irp->IoStatus.Information =
		    FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation) +
		    FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
		    usedspace;
		// FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName)
		// + usedspace;

		dprintf("Struct size 0x%x FileNameLen 0x%lx "
		    "Information retsize 0x%llx\n",
		    (int)sizeof (FILE_ALL_INFORMATION),
		    all->NameInformation.FileNameLength,
		    Irp->IoStatus.Information);
		break;
	case FileAttributeTagInformation:
		Status = file_attribute_tag_information(DeviceObject, Irp,
		    IrpSp, Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileBasicInformation:
		Status = file_basic_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileCompressionInformation:
		Status = file_compression_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileEaInformation:
		Status = file_ea_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileInternalInformation:
		Status = file_internal_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileNormalizedNameInformation:
		dprintf("FileNormalizedNameInformation\n");
	/*
	 * According to chatGPT, the difference between FileNameInformation
	 * and FileNormalizedNameInformation is that the latter will
	 * return a more "portable" name. For example;
	 * "My Photos (2022)" -> "my_photos_2022", as the FS desires.
	 * In this example, unified case, no spaces and limited charset.
	 *
	 * The complications start when the normalized name is passed to
	 * lookup (CreateFile->zfs_vnop_lookup()) as it is expected to
	 * work. Uniqueness would have to be guaranteed (per directory).
	 * And filename matching would be more complicated.
	 *
	 * For now, let's return identical names for Normalized.
	 *
	 */

		normalize = 1;

		zfs_fallthrough;
	case FileNameInformation:
		//
		// If overflow, set Information to input_size and NameLength
		// to required size.
		//
		Status = file_name_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer, &usedspace, normalize);
		Irp->IoStatus.Information =
		    FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + usedspace;
		break;
	case FileNetworkOpenInformation:
		Status = file_network_open_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FilePositionInformation:
		Status = file_position_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileStandardInformation:
		Status = file_standard_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileAlignmentInformation:
		Status = file_alignment_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileStreamInformation:
		Status = file_stream_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileHardLinkInformation:
		Status = file_hard_link_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	// Not used - not handled by ntfs either
	case FileRemoteProtocolInformation:
		dprintf("* %s: FileRemoteProtocolInformation NOT IMPLEMENTED\n",
		    __func__);
#if 0
		Status = file_remote_protocol_information(DeviceObject, Irp,
		    IrpSp, Irp->AssociatedIrp.SystemBuffer);
#endif
		Status = STATUS_INVALID_PARAMETER;
		break;
	case FileStandardLinkInformation:
		Status = file_standard_link_information(DeviceObject, Irp,
		    IrpSp, Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileReparsePointInformation:
		dprintf("* %s: FileReparsePointInformation NOT IMPLEMENTED\n",
		    __func__);
		break;
	case FileIdInformation:
		Status = file_id_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileCaseSensitiveInformation:
		Status = file_case_sensitive_information(DeviceObject, Irp,
		    IrpSp, Irp->AssociatedIrp.SystemBuffer);
		break;
	case FileStatInformation:
		// We call these functions from zfs_vnop_lookup, so size
		// testing goes here
		if (IrpSp->Parameters.QueryFile.Length <
		    sizeof (FILE_STAT_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_STAT_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		Status = file_stat_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		Irp->IoStatus.Information = sizeof (FILE_STAT_INFORMATION);
		break;
	case FileStatLxInformation:
		// We call these functions from zfs_vnop_lookup, so size
		// testing goes here
		if (IrpSp->Parameters.QueryFile.Length <
		    sizeof (FILE_STAT_LX_INFORMATION)) {
			Irp->IoStatus.Information =
			    sizeof (FILE_STAT_LX_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		Status = file_stat_lx_information(DeviceObject, Irp, IrpSp,
		    Irp->AssociatedIrp.SystemBuffer);
		Irp->IoStatus.Information = sizeof (FILE_STAT_LX_INFORMATION);
		break;
	default:
		dprintf("* %s: unknown class 0x%x NOT IMPLEMENTED\n", __func__,
		    IrpSp->Parameters.QueryFile.FileInformationClass);
		break;
	}

	if (vp) {
		VN_RELE(vp);
		vp = NULL;
	}
	return (Status);
}

PVOID
MapUserBuffer(IN OUT PIRP Irp)
{
	//
	// If there is no Mdl, then we must be in the Fsd, and we can simply
	// return the UserBuffer field from the Irp.
	//
	if (Irp->MdlAddress == NULL) {
		return (Irp->UserBuffer);
	} else {
		PVOID Address = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
		    NormalPagePriority | MdlMappingNoExecute);
		return (Address);
	}
}

PVOID
BufferUserBuffer(IN OUT PIRP Irp, IN ULONG BufferLength)
{
	PUCHAR UserBuffer;
	if (BufferLength == 0)
		return (NULL);

	//
	//  If there is no system buffer we must have been supplied an Mdl
	//  describing the users input buffer, which we will now snapshot.
	//
	if (Irp->AssociatedIrp.SystemBuffer == NULL) {
		UserBuffer = MapUserBuffer(Irp);
		Irp->AssociatedIrp.SystemBuffer =
		    FsRtlAllocatePoolWithQuotaTag(NonPagedPoolNx,
		    BufferLength,
		    'qtaf');
		//
		// Set the flags so that the completion code knows to
		// deallocate the buffer.
		//
		Irp->Flags |= (IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER);

		try {
			RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
			    UserBuffer,
			    BufferLength);
		} except(EXCEPTION_EXECUTE_HANDLER) {
			NTSTATUS Status;
			Status = GetExceptionCode();
		}
	}
	return (Irp->AssociatedIrp.SystemBuffer);
}

/*
 * ** This is how I thought it worked:
 * Iterate through the XATTRs of an object, skipping streams. It works
 * like readdir, with saving index point, restart_scan and single_entry flags.
 * It can optionally supply QueryEa.EaList to query specific set of EAs.
 * Each output structure is 4 byte aligned
 *
 * 1: While EAs fit, including name and value-data, we keep packing them
 * in. If we have a non-zero number of valid EAs in the output buffer,
 * we return STATUS_SUCCESS, and IoStatus.Information is set to the
 * number of bytes in the output buffer.
 *
 * 2: If we can't fit (the next) EA at all, and there are no prior valid
 * EAs (they would handled by 1: above) we return STATUS_BUFFER_OVERFLOW
 * and IoStatus.Information is set to what it needs to fit it.
 *
 * 3: If we finished with all EAs, we return the valid records
 * with status STATUS_NO_MORE_EAS.
 *
 *
 * ** But it actually wants everything in one-shot, fit all in the buffer
 * or return STATUS_BUFFER_OVERFLOW.
 */
NTSTATUS
query_ea(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_SUCCESS;

	PUCHAR  Buffer;
	ULONG   UserBufferLength;

	PUCHAR  UserEaList;
	ULONG   UserEaListLength;
	ULONG   UserEaIndex;
	BOOLEAN RestartScan;
	BOOLEAN ReturnSingleEntry;
	BOOLEAN IndexSpecified;
	FILE_FULL_EA_INFORMATION *previous_ea = NULL;
	uint64_t spaceused = 0;
	znode_t *zp = NULL, *xdzp = NULL;
	zfsvfs_t *zfsvfs = NULL;
	zap_cursor_t  zc;
	zap_attribute_t  za;
	int overflow = 0;

	struct vnode *vp = NULL, *xdvp = NULL;

	if (IrpSp->FileObject == NULL)
		return (STATUS_INVALID_PARAMETER);
	vp = IrpSp->FileObject->FsContext;
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	zp = VTOZ(vp);
	zfsvfs = zp->z_zfsvfs;

	UserBufferLength = IrpSp->Parameters.QueryEa.Length;
	UserEaList = IrpSp->Parameters.QueryEa.EaList;
	UserEaListLength = IrpSp->Parameters.QueryEa.EaListLength;
	UserEaIndex = IrpSp->Parameters.QueryEa.EaIndex;
	RestartScan = BooleanFlagOn(IrpSp->Flags, SL_RESTART_SCAN);
	ReturnSingleEntry = BooleanFlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);
	IndexSpecified = BooleanFlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED);

	dprintf("%s\n", __func__);

	Buffer = MapUserBuffer(Irp);

	if (UserBufferLength < sizeof (FILE_FULL_EA_INFORMATION)) {

		if (UserBufferLength == 0) {
			Irp->IoStatus.Information = 0;
			return (STATUS_NO_MORE_EAS);
		}

		Irp->IoStatus.Information = sizeof (FILE_FULL_EA_INFORMATION);
		return (STATUS_BUFFER_OVERFLOW);
		// Docs say to return too-small, but some callers get stuck
		// calling this in a cpu loop if we return it.
		return (STATUS_BUFFER_TOO_SMALL);
	}

	znode_t *xzp = NULL;
	FILE_GET_EA_INFORMATION *ea;
	int error = 0;

	uint64_t start_index = 0;

	zfs_ccb_t *zccb = IrpSp->FileObject->FsContext2;

	if (RestartScan) {
		start_index = 0;
		zccb->ea_index = 0;
	} else if (IndexSpecified)
		start_index = UserEaIndex;
	else
		start_index = zccb->ea_index;

	struct iovec iov;
	iov.iov_base =
	    (void *)Buffer;
	iov.iov_len = UserBufferLength;

	zfs_uio_t uio;
	zfs_uio_iovec_init(&uio, &iov, 1, 0,
	    UIO_SYSSPACE, UserBufferLength, 0);

	// Pass Flags along for ReturnSingleEntry, so
	// lets abuse uio->extflg - no idea what it is for
	// it got copied across to Windows so it's there.
	uio.uio_extflg = IrpSp->Flags;

	/* ********************** */
	if (UserEaList != NULL) {

		uint64_t offset = 0;
		uint64_t current_index = 0;

		do {
			/* bounds check: offset is on INPUT list */
			if (offset > UserEaListLength) {
				if (xdvp) VN_RELE(xdvp);
				return (STATUS_INVALID_PARAMETER);
			}

			ea = (FILE_GET_EA_INFORMATION *)&UserEaList[offset];

			if (offset + ea->EaNameLength > UserEaListLength) {
				if (xdvp) VN_RELE(xdvp);
				return (STATUS_INVALID_PARAMETER);
			}

			/* scan until we get to the index wanted */
			if (current_index >= start_index) {

				error = zpl_xattr_filldir(vp, &uio, ea->EaName,
				    ea->EaNameLength, &previous_ea);

				if (error == ENOENT)
					error = 0;
				else if (error != 0)
					break;

				if (ReturnSingleEntry) {
					current_index++;
					break;
				}
			}
			// if (overflow != 0)
			//	break;

			current_index++;
			offset += ea->NextEntryOffset;

			if (ea->NextEntryOffset == 0)
				break;

		} while (offset != 0);

		if (current_index >= start_index)
			zccb->ea_index = current_index;

		/* ********************** */
	} else {

		zfs_uio_setindex(&uio, start_index);
		Status = zpl_xattr_list(vp, &uio, (ssize_t *)&spaceused, NULL);
		zccb->ea_index = zfs_uio_index(&uio);
	}


out:

	if (xdvp) VN_RELE(xdvp);

	Irp->IoStatus.Information = spaceused;

	// Didn't fit even one
	if (overflow)
		Status = STATUS_BUFFER_OVERFLOW;
	else if (spaceused == 0 && Status == 0)
		Status = STATUS_NO_EAS_ON_FILE;

	return (Status);
}

/*
 * Receive an array of structs to set EAs, iterate until Next is null.
 */
NTSTATUS
set_ea(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	uint32_t input_len = IrpSp->Parameters.SetEa.Length;
	uint8_t *buffer = NULL;
	NTSTATUS Status = STATUS_SUCCESS;
	struct vnode *vp = NULL;
	zfs_ccb_t *zccb = NULL;

	if (IrpSp->FileObject == NULL)
		return (STATUS_INVALID_PARAMETER);

	vp = IrpSp->FileObject->FsContext;
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	zccb = IrpSp->FileObject->FsContext2;
	if (zccb == NULL)
		return (STATUS_INVALID_PARAMETER);

	dprintf("%s\n", __func__);

	if (input_len == 0)
		return (STATUS_INVALID_PARAMETER);

	mount_t *zmo = DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs;
	if (zmo != NULL &&
	    (zfsvfs = vfs_fsprivate(zmo)) != NULL &&
	    zfsvfs->z_rdonly)
		return (STATUS_MEDIA_WRITE_PROTECTED);

	// This magic is straight out of fastfat
	buffer = BufferUserBuffer(Irp, input_len);

	ULONG eaErrorOffset = 0;
	Status = vnode_apply_eas(vp, zccb,
	    (PFILE_FULL_EA_INFORMATION)buffer,
	    input_len, &eaErrorOffset);
	// (Information is ULONG_PTR; as win64 is a LLP64 platform,
	// ULONG isn't the right length.)
	Irp->IoStatus.Information = 0;
	if (!NT_SUCCESS(Status)) {
		dprintf("%s: failed vnode_apply_eas: 0x%lx\n",
		    __func__, Status);
		return (Status);
	}
	return (Status);
}

int
get_reparse_point_impl(znode_t *zp, char *buffer, size_t bufferlen,
    size_t *returnlen)
{
	int err = 0;
	if (zp->z_pflags & ZFS_REPARSE) {

		// Return the needed total size, but only copy as
		// much as we can fit.
		// WEIRDLY, Explorer will crash if we return
		// neededbytes in Information. It should be 0.
		if (zfsctl_is_node(zp)) {
			REPARSE_DATA_BUFFER *rdb = NULL;
			NTSTATUS Status;
			size_t size = 0;

			Status = zfsctl_get_reparse_point(zp, &rdb, &size);
			if (Status == 0 && bufferlen >= size)
				memcpy(buffer, rdb, size);
			if (returnlen)
				*returnlen = 0; // size
		} else {
			struct iovec iov;
			iov.iov_base = (void *)buffer;
			iov.iov_len = MIN(zp->z_size, bufferlen);

			zfs_uio_t uio;
			zfs_uio_iovec_init(&uio, &iov, 1, 0, UIO_SYSSPACE,
			    iov.iov_len, 0);
			err = zfs_readlink(ZTOV(zp), &uio, NULL);
			if (!err && returnlen)
				*returnlen = zp->z_size - zfs_uio_resid(&uio);
		}
	}
	return (err);
}

NTSTATUS
get_reparse_point(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_A_REPARSE_POINT;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	DWORD outlen = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
	void *buffer = Irp->AssociatedIrp.SystemBuffer;
	REPARSE_DATA_BUFFER *rdb = buffer;
	struct vnode *vp;
	DWORD reqlen;

	if (FileObject == NULL)
		return (STATUS_INVALID_PARAMETER);

	vp = FileObject->FsContext;
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	znode_t *zp = VTOZ(vp);

	if (vnode_islnk(vp)) {
		reqlen = offsetof(REPARSE_DATA_BUFFER,
		    GenericReparseBuffer.DataBuffer) + sizeof (uint32_t);

		if (outlen < reqlen) {
			Status = STATUS_BUFFER_OVERFLOW;
			goto end;
		}

		rdb->ReparseTag = IO_REPARSE_TAG_LX_SYMLINK;
		rdb->ReparseDataLength = offsetof(REPARSE_DATA_BUFFER,
		    GenericReparseBuffer.DataBuffer) + sizeof (uint32_t);
		rdb->Reserved = 0;

		*((uint32_t *)rdb->GenericReparseBuffer.DataBuffer) = 1;

		Irp->IoStatus.Information = reqlen;
		goto end;
	}

	Irp->IoStatus.Information = 0;
	if (zp->z_pflags & ZFS_REPARSE) {
		int err;
		size_t size = 0;

		err = get_reparse_point_impl(zp, buffer, outlen, &size);

		if (err)
			Status = STATUS_UNEXPECTED_IO_ERROR;

		if (outlen < size) {
			Status = STATUS_BUFFER_OVERFLOW;
		} else {
			Status = STATUS_SUCCESS;
			Irp->IoStatus.Information = size;

		}
	}

end:

	dprintf("%s: returning 0x%lx\n", __func__, Status);
	return (Status);
}

NTSTATUS
set_reparse_point(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	DWORD inlen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	void *buffer = Irp->AssociatedIrp.SystemBuffer;
	REPARSE_DATA_BUFFER *rdb = buffer;

	if (!FileObject || !IrpSp->FileObject->FsContext)
		return (STATUS_INVALID_PARAMETER);

	struct vnode *vp = IrpSp->FileObject->FsContext;

	if (!vp || !VTOZ(vp))
		return (STATUS_INVALID_PARAMETER);

	if (Irp->UserBuffer)
		return (STATUS_INVALID_PARAMETER);

	if (inlen < sizeof (ULONG)) {
		return (STATUS_INVALID_BUFFER_SIZE);
	}

	Status = FsRtlValidateReparsePointBuffer(inlen, rdb);
	if (!NT_SUCCESS(Status)) {
		dprintf("FsRtlValidateReparsePointBuffer returned %08lx\n",
		    Status);
		return (Status);
	}

	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	if (zfsctl_is_node(zp))
		return (zfsctl_set_reparse_point(zp, rdb, inlen));

	if (zfsvfs == NULL)
		return (STATUS_INVALID_PARAMETER);

	if (zfsvfs->z_rdonly)
		return (STATUS_MEDIA_WRITE_PROTECTED);

	znode_t *dzp = NULL;
	int error;

	vnode_t *dvp = NULL;
	dvp = zfs_parent(vp);
	dzp = VTOZ(dvp);

	// winbtrfs' test/exe will trigger this, add code here.
	// (asked to create reparse point on already reparse point)
	if (zp->z_pflags & ZFS_REPARSE) {
//		DbgBreakPoint();
	}
	// error = zfs_symlink(dzp, , vattr_t * vap, char *link,
	// 	znode_t * *zpp, cred_t * cr, int flags)


	// Like zfs_symlink, write the data as SA attribute.
	dmu_tx_t	*tx;
	boolean_t	fuid_dirtied;

	// Set flags to indicate we are reparse point
	zp->z_pflags |= ZFS_REPARSE;

	// Start TX and save FLAGS, SIZE and SYMLINK to disk.
	// This code should probably call zfs_symlink()
top:
	tx = dmu_tx_create(zfsvfs->z_os);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, MAX(1, inlen));
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, NULL);
	dmu_tx_hold_sa_create(tx, inlen);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);

	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		if (error == ERESTART)
			goto top;
		goto out;
	}

	(void) sa_update(zp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
	    &zp->z_pflags, sizeof (zp->z_pflags), tx);

	mutex_enter(&zp->z_lock);
	if (zp->z_is_sa)
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    buffer, inlen, tx);
	else
		zfs_sa_symlink(zp, buffer, inlen, tx);
	mutex_exit(&zp->z_lock);

	zp->z_size = inlen;
	(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx);

	dmu_tx_commit(tx);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zfsvfs->z_log, 0);

out:
	VN_RELE(dvp);

	dprintf("%s: returning 0x%lx\n", __func__, Status);

	return (Status);
}

NTSTATUS
delete_reparse_point(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	DWORD inlen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	void *buffer = Irp->AssociatedIrp.SystemBuffer;
	REPARSE_DATA_BUFFER *rdb = buffer;
	struct vnode *vp = IrpSp->FileObject->FsContext;

	if (!FileObject)
		return (STATUS_INVALID_PARAMETER);

	if (Irp->UserBuffer)
		return (STATUS_INVALID_PARAMETER);

	if (inlen < sizeof (ULONG)) {
		return (STATUS_INVALID_BUFFER_SIZE);
	}

	if (inlen < offsetof(REPARSE_DATA_BUFFER,
	    GenericReparseBuffer.DataBuffer))
		return (STATUS_INVALID_PARAMETER);

	if (rdb->ReparseDataLength > 0)
		return (STATUS_INVALID_PARAMETER);

	if (VN_HOLD(vp) != 0)
		return (STATUS_INVALID_PARAMETER);

	znode_t *zp = VTOZ(vp);

	if (zfsctl_is_node(zp)) {
		VN_RELE(vp);
		return (zfsctl_delete_reparse_point(zp));
	}

	// STATUS_IO_REPARSE_TAG_MISMATCH
	// rdb->ReparseTag != zp->ReparseTag


	// Like zfs_symlink, write the data as SA attribute.
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	znode_t *dzp = NULL;
	uint64_t parent;
	int error;

	// Fetch parent
	VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (parent)) == 0);
	error = zfs_zget(zfsvfs, parent, &dzp);
	if (error) {
		Status = STATUS_INVALID_PARAMETER;
		goto out;
	}

	dmu_tx_t	*tx;
	boolean_t	fuid_dirtied;

	// Remove flags to indicate we are reparse point
	zp->z_pflags &= ~ZFS_REPARSE;

	// Start TX and save FLAGS, SIZE and SYMLINK to disk.
	// This code should probably call zfs_symlink()
top:
	tx = dmu_tx_create(zfsvfs->z_os);
	fuid_dirtied = zfsvfs->z_fuid_dirty;

	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, NULL); // name
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);

	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		if (error == ERESTART)
			goto top;
		goto out;
	}

	mutex_enter(&zp->z_lock);

	(void) sa_update(zp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
	    &zp->z_pflags, sizeof (zp->z_pflags), tx);

	if (zp->z_is_sa)
		error = sa_remove(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    tx);
	else
		zfs_sa_symlink(zp, buffer, 0, tx);

	zp->z_size = 0;	// If dir size > 2 -> ENOTEMPTY
	(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx);

	mutex_exit(&zp->z_lock);

	dmu_tx_commit(tx);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zfsvfs->z_log, 0);

out:
	if (dzp != NULL)
		zrele(dzp);
	VN_RELE(vp);

	dprintf("%s: returning 0x%lx\n", __func__, Status);

	return (Status);
}

NTSTATUS
create_or_get_object_id(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	DWORD inlen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	void *buffer = Irp->AssociatedIrp.SystemBuffer;
	FILE_OBJECTID_BUFFER *fob = buffer;

	if (!FileObject)
		return (STATUS_INVALID_PARAMETER);

	if (!fob || inlen < sizeof (FILE_OBJECTID_BUFFER)) {
		Irp->IoStatus.Information = sizeof (FILE_OBJECTID_BUFFER);
		return (STATUS_BUFFER_OVERFLOW);
	}

	struct vnode *vp = IrpSp->FileObject->FsContext;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	// ObjectID is 16 bytes to identify the file
	// Should we do endian work here?
	// znode id + pool guid
	RtlCopyMemory(&fob->ObjectId[0], &zp->z_id, sizeof (UINT64));
	uint64_t guid = dmu_objset_fsid_guid(zfsvfs->z_os);
	RtlCopyMemory(&fob->ObjectId[sizeof (UINT64)], &guid, sizeof (UINT64));
	RtlZeroMemory(fob->ExtendedInfo, sizeof (fob->ExtendedInfo));

	Irp->IoStatus.Information = sizeof (FILE_OBJECTID_BUFFER);
	Status = STATUS_SUCCESS;
	return (Status);
}

NTSTATUS
set_sparse(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	uint64_t datalen =
	    IrpSp->Parameters.FileSystemControl.InputBufferLength;
	boolean_t set = B_TRUE;
	struct vnode *vp;
	znode_t *zp;

	if (!IrpSp->FileObject)
		return (STATUS_INVALID_PARAMETER);

	/* Buffer is optional */
	if (Irp->AssociatedIrp.SystemBuffer != NULL &&
	    datalen < sizeof (FILE_SET_SPARSE_BUFFER))
		return (STATUS_INVALID_PARAMETER);

	/* if given */
	FILE_SET_SPARSE_BUFFER *fssb = Irp->AssociatedIrp.SystemBuffer;

	vp = IrpSp->FileObject->FsContext;
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	zp = VTOZ(vp);
	if (zp == NULL)
		return (STATUS_INVALID_PARAMETER);

	if (fssb != NULL)
		set = fssb->SetSparse;

	/* We should at least send events */

	return (STATUS_SUCCESS);
}

#ifndef FSCTL_GET_INTEGRITY_INFORMATION_BUFFER
typedef struct _FSCTL_GET_INTEGRITY_INFORMATION_BUFFER {
    USHORT ChecksumAlgorithm;
    USHORT Reserved;
    ULONG Flags;
    ULONG ChecksumChunkSizeInBytes;
    ULONG ClusterSizeInBytes;
} FSCTL_GET_INTEGRITY_INFORMATION_BUFFER,
	*PFSCTL_GET_INTEGRITY_INFORMATION_BUFFER;
#endif

NTSTATUS
get_integrity_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	uint64_t datalen =
	    IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	FSCTL_GET_INTEGRITY_INFORMATION_BUFFER *fgiib;

	fgiib = (FSCTL_GET_INTEGRITY_INFORMATION_BUFFER *)
	    Irp->AssociatedIrp.SystemBuffer;

	if (!IrpSp->FileObject)
		return (STATUS_INVALID_PARAMETER);

	if (!fgiib ||
	    datalen < sizeof (FSCTL_GET_INTEGRITY_INFORMATION_BUFFER))
		return (STATUS_INVALID_PARAMETER);

	fgiib->ChecksumAlgorithm = 0;
	fgiib->Reserved = 0;
	fgiib->Flags = 0;
	fgiib->ChecksumChunkSizeInBytes = 512;
	fgiib->ClusterSizeInBytes = 512;

	Irp->IoStatus.Information =
	    sizeof (FSCTL_GET_INTEGRITY_INFORMATION_BUFFER);

	return (STATUS_SUCCESS);
}

#ifndef FSCTL_SET_INTEGRITY_INFORMATION_BUFFER
typedef struct _FSCTL_SET_INTEGRITY_INFORMATION_BUFFER {
    USHORT ChecksumAlgorithm;
    USHORT Reserved;
    ULONG Flags;
} FSCTL_SET_INTEGRITY_INFORMATION_BUFFER,
	*PFSCTL_SET_INTEGRITY_INFORMATION_BUFFER;
#endif

NTSTATUS
set_integrity_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	uint64_t datalen =
	    IrpSp->Parameters.DeviceIoControl.InputBufferLength;

	if (!IrpSp->FileObject)
		return (STATUS_INVALID_PARAMETER);

	if (!Irp->AssociatedIrp.SystemBuffer ||
	    datalen < sizeof (FSCTL_SET_INTEGRITY_INFORMATION_BUFFER))
		return (STATUS_INVALID_PARAMETER);

	return (STATUS_SUCCESS);
}

NTSTATUS
duplicate_extents_to_file(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp, boolean_t extended)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
	PFILE_OBJECT sourcefo = NULL;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	DWORD datalen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	void *buffer = Irp->AssociatedIrp.SystemBuffer;
	struct vnode *outvp = NULL, *invp = NULL;
	znode_t *outzp = NULL, *inzp = NULL;
	zfsvfs_t *zfsvfs;
	uint64_t inoff, outoff;
	uint64_t length;

	dprintf("%s\n", extended ? "duplicate_extents_to_file_ex" :
	    "duplicate_extents_to_file");

	if (!FileObject)
		return (STATUS_INVALID_PARAMETER);
	outvp = FileObject->FsContext;
	if (outvp == NULL)
		return (STATUS_INVALID_PARAMETER);

	outzp = VTOZ(outvp);
	if (outzp == NULL)
		return (STATUS_INVALID_PARAMETER);

	zfsvfs = outzp->z_zfsvfs;

	if (vfs_isrdonly(zfsvfs->z_vfs))
		return (STATUS_MEDIA_WRITE_PROTECTED);

#if 0
	if (Irp->RequestorMode == UserMode &&
	    !(ccb->access & FILE_WRITE_DATA)) {
		return (STATUS_ACCESS_DENIED);
	}
#endif
	if (!vnode_isreg(outvp) && !vnode_islnk(outvp))
		return (STATUS_INVALID_PARAMETER);


	if (extended) {
#ifndef _DUPLICATE_EXTENTS_DATA_EX
		typedef struct _DUPLICATE_EXTENTS_DATA_EX {
		    SIZE_T Size;
		    HANDLE FileHandle;
		    LARGE_INTEGER SourceFileOffset;
		    LARGE_INTEGER TargetFileOffset;
		    LARGE_INTEGER ByteCount;
		    ULONG Flags;
		} DUPLICATE_EXTENTS_DATA_EX, *PDUPLICATE_EXTENTS_DATA_EX;
#endif
		DUPLICATE_EXTENTS_DATA_EX *dede =
		    (DUPLICATE_EXTENTS_DATA_EX *)buffer;

		if (!buffer || datalen < sizeof (DUPLICATE_EXTENTS_DATA_EX) ||
		    dede->Size != sizeof (DUPLICATE_EXTENTS_DATA_EX))
			return (STATUS_BUFFER_TOO_SMALL);

		if (dede->ByteCount.QuadPart == 0)
			return (STATUS_SUCCESS);

		inoff = dede->SourceFileOffset.QuadPart;
		outoff = dede->TargetFileOffset.QuadPart;
		length = dede->ByteCount.QuadPart;
		Status = ObReferenceObjectByHandle(dede->FileHandle, 0,
		    *IoFileObjectType, Irp->RequestorMode,
		    (void **)&sourcefo, NULL);

	} else {
#ifndef _DUPLICATE_EXTENTS_DATA
		typedef struct _DUPLICATE_EXTENTS_DATA {
		    HANDLE FileHandle;
		    LARGE_INTEGER SourceFileOffset;
		    LARGE_INTEGER TargetFileOffset;
		    LARGE_INTEGER ByteCount;
		} DUPLICATE_EXTENTS_DATA, *PDUPLICATE_EXTENTS_DATA;
#endif

		DUPLICATE_EXTENTS_DATA *ded =
		    (DUPLICATE_EXTENTS_DATA *)buffer;
		if (!buffer || datalen < sizeof (DUPLICATE_EXTENTS_DATA))
			return (STATUS_BUFFER_TOO_SMALL);
		if (ded->ByteCount.QuadPart == 0)
			return (STATUS_SUCCESS);

		inoff = ded->SourceFileOffset.QuadPart;
		outoff = ded->TargetFileOffset.QuadPart;
		length = ded->ByteCount.QuadPart;
		Status = ObReferenceObjectByHandle(ded->FileHandle, 0,
		    *IoFileObjectType, Irp->RequestorMode,
		    (void **)&sourcefo, NULL);
	}

	if (!NT_SUCCESS(Status)) {
		dprintf("ObReferenceObjectByHandle returned %08lx\n", Status);
		return (Status);
	}

	invp = sourcefo->FsContext;
	if (invp == NULL || VN_HOLD(invp) != 0) {
		invp = NULL;
		Status = STATUS_INVALID_PARAMETER;
		goto out;
	}
	/* Holding invp */

	inzp = VTOZ(invp);
	if (inzp == NULL) {
		Status = STATUS_INVALID_PARAMETER;
		goto out;
	}

	/* From here, release sourcefo */

	/*
	 * zfs_clone_range(znode_t *inzp, uint64_t *inoffp, znode_t *outzp,
	 *    uint64_t *outoffp, uint64_t *lenp, cred_t *cr)
	 */

	Status = zfs_clone_range(inzp, &inoff, outzp, &outoff,
	    &length, NULL);

out:
	ObDereferenceObject(sourcefo);
	if (invp != NULL)
		VN_RELE(invp);
	return (SET_ERROR(Status));
}

/*
 * Thought this was needed for clone, but it is not
 * but keeping it around in case one day we will need it
 */
#if 0
#ifndef FILE_REGION_INFO
typedef struct _FILE_REGION_INFO {
    LONGLONG FileOffset;
    LONGLONG Length;
    ULONG Usage;
    ULONG Reserved;
} FILE_REGION_INFO, *PFILE_REGION_INFO;
#endif

#ifndef FILE_REGION_OUTPUT
typedef struct _FILE_REGION_OUTPUT {
    ULONG Flags;
    ULONG TotalRegionEntryCount;
    ULONG RegionEntryCount;
    ULONG Reserved;
    FILE_REGION_INFO Region[1];
} FILE_REGION_OUTPUT, *PFILE_REGION_OUTPUT;
#endif

#ifndef FILE_REGION_USAGE_VALID_CACHED_DATA
#define	FILE_REGION_USAGE_VALID_CACHED_DATA	0x00000001
#endif
#endif

NTSTATUS
query_file_regions(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
#if 0
	uint64_t inlen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	uint64_t outlen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

	if (!IrpSp->FileObject)
		return (STATUS_INVALID_PARAMETER);

	if (Irp->AssociatedIrp.SystemBuffer != NULL &&
	    inlen < sizeof (FILE_REGION_INFO)) {
		return (STATUS_BUFFER_TOO_SMALL);
	}

	if (Irp->AssociatedIrp.SystemBuffer == NULL ||
	    outlen < sizeof (FILE_REGION_OUTPUT)) {
		Irp->IoStatus.Information = sizeof (FILE_REGION_OUTPUT);
		return (STATUS_BUFFER_TOO_SMALL);
	}

	struct vnode *vp = IrpSp->FileObject->FsContext;
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	znode_t *zp = VTOZ(vp);
	if (zp == NULL)
		return (STATUS_INVALID_PARAMETER);

	if (inlen == 0) {

	} else {
		FILE_REGION_INFO *fri =
		    (FILE_REGION_INFO *)Irp->AssociatedIrp.SystemBuffer;
		if (fri->FileOffset > INT64_MAX || fri->Length > INT64_MAX)
			return (STATUS_INVALID_PARAMETER);
		if ((fri->FileOffset + fri->Length) > INT64_MAX)
			return (STATUS_INVALID_PARAMETER);
		if ((fri->Usage & 3) == 0)
			return (STATUS_INVALID_PARAMETER);
		fri->Reserved = 0;
	}


	FILE_REGION_OUTPUT *fro =
	    (FILE_REGION_OUTPUT *)Irp->AssociatedIrp.SystemBuffer;

	fro->Flags = 0;
	fro->TotalRegionEntryCount = 1;
	fro->RegionEntryCount = 0;
	fro->Reserved = 0;

	Irp->IoStatus.Information = sizeof (FILE_REGION_OUTPUT);
	fro->RegionEntryCount = 1;
	fro->Region[0].FileOffset = 0;
	fro->Region[0].Length = zp->z_size;
	fro->Region[0].Usage = FILE_REGION_USAGE_VALID_CACHED_DATA;
	fro->Region[0].Reserved = 0;

	return (STATUS_SUCCESS);
#endif
	return (STATUS_INVALID_PARAMETER);
}

typedef BOOLEAN
	(__stdcall *tFsRtlCheckLockForOplockRequest)
	(PFILE_LOCK FileLock, PLARGE_INTEGER AllocationSize);
typedef BOOLEAN
	(__stdcall *tFsRtlAreThereCurrentOrInProgressFileLocks)
	(PFILE_LOCK FileLock);
tFsRtlCheckLockForOplockRequest fFsRtlCheckLockForOplockRequest = NULL;
tFsRtlAreThereCurrentOrInProgressFileLocks \
    fFsRtlAreThereCurrentOrInProgressFileLocks = NULL;

NTSTATUS
request_oplock(PDEVICE_OBJECT DeviceObject, PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = 0;
	uint32_t fsctl = IrpSp->Parameters.FileSystemControl.FsControlCode;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	PREQUEST_OPLOCK_INPUT_BUFFER buf = NULL;
	boolean_t oplock_request = FALSE, oplock_ack = FALSE;
	ULONG oplock_count = 0;
	PIRP Irp = *PIrp;
	int error = 0;

	if (FileObject == NULL)
		return (STATUS_INVALID_PARAMETER);

	struct vnode *vp = IrpSp->FileObject->FsContext;

	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);  // This returns EIO if fail
	/* HOLD count, no returns from here. */

	if (VN_HOLD(vp) != 0) {
		Status = STATUS_INVALID_PARAMETER;
		goto exit;
	}

	if (!vnode_isreg(vp) && !vnode_isdir(vp)) {
		Status = STATUS_INVALID_PARAMETER;
		goto out;
	}

	if (fsctl == FSCTL_REQUEST_OPLOCK) {
		if (IrpSp->Parameters.FileSystemControl.InputBufferLength <
		    sizeof (REQUEST_OPLOCK_INPUT_BUFFER)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			goto out;
		}
		if (IrpSp->Parameters.FileSystemControl.OutputBufferLength <
		    sizeof (REQUEST_OPLOCK_OUTPUT_BUFFER)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			goto out;
		}
		buf = Irp->AssociatedIrp.SystemBuffer;

		// flags are mutually exclusive
		if (buf->Flags & REQUEST_OPLOCK_INPUT_FLAG_REQUEST &&
		    buf->Flags & REQUEST_OPLOCK_INPUT_FLAG_ACK) {
			Status = STATUS_INVALID_PARAMETER;
			goto out;
		}

		oplock_request = buf->Flags & REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
		oplock_ack = buf->Flags & REQUEST_OPLOCK_INPUT_FLAG_ACK;

		if (!oplock_request && !oplock_ack) {
			Status = STATUS_INVALID_PARAMETER;
			goto out;
		}
	}

	boolean_t shared_request = (fsctl == FSCTL_REQUEST_OPLOCK_LEVEL_2) ||
	    (fsctl == FSCTL_REQUEST_OPLOCK &&
	    !(buf->RequestedOplockLevel & OPLOCK_LEVEL_CACHE_WRITE));

	if (vnode_isdir(vp) && (fsctl != FSCTL_REQUEST_OPLOCK ||
	    !shared_request)) {
		dprintf("oplock requests on directories can only be "
		    "for read or read-handle oplocks\n");
		Status = STATUS_INVALID_PARAMETER;
		goto out;
	}

	// research this
	// ExAcquireResourceSharedLite(&Vcb->tree_lock, true);

	ExAcquireResourceExclusiveLite(vp->FileHeader.Resource, TRUE);

	// Does windows have something like "attribute availability (function)"
	// for dynamic checks?
	// FastFat example uses #ifdef NTDDI-VERSION which is compile time,
	// we should look into
	// winbtrfs:
	// RtlInitUnicodeString(&name, L"FsRtlCheckLockForOplockRequest");
	// fFsRtlCheckLockForOplockRequest =
	// 	(tFsRtlCheckLockForOplockRequest)
	// 	MmGetSystemRoutineAddress(&name);
	// move me to init place
	static int firstrun = 1;
	if (firstrun) {
		UNICODE_STRING name;
		RtlInitUnicodeString(&name, L"FsRtlCheckLockForOplockRequest");
		fFsRtlCheckLockForOplockRequest =
		    (tFsRtlCheckLockForOplockRequest)
		    MmGetSystemRoutineAddress(&name);
		RtlInitUnicodeString(&name,
		    L"FsRtlAreThereCurrentOrInProgressFileLocks");
		fFsRtlAreThereCurrentOrInProgressFileLocks =
		    (tFsRtlAreThereCurrentOrInProgressFileLocks)
		    MmGetSystemRoutineAddress(&name);
		firstrun = 0;
	}

	if (fsctl == FSCTL_REQUEST_OPLOCK_LEVEL_1 ||
	    fsctl == FSCTL_REQUEST_BATCH_OPLOCK ||
	    fsctl == FSCTL_REQUEST_FILTER_OPLOCK ||
	    fsctl == FSCTL_REQUEST_OPLOCK_LEVEL_2 || oplock_request) {
		if (shared_request) {
			if (vnode_isreg(vp)) {
				if (fFsRtlCheckLockForOplockRequest)
					oplock_count =
					    !fFsRtlCheckLockForOplockRequest(
					    &vp->lock,
					    &vp->FileHeader.AllocationSize);
				else if
				    (fFsRtlAreThereCurrentOrInProgressFileLocks)
		oplock_count =
		    fFsRtlAreThereCurrentOrInProgressFileLocks(&vp->lock);
				else
					oplock_count =
					    FsRtlAreThereCurrentFileLocks(&vp->
					    lock);
			}
		} else
			oplock_count = vnode_iocount(vp);
		}

		zfs_ccb_t *zccb = FileObject->FsContext2;

		if (zccb != NULL &&
		    zccb->magic == ZFS_CCB_MAGIC &&
		    zccb->deleteonclose) {

			if ((fsctl == FSCTL_REQUEST_FILTER_OPLOCK ||
			    fsctl == FSCTL_REQUEST_BATCH_OPLOCK ||
			    (fsctl == FSCTL_REQUEST_OPLOCK &&
			    buf->RequestedOplockLevel &
			    OPLOCK_LEVEL_CACHE_HANDLE))) {
				ExReleaseResourceLite(vp->FileHeader.Resource);
				// ExReleaseResourceLite(&Vcb->tree_lock);
				Status = STATUS_DELETE_PENDING;
				goto out;
			}
		}

	// This will complete the IRP as well.
	// How to stop dispatcher from completing?
	Status = FsRtlOplockFsctrl(vp_oplock(vp), Irp, oplock_count);
	*PIrp = NULL; // Don't complete.

	// *Pirp = NULL;

	// fcb->Header.IsFastIoPossible = fast_io_possible(fcb);

	ExReleaseResourceLite(vp->FileHeader.Resource);
	// ExReleaseResourceLite(&Vcb->tree_lock);

out:
	VN_RELE(vp);
exit:
	zfs_exit(zfsvfs, FTAG);

	return (Status);
}

NTSTATUS
user_fs_request(PDEVICE_OBJECT DeviceObject, PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	PIRP Irp = *PIrp;

	switch (IrpSp->Parameters.FileSystemControl.FsControlCode) {
	case FSCTL_LOCK_VOLUME:
		dprintf("    FSCTL_LOCK_VOLUME\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_UNLOCK_VOLUME:
		dprintf("    FSCTL_UNLOCK_VOLUME\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_DISMOUNT_VOLUME:
		dprintf("    FSCTL_DISMOUNT_VOLUME\n");
		break;
	case FSCTL_MARK_VOLUME_DIRTY:
		dprintf("    FSCTL_MARK_VOLUME_DIRTY\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_IS_VOLUME_MOUNTED:
		dprintf("    FSCTL_IS_VOLUME_MOUNTED\n");
		Status = STATUS_SUCCESS;
		{
			Status = STATUS_VOLUME_NOT_MOUNTED;
			mount_t *zmo;
			zmo = DeviceObject->DeviceExtension;
			zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
			if (zfsvfs) {
				Status = STATUS_VOLUME_MOUNTED;
				if (zfsvfs->z_unmounted)
					Status = STATUS_VERIFY_REQUIRED;
			}
		}
		break;
	case FSCTL_SET_COMPRESSION:
		dprintf("    FSCTL_SET_COMPRESSION\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_IS_PATHNAME_VALID:
		dprintf("    FSCTL_IS_PATHNAME_VALID\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_GET_RETRIEVAL_POINTERS:
		dprintf("    FSCTL_GET_RETRIEVAL_POINTERS\n");
		Status = STATUS_INVALID_PARAMETER;
		break;
	case FSCTL_IS_VOLUME_DIRTY:
		dprintf("    FSCTL_IS_VOLUME_DIRTY\n");
		PULONG VolumeState;

		VolumeState = MapUserBuffer(Irp);

		if (VolumeState == NULL) {
			Status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		if (IrpSp->Parameters.FileSystemControl.OutputBufferLength <
		    sizeof (ULONG)) {
			Status = STATUS_INVALID_PARAMETER;
			break;
		}

		*VolumeState = 0;
		if (0)
			SetFlag(*VolumeState, VOLUME_IS_DIRTY);
		Irp->IoStatus.Information = sizeof (ULONG);
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_GET_REPARSE_POINT:
		dprintf("    FSCTL_GET_REPARSE_POINT\n");
		Status = get_reparse_point(DeviceObject, Irp, IrpSp);
		break;
	case FSCTL_SET_REPARSE_POINT:
		dprintf("    FSCTL_SET_REPARSE_POINT\n");
		Status = set_reparse_point(DeviceObject, Irp, IrpSp);
		break;
	case FSCTL_DELETE_REPARSE_POINT:
		dprintf("    FSCTL_DELETE_REPARSE_POINT\n");
		Status = delete_reparse_point(DeviceObject, Irp, IrpSp);
		break;
	case FSCTL_CREATE_OR_GET_OBJECT_ID:
		dprintf("    FSCTL_CREATE_OR_GET_OBJECT_ID\n");
		Status = create_or_get_object_id(DeviceObject, Irp, IrpSp);
		break;
	case FSCTL_REQUEST_OPLOCK:
		dprintf("    FSCTL_REQUEST_OPLOCK: \n");
		Status = request_oplock(DeviceObject, PIrp, IrpSp);
		break;
	case FSCTL_FILESYSTEM_GET_STATISTICS:
		dprintf("    FSCTL_FILESYSTEM_GET_STATISTICS: \n");
		FILESYSTEM_STATISTICS *fss = Irp->AssociatedIrp.SystemBuffer;

		// btrfs: This is hideously wrong, but at least it stops SMB
		// from breaking

		if (IrpSp->Parameters.FileSystemControl.OutputBufferLength <
		    sizeof (FILESYSTEM_STATISTICS))
			return (STATUS_BUFFER_TOO_SMALL);

		memset(fss, 0, sizeof (FILESYSTEM_STATISTICS));

		fss->Version = 1;
		fss->FileSystemType = FILESYSTEM_STATISTICS_TYPE_NTFS;
		fss->SizeOfCompleteStructure = sizeof (FILESYSTEM_STATISTICS);

		Irp->IoStatus.Information = sizeof (FILESYSTEM_STATISTICS);
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_QUERY_DEPENDENT_VOLUME:
		dprintf("    FSCTL_QUERY_DEPENDENT_VOLUME: \n");
		STORAGE_QUERY_DEPENDENT_VOLUME_REQUEST *req =
		    Irp->AssociatedIrp.SystemBuffer;
		dprintf("RequestLevel %ld: RequestFlags 0x%lx\n",
		    req->RequestLevel, req->RequestFlags);
// #define	QUERY_DEPENDENT_VOLUME_REQUEST_FLAG_HOST_VOLUMES    0x1
// #define	QUERY_DEPENDENT_VOLUME_REQUEST_FLAG_GUEST_VOLUMES   0x2
		STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY *lvl1 =
		    Irp->AssociatedIrp.SystemBuffer;
		STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY *lvl2 =
		    Irp->AssociatedIrp.SystemBuffer;

		Status = STATUS_NOT_SUPPORTED;
		return (Status);

		switch (req->RequestLevel) {
		case 1:
			if (IrpSp->
			    Parameters.FileSystemControl.OutputBufferLength <
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY))
				return (STATUS_BUFFER_TOO_SMALL);
			memset(lvl1, 0,
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY));
			lvl1->EntryLength =
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY);
			Irp->IoStatus.Information =
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY);
			Status = STATUS_SUCCESS;
			break;
		case 2:
			if (IrpSp->
			    Parameters.FileSystemControl.OutputBufferLength <
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY))
				return (STATUS_BUFFER_TOO_SMALL);
			memset(lvl2, 0,
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY));
			lvl2->EntryLength =
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY);
			Irp->IoStatus.Information =
			    sizeof (STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY);
			Status = STATUS_SUCCESS;
			break;
		default:
			Status = STATUS_INVALID_PARAMETER;
			break;
		}
		break;

	case FSCTL_SET_SPARSE:
		dprintf("    FSCTL_SET_SPARSE\n");
		Status = set_sparse(DeviceObject, Irp, IrpSp);
		break;

#ifndef FSCTL_GET_INTEGRITY_INFORMATION
#define	FSCTL_GET_INTEGRITY_INFORMATION CTL_CODE(FILE_DEVICE_FILE_SYSTEM, \
	159, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef FSCTL_SET_INTEGRITY_INFORMATION
#define	FSCTL_SET_INTEGRITY_INFORMATION CTL_CODE(FILE_DEVICE_FILE_SYSTEM, \
	160, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#endif
	case FSCTL_GET_INTEGRITY_INFORMATION:
		dprintf("    FSCTL_GET_INTEGRITY_INFORMATION_BUFFER\n");
		Status = get_integrity_information(DeviceObject, Irp, IrpSp);
		break;

	case FSCTL_SET_INTEGRITY_INFORMATION:
		dprintf("    FSCTL_SET_INTEGRITY_INFORMATION_BUFFER\n");
		Status = set_integrity_information(DeviceObject, Irp, IrpSp);
		break;

#ifndef FSCTL_DUPLICATE_EXTENTS_TO_FILE
#define	FSCTL_DUPLICATE_EXTENTS_TO_FILE	CTL_CODE(FILE_DEVICE_FILE_SYSTEM, \
	209, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif
	case FSCTL_DUPLICATE_EXTENTS_TO_FILE:
		dprintf("    FSCTL_DUPLICATE_EXTENTS_TO_FILE\n");
		Status = duplicate_extents_to_file(DeviceObject, Irp, IrpSp,
		    FALSE);
		break;

	case FSCTL_DUPLICATE_EXTENTS_TO_FILE_EX:
		dprintf("    FSCTL_DUPLICATE_EXTENTS_TO_FILE_EX\n");
		Status = duplicate_extents_to_file(DeviceObject, Irp, IrpSp,
		    TRUE);
		break;

#ifndef FSCTL_QUERY_FILE_REGIONS
#define	FSCTL_QUERY_FILE_REGIONS CTL_CODE(FILE_DEVICE_FILE_SYSTEM, \
	161, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
	case FSCTL_QUERY_FILE_REGIONS:
		dprintf("    FSCTL_QUERY_FILE_REGIONS\n");
		Status = query_file_regions(DeviceObject, Irp, IrpSp);
		break;

	case FSCTL_ZFS_VOLUME_MOUNTPOINT:
		dprintf("    FSCTL_ZFS_VOLUME_MOUNTPOINT\n");
		Status = fsctl_zfs_volume_mountpoint(DeviceObject, Irp, IrpSp);
		break;

	case FSCTL_READ_FILE_USN_DATA:
		dprintf("    FSCTL_READ_FILE_USN_DATA\n");

		Status = STATUS_INVALID_DEVICE_REQUEST;
		break;

	case FSCTL_QUERY_PERSISTENT_VOLUME_STATE:
		dprintf("    FSCTL_QUERY_PERSISTENT_VOLUME_STATE\n");
		PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
		ULONG InputBufferLength =
		    IrpSp->Parameters.FileSystemControl.InputBufferLength;
		ULONG OutputBufferLength =
		    IrpSp->Parameters.FileSystemControl.OutputBufferLength;
		PFILE_FS_PERSISTENT_VOLUME_INFORMATION Info;

		if (NULL == Buffer)
			return (STATUS_INVALID_PARAMETER);

		if (sizeof (FILE_FS_PERSISTENT_VOLUME_INFORMATION) >
		    InputBufferLength ||
		    sizeof (FILE_FS_PERSISTENT_VOLUME_INFORMATION) >
		    OutputBufferLength)
			return (STATUS_BUFFER_TOO_SMALL);

		Info = Buffer;
#if 0
		if (1 != Info->Version ||
		    !FlagOn(Info->FlagMask,
		    PERSISTENT_VOLUME_STATE_SHORT_NAME_CREATION_DISABLED))
			return (STATUS_INVALID_PARAMETER);
#endif
		RtlZeroMemory(Info,
		    sizeof (FILE_FS_PERSISTENT_VOLUME_INFORMATION));
		Info->VolumeFlags =
		    PERSISTENT_VOLUME_STATE_SHORT_NAME_CREATION_DISABLED;
		Irp->IoStatus.Information =
		    sizeof (FILE_FS_PERSISTENT_VOLUME_INFORMATION);

		Status = STATUS_SUCCESS;
		break;

	case FSCTL_SET_ZERO_DATA:
		dprintf("    FSCTL_SET_ZERO_DATA\n");
		Status = fsctl_set_zero_data(DeviceObject, Irp, IrpSp);
		break;

	default:
		dprintf("* %s: unknown class 0x%lx\n", __func__,
		    IrpSp->Parameters.FileSystemControl.FsControlCode);
		break;
	}

	return (Status);
}

NTSTATUS
query_directory_FileFullDirectoryInformation(PDEVICE_OBJECT DeviceObject,
    PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	// FILE_FULL_DIR_INFORMATION *outptr = Irp->UserBuffer;
	int flag_index_specified =
	    IrpSp->Flags & SL_INDEX_SPECIFIED ? 1 : 0;
	int flag_restart_scan =
	    IrpSp->Flags & SL_RESTART_SCAN ? 1 : 0;
	int flag_return_single_entry =
	    IrpSp->Flags & SL_RETURN_SINGLE_ENTRY ? 1 : 0;
	int ret;
	boolean_t initial = B_FALSE;
	mount_t *zmo;
	zfsvfs_t *zfsvfs;
	NTSTATUS Status = STATUS_NO_SUCH_FILE;

	if ((Irp->UserBuffer == NULL && Irp->MdlAddress == NULL) ||
	    IrpSp->Parameters.QueryDirectory.Length <= 0)
		return (STATUS_INSUFFICIENT_RESOURCES);

	if (IrpSp->FileObject == NULL ||
	    IrpSp->FileObject->FsContext == NULL || // vnode
	    IrpSp->FileObject->FsContext2 == NULL)  // ccb
		return (STATUS_INVALID_PARAMETER);

	struct vnode *dvp = IrpSp->FileObject->FsContext;
	zfs_ccb_t *zccb = IrpSp->FileObject->FsContext2;

	if (zccb->magic != ZFS_CCB_MAGIC)
		return (STATUS_INVALID_PARAMETER);

	if (flag_index_specified) {
		zccb->dirlist_index =
		    IrpSp->Parameters.QueryDirectory.FileIndex;
	} else if (flag_restart_scan) {
		zccb->dir_eof = 0;
		zccb->dirlist_index = 0;
		if (zccb->searchname.Buffer != NULL)
			kmem_free(zccb->searchname.Buffer,
			    zccb->searchname.MaximumLength);
		zccb->searchname.Buffer = NULL;
		zccb->searchname.MaximumLength = 0;
	} else {
		/* Just use zccb->dirlist_index from last call */
	}

	// Did last call complete listing?
	if (zccb->dir_eof)
		return (STATUS_NO_MORE_FILES);

	struct iovec iov;
	void *SystemBuffer = MapUserBuffer(Irp);
	iov.iov_base = (void *)SystemBuffer;
	iov.iov_len = IrpSp->Parameters.QueryDirectory.Length;

	zfs_uio_t uio;
	zfs_uio_iovec_init(&uio, &iov, 1, zccb->dirlist_index, UIO_SYSSPACE,
	    IrpSp->Parameters.QueryDirectory.Length, 0);

	// Grab the root zp
	zmo = DeviceObject->DeviceExtension;
	ASSERT(zmo->type == MOUNT_TYPE_VCB);

	zfsvfs = vfs_fsprivate(zmo); // or from zp

	if (!zfsvfs)
		return (STATUS_INTERNAL_ERROR);

	if (zccb->searchname.Buffer == NULL)
		initial = B_TRUE;

	dprintf("%s: starting vp %p Search pattern '%wZ' type %d: "
	    "saved search '%wZ'\n", __func__, dvp,
	    IrpSp->Parameters.QueryDirectory.FileName,
	    IrpSp->Parameters.QueryDirectory.FileInformationClass,
	    &zccb->searchname);

	WCHAR Fat8QMdot3QM[12] = { DOS_QM, DOS_QM, DOS_QM, DOS_QM,
	    DOS_QM, DOS_QM, DOS_QM, DOS_QM, L'.', DOS_QM, DOS_QM, DOS_QM };

	/* All these cases mean we should list all */
	if ((IrpSp->Parameters.QueryDirectory.FileName == NULL) ||
	    (IrpSp->Parameters.QueryDirectory.FileName->Length == 0) ||
	    (IrpSp->Parameters.QueryDirectory.FileName->Buffer == NULL) ||
	    ((IrpSp->Parameters.QueryDirectory.FileName->Length ==
	    sizeof (WCHAR)) &&
	    (IrpSp->Parameters.QueryDirectory.FileName->Buffer[0] == L'*')) ||
	    ((IrpSp->Parameters.QueryDirectory.FileName->Length ==
	    12 * sizeof (WCHAR)) &&
	    (RtlEqualMemory(IrpSp->Parameters.QueryDirectory.FileName->Buffer,
	    Fat8QMdot3QM, /* "????????.???" */
	    12 * sizeof (WCHAR))))) {

		if (zccb->searchname.Buffer != NULL)
			kmem_free(zccb->searchname.Buffer,
			    zccb->searchname.MaximumLength);
		zccb->searchname.Buffer = NULL;

	} else if (IrpSp->Parameters.QueryDirectory.FileName &&
	    IrpSp->Parameters.QueryDirectory.FileName->Buffer) {

		// Save the pattern in the zccb, as it is only given in the
		// first call (citation needed)
		zccb->ContainsWildCards = FsRtlDoesNameContainWildCards(
		    IrpSp->Parameters.QueryDirectory.FileName);

		/* why? */
		if (!zccb->ContainsWildCards && !initial)
			return (STATUS_NO_MORE_FILES);

		// If exists, we should free before setting
		if (zccb->searchname.Buffer != NULL)
			kmem_free(zccb->searchname.Buffer,
			    zccb->searchname.MaximumLength);

		zccb->searchname.MaximumLength =
		    IrpSp->Parameters.QueryDirectory.FileName->Length +
		    sizeof (WCHAR);
		zccb->searchname.Length =
		    IrpSp->Parameters.QueryDirectory.FileName->Length;
		zccb->searchname.Buffer =
		    kmem_alloc(zccb->searchname.MaximumLength,
		    KM_SLEEP);

		if (zccb->ContainsWildCards) {
			Status = RtlUpcaseUnicodeString(&zccb->searchname,
			    IrpSp->Parameters.QueryDirectory.FileName, FALSE);
		} else {
			RtlCopyMemory(zccb->searchname.Buffer,
			    IrpSp->Parameters.QueryDirectory.FileName->Buffer,
			    zccb->searchname.Length);
		}
		dprintf("%s: setting up search '%wZ' (wildcards: %d) "
		    "status 0x%lx\n", __func__,
		    &zccb->searchname, zccb->ContainsWildCards, Status);
	} else {
		if (!flag_restart_scan)
			initial = B_FALSE;
	}

	emitdir_ptr_t ctx;
	ctx.bufsize = (size_t)zfs_uio_resid(&uio);
	ctx.alloc_buf = kmem_zalloc(ctx.bufsize, KM_SLEEP);
	ctx.bufptr = ctx.alloc_buf;
	ctx.outcount = 0;
	ctx.next_offset = NULL;
	ctx.last_alignment = 0;
	ctx.offset = zccb->dirlist_index;
	ctx.numdirent = 0;
	ctx.dirlisttype = IrpSp->Parameters.QueryDirectory.FileInformationClass;

	VN_HOLD(dvp);
	ret = zfs_readdir(dvp, &ctx, NULL, zccb, IrpSp->Flags);
	VN_RELE(dvp);

	/* finished listing dir? */
	if (ret == ENOENT) {
		zccb->dir_eof = 1;
		ret = 0;
	} else if (ret == ENOSPC) {

		/*
		 * If we have no "outcount" then buffer is too small
		 * for the first record. If we do have "outcount", we
		 * return what we have, and wait to be called again.
		 */
		if (ctx.outcount > 0)
			ret = 0;
		else {
			Status = STATUS_BUFFER_OVERFLOW;
			zccb->dirlist_index = ctx.offset;
		}
	}

	if (ret == 0) {
		if (ctx.outcount > 0) {

			if ((ret = zfs_uiomove(ctx.alloc_buf,
			    (long)ctx.outcount, UIO_READ, &uio))) {
				/*
				 * Reset the pointer, by copying in old value
				 */
				ctx.offset = zccb->dirlist_index;
			}
			Status = STATUS_SUCCESS;
		} else { // outcount == 0
			Status = (zccb->dirlist_index == 0) ?
			    STATUS_NO_SUCH_FILE :
			    STATUS_NO_MORE_FILES;
		}
		// Set correct buffer size returned.
		Irp->IoStatus.Information = ctx.outcount;
		//    IrpSp->Parameters.QueryDirectory.Length -
		//    zfs_uio_resid(&uio);

		dprintf("dirlist information in %ld out size %llu\n",
		    IrpSp->Parameters.QueryDirectory.Length,
		    Irp->IoStatus.Information);

		// Remember directory index for next time
		zccb->dirlist_index = ctx.offset;
	}

	kmem_free(ctx.alloc_buf, ctx.bufsize);

	return (Status);
}


NTSTATUS
query_directory(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->Parameters.QueryDirectory.FileInformationClass) {

		// The type is now passed into zfs_vnop.c/zfs_readdir()
		// so check there for support
	case FileBothDirectoryInformation:
	case FileDirectoryInformation:
	case FileFullDirectoryInformation: // ***
	case FileIdBothDirectoryInformation: // ***
	case FileIdFullDirectoryInformation:
	case FileNamesInformation:
	// case FileObjectIdInformation:  // Do we need this one?
	case FileIdExtdDirectoryInformation:
	case FileIdExtdBothDirectoryInformation:
		Status =
		    query_directory_FileFullDirectoryInformation(DeviceObject,
		    Irp, IrpSp);
		break;
	case FileQuotaInformation:
		dprintf("   %s FileQuotaInformation *NotImplemented\n",
		    __func__);
		break;
	case FileReparsePointInformation:
		dprintf("   %s FileReparsePointInformation *NotImplemented\n",
		    __func__);
		Status = STATUS_INVALID_INFO_CLASS;
		break;
	default:
		dprintf("   %s unknown 0x%x *NotImplemented\n",
		    __func__,
		    IrpSp->Parameters.QueryDirectory.FileInformationClass);
		break;
	}

	return (Status);
}

NTSTATUS
notify_change_directory(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT fileObject = IrpSp->FileObject;
	mount_t *zmo;

	dprintf("%s\n", __func__);
	zmo = DeviceObject->DeviceExtension;
	ASSERT(zmo != NULL);
	if (zmo->type != MOUNT_TYPE_VCB) {
		return (STATUS_INVALID_PARAMETER);
	}

	struct vnode *vp = fileObject->FsContext;
	zfs_ccb_t *zccb = fileObject->FsContext2;
	ASSERT(vp != NULL);

	VN_HOLD(vp);
	znode_t *zp = VTOZ(vp);

	if (!vnode_isdir(vp)) {
		VN_RELE(vp);
		return (STATUS_INVALID_PARAMETER);
	}

	if (zccb && zccb->deleteonclose) {
		VN_RELE(vp);
		return (STATUS_DELETE_PENDING);
	}
	ASSERT(zmo->NotifySync != NULL);

	dprintf("%s: '%s' for %wZ\n", __func__,
	    zccb&&zccb->z_name_cache?zccb->z_name_cache:"",
	    &fileObject->FileName);

	FsRtlNotifyFilterChangeDirectory(
	    zmo->NotifySync, &zmo->DirNotifyList, zccb,
	    (PSTRING)&fileObject->FileName,
	    (IrpSp->Flags & SL_WATCH_TREE), FALSE,
	    IrpSp->Parameters.NotifyDirectory.CompletionFilter, Irp,
	    NULL, NULL, NULL);

	VN_RELE(vp);
	return (STATUS_PENDING);
}

NTSTATUS
set_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

	Irp->IoStatus.Information = 0;

	switch (IrpSp->Parameters.SetFile.FileInformationClass) {
	case FileAllocationInformation:
		Status = set_file_endoffile_information(DeviceObject, Irp,
		    IrpSp, B_FALSE, B_TRUE);
		break;
	case FileBasicInformation: // chmod
		dprintf("* SET FileBasicInformation\n");
		Status = set_file_basic_information(DeviceObject, Irp, IrpSp);
		break;
	case FileDispositionInformation: // unlink
		dprintf("* SET FileDispositionInformation\n");
		Status = set_file_disposition_information(DeviceObject, Irp,
		    IrpSp, B_FALSE);
		break;
	case FileDispositionInformationEx: // unlink
		dprintf("* SET FileDispositionInformationEx\n");
		Status = set_file_disposition_information(DeviceObject, Irp,
		    IrpSp, B_TRUE);
		break;
	case FileEndOfFileInformation: // extend?
		Status = set_file_endoffile_information(DeviceObject, Irp,
		    IrpSp, IrpSp->Parameters.SetFile.AdvanceOnly, B_FALSE);
		break;
	case FileLinkInformation: // symlink
		Status = set_file_link_information(DeviceObject, Irp, IrpSp);
		break;
	case FilePositionInformation: // seek
		Status = set_file_position_information(DeviceObject, Irp,
		    IrpSp);
		break;
	case FileRenameInformation: // vnop_rename
	case FileRenameInformationEx:
		Status = set_file_rename_information(DeviceObject, Irp, IrpSp);
		break;
	case FileValidDataLengthInformation:
		Status = set_file_valid_data_length_information(DeviceObject,
		    Irp, IrpSp);
		break;
	case FileCaseSensitiveInformation:
		Status = set_file_case_sensitive_information(DeviceObject,
		    Irp, IrpSp);
		break;
	default:
		dprintf("* %s: unknown type NOTIMPLEMENTED\n", __func__);
		break;
	}

	return (Status);
}

boolean_t
is_top_level(_In_ PIRP Irp)
{
	if (!IoGetTopLevelIrp()) {
		IoSetTopLevelIrp(Irp);
		return (TRUE);
	}

	return (FALSE);
}

NTSTATUS
zfs_read_wrap(vnode_t *vp, uint8_t *data, uint64_t start,
    uint64_t length, uint64_t *pbr, PIRP Irp)
{
	NTSTATUS Status;
	uint32_t bytes_read = 0;
	uint64_t last_end;
	znode_t *zp = VTOZ(vp);

	dprintf("(%p, %p, %I64x, %I64x, %p)\n", vp, data, start, length, pbr);

	VERIFY3P(zp, !=, NULL);

	if (pbr)
		*pbr = 0;

	if (start >= zp->z_size) {
		dprintf("Tried to read beyond end of file\n");
		return (STATUS_END_OF_FILE);
	}

	// pool_type = vp->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ?
	// NonPagedPool : PagedPool;
	struct iovec iov;
	iov.iov_base = (void *)data;
	iov.iov_len = length;

	zfs_uio_t uio;
	zfs_uio_iovec_init(&uio, &iov, 1, start, UIO_SYSSPACE,
	    length, 0);

	dprintf("%s: offset %llx size %llx\n", __func__,
	    start, length);

	Status = zfs_read(zp, &uio, 0, NULL);

	// Update bytes read
	if (pbr)
		*pbr = length - zfs_uio_resid(&uio);

	return (Status);
}

NTSTATUS
fs_read_impl(PIRP Irp, boolean_t wait, uint64_t *bytes_read)
{
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	vnode_t *vp = FileObject->FsContext;
	uint8_t *data = NULL;
	ULONG length = IrpSp->Parameters.Read.Length, addon = 0;
	uint64_t start = IrpSp->Parameters.Read.ByteOffset.QuadPart;
	NTSTATUS Status = STATUS_SUCCESS;
	boolean_t nocache = (Irp->Flags & IRP_NOCACHE);

	if (zfs_disable_wincache)
		nocache = TRUE;

	*bytes_read = 0;

	if (!vp || !VTOZ(vp))
		return (STATUS_INTERNAL_ERROR);

	dprintf("vp = %p\n", vp);
	dprintf("offset = %I64x, length = %lx\n", start, length);
	dprintf("paging_io = %s, no cache = %s\n",
	    Irp->Flags & IRP_PAGING_IO ? "true" : "false",
	    nocache ? "true" : "false");

	if (!vnode_isreg(vp))
		return (STATUS_INVALID_DEVICE_REQUEST);

	if (!(Irp->Flags & IRP_PAGING_IO) &&
	    !FsRtlCheckLockForReadAccess(&vp->lock, Irp)) {
		dprintf("tried to read locked region\n");
		return (STATUS_FILE_LOCK_CONFLICT);
	}

	if (length == 0) {
		dprintf("tried to read zero bytes\n");
		return (STATUS_SUCCESS);
	}

	if (start >= (uint64_t)vp->FileHeader.FileSize.QuadPart) {
		dprintf("read with offset > end (%I64x >= %I64x)\n",
		    start, vp->FileHeader.FileSize.QuadPart);
		return (STATUS_END_OF_FILE);
	}

	dprintf("FileObject %p vp %p FileSize = %I64x st_size = %I64x\n",
	    FileObject, vp, vp->FileHeader.FileSize.QuadPart,
	    VTOZ(vp)->z_size);

	if (!nocache && (IrpSp->MinorFunction & IRP_MN_MDL)) {
		NTSTATUS Status = STATUS_SUCCESS;

		try {
			if (!FileObject->PrivateCacheMap) {
				CC_FILE_SIZES ccfs;

				ccfs.AllocationSize =
				    vp->FileHeader.AllocationSize;
				ccfs.FileSize =
				    vp->FileHeader.FileSize;
				ccfs.ValidDataLength =
				    vp->FileHeader.ValidDataLength;

				zfs_init_cache(FileObject, vp, &ccfs);
			}

			CcMdlRead(FileObject,
			    &IrpSp->Parameters.Read.ByteOffset,
			    length, &Irp->MdlAddress, &Irp->IoStatus);
		} except(EXCEPTION_EXECUTE_HANDLER) {
			Status = GetExceptionCode();
		}

		if (NT_SUCCESS(Status)) {
			Status = Irp->IoStatus.Status;
			Irp->IoStatus.Information += addon;
			*bytes_read = (uint64_t)Irp->IoStatus.Information;
		} else
			dprintf("EXCEPTION - %08lx\n", Status);

		return (Status);
	}

	data = MapUserBuffer(Irp);
	// vp->FileHeader.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE ?
	// HighPagePriority : NormalPagePriority);

	if (Irp->MdlAddress && !data) {
		dprintf("MmGetSystemAddressForMdlSafe returned NULL\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	if (start >= (uint64_t)vp->FileHeader.ValidDataLength.QuadPart) {
		length = (ULONG)min(length,
		    min(start + length,
		    (uint64_t)vp->FileHeader.FileSize.QuadPart) -
		    vp->FileHeader.ValidDataLength.QuadPart);
		RtlZeroMemory(data, length);
		Irp->IoStatus.Information = *bytes_read = length;
		return (STATUS_SUCCESS);
	}

	if (length + start >
	    (uint64_t)vp->FileHeader.ValidDataLength.QuadPart) {
		addon = (ULONG)(min(start + length,
		    (uint64_t)vp->FileHeader.FileSize.QuadPart) -
		    vp->FileHeader.ValidDataLength.QuadPart);
		RtlZeroMemory(data +
		    (vp->FileHeader.ValidDataLength.QuadPart - start),
		    addon);
		length = (ULONG)
		    (vp->FileHeader.ValidDataLength.QuadPart - start);
	}

	if (!nocache) {

		try {
			if (!FileObject->PrivateCacheMap) {
				CC_FILE_SIZES ccfs;

				ccfs.AllocationSize =
				    vp->FileHeader.AllocationSize;
				ccfs.FileSize =
				    vp->FileHeader.FileSize;
				ccfs.ValidDataLength =
				    vp->FileHeader.ValidDataLength;

				zfs_init_cache(FileObject, vp, &ccfs);
			}

#if (NTDDI_VERSION >= NTDDI_WIN8)
	dprintf("CcCopyReadEx(%p, %I64x, %lx, %u, %p, %p, %p)\n",
	    FileObject, IrpSp->Parameters.Read.ByteOffset.QuadPart,
	    length, wait, data, &Irp->IoStatus, Irp->Tail.Overlay.Thread);
	dprintf("sizes = %I64x, %I64x, %I64x\n",
	    vp->FileHeader.AllocationSize.QuadPart,
	    vp->FileHeader.FileSize.QuadPart,
	    vp->FileHeader.ValidDataLength.QuadPart);
			if (!CcCopyReadEx(FileObject,
			    &IrpSp->Parameters.Read.ByteOffset,
			    length, wait, data, &Irp->IoStatus,
			    Irp->Tail.Overlay.Thread)) {
				dprintf("CcCopyReadEx could not wait\n");

				IoMarkIrpPending(Irp);
				return (STATUS_PENDING);
			}
			dprintf("CcCopyReadEx finished\n");
#else
	dprintf("CcCopyRead(%p, %I64x, %lx, %u, %p, %p)\n", FileObject,
	    IrpSp->Parameters.Read.ByteOffset.QuadPart, length, wait,
	    data, &Irp->IoStatus);
	dprintf("sizes = %I64x, %I64x, %I64x\n",
	    vp->FileHeader.AllocationSize.QuadPart,
	    vp->FileHeader.FileSize.QuadPart,
	    vp->FileHeader.ValidDataLength.QuadPart);
			if (!CcCopyRead(FileObject,
			    &IrpSp->Parameters.Read.ByteOffset,
			    length, wait, data, &Irp->IoStatus)) {
				dprintf("CcCopyRead could not wait\n");

				IoMarkIrpPending(Irp);
				return (STATUS_PENDING);
			}
			dprintf("CcCopyRead finished\n");
#endif
		} except(EXCEPTION_EXECUTE_HANDLER) {
			Status = GetExceptionCode();
		}

		if (NT_SUCCESS(Status)) {
			Status = Irp->IoStatus.Status;
			Irp->IoStatus.Information += addon;
			*bytes_read = (uint64_t)Irp->IoStatus.Information;
		} else
			dprintf("EXCEPTION - %08lx\n", Status);

		return (Status);

	} else { /* NOCACHE */

		if (!wait) {
			IoMarkIrpPending(Irp);
			return (STATUS_PENDING);
		}

		Status = zfs_read_wrap(vp, data, start, length, bytes_read,
		    Irp);

		if (!NT_SUCCESS(Status))
			dprintf("read_file returned %08lx\n", Status);
	}

	*bytes_read += addon;
	dprintf("read %llu bytes\n", *bytes_read);

	Irp->IoStatus.Information = *bytes_read;

	return (Status);
}

NTSTATUS
fs_read(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;
	boolean_t top_level;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	boolean_t acquired_vp_lock = FALSE, wait;
	uint64_t bytes_read;
	PAGED_CODE();

	top_level = is_top_level(Irp);

	mount_t *zmo = DeviceObject->DeviceExtension;

#if 0
	if (zmo && zmo->type == MOUNT_TYPE_VCB) {
		// Status = volume_read(DeviceObject, Irp);
		Status = STATUS_NOT_SUPPORTED;
		goto exit;
	} else if (!zmo || zmo->type != MOUNT_TYPE_FCB) {
		Status = STATUS_INVALID_PARAMETER;
		goto exit;
	}
#endif

	Irp->IoStatus.Information = 0;

	if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
		CcMdlReadComplete(IrpSp->FileObject, Irp->MdlAddress);

		Irp->MdlAddress = NULL;
		Status = STATUS_SUCCESS;

		goto exit;
	}


	struct vnode *vp = FileObject->FsContext;
	zfs_ccb_t *zccb = FileObject->FsContext2;

	if (!vp || !zccb || !zmo) {
		Status = STATUS_INVALID_PARAMETER;
		goto exit;
	}

	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);

	/* No return without zfs_exit() */
	if ((Status = zfs_enter(zfsvfs, FTAG)) != 0) {
		Status = STATUS_INVALID_PARAMETER;
		goto exit;
	}

	VERIFY3U(VN_HOLD(vp), ==, 0);

	znode_t *zp = VTOZ(vp);

#if 0
	if (Irp->RequestorMode == UserMode && !(ccb->access & FILE_READ_DATA)) {
		dprintf("insufficient privileges\n");
		Status = STATUS_ACCESS_DENIED;
		goto unlock;
	}
#endif

#if 0
	if (vp == Vcb->volume_vp) {
		dprintf("reading volume vp\n");

		IoSkipCurrentIrpStackLocation(Irp);

		Status = IoCallDriver(Vcb->Vpb->RealDevice, Irp);

		goto unlock;
	}
#endif

	wait = IoIsOperationSynchronous(Irp);

	if (!(Irp->Flags & IRP_PAGING_IO))
		FsRtlCheckOplock(vp_oplock(vp), Irp, NULL, NULL, NULL);
	else
		wait = TRUE;
	// Don't offload jobs when doing paging IO - otherwise this can lead to
	// deadlocks in CcCopyRead.

	if (!(Irp->Flags & IRP_PAGING_IO) && FileObject->SectionObjectPointer &&
	    FileObject->SectionObjectPointer->DataSectionObject) {
		IO_STATUS_BLOCK iosb;

		CcFlushCache(FileObject->SectionObjectPointer,
		    &IrpSp->Parameters.Read.ByteOffset,
		    IrpSp->Parameters.Read.Length,
		    &iosb);
		if (!NT_SUCCESS(iosb.Status)) {
			dprintf("CcFlushCache returned %08lx\n", iosb.Status);
			Status = iosb.Status;
			goto end;
		}
	}

	if (!ExIsResourceAcquiredSharedLite(vp->FileHeader.Resource)) {
		if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource,
		    wait)) {
			Status = STATUS_PENDING;
			IoMarkIrpPending(Irp);
			goto end;
		}
		acquired_vp_lock = TRUE;
	}

	Status = fs_read_impl(Irp, wait, &bytes_read);

	if (acquired_vp_lock)
		ExReleaseResourceLite(vp->FileHeader.Resource);

update:
	if (FileObject->Flags & FO_SYNCHRONOUS_IO &&
	    !(Irp->Flags & IRP_PAGING_IO))
		FileObject->CurrentByteOffset.QuadPart =
		    IrpSp->Parameters.Read.ByteOffset.QuadPart +
		    (NT_SUCCESS(Status) ? bytes_read : 0);

end:
	switch (Status) {
	case 0:
	case STATUS_PENDING:
	default:
		break;
	case EISDIR:
		Status = STATUS_FILE_IS_A_DIRECTORY;
		break;
	}

	Irp->IoStatus.Status = Status;

	dprintf("IrpSp->Parameters.Read.Length = %08lx\n",
	    IrpSp->Parameters.Read.Length);
	dprintf("Irp->IoStatus.Status = %08lx\n",
	    Irp->IoStatus.Status);
	dprintf("Irp->IoStatus.Information = %Iu bytesread %llu\n",
	    Irp->IoStatus.Information, bytes_read);

	VN_RELE(vp);

	zfs_exit(zfsvfs, FTAG);

exit:
	if (top_level)
		IoSetTopLevelIrp(NULL);

	return (Status);
}

NTSTATUS
zfs_write_wrap(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    LARGE_INTEGER offset, void *buf, ULONG *length,
    boolean_t paging_io, boolean_t no_cache,
    boolean_t wait, boolean_t deferred_write, boolean_t write_irp)
{
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	uint64_t off64, newlength, start_data, end_data;
	uint32_t bufhead;
	boolean_t changed_length = FALSE;
	NTSTATUS Status;
	LARGE_INTEGER time;
	timestruc_t now;
	vnode_t *vp;
	zfs_ccb_t *ccb;
	boolean_t paging_lock = FALSE, acquired_vp_lock = FALSE,
	    pagefile;
	ULONG filter = 0;

	dprintf("(%p, %p, %I64x, %p, %lx, %u, %u)\n", DeviceObject,
	    FileObject, offset.QuadPart, buf, *length, paging_io, no_cache);

	if (*length == 0) {
		dprintf("returning success for zero-length write\n");
		return (STATUS_SUCCESS);
	}

	if (!FileObject) {
		dprintf("error - FileObject was NULL\n");
		return (STATUS_ACCESS_DENIED);
	}

	vp = FileObject->FsContext;
	ccb = FileObject->FsContext2;


	znode_t *zp = VTOZ(vp);

	// fileref = ccb ? ccb->fileref : NULL;

	if (!vnode_isreg(vp) && !vnode_islnk(vp)) {
		dprintf("tried to write !file nor !symlink (vp %p)\n",
		    vp);
		return (STATUS_INVALID_DEVICE_REQUEST);
	}

	if (offset.LowPart == FILE_WRITE_TO_END_OF_FILE &&
	    offset.HighPart == -1)
		offset = vp->FileHeader.FileSize;

	off64 = offset.QuadPart;

	dprintf("vp->Header.Flags = %x\n", vp->FileHeader.Flags);

	if (!no_cache && !CcCanIWrite(FileObject, *length, wait,
	    deferred_write)) {
		IoMarkIrpPending(Irp);
		return (STATUS_PENDING);
	}
	if (!wait && no_cache) {
		IoMarkIrpPending(Irp);
		return (STATUS_PENDING);
	}

	if (no_cache && !paging_io &&
	    FileObject->SectionObjectPointer->DataSectionObject) {
		IO_STATUS_BLOCK iosb;

		ExAcquireResourceExclusiveLite(
		    vp->FileHeader.PagingIoResource, TRUE);

		CcFlushCache(FileObject->SectionObjectPointer, &offset,
		    *length, &iosb);

		if (!NT_SUCCESS(iosb.Status)) {
			ExReleaseResourceLite(
			    vp->FileHeader.PagingIoResource);
			dprintf("CcFlushCache returned %08lx\n",
			    iosb.Status);
			return (iosb.Status);
		}

		paging_lock = TRUE;

		CcPurgeCacheSection(FileObject->SectionObjectPointer,
		    &offset, *length, FALSE);
	}

	if (paging_io) {
		if (!ExAcquireResourceSharedLite(
		    vp->FileHeader.PagingIoResource, wait)) {
			Status = STATUS_PENDING;
			IoMarkIrpPending(Irp);
			goto end;
		} else
			paging_lock = TRUE;
	}

	pagefile = vp->FileHeader.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE &&
	    paging_io;

#if 0 // figure out what treelock is
	if (!pagefile &&
	    !ExIsResourceAcquiredExclusiveLite(&Vcb->tree_lock)) {
		if (!ExAcquireResourceSharedLite(&Vcb->tree_lock, wait)) {
			Status = STATUS_PENDING;
			goto end;
		} else
			acquired_tree_lock = true;
	}
#endif

	if (pagefile) {
		if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource,
		    wait)) {
			Status = STATUS_PENDING;
			IoMarkIrpPending(Irp);
			goto end;
		} else {
			acquired_vp_lock = TRUE;
		}
	} else if (!ExIsResourceAcquiredExclusiveLite(
	    vp->FileHeader.Resource)) {

		if (!ExAcquireResourceExclusiveLite(
		    vp->FileHeader.Resource, wait)) {
			Status = STATUS_PENDING;
			IoMarkIrpPending(Irp);
			goto end;
		} else {
			acquired_vp_lock = TRUE;
		}
	}

	newlength = zp->z_size;

	if (zp->z_unlinked)
		newlength = 0;

	dprintf("newlength = %I64x\n", newlength);

	if (off64 + *length > newlength) {
		if (paging_io) {
			if (off64 >= newlength) {
				dprintf(
"paging tried beyond EOF (size = %I64x, off = %I64x, len = %lx)\n",
				    newlength, off64, *length);
				dprintf(
"AllocationSize = %I64x, Size = %I64x, ValidDataLength = %I64x\n",
				    vp->FileHeader.AllocationSize.QuadPart,
				    vp->FileHeader.FileSize.QuadPart,
				    vp->FileHeader.ValidDataLength.QuadPart);
				Irp->IoStatus.Information = 0;

				Status = STATUS_SUCCESS;
				goto end;
			}

			*length = (ULONG)(newlength - off64);
		} else {
			newlength = off64 + *length;
			changed_length = TRUE;

			dprintf("extending length to %I64x\n", newlength);
		}
	}

	if (changed_length) {
		if (newlength >
		    (uint64_t)vp->FileHeader.AllocationSize.QuadPart) {

			Status = zfs_freesp(zp,
			    newlength, 0, FWRITE, B_FALSE);
			if (!NT_SUCCESS(Status)) {
				dprintf("extend_file returned %08lx\n",
				    Status);
				goto end;
			}
		} else {
			zp->z_size = newlength;
		}

		vp->FileHeader.AllocationSize.QuadPart = newlength;
		vp->FileHeader.FileSize.QuadPart = newlength;
		vp->FileHeader.ValidDataLength.QuadPart = newlength;

		dprintf("AllocationSize = %I64x\n",
		    vp->FileHeader.AllocationSize.QuadPart);
		dprintf("FileSize = %I64x\n",
		    vp->FileHeader.FileSize.QuadPart);
		dprintf("ValidDataLength = %I64x\n",
		    vp->FileHeader.ValidDataLength.QuadPart);
	}

	if (!no_cache) {
		Status = STATUS_SUCCESS;

		try {
			if (!FileObject->PrivateCacheMap || changed_length) {
				CC_FILE_SIZES ccfs;

				ccfs.AllocationSize =
				    vp->FileHeader.AllocationSize;
				ccfs.FileSize =
				    vp->FileHeader.FileSize;
				ccfs.ValidDataLength =
				    vp->FileHeader.ValidDataLength;

				if (!FileObject->PrivateCacheMap)
					zfs_init_cache(FileObject, vp, &ccfs);

				CcSetFileSizes(FileObject, &ccfs);
			}

			if (IrpSp->MinorFunction & IRP_MN_MDL) {
				CcPrepareMdlWrite(FileObject, &offset, *length,
				    &Irp->MdlAddress, &Irp->IoStatus);

				Status = Irp->IoStatus.Status;
				goto end;
			} else {
/*
 * We have to wait in CcCopyWrite - if we return STATUS_PENDING
 * and add this to the work queue, it can result in CcFlushCache
 * being called before the job has run. See ifstest ReadWriteTest.
 */

				dprintf("CcCopyWrite(%p, %I64x, %lx, %p, %p)\n",
				    FileObject, off64, *length, buf,
				    Irp->Tail.Overlay.Thread);
#if (NTDDI_VERSION >= NTDDI_WIN8)
				if (!CcCopyWriteEx(FileObject, &offset, *length,
				    TRUE, buf, Irp->Tail.Overlay.Thread)) {
					Status = STATUS_PENDING;
					IoMarkIrpPending(Irp);
					goto end;
				}
#else
				if (!CcCopyWrite(FileObject, &offset, *length,
				    TRUE, buf)) {
					Status = STATUS_PENDING;
					IoMarkIrpPending(Irp);
					goto end;
				}
#endif
				Irp->IoStatus.Information = *length;
			}
		} except(EXCEPTION_EXECUTE_HANDLER) {
			Status = GetExceptionCode();
		}

		if (changed_length) {

			if (zp->z_pflags & ZFS_XATTR) {
				zfs_send_notify_stream(zp->z_zfsvfs,
				    ccb->z_name_cache,
				    ccb->z_name_offset,
				    FILE_NOTIFY_CHANGE_STREAM_SIZE,
				    FILE_ACTION_MODIFIED_STREAM,
				    NULL);
			} else {
				zfs_send_notify(zp->z_zfsvfs, ccb->z_name_cache,
				    ccb->z_name_offset, FILE_NOTIFY_CHANGE_SIZE,
				    FILE_ACTION_MODIFIED);
			}
		}

		goto end;
	}

	struct iovec iov;
	iov.iov_base = (void *)buf;
	iov.iov_len = *length;

	zfs_uio_t uio;
	zfs_uio_iovec_init(&uio, &iov, 1, off64, UIO_SYSSPACE,
	    *length, 0);

	boolean_t locked = FALSE;

	if (write_irp && Irp->MdlAddress) {
		locked = Irp->MdlAddress->MdlFlags &
		    (MDL_PAGES_LOCKED | MDL_PARTIAL);

		if (!locked) {
			Status = STATUS_SUCCESS;

			try {
				MmProbeAndLockPages(Irp->MdlAddress, KernelMode,
				    IoReadAccess);
			} except(EXCEPTION_EXECUTE_HANDLER) {
				Status = GetExceptionCode();
			}

			if (!NT_SUCCESS(Status)) {
				dprintf("MmProbeAndLockPages except %08lx\n",
				    Status);
				goto end;
			}
		}
	}

	if (paging_io) {
		uio.uio_extflg |= SKIP_CHANGE_TIME;
		uio.uio_extflg |= SKIP_WRITE_TIME;
	} else {
		if (ccb->user_set_change_time)
			uio.uio_extflg |= SKIP_CHANGE_TIME;
		if (ccb->user_set_write_time)
			uio.uio_extflg |= SKIP_WRITE_TIME;
	}

	Status = zfs_write(zp, &uio, 0, NULL);

	if (!locked)
		MmUnlockPages(Irp->MdlAddress);

	if (!NT_SUCCESS(Status)) {
		dprintf("zfs_write returned %08lx\n", Status);
		goto end;
	}

	// gethrestime(&now);

	if (!pagefile) {

		if (!ccb->user_set_write_time)
			filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;

		if (!(zp->z_pflags & ZFS_XATTR)) {
			if (changed_length) {
				dprintf("setting st_size to %I64x\n",
				    newlength);
				zp->z_size = newlength;
				filter |= FILE_NOTIFY_CHANGE_SIZE;
			}

		} else {

			if (changed_length)
				filter |= FILE_NOTIFY_CHANGE_STREAM_SIZE;

			filter |= FILE_NOTIFY_CHANGE_STREAM_WRITE;
		}
	}

	if (changed_length) {
		CC_FILE_SIZES ccfs;

		ccfs.AllocationSize = vp->FileHeader.AllocationSize;
		ccfs.FileSize = vp->FileHeader.FileSize;
		ccfs.ValidDataLength = vp->FileHeader.ValidDataLength;

		try {
			CcSetFileSizes(FileObject, &ccfs);
		} except(EXCEPTION_EXECUTE_HANDLER) {
			Status = GetExceptionCode();
			goto end;
		}
	}

	Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = *length;

	if (filter != 0) {
		zfsvfs_t *zfsvfs = zp->z_zfsvfs;

		if (zp->z_pflags & ZFS_XATTR) {
			zfs_send_notify_stream(zfsvfs,
			    ccb->z_name_cache,
			    ccb->z_name_offset,
			    filter,
			    FILE_ACTION_MODIFIED_STREAM,
			    NULL);
		} else {
			zfs_send_notify(zfsvfs, ccb->z_name_cache,
			    ccb->z_name_offset, filter,
			    FILE_ACTION_MODIFIED);
		}
	}

end:
	if (NT_SUCCESS(Status) && FileObject->Flags & FO_SYNCHRONOUS_IO &&
	    !paging_io) {
		dprintf("CurrentByteOffset was: %I64x\n",
		    FileObject->CurrentByteOffset.QuadPart);
		FileObject->CurrentByteOffset.QuadPart =
		    offset.QuadPart + (NT_SUCCESS(Status) ? *length : 0);
		dprintf("CurrentByteOffset now: %I64x\n",
		    FileObject->CurrentByteOffset.QuadPart);
	}

	if (acquired_vp_lock)
		ExReleaseResourceLite(vp->FileHeader.Resource);

	if (paging_lock)
		ExReleaseResourceLite(vp->FileHeader.PagingIoResource);

	return (Status);
}

NTSTATUS
fs_write_impl(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp,
    boolean_t wait, boolean_t deferred_write)
{
	void *buf;
	NTSTATUS Status;
	LARGE_INTEGER offset = IrpSp->Parameters.Write.ByteOffset;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	vnode_t *vp = FileObject ? FileObject->FsContext : NULL;
	boolean_t nocache = (Irp->Flags & IRP_NOCACHE);

	if (zfs_disable_wincache)
		nocache = TRUE;

	Irp->IoStatus.Information = 0;

	dprintf("offset = %I64x\n", offset.QuadPart);
	dprintf("length = %lx\n", IrpSp->Parameters.Write.Length);

	if (IrpSp->Parameters.Write.Length == 0) {
		Status = STATUS_SUCCESS;
		goto exit;
	}

	if (!Irp->AssociatedIrp.SystemBuffer) {
		buf = MapUserBuffer(Irp);
		// , vp && vp->FileHeader.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE
		// ? HighPagePriority : NormalPagePriority);

		if (Irp->MdlAddress && !buf) {
			dprintf("MmGetSystemAddressForMdlSafe returned NULL\n");
			Status = STATUS_INSUFFICIENT_RESOURCES;
			goto exit;
		}
	} else {
		buf = Irp->AssociatedIrp.SystemBuffer;
	}

	dprintf("buf = %p\n", buf);

	if (vp && !(Irp->Flags & IRP_PAGING_IO) &&
	    !FsRtlCheckLockForWriteAccess(&vp->lock, Irp)) {
		dprintf("tried to write to locked region\n");
		Status = STATUS_FILE_LOCK_CONFLICT;
		goto exit;
	}

	Status = zfs_write_wrap(DeviceObject, Irp, offset, buf,
	    &IrpSp->Parameters.Write.Length,
	    (Irp->Flags & IRP_PAGING_IO), nocache,
	    wait, deferred_write, TRUE);

	if (Status == STATUS_PENDING) {
		goto exit;
	} else if (!NT_SUCCESS(Status)) {
		dprintf("write_file2 returned %08lx\n", Status);
		goto exit;
	}

	if (NT_SUCCESS(Status)) {
#if 0 // Updating benchmark stats?
		if (diskacc && Status != STATUS_PENDING &&
		    nocache) {
			PETHREAD thread = NULL;

			if (Irp->Tail.Overlay.Thread &&
			    !IoIsSystemThread(Irp->Tail.Overlay.Thread))
				thread = Irp->Tail.Overlay.Thread;
			else if (!IoIsSystemThread(PsGetCurrentThread()))
				thread = PsGetCurrentThread();
			else if (IoIsSystemThread(PsGetCurrentThread()) &&
			    IoGetTopLevelIrp() == Irp)
				thread = PsGetCurrentThread();

			if (thread)
				fPsUpdateDiskCounters(
				    PsGetThreadProcess(thread),
				    0, IrpSp->Parameters.Write.Length, 0, 1, 0);
		}
#endif
	}

exit:

	return (Status);
}

NTSTATUS
fs_write(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT	fileObject;
	ULONG			bufferLength;
	LARGE_INTEGER	byteOffset;
	NTSTATUS Status = STATUS_SUCCESS;
	int error;
	int pagingio = FlagOn(Irp->Flags, IRP_PAGING_IO);

	PAGED_CODE();

#if 0
	dprintf("   %s minor type %d flags 0x%x mdl %d System %d "
	    "User %d paging %d\n", __func__, IrpSp->MinorFunction,
	    DeviceObject->Flags, (Irp->MdlAddress != 0),
	    (Irp->AssociatedIrp.SystemBuffer != 0),
	    (Irp->UserBuffer != 0),
	    FlagOn(Irp->Flags, IRP_PAGING_IO));
#endif

	fileObject = IrpSp->FileObject;

	if (fileObject == NULL) {
		dprintf("fileObject == NULL\n");
		return (SET_ERROR(STATUS_INVALID_PARAMETER));
	}

	if (fileObject->FsContext == NULL) {
		dprintf("FsContext == NULL\n");
		return (SET_ERROR(STATUS_INVALID_PARAMETER));
	}

	struct vnode *vp = fileObject->FsContext;
	zfs_ccb_t *zccb = fileObject->FsContext2;

	if (zccb == NULL) {
		dprintf("zccb == NULL\n");
		return (SET_ERROR(STATUS_INVALID_PARAMETER));
	}

	if (VTOZ(vp) == NULL) {
		dprintf("zp == NULL\n");
		return (SET_ERROR(STATUS_SUCCESS));
		return (SET_ERROR(STATUS_INVALID_PARAMETER));
	}

	mount_t *zmo = DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	boolean_t wait = fileObject ? IoIsOperationSynchronous(Irp) : TRUE;


	/* No returns from here */
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (SET_ERROR(error));

	VERIFY3U(VN_HOLD(vp), ==, 0);

	/* zp can be NULL */
	znode_t *zp = VTOZ(vp);

	Irp->IoStatus.Information = 0;

	try {
		if (FlagOn(IrpSp->MinorFunction, IRP_MN_COMPLETE)) {
			dprintf("%s: IRP_MN_COMPLETE\n", __func__);
			CcMdlWriteComplete(IrpSp->FileObject,
			    &IrpSp->Parameters.Write.ByteOffset,
			    Irp->MdlAddress);
			// Mdl is now deallocated.
			Irp->MdlAddress = NULL;
			Status = STATUS_SUCCESS;

		} else {

			if (!FlagOn(Irp->Flags, IRP_PAGING_IO))
				FsRtlCheckOplock(vp_oplock(vp), Irp, NULL,
				    NULL, NULL);

			if (FlagOn(Irp->Flags, IRP_PAGING_IO))
				wait = TRUE;

			Status = fs_write_impl(DeviceObject, Irp, IrpSp,
			    wait, FALSE);
		}

	} except(EXCEPTION_EXECUTE_HANDLER) {
		Status = GetExceptionCode();
	}

	switch (error) {
	case 0:
		break;
	case EISDIR:
		Status = STATUS_FILE_IS_A_DIRECTORY;
		break;
	case ENOSPC:
		Status = STATUS_DISK_FULL;
		break;
	case EDQUOT:
		// Status = STATUS_DISK_QUOTA_EXCEEDED;
		Status = STATUS_DISK_FULL;
		break;
	default:
		break;
	}

	VN_RELE(vp);

	zfs_exit(zfsvfs, FTAG);

	// dprintf("Name: %wZ offset 0x%llx len 0x%lx mdl %p System %p\n",
	// &fileObject->FileName, byteOffset.QuadPart, bufferLength,
	// Irp->MdlAddress, Irp->AssociatedIrp.SystemBuffer);

	Irp->IoStatus.Status = Status;

	return (SET_ERROR(Status));
}

NTSTATUS
do_read_job(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	boolean_t top_level = is_top_level(Irp);
	NTSTATUS Status;
	uint64_t bytes_read;
	PIO_STACK_LOCATION IrpSp;
	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	try {
		Status = fs_read_impl(Irp, TRUE, &bytes_read);
	} except(EXCEPTION_EXECUTE_HANDLER) {
		Status = GetExceptionCode();
	}

	if (!NT_SUCCESS(Status))
		dprintf("read_file returned %08lx\n", Status);

	Irp->IoStatus.Status = Status;

	dprintf("read %Iu bytes\n", Irp->IoStatus.Information);

	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	if (top_level)
		IoSetTopLevelIrp(NULL);

	dprintf("returning %08lx\n", Status);

	return (Status);
}

NTSTATUS
do_write_job(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	boolean_t top_level = is_top_level(Irp);
	NTSTATUS Status;

	PIO_STACK_LOCATION IrpSp;
	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	try {
		Status = fs_write_impl(DeviceObject, Irp, IrpSp, TRUE, TRUE);
	} except(EXCEPTION_EXECUTE_HANDLER) {
		Status = GetExceptionCode();
	}

	if (!NT_SUCCESS(Status))
		dprintf("write_file returned %08lx\n", Status);

	Irp->IoStatus.Status = Status;

	dprintf("wrote %Iu bytes\n", Irp->IoStatus.Information);

	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	if (top_level)
		IoSetTopLevelIrp(NULL);

	dprintf("returning %08lx\n", Status);

	return (Status);
}

struct do_job_s {
	PIRP Irp;
	ULONG len;
	uint8_t work_item[0];
};

void
do_job(PDEVICE_OBJECT DeviceObject, struct do_job_s *job)
{
	NTSTATUS Status;
	PIO_STACK_LOCATION IrpSp;
	PIRP Irp = job->Irp;
	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	switch (IrpSp->MajorFunction) {
	case IRP_MJ_READ:
		Status = do_read_job(DeviceObject, Irp);
		break;
	case IRP_MJ_WRITE:
		Status = do_write_job(DeviceObject, Irp);
		break;
	default:
		panic("Unknown do_job(IRP_MJ_ %d (0x%x)",
		    IrpSp->MajorFunction, IrpSp->MajorFunction);
	}

	IoUninitializeWorkItem((PIO_WORKITEM)&job->work_item);
	ExFreePoolWithTag(job, 'ZJOB');
}

boolean_t
add_thread_job(PDEVICE_OBJECT DeviceObject, PIRP Irp, int len,
    LOCK_OPERATION IoStyle)
{
	struct do_job_s *job;

	job = ExAllocatePoolWithTag(NonPagedPoolNx,
	    sizeof (struct do_job_s) + IoSizeofWorkItem(),
	    'ZJOB');

	if (job == NULL)
		return (FALSE);

	if (Irp->MdlAddress == NULL &&
	    Irp->UserBuffer != NULL) {

		PMDL mdl = IoAllocateMdl(Irp->UserBuffer,
		    len, FALSE, FALSE, Irp);

		if (!mdl) {
			// Handle memory allocation failure
			// Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
			// IoCompleteRequest(Irp, IO_NO_INCREMENT);
			ExFreePoolWithTag(job, 'ZJOB');
			return (FALSE);
		}
		try {
			// Lock the pages in memory
			MmProbeAndLockPages(mdl, Irp->RequestorMode,
			    IoStyle);
		} except(EXCEPTION_EXECUTE_HANDLER) {
			// Handle probe and lock failure
			IoFreeMdl(mdl);
			Irp->MdlAddress = NULL;
			ExFreePoolWithTag(job, 'ZJOB');
			// job->Irp->IoStatus.Status = GetExceptionCode();
			// IoCompleteRequest(job->Irp, IO_NO_INCREMENT);
			return (FALSE);
		}
	}

	IoInitializeWorkItem(DeviceObject, (PIO_WORKITEM)&job->work_item);
	job->Irp = Irp;
	job->len = len;

	IoQueueWorkItem((PIO_WORKITEM)&job->work_item,
	    (PIO_WORKITEM_ROUTINE)do_job,
	    DelayedWorkQueue, job);

	return (TRUE);
}

/*
 * The lifetime of a delete.
 * 1) If a file open is marked DELETE_ON_CLOSE in zfs_vnop_lookup() we will
 * call vnode_setdeleteonclose(vp) to signal the intent. This is so
 * file_standard_information can return DeletePending correctly
 * (as well as a few more)
 * 2) Upon IRP_MJ_CLEANUP (closing a file handle) we are expected to remove
 * the file (as tested by IFStest.exe) we will call vnode_setdeleted(vp),
 * this will:
 * 3) Make zfs_vnop_lookup() return ENOENT when "setdeleted" is set.
 * Making it appear as if the file was deleted - but retaining vp and zp
 * as required by Windows.
 * 4) Eventually IRP_MJ_CLOSE is called, and if final, we can release
 * vp and zp, and if "setdeleted" was active, we can finally call
 * delete_entry() to remove the file.
 */
NTSTATUS
delete_entry(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	// In Unix, both zfs_unlink and zfs_rmdir expect a filename,
	// and we do not have that here
	struct vnode *vp = NULL, *dvp = NULL;
	int error;
	char filename[MAXPATHLEN];
	ULONG outlen;
	znode_t *zp = NULL;
	mount_t *zmo = DeviceObject->DeviceExtension;

	if (IrpSp->FileObject->FsContext == NULL ||
	    IrpSp->FileObject->FileName.Buffer == NULL ||
	    IrpSp->FileObject->FileName.Length == 0) {
		dprintf("%s: called with missing arguments, can't delete\n",
		    __func__);
		return (STATUS_INSTANCE_NOT_AVAILABLE); // FIXME
	}

	vp = IrpSp->FileObject->FsContext;
	zp = VTOZ(vp);
	ASSERT(zp != NULL);

	uint64_t parent = 0;

	if (zp->z_is_ctldir)
		return (STATUS_SUCCESS);

	// find parent and hold.
	dvp = zfs_parent(vp);
	if (dvp == NULL)
		return (STATUS_INSTANCE_NOT_AVAILABLE);

	// Unfortunately, filename is littered with "\", clean it up,
	// or search based on ID to get name?
	dprintf("%s: deleting '%.*S'\n", __func__,
	    (int)(IrpSp->FileObject->FileName.Length / sizeof (WCHAR)),
	    IrpSp->FileObject->FileName.Buffer);

	error = RtlUnicodeToUTF8N(filename, MAXPATHLEN - 1, &outlen,
	    IrpSp->FileObject->FileName.Buffer,
	    IrpSp->FileObject->FileName.Length);

	if (error != STATUS_SUCCESS &&
	    error != STATUS_SOME_NOT_MAPPED) {
		VN_RELE(dvp);
		VN_RELE(vp);
		dprintf("%s: some illegal characters\n", __func__);
		return (STATUS_INVALID_PARAMETER); // test.exe
		return (STATUS_ILLEGAL_CHARACTER);
	}
	while (outlen > 0 && filename[outlen - 1] == '\\') outlen--;
	filename[outlen] = 0;

	// FIXME, use z_name_cache and offset
	char *finalname = NULL;
	if ((finalname = strrchr(filename, '\\')) != NULL)
		finalname = &finalname[1];
	else
		finalname = filename;

	// Check if it has :stream
	char *stream_name = NULL;
	error = stream_parse(filename, &stream_name);
	if (error == 0 && stream_name != NULL)
		finalname = stream_name;

	dprintf("final delete name as '%s'\n", finalname);

	// Release final HOLD on item, ready for deletion
	int isdir = vnode_isdir(vp);

	/* ZFS deletes from filename, so RELE last hold on vp. */
	// vnode_flushcache(vp, IrpSp->FileObject, B_TRUE);

	vp->FileHeader.AllocationSize.QuadPart = 0;
	vp->FileHeader.FileSize.QuadPart = 0;
	vp->FileHeader.ValidDataLength.QuadPart = 0;

	if (IrpSp->FileObject) {
		CC_FILE_SIZES ccfs;
		NTSTATUS Status = STATUS_SUCCESS;

		ccfs.AllocationSize = vp->FileHeader.AllocationSize;
		ccfs.FileSize = vp->FileHeader.FileSize;
		ccfs.ValidDataLength = vp->FileHeader.ValidDataLength;

		try {
			CcSetFileSizes(IrpSp->FileObject, &ccfs);
		} except(EXCEPTION_EXECUTE_HANDLER) {
			Status = GetExceptionCode();
		}

		if (!NT_SUCCESS(Status)) {
			dprintf("CcSetFileSizes threw exception %08lx\n",
			    Status);
		}
	}

	VN_RELE(vp);
	vp = NULL;

	if (isdir) {

		error = zfs_rmdir(VTOZ(dvp), finalname, NULL, NULL, 0);

	} else {

		error = zfs_remove(VTOZ(dvp), finalname, NULL, 0);

	}

	if (error == ENOTEMPTY)
		error = STATUS_DIRECTORY_NOT_EMPTY;

	// Release parent.
	VN_RELE(dvp);

	dprintf("%s: returning %d\n", __func__, error);
	return (error);
}

NTSTATUS
flush_buffers(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	NTSTATUS Status = 0;

	dprintf("%s: \n", __func__);

	if (FileObject == NULL || FileObject->FsContext == NULL)
		return (STATUS_INVALID_PARAMETER);

	struct vnode *vp = FileObject->FsContext;
	if (VN_HOLD(vp) == 0) {
		znode_t *zp = VTOZ(vp);
		zfsvfs_t *zfsvfs = zp->z_zfsvfs;
		Status = zfs_vnop_ioctl_fullfsync(vp, NULL, zfsvfs);
		VN_RELE(vp);
	}
	return (Status);
}

NTSTATUS
query_security(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	NTSTATUS Status;

	dprintf("%s: \n", __func__);

	if (FileObject == NULL || FileObject->FsContext == NULL)
		return (STATUS_INVALID_PARAMETER);

	void *buf = MapUserBuffer(Irp);

	struct vnode *vp = FileObject->FsContext;
	VN_HOLD(vp);
	PSECURITY_DESCRIPTOR sd;
	sd = vnode_security(vp);
	ULONG buflen = IrpSp->Parameters.QuerySecurity.Length;
	Status = SeQuerySecurityDescriptorInfo(
	    &IrpSp->Parameters.QuerySecurity.SecurityInformation,
	    buf,
	    &buflen,
	    &sd);
	VN_RELE(vp);

	if (Status == STATUS_BUFFER_TOO_SMALL) {
		Status = STATUS_BUFFER_OVERFLOW; // Needed, checked.
		Irp->IoStatus.Information = buflen;
	} else if (NT_SUCCESS(Status)) {
		Irp->IoStatus.Information = buflen;
	} else {
		dprintf("%s: failed 0x%lx\n", __func__, Status);
		Irp->IoStatus.Information = 0;
	}

	return (Status);
}

// Set Security should save the blob and do nothing else.
NTSTATUS
set_security(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	NTSTATUS Status = STATUS_SUCCESS;

	dprintf("%s: \n", __func__);

	if (FileObject == NULL || FileObject->FsContext == NULL)
		return (STATUS_INVALID_PARAMETER);

	struct vnode *vp = FileObject->FsContext;
	zfs_ccb_t *zccb = FileObject->FsContext2;
	VN_HOLD(vp);
	PSECURITY_DESCRIPTOR oldsd;
	oldsd = vnode_security(vp);


	// READONLY check here
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	if (vfs_isrdonly(zfsvfs->z_vfs) || zfsctl_is_node(zp)) {
		Status = STATUS_MEDIA_WRITE_PROTECTED;
		goto err;
	}

	Status = SeSetSecurityDescriptorInfo(NULL,
	    &IrpSp->Parameters.SetSecurity.SecurityInformation,
	    IrpSp->Parameters.SetSecurity.SecurityDescriptor,
	    (void **)&vp->security_descriptor,
	    PagedPool,
	    IoGetFileObjectGenericMapping());

	if (!NT_SUCCESS(Status))
		goto err;

	ExFreePool(oldsd);

	// Now, we might need to update ZFS ondisk information
	vattr_t vattr;
	vattr.va_mask = 0;
	BOOLEAN defaulted;

	if (IrpSp->Parameters.SetSecurity.SecurityInformation &
	    OWNER_SECURITY_INFORMATION) {
		PSID owner;
		Status = RtlGetOwnerSecurityDescriptor(vnode_security(vp),
		    &owner, &defaulted);
		if (Status == STATUS_SUCCESS) {
			vattr.va_uid = zfs_sid2uid(owner);
			vattr.va_mask |= ATTR_UID;
		}
	}
	if (IrpSp->Parameters.SetSecurity.SecurityInformation &
	    GROUP_SECURITY_INFORMATION) {
		PSID group;
		Status = RtlGetGroupSecurityDescriptor(vnode_security(vp),
		    &group, &defaulted);
		if (Status == STATUS_SUCCESS) {
			// uid/gid reverse is identical
			vattr.va_gid = zfs_sid2uid(group);
			vattr.va_mask |= ATTR_GID;
		}
	}

	// Do we need to update ZFS?
	if (vattr.va_mask != 0) {
		zfs_setattr(zp, &vattr, 0, NULL, NULL);
		Status = STATUS_SUCCESS;
	}

	Irp->IoStatus.Information = 0;
	zfs_send_notify(zfsvfs, zccb->z_name_cache, zccb->z_name_offset,
	    FILE_NOTIFY_CHANGE_SECURITY,
	    FILE_ACTION_MODIFIED);

	zfs_save_ntsecurity(vp);

err:
	VN_RELE(vp);
	return (Status);
}

// When passed SD, it should be merged with parent.
NTSTATUS
merge_security(vnode_t *vp, PACCESS_STATE as)
{
	NTSTATUS Status = STATUS_SUCCESS;
	vnode_t *dvp;
	PSECURITY_DESCRIPTOR parent_sd;
	PSECURITY_DESCRIPTOR new_sd = NULL;

	dvp = zfs_parent(vp);
	parent_sd = vnode_security(dvp);

	Status = SeAssignSecurityEx(parent_sd, as->SecurityDescriptor,
	    (void **) &new_sd, NULL,
	    vnode_isdir(vp), SEF_SACL_AUTO_INHERIT,
	    &as->SubjectSecurityContext,
	    IoGetFileObjectGenericMapping(), PagedPool);

	VN_RELE(dvp);

#if 0
	Status = RtlGetOwnerSecurityDescriptor(fcb->sd, &owner, &defaulted);
	if (!NT_SUCCESS(Status)) {
		zp->z_uid = UID_NOBODY;
	} else {
		zp->z_uid = sid_to_uid(owner);
	}

	find_gid(vp, dvp, &as->SubjectSecurityContext);
#endif

	// We probably do not have an old sd, but just in case.
	PSECURITY_DESCRIPTOR oldsd;
	oldsd = vnode_security(vp);

	vnode_setsecurity(vp, new_sd);
	zfs_save_ntsecurity(vp);

	if (oldsd)
		ExFreePool(oldsd);

	return (Status);
}

// #define	IOCTL_VOLUME_BASE ((DWORD) 'V')
// #define	IOCTL_VOLUME_GET_GPT_ATTRIBUTES
//   CTL_CODE(IOCTL_VOLUME_BASE,14,METHOD_BUFFERED,FILE_ANY_ACCESS)

#define	IOCTL_VOLUME_POST_ONLINE \
	CTL_CODE(IOCTL_VOLUME_BASE, 25, METHOD_BUFFERED, \
    FILE_READ_ACCESS | FILE_WRITE_ACCESS)

NTSTATUS
ioctl_storage_get_device_number(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{

	if (IrpSp->Parameters.QueryFile.Length <
	    sizeof (STORAGE_DEVICE_NUMBER)) {
		Irp->IoStatus.Information = sizeof (STORAGE_DEVICE_NUMBER);
		return (STATUS_BUFFER_TOO_SMALL);
	}

	PSTORAGE_DEVICE_NUMBER sdn = Irp->AssociatedIrp.SystemBuffer;
	sdn->DeviceNumber = 0;
	sdn->DeviceType = FILE_DEVICE_DISK;
	sdn->PartitionNumber = -1; // -1 means can't be partitioned

	Irp->IoStatus.Information = sizeof (STORAGE_DEVICE_NUMBER);
	return (STATUS_SUCCESS);
}


NTSTATUS
ioctl_volume_get_volume_disk_extents(PDEVICE_OBJECT DeviceObject,
    PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	VOLUME_DISK_EXTENTS *vde = Irp->AssociatedIrp.SystemBuffer;
	mount_t *zmo = DeviceObject->DeviceExtension;
	int error;

	// One DISK_EXTENT is included, and we only ever reply with one.
	ULONG requiredSize = sizeof (VOLUME_DISK_EXTENTS);

	if (IrpSp->Parameters.QueryFile.Length < requiredSize) {
		Irp->IoStatus.Information = requiredSize;
		return (STATUS_BUFFER_TOO_SMALL);
	}

	Irp->IoStatus.Information = requiredSize;
	vde->NumberOfDiskExtents = 1;
	vde->Extents[0].DiskNumber = 0;
	vde->Extents[0].StartingOffset.QuadPart = 0ULL;
	vde->Extents[0].ExtentLength.QuadPart = 1024 * 1024 * 1024ULL;

	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	if (zfsvfs != NULL) {
		if ((error = zfs_enter(zfsvfs, FTAG)) == 0) {
			uint64_t refdbytes, availbytes, usedobjs, availobjs;
			dmu_objset_space(zfsvfs->z_os,
			    &refdbytes, &availbytes, &usedobjs, &availobjs);
			vde->Extents[0].ExtentLength.QuadPart =
			    refdbytes * availbytes;
			zfs_exit(zfsvfs, FTAG);
		}
	}

	return (STATUS_SUCCESS);
}

NTSTATUS
volume_create(PDEVICE_OBJECT DeviceObject, PFILE_OBJECT FileObject,
    USHORT ShareAccess, uint64_t AllocationSize, ACCESS_MASK DesiredAccess)
{
	mount_t *zmo = DeviceObject->DeviceExtension;

	if ((zmo->type != MOUNT_TYPE_VCB) &&
	    (zmo->type != MOUNT_TYPE_DCB))
		return (STATUS_INVALID_PARAMETER);

#if 1
	if (zmo->vpb != NULL)
		FileObject->Vpb = zmo->vpb;
	else
		FileObject->Vpb = DeviceObject->Vpb;
#endif

	if (vfs_flags(zmo) & MNT_UNMOUNTING)
		return (STATUS_VOLUME_DISMOUNTED);

	if ((ShareAccess == 0) &&
	    zmo->volume_opens != 0) {
		dprintf("%s: sharing violation\n", __func__);
		return (STATUS_SHARING_VIOLATION);
	}

	int error;
	zfsvfs_t *zfsvfs;
	znode_t *zp;
	struct vnode *vp;
	zfs_ccb_t *zccb = NULL;
	NTSTATUS status = STATUS_VOLUME_DISMOUNTED;

	zfsvfs = (zfsvfs_t *)vfs_fsprivate(zmo);

	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);

	error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);

	if (error == 0) {
		vp = ZTOV(zp);

		if (vp == NULL)
			goto out;
		ASSERT0(FileObject->FsContext);
		dprintf("%s increasing %p\n", __func__, vp);
		zfs_couplefileobject(vp, NULL,
		    FileObject, zp->z_size, &zccb,
		    AllocationSize,
		    DesiredAccess,
		    NULL);
		atomic_inc_64(&zmo->volume_opens);
		VN_RELE(vp);

		status = STATUS_SUCCESS;
	}

out:
	zfs_exit(zfsvfs, FTAG);

	return (status);
}

NTSTATUS
volume_close(PDEVICE_OBJECT DeviceObject, PFILE_OBJECT FileObject)
{
	mount_t *zmo = DeviceObject->DeviceExtension;
	VERIFY(zmo->type == MOUNT_TYPE_DCB);

	int error;
	zfsvfs_t *zfsvfs;
	znode_t *zp;
	struct vnode *vp;
	zfs_ccb_t *zccb = NULL;

	zfsvfs = (zfsvfs_t *)vfs_fsprivate(zmo);

	// This shouldnt happen, but it does sometimes
	if (zfsvfs == NULL)
		return (STATUS_DEVICE_NOT_READY);

	error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);

	if (error == 0) {
		vp = ZTOV(zp);
		zfs_decouplefileobject(vp, FileObject, B_TRUE);
		dprintf("%s decreasing %p\n", __func__, vp);
		vnode_rele(vp);
		atomic_dec_64(&zmo->volume_opens);

		VN_RELE(vp);
		return (STATUS_SUCCESS);
	}

	return (STATUS_SUCCESS);
}

/*
 * IRP_MJ_CLEANUP - sent when Windows is done with FileObject HANDLE
 * (one of many)
 * the vp is not released here, just decrease a count of vp.
 */
int
zfs_fileobject_cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp, vnode_t **hold_vp)
{
	int Status = STATUS_SUCCESS;
	mount_t *zmo = DeviceObject->DeviceExtension;
	PFILE_OBJECT FileObject = IrpSp->FileObject;
	struct vnode *vp = FileObject->FsContext;
	zfs_ccb_t *zccb = FileObject->FsContext2;

	if (zmo->type != MOUNT_TYPE_VCB) {
		Status = STATUS_SUCCESS;
		goto exit;
	}

	if (IrpSp->FileObject->Flags & FO_CLEANUP_COMPLETE) {
		dprintf("FileObject %p already cleaned up\n",
		    IrpSp->FileObject);
		Status = STATUS_SUCCESS;
		goto exit;
	}

	// We have to use the pointer to zmo stored in the vp,
	// as we can receive cleanup messages belonging to other devices.
	// (figure out what this means)
	if (!FileObject || !FileObject->FsContext) {
		Status = STATUS_SUCCESS;
		goto exit;
	}

	boolean_t locked = TRUE;

	FsRtlCheckOplock(vp_oplock(vp), Irp, NULL, NULL, NULL);

	znode_t *zp = VTOZ(vp); // zp for notify removal

	dprintf("IRP_MJ_CLEANUP: '%s' iocount %u usecount %u\n",
	    zccb && zccb->z_name_cache ? zccb->z_name_cache : "",
	    vp->v_iocount, vp->v_usecount);

	// ExAcquireResourceSharedLite(&fcb->Vcb->tree_lock, true);

	ExAcquireResourceExclusiveLite(vp->FileHeader.Resource, TRUE);

	IoRemoveShareAccess(FileObject, &vp->share_access);

	FsRtlFastUnlockAll(&vp->lock, FileObject,
	    IoGetRequestorProcess(Irp), NULL);

	if (zmo && zccb)
		FsRtlNotifyCleanup(zmo->NotifySync, &zmo->DirNotifyList,
		    zccb);

	vnode_setparent(vp, NULL);
	vnode_rele(vp); // Release longterm hold finally.

	// last close, OR, deleting
	if (!vnode_isinuse(vp, 0) ||
	    (zccb && zccb->deleteonclose)) {
		zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);

		if (!vnode_isinuse(vp, 0) &&
		    zccb && zccb->deleteonclose) {

			zccb->deleteonclose = 0;
			int isdir = vnode_isdir(vp);

			if (zccb->z_name_cache != NULL) {
				if (isdir) {
					dprintf("DIR: FileDelete "
					    "'%s' name '%s'\n",
					    zccb->z_name_cache,
					    &zccb->z_name_cache[
					    zccb->z_name_offset]);
					zfs_send_notify(zfsvfs,
					    zccb->z_name_cache,
					    zccb->z_name_offset,
					    FILE_NOTIFY_CHANGE_DIR_NAME,
					    FILE_ACTION_REMOVED);
				} else {
					dprintf("FILE: FileDelete "
					    "'%s' name '%s'\n",
					    zccb->z_name_cache,
					    &zccb->z_name_cache[
					    zccb->z_name_offset]);
				zfs_send_notify(zfsvfs,
				    zccb->z_name_cache,
				    zccb->z_name_offset,
				    FILE_NOTIFY_CHANGE_FILE_NAME,
				    FILE_ACTION_REMOVED);
				}
			}

			// Windows needs us to unlink it now, since
			// CLOSE can be delayed and parent deletions
			// might fail (ENOTEMPTY).

			// This releases zp!
			Status = delete_entry(DeviceObject, Irp, IrpSp);
			if (Status != 0) {
				dprintf("Deletion failed: %d\n",
				    Status);
			}
			// delete_entry will always consume an IOCOUNT.
			*hold_vp = NULL;

			zp = NULL;

// FILE_CLEANUP_UNKNOWN FILE_CLEANUP_WRONG_DEVICE FILE_CLEANUP_FILE_REMAINS
// FILE_CLEANUP_FILE_DELETED FILE_CLEANUP_LINK_DELETED
// FILE_CLEANUP_STREAM_DELETED FILE_CLEANUP_POSIX_STYLE_DELETE
#if defined(ZFS_FS_ATTRIBUTE_CLEANUP_INFO) && defined(ZFS_FS_ATTRIBUTE_POSIX)
			Irp->IoStatus.Information =
			    FILE_CLEANUP_FILE_DELETED |
			    FILE_CLEANUP_POSIX_STYLE_DELETE;
#elif defined(ZFS_FS_ATTRIBUTE_CLEANUP_INFO) && \
	defined(FILE_CLEANUP_FILE_DELETED)
			Irp->IoStatus.Information =
			    FILE_CLEANUP_FILE_DELETED;
#endif


			/* Not deleting, but lastclose */
		} else if (FileObject->Flags & FO_CACHE_SUPPORTED &&
		    FileObject->SectionObjectPointer->DataSectionObject) {
			IO_STATUS_BLOCK iosb;

			if (locked) {
				ExReleaseResourceLite(vp->FileHeader.Resource);
				locked = FALSE;
			}

			CcFlushCache(FileObject->SectionObjectPointer, NULL, 0,
			    &iosb);

			if (!NT_SUCCESS(iosb.Status))
				dprintf("CcFlushCache returned %08lx\n",
				    iosb.Status);

			if (!ExIsResourceAcquiredSharedLite(
			    vp->FileHeader.PagingIoResource)) {
				ExAcquireResourceExclusiveLite(
				    vp->FileHeader.PagingIoResource,
				    TRUE);
				ExReleaseResourceLite(
				    vp->FileHeader.PagingIoResource);
			}

			CcPurgeCacheSection(FileObject->SectionObjectPointer,
			    NULL, 0, FALSE);

			dprintf("flushed cache on close (fo = %p, vp = %p, "
			    "AllocationSize = %I64x, FileSize = %I64x, "
			    "ValidDataLength = %I64x)\n",
			    FileObject, vp,
			    vp->FileHeader.AllocationSize.QuadPart,
			    vp->FileHeader.FileSize.QuadPart,
			    vp->FileHeader.ValidDataLength.QuadPart);
		}
		// if (vp->Vcb && vp != vp->Vcb->volume_vp)
		CcUninitializeCacheMap(FileObject, NULL, NULL);
		if (zccb)
			zccb->cacheinit = 0;
	}

	if (locked)
		ExReleaseResourceLite(vp->FileHeader.Resource);

	// ExReleaseResourceLite(&vp->Vcb->tree_lock);

	FileObject->Flags |= FO_CLEANUP_COMPLETE;

	Status = STATUS_SUCCESS;

exit:
	if (vp && zp)
		dprintf("%s: '%s' iocount %u usecount %u Status 0x%lx.\n",
		    __func__,
		    zccb&&zccb->z_name_cache ? zccb->z_name_cache : "",
		    vp->v_iocount, vp->v_usecount, Status);

	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = 0;

	return (Status);
}

/*
 * IRP_MJ_CLOSE - sent when Windows is done with FileObject, and we can
 * free memory.
 */
int
zfs_fileobject_close(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp, vnode_t **hold_vp)
{
	int Status = STATUS_SUCCESS;

	ASSERT(hold_vp != NULL);

	if (IrpSp->FileObject) {

// Careful not to print something before vnode_fileobject_remove -
// if print is swapped out, we think fileobject is still valid.
// dprintf("IRP_MJ_CLOSE: '%wZ' \n", &IrpSp->FileObject->FileName);

		if (IrpSp->FileObject->FsContext) {
// dprintf("CLOSE clearing FsContext of FO 0x%llx\n",
// IrpSp->FileObject);
// Mark vnode for cleanup, we grab a HOLD to make sure it isn't
// released right here, but marked to be released upon
// reaching 0 count
			vnode_t *vp = IrpSp->FileObject->FsContext;
			zfs_ccb_t *zccb = IrpSp->FileObject->FsContext2;

			int isdir = vnode_isdir(vp);

/*
 * First encourage Windows to release the FileObject, CcMgr etc,
 * flush everything.
 */

			// FileObject should/could no longer point to vp.
			// this also frees zccb
			zfs_decouplefileobject(vp, IrpSp->FileObject, B_FALSE);
			zccb = NULL;

			// vnode_fileobject_remove(vp, IrpSp->FileObject);
//			if (isdir) {

			// fastfat also flushes while(parent) dir here,
			// if !iocount
//			}

/*
 * If we can release now, do so.
 * If the reference count for the per-file context structure reaches zero
 * and both the ImageSectionObject and DataSectionObject of the
 * SectionObjectPointers field from the FILE_OBJECT is zero, the
 * filter driver may then delete the per-file context data.
 *
 */

			if (!vnode_isvroot(vp)) {

/* Take hold from dispatcher, try to release in recycle */
				// *hold_vp = NULL;

// Release vp - vnode_recycle expects iocount==1
// we don't recycle root (unmount does) or RELE on
// recycle error
//				if (vnode_isvroot(vp) ||
//				    (vnode_recycle(vp) != 0)) {
// If recycle failed, manually release dispatcher's HOLD
//					dprintf("IRP_CLOSE failed to recycle. "
//					    "is_empty %d\n",
//					    vnode_fileobject_empty(vp, 1));
				//	VN_RELE(vp);
//				}

				Status = STATUS_SUCCESS;

				vp = NULL; // Paranoia, signal it is gone.

			} else { /* root node */
				Status = STATUS_SUCCESS;
			}
		}
		return (Status);
	}

	return (Status);
}

/*
 * We received a long-lived ioctl, so lets setup a taskq to handle it,
 * and return pending
 * This code was proof-of-concept, and is NOT used.
 */
void
zfsdev_async_thread(void *arg)
{
	NTSTATUS Status;
	PIRP Irp;
	Irp = (PIRP)arg;

	dprintf("%s: starting ioctl\n", __func__);

	/* Use FKIOCTL to make sure it calls memcpy instead */
	Status = zfsdev_ioctl(NULL, Irp, FKIOCTL);

	dprintf("%s: finished ioctl %ld\n", __func__, Status);

	PMDL mdl = Irp->Tail.Overlay.DriverContext[0];
	if (mdl) {
		UnlockAndFreeMdl(mdl);
		Irp->Tail.Overlay.DriverContext[0] = NULL;
	}
	void *fp = Irp->Tail.Overlay.DriverContext[1];
	if (fp) {
		ObDereferenceObject(fp);
		ZwClose(Irp->Tail.Overlay.DriverContext[2]);
	}

	IoCompleteRequest(Irp,
	    Status == STATUS_SUCCESS ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
}

/* Not used */
NTSTATUS
zfsdev_async(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	int error;
	PMDL mdl = NULL;
	PIO_STACK_LOCATION IrpSp;
	zfs_cmd_t *zc;
	void *fp = NULL;

	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	IoMarkIrpPending(Irp);

	/*
	 * A separate thread to the one that called us may not access the
	 * buffer from userland, So we have to map the in/out buffer,
	 * and put that address in its place.
	 */
	error = ddi_copysetup(
	    IrpSp->Parameters.DeviceIoControl.Type3InputBuffer,
	    sizeof (zfs_cmd_t),
	    &IrpSp->Parameters.DeviceIoControl.Type3InputBuffer, &mdl);
	if (error)
		return (error);

	/* Save the MDL so we can free it once done */
	Irp->Tail.Overlay.DriverContext[0] = mdl;

	/*
	 * We would also need to handle zc->zc_nvlist_src and zc->zc_nvlist_dst
	 * which is tricker, since they are unpacked into nvlists deep
	 * in zfsdev_ioctl
	 * The same problem happens for the filedescriptor from userland,
	 * also needs to be kernelMode
	 */
	zc = IrpSp->Parameters.DeviceIoControl.Type3InputBuffer;

	if (zc->zc_cookie) {
		error = ObReferenceObjectByHandle((HANDLE)zc->zc_cookie, 0, 0,
		    KernelMode, &fp, 0);
		if (error != STATUS_SUCCESS)
			goto out;
		Irp->Tail.Overlay.DriverContext[1] = fp;

		HANDLE h = NULL;
		error = ObOpenObjectByPointer(fp,
		    OBJ_FORCE_ACCESS_CHECK | OBJ_KERNEL_HANDLE, NULL,
		    GENERIC_READ|GENERIC_WRITE, *IoFileObjectType,
		    KernelMode, &h);
		if (error != STATUS_SUCCESS)
			goto out;
		dprintf("mapped filed is 0x%p\n", h);
		zc->zc_cookie = (uint64_t)h;
		Irp->Tail.Overlay.DriverContext[2] = h;
	}

	taskq_dispatch(system_taskq, zfsdev_async_thread, (void*)Irp, TQ_SLEEP);
	return (STATUS_PENDING);

out:
	if (mdl) {
		UnlockAndFreeMdl(mdl);
	}
	if (fp) {
		ObDereferenceObject(fp);
	}
	return (error);
}

/*
 * dispatcher for the "bus", so we can add pnp devices for mounting
 */
_Function_class_(DRIVER_DISPATCH)
    static NTSTATUS
    busDispatcher(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;
	PIRP Irp = *PIrp;
	ZFS_DRIVER_EXTENSION(WIN_DriverObject, DriverExtension);

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s busDeviceObject\n",
	    __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
	    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

	Status = STATUS_INVALID_DEVICE_REQUEST;
	int DoForward = 1;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_START_DEVICE:
			dprintf("IRP_MN_START_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = QueryDeviceRelations(DeviceObject, PIrp,
			    IrpSp);
			// No need to call Storport, and they completed Irp.
			// *PIrp = NULL;
			DoForward = 0;
			break;
		case IRP_MN_QUERY_CAPABILITIES:
			Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;

			// maybes
		case IRP_MN_QUERY_INTERFACE:
			Status = pnp_query_di(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_BUS_INFORMATION:
			dprintf("IRP_MN_QUERY_BUS_INFORMATION\n");
			Status = pnp_query_bus_information(DeviceObject, Irp,
			    IrpSp);
			break;

		// these are not handled in btrfs, pass down
		case IRP_MN_DEVICE_ENUMERATED:
			dprintf("IRP_MN_DEVICE_ENUMERATED\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
			dprintf("IRP_MN_FILTER_RESOURCE_REQUIREMENTS\n");
			Status = STATUS_SUCCESS;
			break;
		case 0x18:
			dprintf("IRP_MN_QUERY_LEGACY_BUS_INFORMATION\n");
			break;
		default:
			dprintf("**** unknown pnp IRP_MJ_PNP: 0x%lx\n",
			    IrpSp->MinorFunction); // 0x0b
			DoForward = 0;
			break;
		}

		/* All IRP_MJ_PNP are expected to be sent to lower devices */
		if (DriverExtension->StorportDeviceObject &&
		    DriverExtension->STOR_MajorFunction[IrpSp->MajorFunction] !=
		    NULL &&
		    DoForward) {
			NTSTATUS Status2;

			dprintf("STORport sending down: %s\n",
			    major2str(IrpSp->MajorFunction,
			    IrpSp->MinorFunction));

			IoCopyCurrentIrpStackLocationToNext(Irp);
			Status2 = IoCallDriver(
			    DriverExtension->StorportDeviceObject,
			    Irp);

			dprintf("STORport exit: 0x%lx %s "
			    "Information 0x%llx : %s\n",
			    Status2,
			    common_status_str(Status2),
			    Irp ? Irp->IoStatus.Information : 0,
			    major2str(IrpSp->MajorFunction,
			    IrpSp->MinorFunction));


			// Since we forwarded the Irp, we do not call
			// IrpCompleteRequest()
			*PIrp = NULL;

			if (Status == STATUS_INVALID_DEVICE_REQUEST)
				Status = Status2;
		}
		break;

	case IRP_MJ_DEVICE_CONTROL:
		dprintf("**** unknown pnp IRP_MJ_DEVICE_CONTROL: 0x%lx\n",
		    IrpSp->MinorFunction);
		break;

		/* Allow device \OpenZFS to be opened in WinObj */
	case IRP_MJ_CREATE:
	case IRP_MJ_CLEANUP:
	case IRP_MJ_CLOSE:
		Status = STATUS_SUCCESS;
		break;

		/* No mount expected on the bus */
#if 1
	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			dprintf("IRP_MN_MOUNT_VOLUME ioctl\n");
			Status = zfs_vnop_mount(DeviceObject, Irp, IrpSp);
		}
		break;
#endif
	default:
		dprintf("**** unknown pnp IRP_MJ_: 0x%lx\n",
		    IrpSp->MajorFunction); // 0x0e
	}

	return (Status);
}

/*
 * This is the ioctl handler for ioctl done directly on /dev/zfs node.
 * This means all the internal ZFS ioctls, like ZFS_IOC_SEND etc.
 * But, we will also get general Windows ioctls, not specific to
 * volumes, or filesystems.
 * Incidentally, cstyle is confused about the function_class
 */
_Function_class_(DRIVER_DISPATCH)
    static NTSTATUS
    ioctlDispatcher(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;
	PIRP Irp = *PIrp;
	ZFS_DRIVER_EXTENSION(WIN_DriverObject, DriverExtension);

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s ioctlDeviceObject\n",
	    __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
	    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

	Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("IRP_MJ_CREATE: zfsdev FileObject %p name '%wZ' "
		    "length %u flags 0x%x\n",
		    IrpSp->FileObject, IrpSp->FileObject->FileName,
		    IrpSp->FileObject->FileName.Length, IrpSp->Flags);
		Status = zfsdev_open((dev_t)IrpSp->FileObject, Irp);
		break;
	case IRP_MJ_CLOSE:
		Status = zfsdev_release((dev_t)IrpSp->FileObject, Irp);
		break;
	case IRP_MJ_DEVICE_CONTROL:
		{
			/* Is it a ZFS ioctl? */
			ulong_t cmd =
			    IrpSp->Parameters.DeviceIoControl.IoControlCode;

			if ((DEVICE_TYPE_FROM_CTL_CODE(cmd) == ZFSIOCTL_TYPE)) {
				ulong_t cmd2;

				cmd2 = DEVICE_FUNCTION_FROM_CTL_CODE(cmd);
				if ((cmd2 >= ZFSIOCTL_BASE + ZFS_IOC_FIRST &&
				    cmd2 < ZFSIOCTL_BASE + ZFS_IOC_LAST)) {

					cmd2 -= ZFSIOCTL_BASE;

/*
 * Some IOCTL are very long-living, so we will put them in the
 * background and return PENDING. Possibly we should always do
 * this logic, but some ioctls are really short lived.
 */
					switch (cmd2) {
					case ZFS_IOC_UNREGISTER_FS:
// We abuse returnedBytes to send back busy
						Irp->IoStatus.Information =
						    zfs_ioc_unregister_fs();
						Status = STATUS_SUCCESS;
						break;
/*
 * So to do ioctl in async mode is a hassle, we have to do the copyin/copyout
 * MDL work in *this* thread, as the thread we spawn does not have access.
 * This would also include zc->zc_nvlist_src / zc->zc_nvlist_dst, so
 * zfsdev_ioctl() would need to be changed quite a bit. The file-descriptor
 * passed in (zfs send/recv) also needs to be opened for kernel mode. This
 * code is left here as an example on how it can be done
 * (without zc->zc_nvlist_*) but we currently do not use it.
 * Everything is handled synchronously.
 *
 * case ZFS_IOC_SEND:
 *	Status = zfsdev_async(DeviceObject, Irp);
 *	break;
 *
 */
					default:
						Status = zfsdev_ioctl(
						    DeviceObject, Irp, 0);
					} // switch cmd for async
					break;
				}
			}
			/* Not ZFS ioctl, handle Windows ones */
			switch (cmd) {
			case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
				dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
				Status = 0;
				break;
			case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
				dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
				Status = ioctl_query_device_name(DeviceObject,
				    Irp, IrpSp);
				break;
			case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
				dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
				Status = ioctl_query_unique_id(DeviceObject,
				    Irp, IrpSp);
				break;
			case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
				dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
				Status = ioctl_query_stable_guid(DeviceObject,
				    Irp, IrpSp);
				break;
			case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED:
				dprintf("IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_"
				    "CREATED\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED:
				dprintf("IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_"
				    "DELETED\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
				dprintf("IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_"
				    "NAME\n");
				break;
			case IOCTL_VOLUME_ONLINE:
				dprintf("IOCTL_VOLUME_ONLINE\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_DISK_IS_WRITABLE:
				dprintf("IOCTL_DISK_IS_WRITABLE\n");
				mount_t *zmo = DeviceObject->DeviceExtension;
				VERIFY(zmo->type == MOUNT_TYPE_VCB);
				zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
				if (zfsvfs != NULL && zfsvfs->z_rdonly)
					Status = STATUS_MEDIA_WRITE_PROTECTED;
				else
					Status = STATUS_SUCCESS;
				break;
			case IOCTL_DISK_MEDIA_REMOVAL:
				dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_STORAGE_MEDIA_REMOVAL:
				dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_VOLUME_POST_ONLINE:
				dprintf("IOCTL_VOLUME_POST_ONLINE\n");
				Status = STATUS_SUCCESS;
				break;
				/* kstat ioctls */
			case KSTAT_IOC_CHAIN_ID:
				dprintf("KSTAT_IOC_CHAIN_ID\n");
				Status = spl_kstat_chain_id(DeviceObject, Irp,
				    IrpSp);
				break;
			case KSTAT_IOC_READ:
				dprintf("KSTAT_IOC_READ\n");
				Status = spl_kstat_read(DeviceObject, Irp,
				    IrpSp);
				break;
			case KSTAT_IOC_WRITE:
				dprintf("KSTAT_IOC_WRITE\n");
				Status = spl_kstat_write(DeviceObject, Irp,
				    IrpSp);
				break;
			default:
				dprintf("**** unknown Windows IOCTL: 0x%lx\n",
				    cmd);
			}

		}
		break;

	case IRP_MJ_CLEANUP:
		Status = STATUS_SUCCESS;
		break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			dprintf("IRP_MN_MOUNT_VOLUME ioctl\n");
			Status = zfs_vnop_mount(DeviceObject, Irp, IrpSp);
			break;
		default:
			dprintf("IRP_MJ_FILE_SYSTEM_CONTROL unknown case!\n");
			break;
		}
		break;

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_CAPABILITIES:
			Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = STATUS_NOT_IMPLEMENTED;
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_UNSUCCESSFUL;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
#if 0
			PVPB vpb = DeviceObject->Vpb;
			KIRQL OldIrql;
			IoAcquireVpbSpinLock(&OldIrql);
			vpb->ReferenceCount--;
			IoReleaseVpbSpinLock(&OldIrql);
#endif
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_START_DEVICE:
			dprintf("IRP_MN_START_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_QUERY_INTERFACE:
			Status = pnp_query_di(DeviceObject, Irp, IrpSp);
			break;
		default:
			dprintf("Unknown IRP_MJ_PNP(ioctl): 0x%x\n",
			    IrpSp->MinorFunction);
			break;
		}
		break;

	}

	return (Status);
}

/*
 * This is the IOCTL handler for the "virtual" disk volumes we create
 * to mount ZFS, and ZVOLs, things like get partitions, and volume size.
 * But also open/read/write/close requests of volume access (like dd'ing the
 * /dev/diskX node directly).
 */
_Function_class_(DRIVER_DISPATCH)
    static NTSTATUS
    diskDispatcher(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;
	PIRP Irp = *PIrp;

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s diskDeviceObject\n",
	    __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
	    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

	Status = STATUS_INVALID_DEVICE_REQUEST;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("IRP_MJ_CREATE: volume FileObject %p related %p "
		    "name '%wZ' flags 0x%x\n",
		    IrpSp->FileObject,
		    IrpSp->FileObject ?
		    IrpSp->FileObject->RelatedFileObject : NULL,
		    IrpSp->FileObject->FileName, IrpSp->Flags);

		Status = volume_create(DeviceObject, IrpSp->FileObject,
		    IrpSp->Parameters.Create.ShareAccess,
		    Irp->Overlay.AllocationSize.QuadPart,
		    IrpSp->Parameters.Create.SecurityContext->DesiredAccess);
		if (NT_SUCCESS(Status))
			Irp->IoStatus.Information = FILE_OPENED;
		break;
	case IRP_MJ_CLOSE:
		Status = volume_close(DeviceObject, IrpSp->FileObject);
		break;
	case IRP_MJ_DEVICE_CONTROL:
	{
		ulong_t cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
		/* Not ZFS ioctl, handle Windows ones */
		switch (cmd) {
		case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
			dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
			Status = ioctl_get_gpt_attributes(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
			Status = ioctl_query_device_name(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
			dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
			Status = ioctl_query_unique_id(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
			dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
			Status = ioctl_mountdev_query_stable_guid(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
			Status = ioctl_mountdev_query_suggested_link_name(
			    DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED:
			dprintf("IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED:
			dprintf("IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_MOUNTMGR_DELETE_POINTS:
			dprintf("IOCTL_MOUNTMGR_DELETE_POINTS\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_ONLINE:
			dprintf("IOCTL_VOLUME_ONLINE\n");
			if (vfs_isunmount(DeviceObject->DeviceExtension))
				Status = STATUS_VOLUME_DISMOUNTED;
			else
				Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_OFFLINE:
		case IOCTL_VOLUME_IS_OFFLINE:
			dprintf("IOCTL_VOLUME_OFFLINE\n");
			Status = STATUS_VOLUME_MOUNTED;
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_IS_WRITABLE:
			dprintf("IOCTL_DISK_IS_WRITABLE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_MEDIA_REMOVAL:
			dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_MEDIA_REMOVAL:
			dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_POST_ONLINE:
			dprintf("IOCTL_VOLUME_POST_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_GET_HOTPLUG_INFO:
			dprintf("IOCTL_STORAGE_GET_HOTPLUG_INFO\n");
			Status = ioctl_storage_get_hotplug_info(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_STORAGE_QUERY_PROPERTY:
			dprintf("IOCTL_STORAGE_QUERY_PROPERTY\n");
			Status = ioctl_storage_query_property(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
			dprintf("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\n");
			Status = ioctl_volume_get_volume_disk_extents(
			    DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_STORAGE_GET_DEVICE_NUMBER:
			dprintf("IOCTL_STORAGE_GET_DEVICE_NUMBER\n");
			Status = ioctl_storage_get_device_number(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_CHECK_VERIFY:
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_CHECK_VERIFY2:
			dprintf("IOCTL_STORAGE_CHECK_VERIFY2\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_IS_DYNAMIC:
		{
			uint8_t *buf = (UINT8*)Irp->AssociatedIrp.SystemBuffer;
			*buf = 1;
			Irp->IoStatus.Information = 1;
			Status = STATUS_SUCCESS;
			break;
		}
		case IOCTL_MOUNTDEV_LINK_CREATED:
			dprintf("IOCTL_MOUNTDEV_LINK_CREATED\n");
			Status = STATUS_SUCCESS;
			break;
		case 0x4d0010:
// Same as IOCTL_MOUNTDEV_LINK_CREATED but bit 14,15 are 0 (access permissions)
			dprintf("IOCTL_MOUNTDEV_LINK_CREATED v2\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_MOUNTDEV_LINK_DELETED:
			dprintf("IOCTL_MOUNTDEV_LINK_DELETED\n");
			Status = STATUS_SUCCESS;
			break;
		case 0x4d0014:
// Same as IOCTL_MOUNTDEV_LINK_DELETED but bit 14,15 are 0 (access permissions)
			dprintf("IOCTL_MOUNTDEV_LINK_DELETED v2\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_GET_PARTITION_INFO_EX:
			dprintf("IOCTL_DISK_GET_PARTITION_INFO_EX\n");
			Status = ioctl_disk_get_partition_info_ex(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_DRIVE_GEOMETRY:
			dprintf("IOCTL_DISK_GET_DRIVE_GEOMETRY\n");
			Status = ioctl_disk_get_drive_geometry(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:
			dprintf("IOCTL_DISK_GET_DRIVE_GEOMETRY_EX\n");
			Status = ioctl_disk_get_drive_geometry_ex(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_STORAGE_CHECK_VERIFY:
			dprintf("IOCTL_STORAGE_CHECK_VERIFY\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_GET_PARTITION_INFO:
			dprintf("IOCTL_DISK_GET_PARTITION_INFO\n");
			Status = ioctl_disk_get_partition_info(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_VOLUME_IS_IO_CAPABLE:
			dprintf("IOCTL_VOLUME_IS_IO_CAPABLE\n");
			Status = ioctl_volume_is_io_capable(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_DISK_GET_LENGTH_INFO:
			dprintf("IOCTL_DISK_GET_LENGTH_INFO\n");
			Status = ioctl_disk_get_length_info(DeviceObject, Irp,
			    IrpSp);
			break;
		case FSCTL_GET_VOLUME_BITMAP: // VSS
			Status = STATUS_INVALID_DEVICE_REQUEST;
			break;
		case FSCTL_GET_RETRIEVAL_POINTERS: // VSS
			dprintf("FSCTL_GET_RETRIEVAL_POINTERS\n");
			Status = fsctl_get_retrieval_pointers(DeviceObject, Irp,
			    IrpSp);
			break;
		default:
			dprintf("**** unknown disk Windows IOCTL: 0x%lx\n",
			    cmd);
	//		DbgBreakPoint();
		}

	}
	break;

	case IRP_MJ_CLEANUP:
		Status = STATUS_SUCCESS;
		break;

	// Technically we don't really let them read from the virtual
	// devices that hold the ZFS filesystem, so we just return all zeros.
	case IRP_MJ_READ:
		Status = volume_read(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_WRITE:
		dprintf("disk fake write\n");
		Irp->IoStatus.Information = IrpSp->Parameters.Write.Length;
		Status = STATUS_SUCCESS;
		break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			dprintf("IRP_MN_MOUNT_VOLUME disk\n");
			Status = zfs_vnop_mount(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_USER_FS_REQUEST:
			dprintf("IRP_MN_USER_FS_REQUEST: FsControlCode 0x%lx\n",
			    IrpSp->Parameters.FileSystemControl.FsControlCode);
			Status = user_fs_request(DeviceObject, PIrp, IrpSp);
			break;
		case IRP_MN_KERNEL_CALL:
			dprintf("IRP_MN_KERNEL_CALL: unknown 0x%lx\n",
			    IrpSp->Parameters.FileSystemControl.FsControlCode);
			Status = STATUS_INVALID_DEVICE_REQUEST;
			break;
		default:
			dprintf("IRP_MN_unknown: 0x%x\n", IrpSp->MinorFunction);
			break;
		}
		break;

	case IRP_MJ_QUERY_INFORMATION:
		dprintf("volume calling query_information warning\n");
		Status = query_information(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_QUERY_VOLUME_INFORMATION:
		Status = query_volume_information(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_START_DEVICE:
			dprintf("IRP_MN_START_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
			Status = STATUS_UNSUCCESSFUL;
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = QueryDeviceRelations(DeviceObject, PIrp,
			    IrpSp);
			break;
		case IRP_MN_DEVICE_USAGE_NOTIFICATION:
			dprintf("IRP_MN_DEVICE_USAGE_NOTIFICATION\n");
			Status = STATUS_SUCCESS;
			break;
		// The rest btrfs does not have, pass down
		case IRP_MN_QUERY_CAPABILITIES:
			dprintf("IRP_MN_QUERY_CAPABILITIES\n");
			// Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_INTERFACE:
			dprintf("IRP_MN_QUERY_INTERFACE\n");
			// Status = pnp_query_di(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_DEVICE_TEXT:
			dprintf("IRP_MN_QUERY_DEVICE_TEXT\n");
			break;
		case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
			dprintf("IRP_MN_QUERY_RESOURCE_REQUIREMENTS\n");
			break;
		case IRP_MN_QUERY_RESOURCES:
			dprintf("IRP_MN_QUERY_RESOURCES\n");
			break;
		case IRP_MN_QUERY_BUS_INFORMATION:
			dprintf("IRP_MN_QUERY_BUS_INFORMATION\n");
			/*
			 * not in btrfs Status =
			 * pnp_query_bus_information(DeviceObject, Irp, IrpSp);
			 */
			break;
		case IRP_MN_DEVICE_ENUMERATED:
			dprintf("IRP_MN_DEVICE_ENUMERATED\n");
			Status = STATUS_SUCCESS;
			break;
		default:
			dprintf("Unknown IRP_MJ_PNP(disk): 0x%x\n",
			    IrpSp->MinorFunction); // 0x18 0x0d

			break;
		}
		break;

	}

	return (Status);
}

/*
 * This is the main FileSystem IOCTL handler. This is where the filesystem
 * vnops happen and we handle everything with files and directories in ZFS.
 */
_Function_class_(DRIVER_DISPATCH)
    static NTSTATUS
    fsDispatcher(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP *PIrp,
    PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	struct vnode *hold_vp = NULL;
	PIRP Irp = *PIrp;
	ULONG len = 0;
	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s fsDeviceObject\n",
	    __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
	    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

#ifdef DEBUG_IOCOUNT
	int skiplock = 0;
/*
 * Watch out for re-entrant calls! MJ_READ, can call CCMGR, which calls
 * MJ_READ!
 * This could be a bigger issue, in that any mutex calls in the
 * zfs_read()/zfs_write stack could die - including rangelocks,
 * rwlocks. Investigate. (+zfs_freesp/zfs_trunc)
 */
	if (mutex_owned(&GIANT_SERIAL_LOCK))
		skiplock = 1;
	else
		mutex_enter(&GIANT_SERIAL_LOCK);

	zfsvfs_t *zfsvfs = NULL;
#endif

/*
 * Like VFS layer in upstream, we hold the "vp" here before calling into
 * the VNOP handlers.
 * There is one special case, IRP_MJ_CREATE / zfs_vnop_lookup, which has
 * no vp to start,
 * and assigns the vp on success (held).
 * We also pass "hold_vp" down to delete_entry, so it can release the
 * last hold to delete
 */

	if (IrpSp->FileObject && IrpSp->FileObject->FsContext) {
		hold_vp = IrpSp->FileObject->FsContext;
		if (VN_HOLD(hold_vp) != 0) {

			// If we were given a vp, but can't hold the vp,
			// we should fail this OP.
			Irp->IoStatus.Information = 0;
			hold_vp = NULL;
			// return (STATUS_INVALID_PARAMETER);

		} else {

			// Add FO to vp, if this is the first we've heard of it
			vnode_fileobject_add(IrpSp->FileObject->FsContext,
			    IrpSp->FileObject);

			// This is useful if you have iocount leaks, and do
			// only single-threaded operations
#ifdef DEBUG_IOCOUNT
			if (!vnode_isvroot(hold_vp) && vnode_isdir(hold_vp))
				ASSERT(hold_vp->v_iocount == 1);
			zfsvfs = VTOZ(hold_vp)->z_zfsvfs;
#endif
		}
	}
/*
 * Inside VNOP handlers, we no longer need to call VN_HOLD() on *this* vp
 * (but might for dvp etc) and eventually that code will be removed, if this
 * style works out.
 */

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		if (IrpSp->Parameters.Create.Options & FILE_OPEN_BY_FILE_ID)
			dprintf("IRP_MJ_CREATE: FileObject %p related %p "
			    "FileID 0x%llx flags 0x%x sharing 0x%x options "
			    "0x%lx\n",
			    IrpSp->FileObject,
			    IrpSp->FileObject ?
			    IrpSp->FileObject->RelatedFileObject :
			    NULL,
			    *((uint64_t *)IrpSp->FileObject->FileName.Buffer),
			    IrpSp->Flags, IrpSp->Parameters.Create.ShareAccess,
			    IrpSp->Parameters.Create.Options);
		else
			dprintf("IRP_MJ_CREATE: FileObject %p related %p "
			    "name '%wZ' flags 0x%x sharing 0x%x options "
			    "%s attr 0x%x DesAcc 0x%lx\n",
			    IrpSp->FileObject,
			    IrpSp->FileObject ?
			    IrpSp->FileObject->RelatedFileObject :
			    NULL,
			    IrpSp->FileObject->FileName, IrpSp->Flags,
			    IrpSp->Parameters.Create.ShareAccess,
			    create_options(IrpSp->Parameters.Create.Options),
			    IrpSp->Parameters.Create.FileAttributes,
			    IrpSp->Parameters.Create.SecurityContext->
			    DesiredAccess);

		Irp->IoStatus.Information = FILE_OPENED;
		Status = STATUS_SUCCESS;

#if 0
		// Disallow autorun.inf for now
		if (IrpSp && IrpSp->FileObject &&
		    IrpSp->FileObject->FileName.Buffer &&
		    _wcsicmp(IrpSp->FileObject->FileName.Buffer,
		    L"\\autorun.inf") == 0) {
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			Status = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		}
#endif

		mount_t *zmo = DeviceObject->DeviceExtension;
		VERIFY(zmo->type == MOUNT_TYPE_VCB);

		//
		//  Check if we are opening the volume and not a file/directory.
		//  We are opening the volume if the name is empty and there
		//  isn't a related file object.  If there is a related
		// file object then it is the Vcb itself.
		//

		// We have a name, so we are looking for something specific
		// Attempt to find the requested object
		if (IrpSp && IrpSp->FileObject &&
		    /* IrpSp->FileObject->FileName.Buffer && */
		    zmo) {

			Status = zfs_vnop_lookup(Irp, IrpSp, zmo);

			if (Status == EROFS)
				Status = STATUS_MEDIA_WRITE_PROTECTED;
		}
		break;

/*
 * CLEANUP comes before CLOSE. The IFSTEST.EXE on notifications
 * require them to arrive at CLEANUP time, and deemed too late
 * to be sent from CLOSE. It is required we act on DELETE_ON_CLOSE
 * in CLEANUP, which means we have to call delete here.
 * fastfat:
 * Close is invoked whenever the last reference to a file object is deleted.
 * Cleanup is invoked when the last handle to a file object is closed, and
 * is called before close.
 * The function of close is to completely tear down and remove the fcb/dcb/ccb
 * structures associated with the file object.
 * So for ZFS, CLEANUP will leave FsContext=vp around - to have it be freed in
 * CLOSE.
 */
	case IRP_MJ_CLEANUP:
		Status = zfs_fileobject_cleanup(DeviceObject, Irp, IrpSp,
		    &hold_vp);
		break;

	case IRP_MJ_CLOSE:
		Status = zfs_fileobject_close(DeviceObject, Irp, IrpSp,
		    &hold_vp);
		break;

	case IRP_MJ_DEVICE_CONTROL:
	{
		ulong_t cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
		/* Not ZFS ioctl, handle Windows ones */
		switch (cmd) {

		case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
			dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
			Status = ioctl_query_stable_guid(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_DISK_IS_WRITABLE:
			dprintf("IOCTL_DISK_IS_WRITABLE\n");
			Status = STATUS_SUCCESS;
			break;
#if 0
		case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
			Status = ioctl_query_device_name(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
			dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
			Status = 0;
			break;
		case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
			dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
			Status = ioctl_query_unique_id(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_VOLUME_ONLINE:
			dprintf("IOCTL_VOLUME_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_OFFLINE:
		case IOCTL_VOLUME_IS_OFFLINE:
			dprintf("IOCTL_VOLUME_OFFLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_MEDIA_REMOVAL:
			dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_MEDIA_REMOVAL:
			dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_POST_ONLINE:
			dprintf("IOCTL_VOLUME_POST_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_CHECK_VERIFY:
		case IOCTL_STORAGE_CHECK_VERIFY:
			dprintf("IOCTL_STORAGE_CHECK_VERIFY\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_GET_DRIVE_GEOMETRY:
			dprintf("IOCTL_DISK_GET_DRIVE_GEOMETRY\n");
			Status = ioctl_disk_get_drive_geometry(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:
			dprintf("IOCTL_DISK_GET_DRIVE_GEOMETRY_EX\n");
			Status = ioctl_disk_get_drive_geometry_ex(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_PARTITION_INFO:
			dprintf("IOCTL_DISK_GET_PARTITION_INFO\n");
			Status = ioctl_disk_get_partition_info(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_PARTITION_INFO_EX:
			dprintf("IOCTL_DISK_GET_PARTITION_INFO_EX\n");
			Status = ioctl_disk_get_partition_info_ex(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_VOLUME_IS_IO_CAPABLE:
			dprintf("IOCTL_VOLUME_IS_IO_CAPABLE\n");
			Status = ioctl_volume_is_io_capable(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_STORAGE_GET_HOTPLUG_INFO:
			dprintf("IOCTL_STORAGE_GET_HOTPLUG_INFO\n");
			Status = ioctl_storage_get_hotplug_info(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_DISK_GET_LENGTH_INFO:
			dprintf("IOCTL_DISK_GET_LENGTH_INFO\n");
			Status = ioctl_disk_get_length_info(DeviceObject, Irp,
			    IrpSp);
			break;
		case IOCTL_STORAGE_GET_DEVICE_NUMBER:
			dprintf("IOCTL_STORAGE_GET_DEVICE_NUMBER\n");
			Status = ioctl_storage_get_device_number(DeviceObject,
			    Irp, IrpSp);
			break;
		case IOCTL_STORAGE_QUERY_PROPERTY:
			dprintf("IOCTL_STORAGE_QUERY_PROPERTY\n");
			Status = ioctl_storage_query_property(DeviceObject, Irp,
			    IrpSp);
			break;
#endif
		case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS: // VSS
			dprintf("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\n");
			Status = ioctl_volume_get_volume_disk_extents(
			    DeviceObject, Irp, IrpSp);
			break;
		case FSCTL_DISMOUNT_VOLUME:
			dprintf("FSCTL_DISMOUNT_VOLUME\n");
			Status = STATUS_SUCCESS;
			break;
		case FSCTL_LOCK_VOLUME:
			dprintf("FSCTL_LOCK_VOLUME\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_MOUNTDEV_LINK_DELETED:
			dprintf("IOCTL_MOUNTDEV_LINK_DELETED\n");
			Status = STATUS_SUCCESS;
			break;
		case 0x4d0014:
// Same as IOCTL_MOUNTDEV_LINK_DELETED but bit 14,15 are 0 (access permissions)
			dprintf("IOCTL_MOUNTDEV_LINK_DELETED v2\n");
			Status = STATUS_SUCCESS;
			break;

		case IOCTL_VOLSNAP_FLUSH_AND_HOLD_WRITES:
		case 0x530018:
		case 0x534058:
			dprintf("IOCTL_VOLSNAP_FLUSH_AND_HOLD_WRITES\n");
			Status = STATUS_NOT_SUPPORTED;
			break;

		default:
			dprintf("**** unknown fsWindows IOCTL: 0x%lx\n", cmd);
		}

	}
	break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			dprintf("IRP_MN_MOUNT_VOLUME fs\n");
			Status = zfs_vnop_mount(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_USER_FS_REQUEST:
			Status = user_fs_request(DeviceObject, PIrp, IrpSp);
			break;
		case IRP_MN_VERIFY_VOLUME:
			Status = STATUS_SUCCESS;
			break;
			// FSCTL_QUERY_VOLUME_CONTAINER_STATE 0x90930
		case IRP_MN_KERNEL_CALL:
			dprintf("IRP_MN_KERNEL_CALL: unknown 0x%lx\n",
			    IrpSp->Parameters.FileSystemControl.FsControlCode);
			Status = STATUS_INVALID_DEVICE_REQUEST;
			break;
		default:
			dprintf("IRP_MJ_FILE_SYSTEM_CONTROL: unknown 0x%x\n",
			    IrpSp->MinorFunction);
			Status = STATUS_INVALID_DEVICE_REQUEST;
		}
		break;
	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = QueryDeviceRelations(DeviceObject, PIrp,
			    IrpSp);
			dprintf("DeviceRelations.Type 0x%x\n",
			    IrpSp->Parameters.QueryDeviceRelations.Type);
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		// These are not implemented in btrfs, so pass down.
		case IRP_MN_QUERY_INTERFACE:
			dprintf("IRP_MN_QUERY_DEVICE_TEXT\n");
			break;
		case IRP_MN_QUERY_DEVICE_TEXT:
			dprintf("IRP_MN_QUERY_DEVICE_TEXT\n");
			break;
		case IRP_MN_QUERY_BUS_INFORMATION:
			dprintf("IRP_MN_QUERY_BUS_INFORMATION\n");
			break;
		case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
			dprintf("IRP_MN_QUERY_RESOURCE_REQUIREMENTS\n");
			break;
		case IRP_MN_QUERY_RESOURCES:
			dprintf("IRP_MN_QUERY_RESOURCES\n");
			break;
		case IRP_MN_QUERY_CAPABILITIES:
			dprintf("IRP_MN_QUERY_CAPABILITIES\n");
			break;
		case IRP_MN_DEVICE_ENUMERATED:
			dprintf("IRP_MN_DEVICE_ENUMERATED\n");
			Status = STATUS_SUCCESS;
			break;
#if 0
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_DEVICE_USAGE_NOTIFICATION:
			dprintf("IRP_MN_DEVICE_USAGE_NOTIFICATION\n");
			Status = pnp_device_usage_notification(DeviceObject,
			    Irp, IrpSp);
			break;
#endif
		default:
			dprintf("Unknown IRP_MJ_PNP(fs): 0x%x\n",
			    IrpSp->MinorFunction);
			break;
		}
		break;
#if 1
	case IRP_MJ_QUERY_VOLUME_INFORMATION:
		Status = query_volume_information(DeviceObject, Irp, IrpSp);
		break;
#endif
	case IRP_MJ_LOCK_CONTROL:
		xprintf("lockcontrol!\n");
		Status = lock_control(DeviceObject, Irp, IrpSp);
		xprintf("lockcontrol ret %d\n", Status);
		break;

	case IRP_MJ_QUERY_INFORMATION:
		Status = query_information(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_DIRECTORY_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
			Status = notify_change_directory(DeviceObject, Irp,
			    IrpSp);
			break;
		case IRP_MN_QUERY_DIRECTORY:
			Status = query_directory(DeviceObject, Irp, IrpSp);
			break;
		}
		break;
	case IRP_MJ_SET_INFORMATION:
		Status = set_information(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_READ:
		len = IrpSp->Parameters.Read.Length;
		Status = fs_read(DeviceObject, Irp, IrpSp);
		if (Status == STATUS_PENDING) {
			IoMarkIrpPending(Irp);
			if (add_thread_job(DeviceObject, Irp,
			    len, IoWriteAccess))
				*PIrp = NULL;
			else
				Status = do_read_job(DeviceObject, Irp);
		}
		break;
	case IRP_MJ_WRITE:
		len = IrpSp->Parameters.Write.Length;
		Status = fs_write(DeviceObject, Irp, IrpSp);
		if (Status == STATUS_PENDING) {
			IoMarkIrpPending(Irp);
			if (add_thread_job(DeviceObject, Irp,
			    len, IoReadAccess))
				*PIrp = NULL;
			else
				Status = do_write_job(DeviceObject, Irp);
		}
		break;
	case IRP_MJ_FLUSH_BUFFERS:
		Status = flush_buffers(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_QUERY_SECURITY:
		Status = query_security(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_SET_SECURITY:
		Status = set_security(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_QUERY_EA:
		Status = query_ea(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_SET_EA:
		Status = set_ea(DeviceObject, Irp, IrpSp);
		break;
	case IRP_MJ_SHUTDOWN:
		dprintf("IRP_MJ_SHUTDOWN\n");
		Status = STATUS_SUCCESS;
		break;
	default:
		dprintf("**** unknown fsWindows IOCTL: 0x%lx\n",
		    IrpSp->MajorFunction);
		break;
	}

	/* If we held the vp above, release it now. */
	if (hold_vp != NULL) {
		VN_RELE(hold_vp);
	}

#ifdef DEBUG_IOCOUNT
	// Since we have serialised all fsdispatch() calls, and we are
	// about to leave - all iocounts should be zero, check that is true.
	// ZFSCallbackAcquireForCreateSection() can give false positives, as
	// they are called async, outside IRP dispatcher.
	if (!skiplock) {
		// Wait for all async_rele to finish
		if (zfsvfs)
			taskq_wait(dsl_pool_zrele_taskq(dmu_objset_pool(
			    zfsvfs->z_os)));
		vnode_check_iocount();
		mutex_exit(&GIANT_SERIAL_LOCK);
	}
#endif

	return (Status);
}

/*
 * ALL ioctl requests come in here, and we do the Windows specific
 * work to handle IRPs then we sort out the type of request
 * (ioctl, volume, filesystem) and call each respective handler.
 * Update
 * Our fsDispatcher() (VDO) can get a request it would normally not
 * handle, so it should pass it down using
 * IoCallDriver(DeviceObject->AttachedDevice, Irp);
 * This is probably our diskDispatcher() (PDO), but that is the
 * standard Windows driver model.
 * diskDispatcher() should also do the same.
 * We use the ntstatus STATUS_INVALID_DEVICE_REQUEST to indicate
 * we have not handled the request at this level.
 */
_Function_class_(DRIVER_DISPATCH)
    NTSTATUS
    dispatcher(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
	BOOLEAN TopLevel = FALSE;
	BOOLEAN AtIrqlPassiveLevel = FALSE;
	PIO_STACK_LOCATION IrpSp;
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
	mount_t *zmo = NULL;

	// Storport can call itself (and hence, ourselves) so this isn't
	// always true.
	PAGED_CODE();

	// dprintf("%s: enter\n", __func__);

	//  If we were called with our file system device object instead of a
	//  volume device object, just complete this request with STATUS_SUCCESS
#if 0
	if (vnop_deviceObject == VolumeDeviceObject) {
		dprintf("%s: own object\n", __func__);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		Irp->IoStatus.Information = FILE_OPENED;
		IoCompleteRequest(Irp, IO_DISK_INCREMENT);
		return (STATUS_SUCCESS);
	}
#endif
	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	dprintf("%s: enter: major %d: minor %d: %s: type 0x%x: fo %p\n",
	    __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
	    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction),
	    Irp->Type, IrpSp->FileObject);

	KIRQL saveIRQL;
	saveIRQL = KeGetCurrentIrql();

	ZFS_DRIVER_EXTENSION(WIN_DriverObject, DriverExtension);

	AtIrqlPassiveLevel = (KeGetCurrentIrql() == PASSIVE_LEVEL);
	if (AtIrqlPassiveLevel) {
		FsRtlEnterFileSystem();
	}
	if (IoGetTopLevelIrp() == NULL) {
		IoSetTopLevelIrp(Irp);
		TopLevel = TRUE;
	}

	if (DeviceObject == DriverExtension->ioctlDeviceObject)
		Status = ioctlDispatcher(DeviceObject, &Irp, IrpSp);
	else {
		zmo = DeviceObject->DeviceExtension;
		if (zmo && zmo->type == MOUNT_TYPE_BUS)
			Status = busDispatcher(DeviceObject, &Irp, IrpSp);
		else if (zmo && zmo->type == MOUNT_TYPE_DGL)
			Status = busDispatcher(DeviceObject, &Irp, IrpSp);
		else if (zmo && zmo->type == MOUNT_TYPE_DCB)
			Status = diskDispatcher(DeviceObject, &Irp, IrpSp);
		else if (zmo && zmo->type == MOUNT_TYPE_VCB)
			Status = fsDispatcher(DeviceObject, &Irp, IrpSp);
		else {

			if (DriverExtension->STOR_MajorFunction[
			    IrpSp->MajorFunction] != NULL) {

				if (TopLevel) {
					IoSetTopLevelIrp(NULL);
				}
				if (AtIrqlPassiveLevel) {
					FsRtlExitFileSystem();
				}

				// If unload, refuse everything.
				if (DriverExtension->ioctlDeviceObject ==
				    NULL) {
					Status = STATUS_DEVICE_NOT_READY;
					Irp->IoStatus.Status = Status;

					IoCompleteRequest(Irp,
					    Status == STATUS_SUCCESS ?
					    IO_DISK_INCREMENT :
					    IO_NO_INCREMENT);
					return (Status);
				}

				dprintf("dispatcher: IRP to STORport\n");

				Status = DriverExtension->STOR_MajorFunction[
				    IrpSp->MajorFunction]
				    (DeviceObject, Irp);

				dprintf("dispatcher: STORport exit: "
				    "0x%lx %s Information 0x%llx : %s\n",
				    Status,
				    common_status_str(Status),
				    Irp ? Irp->IoStatus.Information : 0,
				    major2str(IrpSp->MajorFunction,
				    IrpSp->MinorFunction));
				return (Status);
			}

			dprintf("This is weird, nobody to handle IRP\n");
			// Got a request we don't care about?
			Status = STATUS_INVALID_DEVICE_REQUEST;
			Irp->IoStatus.Information = 0;
			zmo = NULL;
		}
	}

	if (AtIrqlPassiveLevel) {
		FsRtlExitFileSystem();
	}
	if (TopLevel) {
		IoSetTopLevelIrp(NULL);
	}

	switch (Status) {
	case STATUS_INVALID_DEVICE_REQUEST:
		break;
	case STATUS_SUCCESS:
	case STATUS_BUFFER_OVERFLOW:
		break;
	case STATUS_PENDING:
		break;
	default:
		dprintf("%s: exit: 0x%lx %s Information 0x%llx : %s\n",
		    __func__, Status,
		    common_status_str(Status),
		    Irp ? Irp->IoStatus.Information : 0,
		    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));
	}

	// Complete the request if it isn't pending (ie, we
	// called zfsdev_async())
#if 0
	if ((Status == STATUS_INVALID_DEVICE_REQUEST) && Irp &&
	    zmo != NULL &&
	    zmo->AttachedDevice != NULL &&
	    DriverExtension->ioctlDeviceObject) {
		dprintf("Passing request %s down\n",
		    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

		IoSkipCurrentIrpStackLocation(Irp);
		Status = IoCallDriver(zmo->AttachedDevice, Irp);
		dprintf("Lower Device said 0x%0x %s\n", Status,
		    common_status_str(Status));

	} else
#endif
	if ((Status == STATUS_INVALID_DEVICE_REQUEST) && Irp &&
	    zmo != NULL &&
	    DriverExtension->LowerDeviceObject != NULL &&
	    DriverExtension->ioctlDeviceObject) {
#if 1
		dprintf("Passing request %s down bus\n",
		    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));
		PDEVICE_OBJECT Attached;
		Attached = DriverExtension->LowerDeviceObject;
		while (Attached->DriverObject == WIN_DriverObject)
			Attached = Attached->AttachedDevice;
		if (Attached) {
			IoSkipCurrentIrpStackLocation(Irp);
			Status = IoCallDriver(
			    DriverExtension->LowerDeviceObject,
			    Irp);
			dprintf("Lower Bus Device said 0x%0x %s\n", Status,
			    common_status_str(Status));
		}
#endif
	} else if ((Status == STATUS_INVALID_DEVICE_REQUEST) && Irp &&
	    zmo != NULL && // vcb->parent_device = dcb
	    zmo->parent_device != NULL &&
	    DriverExtension->ioctlDeviceObject) {
		dprintf("Passing request %s down bus\n",
		    major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

		zmo = zmo->parent_device; // We are now dcb
		Status = diskDispatcher(zmo->FunctionalDeviceObject, &Irp,
		    IrpSp);
		dprintf("Direct DDCB said 0x%0x %s\n", Status,
		    common_status_str(Status));

	} else if (Status != STATUS_PENDING && Irp != NULL) {
		// IOCTL_STORAGE_GET_HOTPLUG_INFO
		// IOCTL_DISK_CHECK_VERIFY
		// IOCTL_STORAGE_QUERY_PROPERTY

		Irp->IoStatus.Status = Status;

		IoCompleteRequest(Irp,
		    Status == STATUS_SUCCESS ? IO_DISK_INCREMENT :
		    IO_NO_INCREMENT);

	} else if (Status == STATUS_PENDING && Irp == NULL) {
		// If Irp is NULL, we are not to IoComplete the IRP
		// as we are to wait, see FSCTL_REQUEST_OPLOCK.

	} else {
		// DbgBreakPoint();
	}

	VERIFY3U(saveIRQL, ==, KeGetCurrentIrql());


	return (Status);
}

NTSTATUS
ZFSCallbackAcquireForCreateSection(
	IN PFS_FILTER_CALLBACK_DATA CallbackData,
	OUT PVOID *CompletionContext)
{
	ASSERT(CallbackData->SizeOfFsFilterCallbackData ==
	    sizeof (FS_FILTER_CALLBACK_DATA));

	dprintf("%s: Operation 0x%x \n", __func__,
	    CallbackData->Operation);

	struct vnode *vp;
	vp = CallbackData->FileObject->FsContext;

	ASSERT(vp != NULL);
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

#ifdef DEBUG_IOCOUNT
	int nolock = 0;
	if (mutex_owned(&GIANT_SERIAL_LOCK))
		nolock = 1;
	else
		mutex_enter(&GIANT_SERIAL_LOCK);
#endif
	if (VN_HOLD(vp) == 0) {
		dprintf("%s: locked: %p\n", __func__, vp->FileHeader.Resource);
		ExAcquireResourceExclusiveLite(vp->FileHeader.Resource, TRUE);
		vnode_ref(vp);
		VN_RELE(vp);
	} else {
#ifdef DEBUG_IOCOUNT
		if (!nolock)
			mutex_exit(&GIANT_SERIAL_LOCK);
#endif
		return (STATUS_INVALID_PARAMETER);
	}
#ifdef DEBUG_IOCOUNT
	if (!nolock)
		mutex_exit(&GIANT_SERIAL_LOCK);
#endif

	if (CallbackData->
	    Parameters.AcquireForSectionSynchronization.SyncType !=
	    SyncTypeCreateSection) {
		return (STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY);
	} else if (vp->share_access.Writers == 0) {
		return (STATUS_FILE_LOCKED_WITH_ONLY_READERS);
	} else {
		return (STATUS_FILE_LOCKED_WITH_WRITERS);
	}

}

NTSTATUS
ZFSCallbackReleaseForCreateSection(
	IN PFS_FILTER_CALLBACK_DATA CallbackData,
	OUT PVOID *CompletionContext)
{
	struct vnode *vp;
	vp = CallbackData->FileObject->FsContext;

	dprintf("%s: vp %p\n", __func__, vp);

	ASSERT(vp != NULL);
	if (vp == NULL)
		return (STATUS_INVALID_PARAMETER);

	dprintf("%s: unlocked: %p\n",
	    __func__, vp->FileHeader.Resource);
	if (VN_HOLD(vp) == 0) {
		ExReleaseResourceLite(vp->FileHeader.Resource);
		vnode_rele(vp);
		VN_RELE(vp);
		return (STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY);
	}

	dprintf("%s WARNING FAILED\n", __func__);
	return (STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY);
}

void
zfs_windows_vnops_callback(PDEVICE_OBJECT deviceObject)
{

}

int
zfs_vfsops_init(void)
{

#ifdef DEBUG_IOCOUNT
	mutex_init(&GIANT_SERIAL_LOCK, NULL, MUTEX_DEFAULT, NULL);
#endif
	return (0);
}

int
zfs_vfsops_fini(void)
{

#ifdef DEBUG_IOCOUNT
	mutex_destroy(&GIANT_SERIAL_LOCK);
#endif
	return (0);
}

NTSTATUS
pnp_query_di(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS status;

	if (IsEqualGUID(IrpSp->Parameters.QueryInterface.InterfaceType,
	    &ZFSZVOLDI_GUID)) {
		if (IrpSp->Parameters.QueryInterface.Version < 1)
			status = STATUS_NOT_SUPPORTED;
		else if (IrpSp->Parameters.QueryInterface.Size <
		    sizeof (zfsdizvol_t))
			status = STATUS_BUFFER_TOO_SMALL;
		else if ((IrpSp->
		    Parameters.QueryInterface.InterfaceSpecificData ==
		    NULL) || strlen(IrpSp->
		    Parameters.QueryInterface.InterfaceSpecificData) <= 8)
			status = STATUS_INVALID_PARAMETER;
		else {
			PVOID zv; // zvol_state_t*, opaque here
			uint32_t openCount;
			extern PVOID zvol_name2zvolState(const char *name,
			    uint32_t *openCount);
			PCHAR vendorUniqueId = (PCHAR)IrpSp->
			    Parameters.QueryInterface.InterfaceSpecificData;
			zv = zvol_name2zvolState(&vendorUniqueId[8],
			    &openCount);
			dprintf("what is this pnp\n");
			// check that the minor number is non-zero: that
			// signifies the zvol has fully completed its
			// bringup phase.
			if (zv && openCount) {
				extern void IncZvolRef(PVOID Context);
				extern void DecZvolRef(PVOID Context);
				extern NTSTATUS ZvolDiRead(PVOID Context,
				    zfsiodesc_t *pIo);
				extern NTSTATUS ZvolDiWrite(PVOID Context,
				    zfsiodesc_t *pIo);
				// lock in an extra reference on the zvol
				IncZvolRef(zv);
				zfsdizvol_t *pDI = (zfsdizvol_t *)IrpSp->
				    Parameters.QueryInterface.Interface;
				pDI->header.Size = sizeof (zfsdizvol_t);
				pDI->header.Version = ZFSZVOLDI_VERSION;
				pDI->header.Context = zv;
				pDI->header.InterfaceReference = IncZvolRef;
				pDI->header.InterfaceDereference = DecZvolRef;
				pDI->Read = ZvolDiRead;
				pDI->Write = ZvolDiWrite;
				Irp->IoStatus.Information = 0;
				status = STATUS_SUCCESS;
			}
			else
				status = STATUS_NOT_FOUND;
		}
	} else
		status = STATUS_INVALID_DEVICE_REQUEST;

	return (status);
}

DEFINE_GUID(GUID_BUS_TYPE_PCI,
    0xc8ebdfb0, 0xb510, 0x11d0, 0x80, 0xe5, 0x00, 0xa0,
    0xc9, 0x25, 0x42, 0xe3);

NTSTATUS
pnp_query_bus_information(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	PPNP_BUS_INFORMATION busInfo;

	// Allocate memory for the bus information structure
	busInfo = (PPNP_BUS_INFORMATION)
	    ExAllocatePoolWithTag(PagedPool,
	    sizeof (PNP_BUS_INFORMATION),
	    'nIbQ');

	if (busInfo == NULL)
		return (STATUS_INSUFFICIENT_RESOURCES);

	// Fill in the bus information structure
	busInfo->BusTypeGuid = GUID_BUS_TYPE_PCI;  // Change as appropriate
	busInfo->LegacyBusType = PNPBus;
	busInfo->BusNumber = 0;  // Change as appropriate

	// Set the bus information in the IRP
	Irp->IoStatus.Information = (ULONG_PTR)busInfo;

	return (STATUS_SUCCESS);
}

NTSTATUS
pnp_device_usage_notification(PDEVICE_OBJECT DeviceObject, PIRP Irp,
    PIO_STACK_LOCATION IrpSp)
{
	char *type;

	switch (IrpSp->Parameters.UsageNotification.Type) {
	case DeviceUsageTypePaging:
		type = "Paging";
		break;
	case DeviceUsageTypeDumpFile:
		type = "DumpFile";
		break;
	case DeviceUsageTypeBoot:
		type = "Boot";
		break;
	case DeviceUsageTypeGuestAssigned:
		type = "GuestAssigned";
		break;
	case DeviceUsageTypeHibernation:
		type = "Hibernation";
		break;
	case DeviceUsageTypePostDisplay:
		type = "PostDisplay";
		break;
	default:
		type = "Unknown";
		break;
	}

	dprintf("NT wants to %s a %s file.\n",
	    IrpSp->Parameters.UsageNotification.InPath ?
	    "create" : "remove",
	    type);

	/*
	 * I believe we should in fact send the IRP lower down
	 * and that is true of a few IRPs.
	 */

	return (STATUS_SUCCESS);
}


/* FastIO support */


#ifdef ZFS_HAVE_FASTIO

static BOOLEAN __stdcall
fastio_check_if_possible(PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
    ULONG LockKey, BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = FileObject->FsContext;
	LARGE_INTEGER quadlen;

	quadlen.QuadPart = Length;

	if (CheckForReadOperation) {
		if (FsRtlFastCheckLockForRead(&vp->lock, FileOffset,
		    &quadlen, LockKey, FileObject, PsGetCurrentProcess()))
			return (TRUE);
	} else {
		if (/*!vp->Vcb->readonly &&
		    !is_subvol_readonly(vp->subvol, NULL) && */
		    FsRtlFastCheckLockForWrite(&vp->lock, FileOffset,
		    &quadlen, LockKey, FileObject, PsGetCurrentProcess()))
			return (TRUE);
	}

	return (FALSE);
}

static BOOLEAN __stdcall
fastio_write(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
    ULONG Length, BOOLEAN Wait, ULONG LockKey, PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = FileObject->FsContext;
	BOOLEAN ret;

	// treelock
	ret = FsRtlCopyWrite(FileObject, FileOffset, Length,
	    Wait, LockKey, Buffer, IoStatus, DeviceObject);

	return (ret);
}

// It would perhaps be re-use the query_basic_info() call above, if some of the
// IrpSp is moved out of it.
static BOOLEAN __stdcall
fastio_query_basic_info(PFILE_OBJECT FileObject, BOOLEAN wait,
    PFILE_BASIC_INFORMATION fbi, PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp;

	dprintf("%s: \n", __func__);

	if (!FileObject || !FileObject->FsContext ||
	    !FileObject->FsContext2) {
		return (FALSE);
	}

	FsRtlEnterFileSystem();

	vp = FileObject->FsContext;

	if (VN_HOLD(vp) != 0) {
		FsRtlExitFileSystem();
		return (FALSE);
	}

	if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource, wait)) {
		VN_RELE(vp);
		FsRtlExitFileSystem();
		return (FALSE);
	}

	file_basic_information_impl(DeviceObject, FileObject, fbi, IoStatus);

	ExReleaseResourceLite(vp->FileHeader.Resource);
	VN_RELE(vp);
	FsRtlExitFileSystem();

	/* Return TRUE to say IoStatus was filled out */
	return (TRUE);
}

static BOOLEAN __stdcall
fastio_query_standard_info(PFILE_OBJECT FileObject,
    BOOLEAN wait, PFILE_STANDARD_INFORMATION fsi, PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp;

	dprintf("%s: \n", __func__);

	if (!FileObject || !FileObject->FsContext ||
	    !FileObject->FsContext2) {
		return (FALSE);
	}

	FsRtlEnterFileSystem();

	vp = FileObject->FsContext;

	if (VN_HOLD(vp) != 0) {
		FsRtlExitFileSystem();
		return (FALSE);
	}

	if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource, wait)) {
		VN_RELE(vp);
		FsRtlExitFileSystem();
		return (FALSE);
	}

	file_standard_information_impl(DeviceObject, FileObject, fsi,
	    sizeof (FILE_STANDARD_INFORMATION), IoStatus);

	ExReleaseResourceLite(vp->FileHeader.Resource);
	VN_RELE(vp);
	FsRtlExitFileSystem();

	/* Return TRUE to say IoStatus was filled out */
	return (TRUE);
}

#define	fastio_possible(vp) (!FsRtlAreThereCurrentFileLocks(&vp->lock) \
	/* && !fcb->Vcb->readonly */ ? FastIoIsPossible : FastIoIsQuestionable)

static BOOLEAN __stdcall
fastio_lock(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
    PLARGE_INTEGER Length, PEPROCESS ProcessId,	ULONG Key,
    BOOLEAN FailImmediately, BOOLEAN ExclusiveLock, PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject)
{
	BOOLEAN ret;
	vnode_t *vp = FileObject->FsContext;

	dprintf("%s: \n", __func__);

	if (!vnode_isreg(vp)) {
		dprintf("%s: can only lock files\n", __func__);
		IoStatus->Status = STATUS_INVALID_PARAMETER;
		IoStatus->Information = 0;
		return (TRUE);
	}

	FsRtlEnterFileSystem();
	ExAcquireResourceSharedLite(vp->FileHeader.Resource, TRUE);

	ret = FsRtlFastLock(&vp->lock, FileObject, FileOffset, Length,
	    ProcessId, Key, FailImmediately, ExclusiveLock, IoStatus,
	    NULL, FALSE);

	if (ret)
		vp->FileHeader.IsFastIoPossible = fastio_possible(vp);

	ExReleaseResourceLite(vp->FileHeader.Resource);
	FsRtlExitFileSystem();

	return (ret);
}

static BOOLEAN __stdcall
fastio_unlock_single(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
    PLARGE_INTEGER Length, PEPROCESS ProcessId,	ULONG Key,
    PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = FileObject->FsContext;

	dprintf("%s: \n", __func__);

	IoStatus->Information = 0;

	if (!vnode_isreg(vp)) {
		dprintf("%s: can only lock files\n", __func__);
		IoStatus->Status = STATUS_INVALID_PARAMETER;
		return (TRUE);
	}

	FsRtlEnterFileSystem();

	IoStatus->Status = FsRtlFastUnlockSingle(&vp->lock, FileObject,
	    FileOffset, Length, ProcessId, Key, NULL, FALSE);

	vp->FileHeader.IsFastIoPossible = fastio_possible(vp);

	FsRtlExitFileSystem();

	return (TRUE);
}

static BOOLEAN __stdcall
fastio_unlock_all(PFILE_OBJECT FileObject, PEPROCESS ProcessId,
    PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = FileObject->FsContext;

	dprintf("%s: \n", __func__);

	IoStatus->Information = 0;

	if (!vnode_isreg(vp)) {
		dprintf("%s: can only lock files\n", __func__);
		IoStatus->Status = STATUS_INVALID_PARAMETER;
		return (TRUE);
	}

	FsRtlEnterFileSystem();

	ExAcquireResourceSharedLite(vp->FileHeader.Resource, TRUE);

	IoStatus->Status = FsRtlFastUnlockAll(&vp->lock, FileObject,
	    ProcessId, NULL);

	vp->FileHeader.IsFastIoPossible = fastio_possible(vp);

	ExReleaseResourceLite(vp->FileHeader.Resource);

	FsRtlExitFileSystem();

	return (TRUE);
}

static BOOLEAN __stdcall
fastio_unlock_all_by_key(PFILE_OBJECT FileObject, PVOID ProcessId,
    ULONG Key, PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = FileObject->FsContext;

	dprintf("%s: \n", __func__);

	IoStatus->Information = 0;

	if (!vnode_isreg(vp)) {
		dprintf("%s: can only lock files\n", __func__);
		IoStatus->Status = STATUS_INVALID_PARAMETER;
		return (TRUE);
	}

	FsRtlEnterFileSystem();

	ExAcquireResourceSharedLite(vp->FileHeader.Resource, TRUE);

	IoStatus->Status = FsRtlFastUnlockAllByKey(&vp->lock, FileObject,
	    ProcessId, Key, NULL);

	vp->FileHeader.IsFastIoPossible = fastio_possible(vp);

	ExReleaseResourceLite(vp->FileHeader.Resource);

	FsRtlExitFileSystem();

	return (TRUE);
}

static BOOLEAN
fastio_device_control(
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN Wait,
    IN PVOID InputBuffer OPTIONAL,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer OPTIONAL,
    IN ULONG OutputBufferLength,
    IN ULONG IoControlCode,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static void
fastio_acquire_file_for_ntsection(
    IN PFILE_OBJECT FileObject)
{
	vnode_t *vp;

	dprintf("%s: \n", __func__);

	if (!FileObject)
		return;

	vp = FileObject->FsContext;

	if (!vp || VN_HOLD(vp) != 0)
		return;

	FsRtlEnterFileSystem();
	ExAcquireResourceExclusiveLite(vp->FileHeader.Resource, TRUE);
	vnode_ref(vp);
	VN_RELE(vp);
	FsRtlExitFileSystem();
}

static void
fastio_release_file_for_ntsection(
    IN PFILE_OBJECT FileObject)
{
	vnode_t *vp;

	dprintf("%s: \n", __func__);

	if (!FileObject)
		return;

	vp = FileObject->FsContext;

	if (!vp || VN_HOLD(vp) != 0)
		return;

	FsRtlEnterFileSystem();
	ExReleaseResourceLite(vp->FileHeader.Resource);
	vnode_rele(vp);
	VN_RELE(vp);
	FsRtlExitFileSystem();
}

static BOOLEAN
fastio_detach_device(
    IN PDEVICE_OBJECT SourceDevice,
    IN PDEVICE_OBJECT TargetDevice)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static BOOLEAN __stdcall
fastio_query_network_open_info(PFILE_OBJECT FileObject,
    BOOLEAN Wait, FILE_NETWORK_OPEN_INFORMATION *fnoi,
    PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp;

	dprintf("%s: \n", __func__);

	if (!FileObject || !FileObject->FsContext ||
	    !FileObject->FsContext2) {
		return (FALSE);
	}

	FsRtlEnterFileSystem();

	vp = FileObject->FsContext;

	if (VN_HOLD(vp) != 0) {
		FsRtlExitFileSystem();
		return (FALSE);
	}

	if (!ExAcquireResourceSharedLite(vp->FileHeader.Resource, Wait)) {
		VN_RELE(vp);
		FsRtlExitFileSystem();
		return (FALSE);
	}

	file_network_open_information_impl(DeviceObject, FileObject, vp,
	    fnoi,
	    IoStatus);

	ExReleaseResourceLite(vp->FileHeader.Resource);
	VN_RELE(vp);
	FsRtlExitFileSystem();

	/* Return TRUE to say IoStatus was filled out */
	return (TRUE);
}

static NTSTATUS __stdcall
fastio_acquire_for_mod_write(PFILE_OBJECT FileObject,
    PLARGE_INTEGER EndingOffset, struct _ERESOURCE **ResourceToRelease,
    PDEVICE_OBJECT DeviceObject)
{
	vnode_t *vp = NULL;
	mount_t *zmo = DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	int error;
	NTSTATUS Status = STATUS_INVALID_PARAMETER;
	dprintf("%s: \n", __func__);

	if (unlikely(zfsvfs == NULL)) {
		dprintf("%s: fo %p already freed zfsvfs\n", __func__,
		    FileObject);
		return (STATUS_INVALID_PARAMETER);
	}

	FsRtlEnterFileSystem();

	if (vfs_busy(zfsvfs->z_vfs, 0) != 0) {
		FsRtlExitFileSystem();
		return (STATUS_INVALID_PARAMETER);
	}

	if (zfsvfs->z_unmounted ||
	    zfs_enter(zfsvfs, FTAG) != 0) {
		vfs_unbusy(zfsvfs->z_vfs);
		FsRtlExitFileSystem();
		return (STATUS_INVALID_PARAMETER);
	}

	vfs_unbusy(zfsvfs->z_vfs);

	vp = FileObject->FsContext;

	if (vp == NULL ||
	    VTOZ(vp) == NULL ||
	    VN_HOLD(vp) != 0) {
		zfs_exit(zfsvfs, FTAG);
		FsRtlExitFileSystem();
		return (STATUS_INVALID_PARAMETER);
	}
	zfs_exit(zfsvfs, FTAG);

	znode_t *zp = VTOZ(vp);

	if (!ExAcquireResourceExclusiveLite(vp->FileHeader.Resource, FALSE)) {
		dprintf("%s: returning STATUS_CANT_WAIT\n", __func__);
		Status = STATUS_CANT_WAIT;
		goto out;
	}

	*ResourceToRelease = vp->FileHeader.Resource;
	vnode_ref(vp);
	Status = STATUS_SUCCESS;

out:
	VN_RELE(vp);

	// No zfs_exit(zfsvfs, FTAG) until below

	dprintf("%s: returning STATUS_SUCCESS\n", __func__);
	FsRtlExitFileSystem();

	return (Status);
}

static NTSTATUS __stdcall
fastio_release_for_mod_write(PFILE_OBJECT FileObject,
    struct _ERESOURCE *ResourceToRelease, PDEVICE_OBJECT DeviceObject)
{
	mount_t *zmo = DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	vnode_t *vp;

	dprintf("%s:\n", __func__);

	FsRtlEnterFileSystem();

	ExReleaseResourceLite(ResourceToRelease);

	vp = FileObject->FsContext;
	if (vp && VN_HOLD(vp) == 0) {

		VERIFY3P(ResourceToRelease, ==, vp->FileHeader.Resource);
		vnode_rele(vp);
		VN_RELE(vp);

		FsRtlExitFileSystem();
		return (STATUS_SUCCESS);
	}

	dprintf("%s WARNING FAILED\n", __func__);
	FsRtlExitFileSystem();
	return (STATUS_SUCCESS);
}

static BOOLEAN
fastio_read_compressed(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN ULONG LockKey,
    IN PVOID Buffer,
    OUT PMDL *MdlChain,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PCOMPRESSED_DATA_INFO CompressedDataInfo,
    IN ULONG CompressedDataInfoLength,
    IN PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static BOOLEAN
fastio_write_compressed(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN ULONG LockKey,
    IN PVOID Buffer,
    OUT PMDL *MdlChain,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PCOMPRESSED_DATA_INFO CompressedDataInfo,
    IN ULONG CompressedDataInfoLength,
    IN PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static BOOLEAN
fastio_read_complete_compressed(
    IN PFILE_OBJECT FileObject,
    IN PMDL MdlChain,
    IN PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static BOOLEAN
fastio_write_complete_compressed(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PMDL MdlChain,
    IN PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	return (FALSE);
}

static BOOLEAN
fastio_query_open(PIRP Irp,
    OUT PFILE_NETWORK_OPEN_INFORMATION NetworkInformation,
    IN PDEVICE_OBJECT DeviceObject)
{
	/* Attempt to open Irp->FileObject->Filename and return stat() */
	char *filename, *lastname = NULL;
	int error = STATUS_INVALID_PARAMETER;
	ULONG outlen;
	mount_t *zmo = DeviceObject->DeviceExtension;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);
	struct vnode *vp = NULL, *dvp = NULL;

	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
#if 0
	// If it has never been open, make it do that through full
	// first, so vp is set.
	if (IrpSp->FileObject->FsContext == NULL)
		return (FALSE);
#endif
	dprintf("%s:\n", __func__);

	FsRtlEnterFileSystem();

	if (IrpSp->FileObject->FileName.Buffer != NULL &&
	    IrpSp->FileObject->FileName.Length > 0) {

		filename = kmem_alloc(PATH_MAX, KM_SLEEP);

		// Convert incoming filename to utf8
		error = RtlUnicodeToUTF8N(filename, PATH_MAX - 1, &outlen,
		    IrpSp->FileObject->FileName.Buffer,
		    IrpSp->FileObject->FileName.Length);

		if (error != STATUS_SUCCESS &&
		    error != STATUS_SOME_NOT_MAPPED) {
			dprintf("RtlUnicodeToUTF8N returned 0x%x "
			    "input len %d\n",
			    error, IrpSp->FileObject->FileName.Length);
			kmem_free(filename, PATH_MAX);
			FsRtlExitFileSystem();
			Irp->IoStatus.Status = STATUS_OBJECT_NAME_INVALID;
			Irp->IoStatus.Information = 0;
			return (FALSE);
		}

		filename[outlen] = 0;

		error = zfs_find_dvp_vp(zfsvfs, filename, 0, 0,
		    &lastname, &dvp, &vp, 0, 0);

// Handle reparse, or return FALSE/FAIL?
//		if (error == STATUS_REPARSE)
//			allocate_reparse(vp, lastname, Irp);
// But NTFS returns FALSE, so let's do same, so traces match.

		kmem_free(filename, PATH_MAX);

		if (dvp)
			VN_RELE(dvp);

		if (error == 0) {
			/* call sets the IoStatus */
			dprintf("%s: open OK stat()ing.\n", __func__);

			file_network_open_information_impl(DeviceObject,
			    IrpSp->FileObject, vp ? vp : dvp,
			    NetworkInformation,
			    &Irp->IoStatus);
			if (vp)
				VN_RELE(vp);

			FsRtlExitFileSystem();
			return (TRUE);
		}

		if (vp)
			VN_RELE(vp);

	}

	FsRtlExitFileSystem();

	/* Probably can skip setting these, we return FALSE */
	Irp->IoStatus.Status = error;
//	if (error == STATUS_REPARSE)
//		return (TRUE);

	Irp->IoStatus.Information = 0;

	return (FALSE);
}

static NTSTATUS __stdcall
fastio_acquire_for_ccflush(PFILE_OBJECT FileObject,
    PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
	return (STATUS_SUCCESS);
}

static NTSTATUS __stdcall
fastio_release_for_ccflush(PFILE_OBJECT FileObject,
    PDEVICE_OBJECT DeviceObject)
{
	dprintf("%s:\n", __func__);
	if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP)
		IoSetTopLevelIrp(NULL);
	return (STATUS_SUCCESS);
}

static FAST_IO_DISPATCH FastIoDispatch;

#endif // ZFS_HAVE_FASTIO

void
fastio_init(FAST_IO_DISPATCH **fast)
{
#ifdef ZFS_HAVE_FASTIO
	RtlZeroMemory(&FastIoDispatch, sizeof (FastIoDispatch));
	FastIoDispatch.SizeOfFastIoDispatch = sizeof (FAST_IO_DISPATCH);

	FastIoDispatch.FastIoCheckIfPossible = fastio_check_if_possible;
	FastIoDispatch.FastIoRead = FsRtlCopyRead;
	FastIoDispatch.FastIoWrite = fastio_write;
	FastIoDispatch.FastIoQueryBasicInfo = fastio_query_basic_info;
	FastIoDispatch.FastIoQueryStandardInfo = fastio_query_standard_info;
	FastIoDispatch.FastIoLock = fastio_lock;
	FastIoDispatch.FastIoUnlockSingle = fastio_unlock_single;
	FastIoDispatch.FastIoUnlockAll = fastio_unlock_all;
	FastIoDispatch.FastIoUnlockAllByKey = fastio_unlock_all_by_key;
	FastIoDispatch.FastIoDeviceControl = fastio_device_control;
	FastIoDispatch.AcquireFileForNtCreateSection =
	    fastio_acquire_file_for_ntsection;
	FastIoDispatch.ReleaseFileForNtCreateSection =
	    fastio_release_file_for_ntsection;
	FastIoDispatch.FastIoDetachDevice =
	    (PFAST_IO_DETACH_DEVICE) fastio_detach_device;
//	FastIoDispatch.FastIoQueryNetworkOpenInfo =
//	    fastio_query_network_open_info;
	FastIoDispatch.AcquireForModWrite = fastio_acquire_for_mod_write;
	FastIoDispatch.MdlRead = FsRtlMdlReadDev;
	FastIoDispatch.MdlReadComplete = FsRtlMdlReadCompleteDev;
	FastIoDispatch.PrepareMdlWrite = FsRtlPrepareMdlWriteDev;
	FastIoDispatch.MdlWriteComplete = FsRtlMdlWriteCompleteDev;
	FastIoDispatch.FastIoReadCompressed =
	    (PFAST_IO_READ_COMPRESSED) fastio_read_compressed;
	FastIoDispatch.FastIoWriteCompressed =
	    (PFAST_IO_WRITE_COMPRESSED) fastio_write_compressed;
	FastIoDispatch.MdlReadCompleteCompressed =
	    fastio_read_complete_compressed;
	FastIoDispatch.MdlWriteCompleteCompressed =
	    fastio_write_complete_compressed;
//	FastIoDispatch.FastIoQueryOpen = fastio_query_open;
	FastIoDispatch.ReleaseForModWrite = fastio_release_for_mod_write;
	FastIoDispatch.AcquireForCcFlush = fastio_acquire_for_ccflush;
	FastIoDispatch.ReleaseForCcFlush = fastio_release_for_ccflush;

	*fast = &FastIoDispatch;
//	dprintf("Using FASTIO\n");
#endif // ZFS_HAVE_FASTIO

}

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

/*
 * vss_provider.cpp - ZFS VSS software snapshot provider.
 *
 * Implements the three COM interfaces required by Windows VSS for a
 * software snapshot provider:
 *
 *   IVssSoftwareSnapshotProvider  - snapshot CRUD + volume queries
 *   IVssProviderCreateSnapshotSet - Prepare/Commit/Abort lifecycle
 *   IVssProviderNotifications     - load/unload hooks
 *
 * Snapshot lifecycle (VSS Coordinator calls in order):
 *   BeginPrepareSnapshot  - record (SnapshotId, VolumeName) pair
 *   EndPrepareSnapshots   - no-op
 *   PreCommitSnapshots    - no-op (ZFS needs no IO freeze)
 *   CommitSnapshots       - zfs_snapshot() via libzfs; store mapping
 *   PostCommitSnapshots   - no-op
 *   PostFinalCommitSnapshots - no-op
 *
 * GetSnapshotProperties fills m_pwszSnapshotDeviceObject with:
 *   "\\?\GLOBALROOT\Device\ZfsSnapshot<hex16>"
 * where hex16 is the ZFS ds_guid, matching the kernel device created
 * by zfs_vss.c in response to the snapshot creation event.
 */

#define	_CRT_SECURE_NO_WARNINGS
#define	WIN32_LEAN_AND_MEAN

#define MNTTAB_NO_DIRENT_H
extern "C" {
#include <libzfs.h>
}

#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>
#include <vss.h>
#include <vsprov.h>
#include <new>
#include <stdio.h>
#include <stdint.h>


#include "vss_provider.h"


void vss_provider_signal_stop(void);


/* ------------------------------------------------------------------ */
/* Diagnostic logging to C:\Windows\Temp\ZfsVssProvider.log            */
/* ------------------------------------------------------------------ */

static void
vss_log(const char *fmt, ...)
{
	static const wchar_t *path =
	    L"C:\\Windows\\Temp\\ZfsVssProvider.log";
	HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	char buf[1024];
	int hdr = _snprintf(buf, sizeof (buf), "[%lu] ", GetCurrentProcessId());
	if (hdr < 0)
		hdr = 0;

	va_list ap;
	va_start(ap, fmt);
	int len = _vsnprintf(buf + hdr, sizeof (buf) - hdr, fmt, ap);
	va_end(ap);
	if (len < 0)
		len = 0;

	DWORD written;
	WriteFile(h, buf, (DWORD)(hdr + len), &written, NULL);
	CloseHandle(h);
}

/*
 * CLSID_ZfsVssProvider = {89300202-3CAE-4584-B38F-E72F2B3A2FBF}
 * Used as both the COM CLSID and the VSS ProviderID.
 */
static const GUID CLSID_ZfsVssProvider = {
	0x89300202, 0x3CAE, 0x4584,
	{ 0xB3, 0x8F, 0xE7, 0x2F, 0x2B, 0x3A, 0x2F, 0xBF }
};

static LONG g_server_locks = 0;
static LONG g_obj_count    = 0;

/* ------------------------------------------------------------------ */
/* Pending snapshot state (filled by BeginPrepareSnapshot)             */
/* ------------------------------------------------------------------ */

#define	MAX_PENDING	32

struct PendingSnap {
	VSS_ID   SnapshotId;
	VSS_ID   SnapshotSetId;
	wchar_t  VolumeName[MAX_PATH];
	BOOL     used;
};

/* ------------------------------------------------------------------ */
/* IVssEnumObject - simple array-backed enumerator                     */
/* ------------------------------------------------------------------ */

class ZfsVssEnumObject : public IVssEnumObject {
public:
	ZfsVssEnumObject() : m_ref(1), m_pos(0), m_count(0), m_props(NULL) {
		InterlockedIncrement(&g_obj_count);
	}

	~ZfsVssEnumObject() {
		for (ULONG i = 0; i < m_count; i++) {
			if (m_props[i].Type != VSS_OBJECT_SNAPSHOT)
				continue;
			VSS_SNAPSHOT_PROP &s = m_props[i].Obj.Snap;
			CoTaskMemFree(s.m_pwszSnapshotDeviceObject);
			CoTaskMemFree(s.m_pwszOriginalVolumeName);
			CoTaskMemFree(s.m_pwszOriginatingMachine);
			CoTaskMemFree(s.m_pwszServiceMachine);
			CoTaskMemFree(s.m_pwszExposedName);
			CoTaskMemFree(s.m_pwszExposedPath);
		}
		delete[] m_props;
		InterlockedDecrement(&g_obj_count);
	}

	BOOL Init(ULONG n) {
		m_props = new(std::nothrow) VSS_OBJECT_PROP[n ? n : 1];
		if (!m_props)
			return (FALSE);
		ZeroMemory(m_props, (n ? n : 1) * sizeof (VSS_OBJECT_PROP));
		m_count = 0;
		return (TRUE);
	}

	void Add(const VSS_OBJECT_PROP &p) { m_props[m_count++] = p; }

	STDMETHOD_(ULONG, AddRef)() { return (InterlockedIncrement(&m_ref)); }
	STDMETHOD_(ULONG, Release)() {
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return (r);
	}
	STDMETHOD(QueryInterface)(REFIID riid, void **ppv) {
		if (riid == IID_IUnknown || riid == IID_IVssEnumObject) {
			*ppv = static_cast<IVssEnumObject *>(this);
			AddRef();
			return (S_OK);
		}
		*ppv = NULL;
		return (E_NOINTERFACE);
	}

	STDMETHOD(Next)(ULONG celt, VSS_OBJECT_PROP *rgelt,
	    ULONG *pceltFetched) {
		ULONG f = 0;
		while (f < celt && m_pos < m_count) {
			rgelt[f] = m_props[m_pos];
			/*
			 * VSS takes ownership of the CoTaskMem strings in the
			 * returned VSS_SNAPSHOT_PROP.  NULL them out in our
			 * copy so the destructor does not double-free them.
			 */
			if (m_props[m_pos].Type == VSS_OBJECT_SNAPSHOT) {
				VSS_SNAPSHOT_PROP &s =
				    m_props[m_pos].Obj.Snap;
				vss_log("Next[%lu]: device=%ls vol=%ls"
				    " machine=%ls svc=%ls"
				    " expname=%ls exppath=%ls"
				    " attrs=0x%lx status=%d count=%ld\n",
				    m_pos,
				    s.m_pwszSnapshotDeviceObject
				    ? s.m_pwszSnapshotDeviceObject : L"(null)",
				    s.m_pwszOriginalVolumeName
				    ? s.m_pwszOriginalVolumeName : L"(null)",
				    s.m_pwszOriginatingMachine
				    ? s.m_pwszOriginatingMachine : L"(null)",
				    s.m_pwszServiceMachine
				    ? s.m_pwszServiceMachine : L"(null)",
				    s.m_pwszExposedName
				    ? s.m_pwszExposedName : L"(null)",
				    s.m_pwszExposedPath
				    ? s.m_pwszExposedPath : L"(null)",
				    (ULONG)s.m_lSnapshotAttributes,
				    (int)s.m_eStatus,
				    s.m_lSnapshotsCount);
				s.m_pwszSnapshotDeviceObject = NULL;
				s.m_pwszOriginalVolumeName   = NULL;
				s.m_pwszOriginatingMachine   = NULL;
				s.m_pwszServiceMachine       = NULL;
				s.m_pwszExposedName          = NULL;
				s.m_pwszExposedPath          = NULL;
			}
			m_pos++;
			f++;
		}
		if (pceltFetched) *pceltFetched = f;
		vss_log("Next: celt=%lu fetched=%lu pos=%lu/%lu\n",
		    celt, f, m_pos, m_count);
		return (f == celt ? S_OK : S_FALSE);
	}
	STDMETHOD(Skip)(ULONG celt) {
		m_pos = m_pos + celt < m_count ? m_pos + celt : m_count;
		return (S_OK);
	}
	STDMETHOD(Reset)() { m_pos = 0; return (S_OK); }
	STDMETHOD(Clone)(IVssEnumObject **ppEnum) {
		vss_log("Clone: called, count=%lu pos=%lu\n", m_count, m_pos);
		if (!ppEnum)
			return (E_INVALIDARG);
		/*
		 * The VSS coordinator calls Clone() to snapshot the enumerator
		 * state before aggregating results from multiple providers.
		 * Create a new object with the same props and position.
		 */
		ZfsVssEnumObject *p = new(std::nothrow) ZfsVssEnumObject();
		if (!p)
			return (E_OUTOFMEMORY);
		if (!p->Init(m_count)) {
			p->Release();
			return (E_OUTOFMEMORY);
		}
		for (ULONG i = 0; i < m_count; i++) {
			VSS_OBJECT_PROP op = m_props[i];
			if (op.Type == VSS_OBJECT_SNAPSHOT) {
				VSS_SNAPSHOT_PROP &s = op.Obj.Snap;
				/* Deep-copy all CoTaskMem strings */
#define	DUPWSZ(f) \
	if (s.f) { \
		size_t _n = (wcslen(s.f) + 1) * sizeof(wchar_t); \
		s.f = (VSS_PWSZ)CoTaskMemAlloc(_n); \
		if (s.f) memcpy(s.f, m_props[i].Obj.Snap.f, _n); \
	}
				DUPWSZ(m_pwszSnapshotDeviceObject)
				DUPWSZ(m_pwszOriginalVolumeName)
				DUPWSZ(m_pwszOriginatingMachine)
				DUPWSZ(m_pwszServiceMachine)
				DUPWSZ(m_pwszExposedName)
				DUPWSZ(m_pwszExposedPath)
#undef DUPWSZ
			}
			p->m_props[i] = op;
			p->m_count++;
		}
		p->m_pos = m_pos;
		*ppEnum = p;
		return (S_OK);
	}

	/* Allow Clone() to access internals of another instance */
	friend class ZfsVssEnumObject;

private:
	LONG             m_ref;
	ULONG            m_pos;
	ULONG            m_count;
	VSS_OBJECT_PROP *m_props;
};

/* ------------------------------------------------------------------ */
/* Helper: fill VSS_SNAPSHOT_PROP from the registry                    */
/* ------------------------------------------------------------------ */

static HRESULT
load_snap_prop(const VSS_ID &sid, VSS_SNAPSHOT_PROP *p)
{
	wchar_t volume[MAX_PATH];
	char    zfsname[256];
	uint64_t zfsguid;
	GUID    setid;
	LONGLONG ts;
	LONG    attrs;

	if (vss_reg_load(&sid, volume, MAX_PATH, zfsname, 256,
	    &zfsguid, &setid, &ts, &attrs) != 0)
		return (VSS_E_OBJECT_NOT_FOUND);

	ZeroMemory(p, sizeof (*p));
	p->m_SnapshotId         = sid;
	p->m_SnapshotSetId      = setid;
	p->m_lSnapshotsCount    = 0;
	p->m_ProviderId         = CLSID_ZfsVssProvider;
	p->m_tsCreationTimestamp = ts;
	p->m_eStatus            = VSS_SS_CREATED;
	p->m_lSnapshotAttributes = attrs;

	/* Device path that exposes the frozen ZFS snapshot */
	wchar_t devname[128];
	_snwprintf(devname, 128,
	    L"\\\\?\\GLOBALROOT\\Device\\ZfsSnapshot%016I64x",
	    (unsigned long long)zfsguid);

	p->m_pwszSnapshotDeviceObject =
	    (VSS_PWSZ)CoTaskMemAlloc(
	    (wcslen(devname) + 1) * sizeof (wchar_t));
	if (!p->m_pwszSnapshotDeviceObject)
		return (E_OUTOFMEMORY);
	wcscpy(p->m_pwszSnapshotDeviceObject, devname);

	p->m_pwszOriginalVolumeName =
	    (VSS_PWSZ)CoTaskMemAlloc(
	    (wcslen(volume) + 1) * sizeof (wchar_t));
	if (!p->m_pwszOriginalVolumeName) {
		CoTaskMemFree(p->m_pwszSnapshotDeviceObject);
		return (E_OUTOFMEMORY);
	}
	wcscpy(p->m_pwszOriginalVolumeName, volume);

	wchar_t machine[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD mlen = MAX_COMPUTERNAME_LENGTH + 1;
	GetComputerNameW(machine, &mlen);

	p->m_pwszOriginatingMachine =
	    (VSS_PWSZ)CoTaskMemAlloc((mlen + 1) * sizeof (wchar_t));
	if (p->m_pwszOriginatingMachine)
		wcscpy(p->m_pwszOriginatingMachine, machine);

	p->m_pwszServiceMachine =
	    (VSS_PWSZ)CoTaskMemAlloc((mlen + 1) * sizeof (wchar_t));
	if (p->m_pwszServiceMachine)
		wcscpy(p->m_pwszServiceMachine, machine);

	/* VSS expects non-NULL pointers for all BSTR/LPWSTR fields */
	p->m_pwszExposedName =
	    (VSS_PWSZ)CoTaskMemAlloc(sizeof (wchar_t));
	if (p->m_pwszExposedName)
		p->m_pwszExposedName[0] = L'\0';

	p->m_pwszExposedPath =
	    (VSS_PWSZ)CoTaskMemAlloc(sizeof (wchar_t));
	if (p->m_pwszExposedPath)
		p->m_pwszExposedPath[0] = L'\0';

	return (S_OK);
}

/* ------------------------------------------------------------------ */
/* Helper: find ZFS dataset for a given volume name                    */
/* ------------------------------------------------------------------ */

/*
 * Replicate the kernel's zfs_vfs_uuid_gen / zfs_vfs_uuid_unparse to
 * compute the deterministic Windows volume GUID for a ZFS dataset name.
 * The GUID is a version-3 (MD5) UUID with namespace
 * {50670853-FBD2-4EC3-9802-73D847BF7E62}.
 *
 * Returns the GUID as "\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\".
 */
static void
ds_to_vol_guid(const char *dsname, wchar_t *out, int outchars)
{
	/* Same namespace as the kernel */
	static const unsigned char ns[16] = {
	    0x50, 0x67, 0x08, 0x53,
	    0xfb, 0xd2, 0x4e, 0xc3,
	    0x98, 0x02,
	    0x73, 0xd8, 0x47, 0xbf, 0x7e, 0x62
	};

	/* MD5 via Windows CryptAPI */
	HCRYPTPROV hprov = 0;
	HCRYPTHASH hhash = 0;
	unsigned char uuid[16] = {0};
	DWORD hashlen = 16;

	if (CryptAcquireContextW(&hprov, NULL, NULL, PROV_RSA_FULL,
	    CRYPT_VERIFYCONTEXT)) {
		if (CryptCreateHash(hprov, CALG_MD5, 0, 0, &hhash)) {
			CryptHashData(hhash, ns, 16, 0);
			CryptHashData(hhash, (const BYTE *)dsname,
			    (DWORD)strlen(dsname), 0);
			CryptGetHashParam(hhash, HP_HASHVAL, uuid,
			    &hashlen, 0);
			CryptDestroyHash(hhash);
		}
		CryptReleaseContext(hprov, 0);
	}

	/* Set UUID version 3 bits */
	uuid[6] = (uuid[6] & 0x0F) | 0x30;
	uuid[8] = (uuid[8] & 0x3F) | 0x80;

	/*
	 * Build the ZFS device volume name from the UUID.
	 * Then ask MountMgr for the canonical GUID it assigned when it
	 * mounted this device — that is what VSS passes us as volname.
	 */
	wchar_t zfs_vol[MAX_PATH];
	_snwprintf(zfs_vol, MAX_PATH,
	    L"\\\\?\\Volume{%02x%02x%02x%02x-%02x%02x-%02x%02x-"
	    L"%02x%02x-%02x%02x%02x%02x%02x%02x}\\",
	    uuid[0], uuid[1], uuid[2], uuid[3],
	    uuid[4], uuid[5], uuid[6], uuid[7],
	    uuid[8], uuid[9],
	    uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);

	/*
	 * GetVolumeNameForVolumeMountPointW translates our ZFS device
	 * volume name to the MountMgr-assigned canonical GUID, which is
	 * the GUID that VSS uses.
	 */
	if (!GetVolumeNameForVolumeMountPointW(zfs_vol, out, outchars))
		wcsncpy(out, zfs_vol, outchars);
}

struct FindCtx {
	const wchar_t *vol_guid;	/* canonical "\\?\Volume{...}\" */
	char found_ds[256];
};

static int
find_dataset_cb(zfs_handle_t *zhp, void *arg)
{
	struct FindCtx *ctx = (struct FindCtx *)arg;
	char mp[MAXPATHLEN];
	int rc;

	if (zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mp, sizeof (mp),
	    NULL, NULL, 0, B_FALSE) == 0) {
		/*
		 * Compute the deterministic Windows volume GUID for this
		 * dataset name (same MD5/UUID-v3 algorithm as the kernel's
		 * zfs_vfs_uuid_gen) and compare against the target GUID.
		 */
		wchar_t ds_guid[MAX_PATH];
		ds_to_vol_guid(zfs_get_name(zhp), ds_guid, MAX_PATH);

		vss_log("find_dataset_cb: ds=%s guid=%ls vs %ls\n",
		    zfs_get_name(zhp), ds_guid, ctx->vol_guid);

		if (_wcsicmp(ds_guid, ctx->vol_guid) == 0) {
			strncpy(ctx->found_ds, zfs_get_name(zhp), 256);
			zfs_close(zhp);
			return (1);
		}
	}

	rc = zfs_iter_filesystems(zhp, find_dataset_cb, ctx);
	zfs_close(zhp);
	return (rc);
}

/*
 * Resolve a VSS volume name ("\\?\Volume{guid}\") to the ZFS dataset
 * mounted there.  Returns 0 on success, -1 if not found or not ZFS.
 */
/*
 * Resolve a VSS volume name ("\\?\Volume{guid}\") to the ZFS dataset
 * mounted there by comparing the volume GUID against the deterministic
 * GUID derived from each dataset name.  Returns 0 on success.
 */
static int
volume_to_zfs_dataset(libzfs_handle_t *lzh, const wchar_t *volname,
    char *ds_out, int ds_len)
{
	vss_log("volume_to_zfs_dataset: looking for %ls\n", volname);

	struct FindCtx ctx;
	ctx.vol_guid = volname;	/* already in canonical \\?\Volume{...}\ form */
	ctx.found_ds[0] = '\0';

	zfs_iter_root(lzh, find_dataset_cb, &ctx);

	if (ctx.found_ds[0] == '\0') {
		vss_log("volume_to_zfs_dataset: no matching dataset\n");
		return (-1);
	}

	strncpy(ds_out, ctx.found_ds, ds_len);
	return (0);
}

/* ------------------------------------------------------------------ */
/* ZfsVssProvider                                                       */
/* ------------------------------------------------------------------ */

class ZfsVssProvider
    : public IVssSoftwareSnapshotProvider
    , public IVssProviderCreateSnapshotSet
    , public IVssProviderNotifications
{
public:
	ZfsVssProvider() : m_ref(1), m_lzh(NULL), m_ctx(VSS_CTX_BACKUP) {
		ZeroMemory(m_pending, sizeof (m_pending));
		InterlockedIncrement(&g_obj_count);
	}

	~ZfsVssProvider() {
		if (m_lzh)
			libzfs_fini(m_lzh);
		InterlockedDecrement(&g_obj_count);
	}

	/* ---- IUnknown ---- */
	STDMETHOD_(ULONG, AddRef)() {
		return (InterlockedIncrement(&m_ref));
	}
	STDMETHOD_(ULONG, Release)() {
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return (r);
	}
	STDMETHOD(QueryInterface)(REFIID riid, void **ppv) {
		if (riid == IID_IUnknown) {
			*ppv = static_cast<IVssSoftwareSnapshotProvider *>(this);
		} else if (riid == IID_IVssSoftwareSnapshotProvider) {
			*ppv = static_cast<IVssSoftwareSnapshotProvider *>(this);
		} else if (riid == IID_IVssProviderCreateSnapshotSet) {
			*ppv = static_cast<IVssProviderCreateSnapshotSet *>(this);
		} else if (riid == IID_IVssProviderNotifications) {
			*ppv = static_cast<IVssProviderNotifications *>(this);
		} else {
			*ppv = NULL;
			return (E_NOINTERFACE);
		}
		AddRef();
		return (S_OK);
	}

	/* ---- IVssSoftwareSnapshotProvider ---- */

	STDMETHOD(SetContext)(LONG lContext) {
		vss_log("SetContext: lContext=0x%lx\n", (ULONG)lContext);
		m_ctx = lContext;
		return (S_OK);
	}

	/*
	 * IsVolumeSupported - return TRUE if the volume has a ZFS dataset
	 * mounted on it, determined via libzfs iteration.  We do not rely
	 * on GetVolumeInformation filesystem type because the ZFS driver
	 * may report a compatibility alias instead of "ZFS".
	 */
	STDMETHOD(IsVolumeSupported)(VSS_PWSZ pwszVolumeName,
	    BOOL *pbSupportedByThisProvider)
	{
		vss_log("IsVolumeSupported: %ls\n", pwszVolumeName);
		if (!pbSupportedByThisProvider)
			return (E_INVALIDARG);
		*pbSupportedByThisProvider = FALSE;

		if (!m_lzh) {
			m_lzh = libzfs_init();
			if (!m_lzh) {
				vss_log("IsVolumeSupported: libzfs_init failed\n");
				return (S_OK);
			}
		}

		char ds[256];
		if (volume_to_zfs_dataset(m_lzh, pwszVolumeName,
		    ds, sizeof (ds)) == 0) {
			*pbSupportedByThisProvider = TRUE;
			vss_log("IsVolumeSupported: TRUE (ds=%s)\n", ds);
		} else {
			vss_log("IsVolumeSupported: FALSE (no ZFS dataset)\n");
		}

		return (S_OK);
	}

	/*
	 * IsVolumeSnapshotted - report whether any of our snapshots cover
	 * this volume.
	 */
	STDMETHOD(IsVolumeSnapshotted)(VSS_PWSZ pwszVolumeName,
	    BOOL *pbSnapshotsPresent, LONG *plSnapshotCompatibility)
	{
		vss_log("IsVolumeSnapshotted: %ls\n", pwszVolumeName);
		if (!pbSnapshotsPresent || !plSnapshotCompatibility)
			return (E_INVALIDARG);

		*pbSnapshotsPresent = FALSE;
		*plSnapshotCompatibility = 0;

		GUID ids[256];
		int n = vss_reg_enum(ids, 256);
		for (int i = 0; i < n; i++) {
			wchar_t vol[MAX_PATH];
			char    zn[256];
			uint64_t zg;
			GUID    setid;
			LONGLONG ts;
			LONG    attrs;

			if (vss_reg_load(&ids[i], vol, MAX_PATH,
			    zn, 256, &zg, &setid, &ts, &attrs) != 0)
				continue;
			if (_wcsicmp(vol, pwszVolumeName) == 0) {
				*pbSnapshotsPresent = TRUE;
				break;
			}
		}
		return (S_OK);
	}

	/*
	 * BeginPrepareSnapshot - record (SnapshotId, VolumeName) for later
	 * use by CommitSnapshots.
	 */
	STDMETHOD(BeginPrepareSnapshot)(VSS_ID SnapshotSetId,
	    VSS_ID SnapshotId, VSS_PWSZ pwszVolumeName, LONG lNewContext)
	{
		vss_log("BeginPrepareSnapshot: %ls ctx=%ld\n",
		    pwszVolumeName, lNewContext);
		(void)lNewContext;
		for (int i = 0; i < MAX_PENDING; i++) {
			if (m_pending[i].used)
				continue;
			m_pending[i].SnapshotId    = SnapshotId;
			m_pending[i].SnapshotSetId = SnapshotSetId;
			wcsncpy(m_pending[i].VolumeName, pwszVolumeName,
			    MAX_PATH);
			m_pending[i].used = TRUE;
			return (S_OK);
		}
		return (VSS_E_MAXIMUM_NUMBER_OF_SNAPSHOTS_REACHED);
	}

	/*
	 * GetSnapshotProperties - fill VSS_SNAPSHOT_PROP from registry.
	 * Strings must be allocated with CoTaskMemAlloc; VSS frees them.
	 */
	STDMETHOD(GetSnapshotProperties)(VSS_ID SnapshotId,
	    VSS_SNAPSHOT_PROP *pProp)
	{
		wchar_t guidstr[64];
		StringFromGUID2(SnapshotId, guidstr, 64);
		vss_log("GetSnapshotProperties: id=%ls\n", guidstr);
		if (!pProp)
			return (E_INVALIDARG);
		HRESULT hr = load_snap_prop(SnapshotId, pProp);
		vss_log("GetSnapshotProperties: hr=0x%lx\n", (ULONG)hr);
		return (hr);
	}

	/*
	 * Query - enumerate snapshot objects, optionally filtered by
	 * snapshot set ID.
	 */
	STDMETHOD(Query)(VSS_ID QueriedObjectId,
	    VSS_OBJECT_TYPE eQueriedObjectType,
	    VSS_OBJECT_TYPE eReturnedObjectsType,
	    IVssEnumObject **ppEnum)
	{
		vss_log("Query: eQueried=%d eReturned=%d\n",
		    (int)eQueriedObjectType, (int)eReturnedObjectsType);
		if (!ppEnum)
			return (E_INVALIDARG);
		*ppEnum = NULL;

		if (eReturnedObjectsType != VSS_OBJECT_SNAPSHOT) {
			vss_log("Query: unsupported returned type %d\n",
			    (int)eReturnedObjectsType);
			return (E_INVALIDARG);
		}

		if (!m_lzh) {
			m_lzh = libzfs_init();
			if (!m_lzh)
				vss_log("Query: libzfs_init failed"
				    " (snapshot validation disabled)\n");
		}

		GUID ids[256];
		int n = vss_reg_enum(ids, 256);
		vss_log("Query: vss_reg_enum returned %d entries\n", n);

		ZfsVssEnumObject *pEnum =
		    new(std::nothrow) ZfsVssEnumObject();
		if (!pEnum)
			return (E_OUTOFMEMORY);

		if (!pEnum->Init(n)) {
			pEnum->Release();
			return (E_OUTOFMEMORY);
		}

		int added = 0;
		for (int i = 0; i < n; i++) {
			wchar_t vol[MAX_PATH];
			char    zn[256];
			uint64_t zg;
			GUID    setid;
			LONGLONG ts;
			LONG    attrs;

			if (vss_reg_load(&ids[i], vol, MAX_PATH,
			    zn, 256, &zg, &setid, &ts, &attrs) != 0) {
				vss_log("Query: reg_load[%d] failed, skipping\n",
				    i);
				continue;
			}

			/* Filter by set ID if the query is for a set */
			if (eQueriedObjectType == VSS_OBJECT_SNAPSHOT_SET &&
			    memcmp(&setid, &QueriedObjectId,
			    sizeof (GUID)) != 0)
				continue;

			/* Validate the ZFS snapshot still exists */
			if (m_lzh) {
				zfs_handle_t *zhp = zfs_open(m_lzh, zn,
				    ZFS_TYPE_SNAPSHOT);
				if (!zhp) {
					vss_log("Query: snapshot %s gone,"
					    " cleaning registry\n", zn);
					vss_reg_delete(&ids[i]);
					continue;
				}
				zfs_close(zhp);
			}

			VSS_OBJECT_PROP op;
			op.Type = VSS_OBJECT_SNAPSHOT;
			HRESULT hr = load_snap_prop(ids[i], &op.Obj.Snap);
			vss_log("Query: load_snap_prop[%d] hr=0x%lx zfs=%s\n",
			    i, hr, zn);
			if (SUCCEEDED(hr)) {
				pEnum->Add(op);
				added++;
			}
		}

		vss_log("Query: returning S_OK with %d valid entries"
		    " (of %d in registry)\n", added, n);
		*ppEnum = pEnum;
		return (S_OK);
	}

	/*
	 * DeleteSnapshots - destroy ZFS snapshots and remove registry
	 * entries.
	 */
	STDMETHOD(DeleteSnapshots)(VSS_ID SourceObjectId,
	    VSS_OBJECT_TYPE eSourceObjectType, BOOL bForceDelete,
	    LONG *plDeletedSnapshots, VSS_ID *pNondeletedSnapshotID)
	{
		(void)bForceDelete;
		if (!plDeletedSnapshots || !pNondeletedSnapshotID)
			return (E_INVALIDARG);

		*plDeletedSnapshots = 0;
		ZeroMemory(pNondeletedSnapshotID, sizeof (VSS_ID));

		if (!m_lzh) {
			m_lzh = libzfs_init();
			if (!m_lzh)
				return (E_FAIL);
		}

		GUID ids[256];
		int n;

		if (eSourceObjectType == VSS_OBJECT_SNAPSHOT) {
			ids[0] = SourceObjectId;
			n = 1;
		} else if (eSourceObjectType == VSS_OBJECT_SNAPSHOT_SET) {
			GUID all[256];
			int total = vss_reg_enum(all, 256);
			n = 0;
			for (int i = 0; i < total && n < 256; i++) {
				wchar_t vol[MAX_PATH];
				char    zn[256];
				uint64_t zg;
				GUID    setid;
				LONGLONG ts;
				LONG    attrs;
				if (vss_reg_load(&all[i], vol, MAX_PATH,
				    zn, 256, &zg, &setid, &ts, &attrs) == 0 &&
				    memcmp(&setid, &SourceObjectId,
				    sizeof (GUID)) == 0)
					ids[n++] = all[i];
			}
		} else {
			return (E_INVALIDARG);
		}

		for (int i = 0; i < n; i++) {
			wchar_t vol[MAX_PATH];
			char    zname[256];
			uint64_t zg;
			GUID    setid;
			LONGLONG ts;
			LONG    attrs;

			if (vss_reg_load(&ids[i], vol, MAX_PATH,
			    zname, 256, &zg, &setid, &ts, &attrs) != 0) {
				*pNondeletedSnapshotID = ids[i];
				continue;
			}

			zfs_handle_t *zhp = zfs_open(m_lzh, zname,
			    ZFS_TYPE_SNAPSHOT);
			if (!zhp) {
				*pNondeletedSnapshotID = ids[i];
				continue;
			}
			int rc = zfs_destroy(zhp, B_FALSE);
			zfs_close(zhp);
			if (rc != 0) {
				*pNondeletedSnapshotID = ids[i];
				continue;
			}

			vss_reg_delete(&ids[i]);
			(*plDeletedSnapshots)++;
		}

		return (*plDeletedSnapshots == n ?
		    S_OK : VSS_E_OBJECT_NOT_FOUND);
	}

	/* Stubs for IVssSoftwareSnapshotProvider */
	STDMETHOD(SetSnapshotProperty)(VSS_ID, VSS_SNAPSHOT_PROPERTY_ID,
	    VARIANT) { return (E_NOTIMPL); }
	STDMETHOD(RevertToSnapshot)(VSS_ID) { return (E_NOTIMPL); }
	STDMETHOD(QueryRevertStatus)(VSS_PWSZ, IVssAsync **)
	    { return (E_NOTIMPL); }

	/* ---- IVssProviderCreateSnapshotSet ---- */

	STDMETHOD(EndPrepareSnapshots)(VSS_ID SnapshotSetId) {
		vss_log("EndPrepareSnapshots: called\n");
		(void)SnapshotSetId;
		return (S_OK);
	}

	/* PreCommitSnapshots - no IO freeze needed for ZFS */
	STDMETHOD(PreCommitSnapshots)(VSS_ID SnapshotSetId) {
		vss_log("PreCommitSnapshots: called\n");
		(void)SnapshotSetId;
		return (S_OK);
	}

	/*
	 * CommitSnapshots - create the actual ZFS snapshot for each pending
	 * entry that belongs to this snapshot set.
	 */
	STDMETHOD(CommitSnapshots)(VSS_ID SnapshotSetId)
	{
		vss_log("CommitSnapshots called\n");
		if (!m_lzh) {
			m_lzh = libzfs_init();
			if (!m_lzh)
				return (E_FAIL);
		}

		SYSTEMTIME st;
		GetSystemTime(&st);
		char ts_str[32];
		_snprintf(ts_str, sizeof (ts_str),
		    "%04d%02d%02dT%02d%02d%02d",
		    st.wYear, st.wMonth, st.wDay,
		    st.wHour, st.wMinute, st.wSecond);

		FILETIME ft;
		SystemTimeToFileTime(&st, &ft);
		LONGLONG timestamp =
		    ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

		for (int i = 0; i < MAX_PENDING; i++) {
			if (!m_pending[i].used)
				continue;
			if (memcmp(&m_pending[i].SnapshotSetId,
			    &SnapshotSetId, sizeof (VSS_ID)) != 0)
				continue;

			char ds[256];
			if (volume_to_zfs_dataset(m_lzh,
			    m_pending[i].VolumeName, ds, sizeof (ds)) != 0)
				return (VSS_E_VOLUME_NOT_SUPPORTED);

			char snapname[512];
			_snprintf(snapname, sizeof (snapname),
			    "%s@vss-%s", ds, ts_str);

			vss_log("CommitSnapshots: creating snapshot %s\n",
			    snapname);
			int rc = zfs_snapshot(m_lzh, snapname, B_FALSE, NULL);
			vss_log("CommitSnapshots: zfs_snapshot returned %d"
			    " (err=%s)\n", rc,
			    rc ? libzfs_error_description(m_lzh) : "ok");
			if (rc != 0)
				return (E_FAIL);

			zfs_handle_t *zhp = zfs_open(m_lzh, snapname,
			    ZFS_TYPE_SNAPSHOT);
			vss_log("CommitSnapshots: zfs_open %s\n",
			    zhp ? "ok" : "FAILED");
			if (!zhp)
				return (E_FAIL);

			char guidbuf[32] = { 0 };
			uint64_t zfsguid = 0;
			if (zfs_prop_get(zhp, ZFS_PROP_GUID, guidbuf,
			    sizeof (guidbuf), NULL, NULL, 0, B_FALSE) == 0)
				zfsguid = strtoull(guidbuf, NULL, 10);
			zfs_close(zhp);

			LONG attrs =
			    VSS_VOLSNAP_ATTR_NO_AUTO_RELEASE |
			    VSS_VOLSNAP_ATTR_PERSISTENT;

			vss_reg_store(&m_pending[i].SnapshotId,
			    &SnapshotSetId,
			    m_pending[i].VolumeName,
			    snapname, zfsguid, timestamp, attrs);

			vss_log("CommitSnapshots: snapshot %s stored\n",
			    snapname);
			m_pending[i].used = FALSE;
		}
		vss_log("CommitSnapshots: returning S_OK\n");
		return (S_OK);
	}

	STDMETHOD(PostCommitSnapshots)(VSS_ID SnapshotSetId,
	    LONG lSnapshotsCount) {
		vss_log("PostCommitSnapshots: count=%ld\n", lSnapshotsCount);
		(void)SnapshotSetId;
		(void)lSnapshotsCount;
		return (S_OK);
	}

	STDMETHOD(PreFinalCommitSnapshots)(VSS_ID SnapshotSetId) {
		vss_log("PreFinalCommitSnapshots: called\n");
		(void)SnapshotSetId;
		return (S_OK);
	}

	STDMETHOD(PostFinalCommitSnapshots)(VSS_ID SnapshotSetId) {
		vss_log("PostFinalCommitSnapshots: called\n");
		(void)SnapshotSetId;
		return (S_OK);
	}

	STDMETHOD(AbortSnapshots)(VSS_ID SnapshotSetId) {
		/* Clear all pending entries for this set */
		for (int i = 0; i < MAX_PENDING; i++) {
			if (m_pending[i].used &&
			    memcmp(&m_pending[i].SnapshotSetId,
			    &SnapshotSetId, sizeof (VSS_ID)) == 0)
				m_pending[i].used = FALSE;
		}
		return (S_OK);
	}

	/* ---- IVssProviderNotifications ---- */

	STDMETHOD(OnLoad)(IUnknown *pCallback) {
		vss_log("OnLoad: called\n");
		(void)pCallback;
		return (S_OK);
	}
	STDMETHOD(OnUnload)(BOOL bForceUnload) {
		vss_log("OnUnload: force=%d\n", (int)bForceUnload);
		return (S_OK);
	}

private:
	LONG             m_ref;
	libzfs_handle_t *m_lzh;
	LONG             m_ctx;
	PendingSnap      m_pending[MAX_PENDING];
};

/* ------------------------------------------------------------------ */
/* Class factory                                                        */
/* ------------------------------------------------------------------ */

class ZfsVssClassFactory : public IClassFactory {
public:
	ZfsVssClassFactory() : m_ref(1) {
		InterlockedIncrement(&g_obj_count);
	}
	~ZfsVssClassFactory() { InterlockedDecrement(&g_obj_count); }

	STDMETHOD_(ULONG, AddRef)() { return (InterlockedIncrement(&m_ref)); }
	STDMETHOD_(ULONG, Release)() {
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return (r);
	}
	STDMETHOD(QueryInterface)(REFIID riid, void **ppv) {
		if (riid == IID_IUnknown || riid == IID_IClassFactory) {
			*ppv = static_cast<IClassFactory *>(this);
			AddRef();
			return (S_OK);
		}
		*ppv = NULL;
		return (E_NOINTERFACE);
	}

	STDMETHOD(CreateInstance)(IUnknown *pUnkOuter, REFIID riid, void **ppv)
	{
		vss_log("CreateInstance called\n");
		if (pUnkOuter)
			return (CLASS_E_NOAGGREGATION);
		ZfsVssProvider *p = new(std::nothrow) ZfsVssProvider();
		if (!p)
			return (E_OUTOFMEMORY);
		HRESULT hr = p->QueryInterface(riid, ppv);
		p->Release();
		return (hr);
	}

	STDMETHOD(LockServer)(BOOL fLock) {
		if (fLock)
			InterlockedIncrement(&g_server_locks);
		else
			InterlockedDecrement(&g_server_locks);
		return (S_OK);
	}

private:
	LONG m_ref;
};

/* ------------------------------------------------------------------ */
/* Windows Service + COM LocalServer32                                  */
/*                                                                      */
/* VSS activates software providers (Type=2) via CLSCTX_LOCAL_SERVER   */
/* with AppID LocalService.  COM starts our service, we register the    */
/* class factory, VSS connects via RPC, we run until VSS releases us.   */
/* ------------------------------------------------------------------ */

#define	ZFS_VSS_SERVICE_NAME	L"ZfsVssProvider"

static SERVICE_STATUS_HANDLE	g_ssh = NULL;
static HANDLE			g_stop_event = NULL;
static DWORD			g_reg_token = 0;

void
vss_provider_signal_stop(void)
{
	if (g_stop_event)
		SetEvent(g_stop_event);
}

static void
set_service_status(DWORD state, DWORD exit_code)
{
	SERVICE_STATUS ss = {0};
	ss.dwServiceType		= SERVICE_WIN32_OWN_PROCESS;
	ss.dwCurrentState		= state;
	ss.dwControlsAccepted		= (state == SERVICE_RUNNING)
	    ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
	ss.dwWin32ExitCode		= exit_code;
	ss.dwWaitHint			= 5000;
	if (g_ssh)
		SetServiceStatus(g_ssh, &ss);
}

static VOID WINAPI
ServiceCtrlHandler(DWORD ctrl)
{
	if (ctrl == SERVICE_CONTROL_STOP ||
	    ctrl == SERVICE_CONTROL_SHUTDOWN) {
		vss_log("ServiceCtrlHandler: stop/shutdown requested\n");
		set_service_status(SERVICE_STOP_PENDING, NO_ERROR);
		SetEvent(g_stop_event);
	}
}

static VOID WINAPI
ServiceMain(DWORD argc, LPWSTR *argv)
{
	(void)argc; (void)argv;

	g_ssh = RegisterServiceCtrlHandlerW(ZFS_VSS_SERVICE_NAME,
	    ServiceCtrlHandler);
	if (!g_ssh) {
		vss_log("RegisterServiceCtrlHandlerW failed: %lu\n",
		    GetLastError());
		return;
	}

	set_service_status(SERVICE_START_PENDING, NO_ERROR);
	vss_log("ServiceMain: started pid=%lu\n", GetCurrentProcessId());

	g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_stop_event) {
		vss_log("CreateEvent failed: %lu\n", GetLastError());
		set_service_status(SERVICE_STOPPED, GetLastError());
		return;
	}

	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr)) {
		vss_log("CoInitializeEx failed: hr=0x%lx\n", hr);
		set_service_status(SERVICE_STOPPED, ERROR_GEN_FAILURE);
		CloseHandle(g_stop_event);
		return;
	}

	ZfsVssClassFactory *pFactory = new(std::nothrow) ZfsVssClassFactory();
	if (!pFactory) {
		vss_log("Out of memory allocating class factory\n");
		CoUninitialize();
		set_service_status(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY);
		CloseHandle(g_stop_event);
		return;
	}

	hr = CoRegisterClassObject(CLSID_ZfsVssProvider, pFactory,
	    CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &g_reg_token);
	pFactory->Release();

	if (FAILED(hr)) {
		vss_log("CoRegisterClassObject failed: hr=0x%lx\n", hr);
		CoUninitialize();
		set_service_status(SERVICE_STOPPED, ERROR_GEN_FAILURE);
		CloseHandle(g_stop_event);
		return;
	}

	vss_log("Class factory registered, service running\n");
	set_service_status(SERVICE_RUNNING, NO_ERROR);

	/* Wait for stop signal from SCM or from last object release */
	WaitForSingleObject(g_stop_event, INFINITE);

	vss_log("Shutting down\n");
	CoRevokeClassObject(g_reg_token);
	CoUninitialize();
	CloseHandle(g_stop_event);
	g_stop_event = NULL;
	set_service_status(SERVICE_STOPPED, NO_ERROR);
}

int
wmain(int argc, wchar_t *argv[])
{
	(void)argc; (void)argv;

	static SERVICE_TABLE_ENTRYW table[] = {
		{ (LPWSTR)ZFS_VSS_SERVICE_NAME, ServiceMain },
		{ NULL, NULL }
	};

	/*
	 * StartServiceCtrlDispatcher blocks until the service stops.
	 * If it fails with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT we
	 * are running interactively — run directly for debugging.
	 */
	if (!StartServiceCtrlDispatcherW(table)) {
		DWORD err = GetLastError();
		if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
			vss_log("Running interactively (not as a service)\n");
			g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
			HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
			if (SUCCEEDED(hr)) {
				ZfsVssClassFactory *pFactory =
				    new(std::nothrow) ZfsVssClassFactory();
				if (pFactory) {
					CoRegisterClassObject(
					    CLSID_ZfsVssProvider, pFactory,
					    CLSCTX_LOCAL_SERVER,
					    REGCLS_MULTIPLEUSE, &g_reg_token);
					pFactory->Release();
					vss_log("Interactive: factory registered,"
					    " press Ctrl-C to stop\n");
					WaitForSingleObject(g_stop_event,
					    INFINITE);
					CoRevokeClassObject(g_reg_token);
				}
				CoUninitialize();
			}
			CloseHandle(g_stop_event);
		} else {
			vss_log("StartServiceCtrlDispatcherW failed: %lu\n",
			    err);
		}
	}
	return (0);
}

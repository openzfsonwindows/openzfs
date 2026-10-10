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
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>.
 */

/*
 * OP_FILE_VERSIONS: given a path, report every snapshot that holds a version
 * of that file or directory, with size/mtime, without mounting the snapshots.
 *
 * The Windows FileId of a ZFS object has the ZFS object number in its low 64
 * bits, and ZFS_IOC_OBJ_TO_STATS can be asked about that object number in any
 * snapshot of the dataset. A different "gen" means the object number was
 * reused by another file, so that snapshot has no version of this file.
 */

#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libzfs.h>
#include <sys/nvpair.h>

#include "ops_common.h"
#include "ops_versions.h"
#include "memfile.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

extern libzfs_handle_t *g_lzh;

#define	ZED_VERSIONS_MAX	4096

typedef struct {
	wchar_t vol_guid[MAX_PATH];
	char ds[ZFS_MAX_DATASET_NAME_LEN];
	char mount[MAX_PATH * 3]; // UTF-8, '\' separators, no trailing '\'
	char mount_raw[MAX_PATH * 3]; // as zfs_is_mounted() returned it
	BOOL found;
} find_ds_ctx_t;

/*
 * Find the mounted filesystem whose volume GUID matches ctx->vol_guid.
 * Same approach as the VSS provider: zfs_is_mounted() hands back the
 * Windows mount path, which GetVolumeNameForVolumeMountPoint() resolves.
 */
static int
find_ds_cb(zfs_handle_t *zhp, void *data)
{
	find_ds_ctx_t *ctx = data;
	int rc = 0;

	if (zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM) {
		char *actual = NULL;

		if (zfs_is_mounted(zhp, &actual) && actual != NULL) {
			wchar_t mp[MAX_PATH];
			wchar_t guid[MAX_PATH];

			if (MultiByteToWideChar(CP_UTF8, 0, actual, -1, mp,
			    MAX_PATH) > 0) {
				size_t n;
				for (wchar_t *p = mp; *p; p++)
					if (*p == L'/')
						*p = L'\\';
				n = wcslen(mp);
				if (n > 0 && mp[n - 1] != L'\\' &&
				    n + 2 < MAX_PATH) {
					mp[n] = L'\\';
					mp[n + 1] = L'\0';
				}
				if (GetVolumeNameForVolumeMountPointW(mp,
				    guid, MAX_PATH) &&
				    _wcsicmp(guid, ctx->vol_guid) == 0) {
					char *q;
					_snprintf_s(ctx->ds, sizeof (ctx->ds),
					    _TRUNCATE, "%s", zfs_get_name(zhp));
					_snprintf_s(ctx->mount_raw,
					    sizeof (ctx->mount_raw),
					    _TRUNCATE, "%s", actual);
					_snprintf_s(ctx->mount,
					    sizeof (ctx->mount), _TRUNCATE,
					    "%s", actual);
					for (q = ctx->mount; *q; q++)
						if (*q == '/')
							*q = '\\';
					n = strlen(ctx->mount);
					while (n > 0 &&
					    ctx->mount[n - 1] == '\\')
						ctx->mount[--n] = '\0';
					ctx->found = TRUE;
					rc = 1;
				}
			}
			free(actual);
		}
		if (rc == 0)
			rc = zfs_iter_filesystems(zhp, find_ds_cb, ctx);
	}
	zfs_close(zhp);
	return (rc);
}

typedef struct {
	char name[ZFS_MAX_DATASET_NAME_LEN]; // short snapshot name
	uint64_t creation;
	zfs_obj_version_t v;
	char path[MAX_PATH * 3]; // path within the snapshot, '\' separators
} ver_ent_t;

typedef struct {
	const char *ds;
	uint64_t obj;
	uint64_t gen; // live gen: other values mean object number reuse
	ver_ent_t *ents;
	int nents;
} snap_ctx_t;

static int
snap_cb(zfs_handle_t *zhp, void *data)
{
	snap_ctx_t *ctx = data;
	const char *full = zfs_get_name(zhp);
	const char *at = strchr(full, '@');
	zfs_obj_version_t v;
	char path[MAX_PATH * 3];

	if (at != NULL && ctx->nents < ZED_VERSIONS_MAX &&
	    zfs_get_obj_version(g_lzh, full, ctx->obj, &v, path,
	    sizeof (path)) == 0 && v.zov_gen == ctx->gen) {
		ver_ent_t *e = &ctx->ents[ctx->nents++];
		char *q;

		_snprintf_s(e->name, sizeof (e->name), _TRUNCATE, "%s", at + 1);
		e->creation = zfs_prop_get_int(zhp, ZFS_PROP_CREATION);
		e->v = v;
		_snprintf_s(e->path, sizeof (e->path), _TRUNCATE, "%s", path);
		for (q = e->path; *q; q++)
			if (*q == '/')
				*q = '\\';
	}

	zfs_close(zhp);
	return (0);
}

static void
add_u64_str(nvlist_t *nv, const char *key, uint64_t val)
{
	char buf[32];

	_snprintf_s(buf, sizeof (buf), _TRUNCATE, "%llu",
	    (unsigned long long)val);
	fnvlist_add_string(nv, key, buf);
}

static char *
finish_json(nvlist_t *res, size_t *out_len)
{
	memfile_t mf;
	FILE *fp = memfile_open(&mf);

	if (!fp) {
		fnvlist_free(res);
		return (NULL);
	}
	nvlist_print_json(fp, res);
	fclose(fp);
	fnvlist_free(res);
	return (memfile_take(&mf, out_len));
}

static char *
fail_json(nvlist_t *res, const char *msg, size_t *out_len)
{
	fnvlist_add_string(res, "err", msg);
	return (finish_json(res, out_len));
}

typedef struct {
	find_ds_ctx_t f;
	wchar_t full[MAX_PATH * 2]; // DOS path of the item
	wchar_t rel[MAX_PATH * 2];  // below the volume root, no edge '\'
	uint64_t obj;
	BOOL is_dir;
} item_t;

/*
 * The path is opened while impersonating the pipe client, so a user can only
 * enumerate versions of objects they themselves are allowed to open. The
 * object number found that way is then used with the service's own token.
 * On failure, *msg is set and FALSE returned.
 */
static BOOL
resolve_item(void *client, const char *path_utf8, item_t *it, char *msg,
    size_t msgsz)
{
	wchar_t wpath[MAX_PATH * 2];
	wchar_t full[MAX_PATH * 2];
	wchar_t root[MAX_PATH];
	HANDLE h;
	FILE_ID_INFO fid;
	BY_HANDLE_FILE_INFORMATION bhfi;
	BOOL got_id = FALSE, got_info = FALSE;
	DWORD gle = 0;
	const wchar_t *p;
	size_t n;

	memset(it, 0, sizeof (*it));

	if (path_utf8 == NULL || path_utf8[0] == '\0' ||
	    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath,
	    MAX_PATH * 2) <= 0) {
		_snprintf_s(msg, msgsz, _TRUNCATE, "invalid path");
		return (FALSE);
	}

	if (!ImpersonateNamedPipeClient((HANDLE)client)) {
		_snprintf_s(msg, msgsz, _TRUNCATE,
		    "cannot impersonate caller");
		return (FALSE);
	}

	h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		gle = GetLastError();
	} else {
		got_id = GetFileInformationByHandleEx(h, FileIdInfo, &fid,
		    sizeof (fid));
		if (!got_id)
			gle = GetLastError();
		got_info = GetFileInformationByHandle(h, &bhfi);
		if (GetFinalPathNameByHandleW(h, full, MAX_PATH * 2,
		    VOLUME_NAME_DOS) == 0)
			full[0] = L'\0';
		CloseHandle(h);
	}
	RevertToSelf();

	if (h == INVALID_HANDLE_VALUE || !got_id) {
		_snprintf_s(msg, msgsz, _TRUNCATE,
		    "cannot open path (error %lu)", (unsigned long)gle);
		return (FALSE);
	}

	// Low 64 bits of the 128-bit FileId are the ZFS object number.
	memcpy(&it->obj, fid.FileId.Identifier, sizeof (it->obj));
	it->is_dir = got_info &&
	    (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

	if (full[0] == L'\0' || !GetVolumePathNameW(full, root, MAX_PATH) ||
	    !GetVolumeNameForVolumeMountPointW(root, it->f.vol_guid,
	    MAX_PATH)) {
		_snprintf_s(msg, msgsz, _TRUNCATE, "cannot determine volume");
		return (FALSE);
	}

	(void) zfs_iter_root(g_lzh, find_ds_cb, &it->f);
	if (!it->f.found) {
		_snprintf_s(msg, msgsz, _TRUNCATE, "not on a ZFS filesystem");
		return (FALSE);
	}

	// Strip the \?\ prefix, then the volume root, to get the relpath.
	p = full;
	if (wcsncmp(p, L"\\?\\", 4) == 0)
		p += 4;
	lstrcpynW(it->full, p, ARRAYSIZE(it->full));
	n = wcslen(root);
	if (wcsncmp(root, L"\\?\\", 4) == 0)
		n -= 4;
	p = it->full;
	p += (wcslen(p) >= n) ? n : wcslen(p);
	lstrcpynW(it->rel, p, ARRAYSIZE(it->rel));
	n = wcslen(it->rel);
	while (n > 0 && it->rel[n - 1] == L'\\')
		it->rel[--n] = L'\0';
	return (TRUE);
}

char *
zed_file_versions_json(void *client, const char *path_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	item_t it;
	find_ds_ctx_t fctx;
	snap_ctx_t sctx;
	zfs_obj_version_t live;
	char livepath[MAX_PATH * 3];
	char msg[96];
	zfs_handle_t *zhp;
	uint64_t obj;
	nvlist_t **arr = NULL;
	BOOL got_info;
	int i, nout = 0, err;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	if (!resolve_item(client, path_utf8, &it, msg, sizeof (msg)))
		return (fail_json(res, msg, out_len));
	fctx = it.f;
	obj = it.obj;
	got_info = it.is_dir;

	err = zfs_get_obj_version(g_lzh, fctx.ds, obj, &live, livepath,
	    sizeof (livepath));
	if (err != 0)
		return (fail_json(res, "cannot read object stats", out_len));

	fnvlist_add_string(res, "dataset", fctx.ds);
	fnvlist_add_string(res, "mount", fctx.mount);
	add_u64_str(res, "obj", obj);
	fnvlist_add_boolean_value(res, "is_dir",
	    got_info ? B_TRUE : B_FALSE);
	add_u64_str(res, "size", live.zov_size);
	add_u64_str(res, "mtime", live.zov_mtime[0]);
	add_u64_str(res, "mtime_ns", live.zov_mtime[1]);

	memset(&sctx, 0, sizeof (sctx));
	sctx.ds = fctx.ds;
	sctx.obj = obj;
	sctx.gen = live.zov_gen;
	sctx.ents = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
	    ZED_VERSIONS_MAX * sizeof (ver_ent_t));
	if (sctx.ents == NULL)
		return (fail_json(res, "out of memory", out_len));

	zhp = zfs_open(g_lzh, fctx.ds, ZFS_TYPE_FILESYSTEM);
	if (zhp != NULL) {
		// Oldest first, so "changed" can compare to the prior entry.
		(void) zfs_iter_snapshots_sorted(zhp, snap_cb, &sctx, 0, 0);
		zfs_close(zhp);
	}

	if (sctx.nents > 0) {
		arr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		    sctx.nents * sizeof (nvlist_t *));
	}
	for (i = 0; arr != NULL && i < sctx.nents; i++) {
		const ver_ent_t *e = &sctx.ents[i];
		const zfs_obj_version_t *p =
		    (i > 0) ? &sctx.ents[i - 1].v : NULL;
		nvlist_t *ent = fnvlist_alloc();
		char sp[MAX_PATH * 4];

		fnvlist_add_string(ent, "snap", e->name);
		add_u64_str(ent, "creation", e->creation);
		add_u64_str(ent, "size", e->v.zov_size);
		add_u64_str(ent, "mtime", e->v.zov_mtime[0]);
		add_u64_str(ent, "mtime_ns", e->v.zov_mtime[1]);
		// Path of this version inside the mounted snapshot (the
		// snapshot is auto-mounted on first real access).
		_snprintf_s(sp, sizeof (sp), _TRUNCATE,
		    "%s\\.zfs\\snapshot\\%s%s%s", fctx.mount, e->name,
		    (e->path[0] == '\\' || e->path[0] == '\0') ? "" : "\\",
		    e->path);
		fnvlist_add_string(ent, "path", sp);
		fnvlist_add_boolean_value(ent, "same_as_live",
		    (e->v.zov_size == live.zov_size &&
		    e->v.zov_mtime[0] == live.zov_mtime[0] &&
		    e->v.zov_mtime[1] == live.zov_mtime[1] &&
		    e->v.zov_ctime[0] == live.zov_ctime[0] &&
		    e->v.zov_ctime[1] == live.zov_ctime[1]) ?
		    B_TRUE : B_FALSE);
		fnvlist_add_boolean_value(ent, "changed",
		    (p == NULL ||
		    e->v.zov_size != p->zov_size ||
		    e->v.zov_mtime[0] != p->zov_mtime[0] ||
		    e->v.zov_mtime[1] != p->zov_mtime[1] ||
		    e->v.zov_ctime[0] != p->zov_ctime[0] ||
		    e->v.zov_ctime[1] != p->zov_ctime[1]) ? B_TRUE : B_FALSE);
		arr[nout++] = ent;
	}

	// Add the array exactly once, see zed_list_disks_json().
	fnvlist_add_nvlist_array(res, "versions", (const nvlist_t **)arr,
	    nout);
	for (i = 0; i < nout; i++)
		fnvlist_free(arr[i]);
	if (arr != NULL)
		HeapFree(GetProcessHeap(), 0, arr);
	HeapFree(GetProcessHeap(), 0, sctx.ents);

	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/* -------------------- deleted items -------------------- */

/*
 * OP_DELETED_ITEMS: names that no longer exist in a directory, but do in
 * some snapshot of it. There is no way to list a directory of a snapshot
 * by object number, so the snapshots are read through their
 * <mount>\.zfs\snapshot\<snap> path (which auto-mounts them) as the
 * calling user. Only the newest ZED_DELETED_MAX_SNAPS snapshots are read.
 */
#define	ZED_DELETED_MAX_SNAPS	512
#define	ZED_DELETED_MAX_ITEMS	20000

typedef struct {
	wchar_t name[MAX_PATH];
	BOOL is_dir;
	uint64_t size;
	uint64_t mtime;
} dent_t;

typedef struct {
	int newest;	// snapshot index (newest snapshot with this version)
	int oldest;	// oldest consecutive snapshot with the same version
	uint64_t size;
	uint64_t mtime;
	char *path;	// path within that snapshot, or NULL
} dver_t;

typedef struct {
	wchar_t name[MAX_PATH];
	BOOL is_dir;
	dver_t *vers;
	int nvers;
	int capvers;
} ditem_t;

typedef struct {
	char name[ZFS_MAX_DATASET_NAME_LEN];
	uint64_t creation;
} snapname_t;

typedef struct {
	snapname_t *snaps;
	int nsnaps;
	int capsnaps;
} snaplist_t;

static int
snaplist_cb(zfs_handle_t *zhp, void *data)
{
	snaplist_t *l = data;
	const char *full = zfs_get_name(zhp);
	const char *at = strchr(full, '@');

	if (at != NULL) {
		if (l->nsnaps == l->capsnaps) {
			int ncap = l->capsnaps ? l->capsnaps * 2 : 64;
			snapname_t *n = realloc(l->snaps,
			    ncap * sizeof (snapname_t));
			if (n == NULL) {
				zfs_close(zhp);
				return (0);
			}
			l->snaps = n;
			l->capsnaps = ncap;
		}
		_snprintf_s(l->snaps[l->nsnaps].name,
		    sizeof (l->snaps[0].name), _TRUNCATE, "%s", at + 1);
		l->snaps[l->nsnaps].creation =
		    zfs_prop_get_int(zhp, ZFS_PROP_CREATION);
		l->nsnaps++;
	}
	zfs_close(zhp);
	return (0);
}

static int
dent_cmp(const void *a, const void *b)
{
	return (_wcsicmp(((const dent_t *)a)->name,
	    ((const dent_t *)b)->name));
}

static uint64_t
ft_to_unix(const FILETIME *ft)
{
	ULARGE_INTEGER u;

	u.LowPart = ft->dwLowDateTime;
	u.HighPart = ft->dwHighDateTime;
	return (u.QuadPart / 10000000ULL - 11644473600ULL);
}

// Read one directory into *out (caller frees). Returns count, -1 on error.
static int
read_dir(const wchar_t *dir, dent_t **out)
{
	wchar_t pat[MAX_PATH * 3];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	dent_t *a = NULL;
	int n = 0, cap = 0;

	*out = NULL;
	_snwprintf_s(pat, ARRAYSIZE(pat), _TRUNCATE, L"%s\\*", dir);
	h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch,
	    NULL, FIND_FIRST_EX_LARGE_FETCH);
	if (h == INVALID_HANDLE_VALUE)
		return (-1);
	do {
		dent_t *e;

		if (wcscmp(fd.cFileName, L".") == 0 ||
		    wcscmp(fd.cFileName, L"..") == 0)
			continue;
		if (n == cap) {
			int ncap = cap ? cap * 2 : 128;
			dent_t *t = realloc(a, ncap * sizeof (dent_t));
			if (t == NULL)
				break;
			a = t;
			cap = ncap;
		}
		e = &a[n++];
		lstrcpynW(e->name, fd.cFileName, ARRAYSIZE(e->name));
		e->is_dir = (fd.dwFileAttributes &
		    FILE_ATTRIBUTE_DIRECTORY) != 0;
		e->size = ((uint64_t)fd.nFileSizeHigh << 32) |
		    fd.nFileSizeLow;
		e->mtime = ft_to_unix(&fd.ftLastWriteTime);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	*out = a;
	return (n);
}

static void
to_utf8(const wchar_t *w, char *out, size_t outsz)
{
	if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outsz, NULL,
	    NULL) <= 0)
		out[0] = '\0';
}

/*
 * Record one sighting of a name in one snapshot. Items are kept sorted by
 * name; versions are collapsed while consecutive snapshots are identical.
 * "path" (optional) is the item's path within that snapshot.
 */
static void
note_entry(ditem_t **items, int *nitems, int *capitems, const dent_t *e,
    int snap, const char *path)
{
	ditem_t *di = NULL;
	int lo = 0, hi = *nitems - 1, mid, c;

	while (lo <= hi) {
		mid = (lo + hi) / 2;
		c = _wcsicmp(e->name, (*items)[mid].name);
		if (c == 0) {
			di = &(*items)[mid];
			break;
		}
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}

	if (di == NULL) {
		if (*nitems >= ZED_DELETED_MAX_ITEMS)
			return;
		if (*nitems == *capitems) {
			int ncap = *capitems ? *capitems * 2 : 64;
			ditem_t *t = realloc(*items, ncap * sizeof (ditem_t));
			if (t == NULL)
				return;
			*items = t;
			*capitems = ncap;
		}
		memmove(&(*items)[lo + 1], &(*items)[lo],
		    (*nitems - lo) * sizeof (ditem_t));
		di = &(*items)[lo];
		memset(di, 0, sizeof (*di));
		lstrcpynW(di->name, e->name, ARRAYSIZE(di->name));
		di->is_dir = e->is_dir;
		(*nitems)++;
	}

	if (di->nvers > 0 && di->vers[di->nvers - 1].size == e->size &&
	    di->vers[di->nvers - 1].mtime == e->mtime) {
		di->vers[di->nvers - 1].oldest = snap;
		return;
	}
	if (di->nvers == di->capvers) {
		int ncap = di->capvers ? di->capvers * 2 : 4;
		dver_t *t = realloc(di->vers, ncap * sizeof (dver_t));
		if (t == NULL)
			return;
		di->vers = t;
		di->capvers = ncap;
	}
	di->vers[di->nvers].newest = snap;
	di->vers[di->nvers].oldest = snap;
	di->vers[di->nvers].size = e->size;
	di->vers[di->nvers].mtime = e->mtime;
	di->vers[di->nvers].path = (path != NULL) ? _strdup(path) : NULL;
	di->nvers++;
}

static void
free_items(ditem_t *items, int nitems)
{
	int i, k;

	for (i = 0; i < nitems; i++) {
		for (k = 0; k < items[i].nvers; k++)
			free(items[i].vers[k].path);
		free(items[i].vers);
	}
	free(items);
}

// Build the JSON reply from the collected items, and free them.
static char *
build_reply(nvlist_t *res, const item_t *it, const snaplist_t *sl, int first,
    BOOL truncated, ditem_t *items, int nitems, const char *rel8,
    size_t *out_len)
{
	nvlist_t **arr;
	int i, nout = 0;

	arr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
	    (nitems > 0 ? nitems : 1) * sizeof (nvlist_t *));
	for (i = 0; arr != NULL && i < nitems; i++) {
		const ditem_t *di = &items[i];
		nvlist_t **varr;
		nvlist_t *ent = fnvlist_alloc();
		char nm[MAX_PATH * 3];
		int k;

		to_utf8(di->name, nm, sizeof (nm));
		fnvlist_add_string(ent, "name", nm);
		fnvlist_add_boolean_value(ent, "is_dir",
		    di->is_dir ? B_TRUE : B_FALSE);

		varr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		    (di->nvers > 0 ? di->nvers : 1) * sizeof (nvlist_t *));
		for (k = 0; varr != NULL && k < di->nvers; k++) {
			const dver_t *dv = &di->vers[k];
			nvlist_t *v = fnvlist_alloc();
			char pth[MAX_PATH * 4];
			char *q;

			fnvlist_add_string(v, "snap",
			    sl->snaps[dv->newest].name);
			fnvlist_add_string(v, "oldest_snap",
			    sl->snaps[dv->oldest].name);
			add_u64_str(v, "creation",
			    sl->snaps[dv->newest].creation);
			add_u64_str(v, "size", dv->size);
			add_u64_str(v, "mtime", dv->mtime);
			if (dv->path != NULL) {
				// as found in that snapshot, so renames follow
				_snprintf_s(pth, sizeof (pth), _TRUNCATE,
				    "%s\\.zfs\\snapshot\\%s%s%s", it->f.mount,
				    sl->snaps[dv->newest].name,
				    (dv->path[0] == '/' ||
				    dv->path[0] == '\\') ? "" : "\\",
				    dv->path);
			} else {
				_snprintf_s(pth, sizeof (pth), _TRUNCATE,
				    "%s\\.zfs\\snapshot\\%s%s%s\\%s",
				    it->f.mount, sl->snaps[dv->newest].name,
				    rel8[0] ? "\\" : "", rel8, nm);
			}
			for (q = pth; *q; q++)
				if (*q == '/')
					*q = '\\';
			fnvlist_add_string(v, "path", pth);
			varr[k] = v;
		}
		fnvlist_add_nvlist_array(ent, "versions",
		    (const nvlist_t **)varr, di->nvers);
		for (k = 0; varr != NULL && k < di->nvers; k++)
			fnvlist_free(varr[k]);
		if (varr != NULL)
			HeapFree(GetProcessHeap(), 0, varr);
		arr[nout++] = ent;
	}

	fnvlist_add_string(res, "dataset", it->f.ds);
	fnvlist_add_boolean_value(res, "truncated",
	    truncated ? B_TRUE : B_FALSE);
	add_u64_str(res, "snapshots", (uint64_t)(sl->nsnaps - first));
	fnvlist_add_nvlist_array(res, "items", (const nvlist_t **)arr, nout);
	for (i = 0; i < nout; i++)
		fnvlist_free(arr[i]);
	if (arr != NULL)
		HeapFree(GetProcessHeap(), 0, arr);

	free_items(items, nitems);

	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/*
 * Mount-free: "zfs diff" between each pair of consecutive snapshots names
 * (and numbers) the objects removed in that interval, and
 * ZFS_IOC_OBJ_TO_STATS then gives each one's versions in every snapshot, the
 * same way file versions are found. No snapshot is mounted.
 */
typedef struct {
	uint64_t obj;
	wchar_t name[MAX_PATH];
	BOOL is_dir;
	int last_snap;	// newest snapshot index known to hold the object
} delobj_t;

static void
norm_slashes(char *s)
{
	for (; *s; s++)
		if (*s == '\\')
			*s = '/';
}

/*
 * Parse "zfs diff -H -F" output with ZFS_DIFF_OBJNUM:
 *   <obj> TAB <change> TAB <class> TAB <path>
 * and keep removals ('-') whose parent directory is dir_slash ("" for the
 * volume root, else "/a/b"). mount_raw is the prefix zfs puts on paths.
 */
static void
parse_diff_removals(char *buf, const char *mount_raw, const char *dir_slash,
    int snap, delobj_t **arr, int *n, int *cap)
{
	size_t mlen = strlen(mount_raw);
	char *line = buf, *next;

	for (; line != NULL && *line != '\0'; line = next) {
		char *f[4], *p = line, *path, *base;
		wchar_t wname[MAX_PATH];
		uint64_t obj;
		delobj_t *d;
		size_t len;
		int k, nf = 0;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		len = strlen(line);
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';

		while (nf < 4) {
			f[nf++] = p;
			p = strchr(p, '\t');
			if (p == NULL)
				break;
			*p++ = '\0';
		}
		if (nf < 4 || f[1][0] != '-' || f[1][1] != '\0')
			continue;

		path = f[3];
		norm_slashes(path);
		if (_strnicmp(path, mount_raw, mlen) == 0)
			path += mlen;
		while (path[0] == '/' && path[1] == '/')
			path++;
		if (path[0] != '/')
			continue;

		base = strrchr(path, '/');
		len = (size_t)(base - path);
		if (len != strlen(dir_slash) ||
		    _strnicmp(path, dir_slash, len) != 0)
			continue;
		base++;
		if (*base == '\0' ||
		    MultiByteToWideChar(CP_UTF8, 0, base, -1, wname,
		    MAX_PATH) <= 0)
			continue;

		obj = strtoull(f[0], NULL, 10);
		for (k = 0; k < *n; k++) {
			if ((*arr)[k].obj == obj &&
			    _wcsicmp((*arr)[k].name, wname) == 0)
				break;
		}
		if (k < *n) {
			(*arr)[k].last_snap = snap;
			continue;
		}
		if (*n >= ZED_DELETED_MAX_ITEMS)
			continue;
		if (*n == *cap) {
			int ncap = *cap ? *cap * 2 : 64;
			delobj_t *t = realloc(*arr, ncap * sizeof (delobj_t));
			if (t == NULL)
				return;
			*arr = t;
			*cap = ncap;
		}
		d = &(*arr)[(*n)++];
		memset(d, 0, sizeof (*d));
		d->obj = obj;
		lstrcpynW(d->name, wname, ARRAYSIZE(d->name));
		d->is_dir = (f[2][0] == '/');
		d->last_snap = snap;
	}
}

// Run one diff into a temp file and parse its removals. 0 if it ran.
static int
diff_pair(zfs_handle_t *zhp, const char *from, const char *to,
    const char *mount_raw, const char *dir_slash, int snap, delobj_t **arr,
    int *n, int *cap)
{
	wchar_t tdir[MAX_PATH], tfile[MAX_PATH];
	HANDLE h, r;
	int rc;

	if (GetTempPathW(MAX_PATH, tdir) == 0 ||
	    GetTempFileNameW(tdir, L"zdf", 0, tfile) == 0)
		return (-1);
	h = CreateFileW(tfile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	    CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DeleteFileW(tfile);
		return (-1);
	}

	// zfs_show_diffs() takes ownership of (and closes) the descriptor.
	rc = zfs_show_diffs(zhp, (int)(INT_PTR)h, from, to,
	    ZFS_DIFF_PARSEABLE | ZFS_DIFF_CLASSIFY | ZFS_DIFF_NO_MANGLE |
	    ZFS_DIFF_OBJNUM);

	if (rc == 0) {
		r = CreateFileW(tfile, GENERIC_READ, FILE_SHARE_READ |
		    FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		    FILE_ATTRIBUTE_NORMAL, NULL);
		if (r != INVALID_HANDLE_VALUE) {
			DWORD sz = GetFileSize(r, NULL), got = 0;

			if (sz != INVALID_FILE_SIZE && sz < (64u << 20)) {
				char *buf = malloc(sz + 1);

				if (buf != NULL &&
				    ReadFile(r, buf, sz, &got, NULL)) {
					buf[got] = '\0';
					parse_diff_removals(buf, mount_raw,
					    dir_slash, snap, arr, n, cap);
				}
				free(buf);
			}
			CloseHandle(r);
		}
	}
	DeleteFileW(tfile);
	return (rc);
}

/*
 * Fill *items from diffs. Returns -1 when diffs are unusable here, so the
 * caller can fall back to reading the snapshots through their mounts.
 */
static int
collect_by_diff(const item_t *it, const snaplist_t *sl, int first,
    const dent_t *live, int nlive, const char *rel8, ditem_t **items,
    int *nitems, int *capitems)
{
	zfs_handle_t *zhp;
	delobj_t *objs = NULL;
	int nobjs = 0, capobjs = 0;
	char dir_slash[MAX_PATH * 3];
	char from[ZFS_MAX_DATASET_NAME_LEN * 2];
	char to[ZFS_MAX_DATASET_NAME_LEN * 2];
	char full[ZFS_MAX_DATASET_NAME_LEN * 2];
	int i, o, tried = 0, ok_pairs = 0;

	zhp = zfs_open(g_lzh, it->f.ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (-1);

	if (rel8[0] != '\0') {
		char *q;
		_snprintf_s(dir_slash, sizeof (dir_slash), _TRUNCATE, "/%s",
		    rel8);
		for (q = dir_slash; *q; q++)
			if (*q == '\\')
				*q = '/';
	} else {
		dir_slash[0] = '\0';
	}

	// Consecutive pairs among the searched snapshots, then newest -> live.
	for (i = first; i < sl->nsnaps; i++) {
		_snprintf_s(from, sizeof (from), _TRUNCATE, "%s@%s",
		    it->f.ds, sl->snaps[i].name);
		if (i + 1 < sl->nsnaps)
			_snprintf_s(to, sizeof (to), _TRUNCATE, "%s@%s",
			    it->f.ds, sl->snaps[i + 1].name);
		else
			_snprintf_s(to, sizeof (to), _TRUNCATE, "%s",
			    it->f.ds);
		tried++;
		if (diff_pair(zhp, from, to, it->f.mount_raw, dir_slash, i,
		    &objs, &nobjs, &capobjs) == 0)
			ok_pairs++;
	}
	zfs_close(zhp);

	if (tried > 0 && ok_pairs == 0) {
		free(objs);
		return (-1);
	}

	// The versions of each removed object, in every snapshot it was in.
	for (o = 0; o < nobjs; o++) {
		zfs_obj_version_t v;
		char path[MAX_PATH * 3];
		dent_t e;
		uint64_t gen;
		int s;

		// Names that exist again are not deleted.
		lstrcpynW(e.name, objs[o].name, ARRAYSIZE(e.name));
		if (nlive > 0 && bsearch(&e, live, nlive, sizeof (dent_t),
		    dent_cmp) != NULL)
			continue;

		_snprintf_s(full, sizeof (full), _TRUNCATE, "%s@%s", it->f.ds,
		    sl->snaps[objs[o].last_snap].name);
		if (zfs_get_obj_version(g_lzh, full, objs[o].obj, &v, path,
		    sizeof (path)) != 0)
			continue;
		gen = v.zov_gen;

		for (s = sl->nsnaps - 1; s >= first; s--) {
			_snprintf_s(full, sizeof (full), _TRUNCATE, "%s@%s",
			    it->f.ds, sl->snaps[s].name);
			if (zfs_get_obj_version(g_lzh, full, objs[o].obj, &v,
			    path, sizeof (path)) != 0 || v.zov_gen != gen)
				continue;
			lstrcpynW(e.name, objs[o].name, ARRAYSIZE(e.name));
			e.is_dir = objs[o].is_dir;
			e.size = v.zov_size;
			e.mtime = v.zov_mtime[0];
			note_entry(items, nitems, capitems, &e, s, path);
		}
	}
	free(objs);
	return (0);
}

char *
zed_deleted_items_json(void *client, const char *path_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	item_t it;
	char msg[96];
	char rel8[MAX_PATH * 3];
	snaplist_t sl;
	zfs_handle_t *zhp;
	wchar_t mount_w[MAX_PATH * 3];
	dent_t *live = NULL;
	int nlive, i, j, first;
	ditem_t *items = NULL;
	int nitems = 0, capitems = 0;
	BOOL truncated = FALSE;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	if (!resolve_item(client, path_utf8, &it, msg, sizeof (msg)))
		return (fail_json(res, msg, out_len));
	if (!it.is_dir)
		return (fail_json(res, "not a directory", out_len));

	MultiByteToWideChar(CP_UTF8, 0, it.f.mount, -1, mount_w,
	    ARRAYSIZE(mount_w));
	to_utf8(it.rel, rel8, sizeof (rel8));

	memset(&sl, 0, sizeof (sl));
	zhp = zfs_open(g_lzh, it.f.ds, ZFS_TYPE_FILESYSTEM);
	if (zhp != NULL) {
		(void) zfs_iter_snapshots_sorted(zhp, snaplist_cb, &sl, 0, 0);
		zfs_close(zhp);
	}

	first = 0;
	if (sl.nsnaps > ZED_DELETED_MAX_SNAPS) {
		first = sl.nsnaps - ZED_DELETED_MAX_SNAPS;
		truncated = TRUE;
	}

	// The live listing, read as the caller so ACLs are honoured.
	if (!ImpersonateNamedPipeClient((HANDLE)client)) {
		free(sl.snaps);
		return (fail_json(res, "cannot impersonate caller", out_len));
	}
	nlive = read_dir(it.full, &live);
	RevertToSelf();
	if (nlive < 0) {
		free(sl.snaps);
		return (fail_json(res, "cannot read directory", out_len));
	}
	qsort(live, nlive, sizeof (dent_t), dent_cmp);

	// Preferred: no mounts, just diffs and object stats.
	if (collect_by_diff(&it, &sl, first, live, nlive, rel8, &items,
	    &nitems, &capitems) == 0) {
		char *json = build_reply(res, &it, &sl, first, truncated,
		    items, nitems, rel8, out_len);
		free(live);
		free(sl.snaps);
		return (json);
	}
	free_items(items, nitems);
	items = NULL;
	nitems = capitems = 0;

	// Fallback: read each snapshot's directory through its mount.
	if (!ImpersonateNamedPipeClient((HANDLE)client)) {
		free(live);
		free(sl.snaps);
		return (fail_json(res, "cannot impersonate caller", out_len));
	}

	// Newest to oldest, so the first sighting is the newest version.
	for (i = sl.nsnaps - 1; i >= first; i--) {
		wchar_t snapw[ZFS_MAX_DATASET_NAME_LEN];
		wchar_t dir[MAX_PATH * 3];
		dent_t *ents = NULL;
		int n;

		MultiByteToWideChar(CP_UTF8, 0, sl.snaps[i].name, -1, snapw,
		    ARRAYSIZE(snapw));
		_snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE,
		    L"%s\\.zfs\\snapshot\\%s%s%s", mount_w, snapw,
		    it.rel[0] ? L"\\" : L"", it.rel);
		n = read_dir(dir, &ents);
		for (j = 0; j < n; j++) {
			if (bsearch(&ents[j], live, nlive, sizeof (dent_t),
			    dent_cmp) != NULL)
				continue;
			note_entry(&items, &nitems, &capitems, &ents[j], i,
			    NULL);
		}
		free(ents);
	}
	RevertToSelf();

	{
		char *json = build_reply(res, &it, &sl, first, truncated,
		    items, nitems, rel8, out_len);
		free(live);
		free(sl.snaps);
		return (json);
	}
}

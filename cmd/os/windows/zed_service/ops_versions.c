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

/*
 * The path is opened while impersonating the pipe client, so a user can only
 * enumerate versions of objects they themselves are allowed to open. The
 * object number found that way is then used with the service's own token.
 */
char *
zed_file_versions_json(void *client, const char *path_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	wchar_t wpath[MAX_PATH * 2];
	wchar_t full[MAX_PATH * 2];
	wchar_t root[MAX_PATH];
	HANDLE h;
	FILE_ID_INFO fid;
	BY_HANDLE_FILE_INFORMATION bhfi;
	BOOL got_id = FALSE, got_info = FALSE;
	DWORD gle = 0;
	find_ds_ctx_t fctx;
	snap_ctx_t sctx;
	zfs_obj_version_t live;
	char livepath[MAX_PATH * 3];
	zfs_handle_t *zhp;
	uint64_t obj;
	nvlist_t **arr = NULL;
	int i, nout = 0, err;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	if (path_utf8 == NULL || path_utf8[0] == '\0' ||
	    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath,
	    MAX_PATH * 2) <= 0)
		return (fail_json(res, "invalid path", out_len));

	if (!ImpersonateNamedPipeClient((HANDLE)client))
		return (fail_json(res, "cannot impersonate caller", out_len));

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
		char msg[96];
		_snprintf_s(msg, sizeof (msg), _TRUNCATE,
		    "cannot open path (error %lu)", (unsigned long)gle);
		return (fail_json(res, msg, out_len));
	}

	// Low 64 bits of the 128-bit FileId are the ZFS object number.
	memcpy(&obj, fid.FileId.Identifier, sizeof (obj));

	if (full[0] == L'\0' || !GetVolumePathNameW(full, root, MAX_PATH))
		return (fail_json(res, "cannot determine volume", out_len));

	memset(&fctx, 0, sizeof (fctx));
	if (!GetVolumeNameForVolumeMountPointW(root, fctx.vol_guid, MAX_PATH))
		return (fail_json(res, "cannot determine volume", out_len));

	(void) zfs_iter_root(g_lzh, find_ds_cb, &fctx);
	if (!fctx.found)
		return (fail_json(res, "not on a ZFS filesystem", out_len));

	err = zfs_get_obj_version(g_lzh, fctx.ds, obj, &live, livepath,
	    sizeof (livepath));
	if (err != 0)
		return (fail_json(res, "cannot read object stats", out_len));

	fnvlist_add_string(res, "dataset", fctx.ds);
	fnvlist_add_string(res, "mount", fctx.mount);
	add_u64_str(res, "obj", obj);
	fnvlist_add_boolean_value(res, "is_dir",
	    (got_info && (bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ?
	    B_TRUE : B_FALSE);
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

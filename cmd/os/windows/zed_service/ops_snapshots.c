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
 * Copyright (c) 2026 Jorgen Lundman <lundman@lundman.net>.
 */

/*
 * Dataset and snapshot management for zfs_tray: list the datasets, list the
 * snapshots of one dataset, create and destroy snapshots. See ops_snapshots.h
 * for the JSON shapes.
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
#include "ops_snapshots.h"
#include "ops_pool.h"
#include "memfile.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

extern libzfs_handle_t *g_lzh;

#define	ZED_SNAP_MAX_DATASETS	16384
#define	ZED_SNAP_MAX_SNAPSHOTS	65536

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
	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	fnvlist_add_string(res, "err", msg);
	return (finish_json(res, out_len));
}

/* The last libzfs error, or the fallback if it has nothing to say. */
static const char *
lzh_err(const char *fallback)
{
	const char *d = g_lzh ? libzfs_error_description(g_lzh) : NULL;

	return ((d != NULL && d[0] != '\0') ? d : fallback);
}

/*
 * A growable array of nvlists, serialised into the result with a single
 * fnvlist_add_nvlist_array(), see zed_list_disks_json().
 */
typedef struct {
	nvlist_t **v;
	int n;
	int cap;
	int max;
} nvarr_t;

static BOOL
nvarr_add(nvarr_t *a, nvlist_t *e)
{
	if (a->n >= a->max) {
		fnvlist_free(e);
		return (FALSE);
	}
	if (a->n == a->cap) {
		int ncap = a->cap ? a->cap * 2 : 64;
		nvlist_t **nv;

		if (a->v == NULL)
			nv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			    ncap * sizeof (nvlist_t *));
		else
			nv = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			    a->v, ncap * sizeof (nvlist_t *));
		if (nv == NULL) {
			fnvlist_free(e);
			return (FALSE);
		}
		a->v = nv;
		a->cap = ncap;
	}
	a->v[a->n++] = e;
	return (TRUE);
}

static void
nvarr_finish(nvarr_t *a, nvlist_t *res, const char *key)
{
	int i;

	fnvlist_add_nvlist_array(res, key, (const nvlist_t **)a->v, a->n);
	for (i = 0; i < a->n; i++)
		fnvlist_free(a->v[i]);
	if (a->v != NULL)
		HeapFree(GetProcessHeap(), 0, a->v);
	memset(a, 0, sizeof (*a));
}

/* -------------------- datasets -------------------- */

typedef struct {
	nvarr_t arr;
	BOOL truncated;
} ds_ctx_t;

static int ds_cb(zfs_handle_t *zhp, void *arg);

static nvlist_t *
dataset_entry(zfs_handle_t *zhp)
{
	nvlist_t *e = fnvlist_alloc();
	const char *name = zfs_get_name(zhp);
	zfs_type_t type = zfs_get_type(zhp);
	char buf[ZFS_MAXPROPLEN];
	char *where = NULL;

	fnvlist_add_string(e, "name", name);
	fnvlist_add_boolean_value(e, "pool",
	    strchr(name, '/') == NULL ? B_TRUE : B_FALSE);
	fnvlist_add_boolean_value(e, "volume",
	    type == ZFS_TYPE_VOLUME ? B_TRUE : B_FALSE);

	fnvlist_add_boolean_value(e, "mounted",
	    (type == ZFS_TYPE_FILESYSTEM && zfs_is_mounted(zhp, &where)) ?
	    B_TRUE : B_FALSE);
	free(where);

	buf[0] = '\0';
	if (type == ZFS_TYPE_FILESYSTEM)
		(void) zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, buf,
		    sizeof (buf), NULL, NULL, 0, B_FALSE);
	fnvlist_add_string(e, "mountpoint", buf);

	add_u64_str(e, "used", zfs_prop_get_int(zhp, ZFS_PROP_USED));
	add_u64_str(e, "avail", zfs_prop_get_int(zhp, ZFS_PROP_AVAILABLE));
	add_u64_str(e, "refer", zfs_prop_get_int(zhp, ZFS_PROP_REFERENCED));

	buf[0] = '\0';
	(void) zfs_prop_get(zhp, ZFS_PROP_ORIGIN, buf, sizeof (buf), NULL,
	    NULL, 0, B_FALSE);
	// "-" means no origin
	fnvlist_add_string(e, "origin", strcmp(buf, "-") == 0 ? "" : buf);

	fnvlist_add_boolean_value(e, "encrypted",
	    zfs_prop_get_int(zhp, ZFS_PROP_ENCRYPTION) != ZIO_CRYPT_OFF ?
	    B_TRUE : B_FALSE);
	fnvlist_add_boolean_value(e, "keyloaded",
	    zfs_prop_get_int(zhp, ZFS_PROP_KEYSTATUS) ==
	    ZFS_KEYSTATUS_AVAILABLE ? B_TRUE : B_FALSE);
	return (e);
}

/* Takes ownership of zhp; parents are listed before their children. */
static int
ds_cb(zfs_handle_t *zhp, void *arg)
{
	ds_ctx_t *ctx = arg;

	if (!nvarr_add(&ctx->arr, dataset_entry(zhp))) {
		ctx->truncated = TRUE;
		zfs_close(zhp);
		return (0);
	}
	if (zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM)
		// Not zfs_iter_children(): that lists the snapshots too.
		(void) zfs_iter_filesystems(zhp, ds_cb, ctx);
	zfs_close(zhp);
	return (0);
}

char *
zed_list_datasets_json(size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	ds_ctx_t ctx;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));

	memset(&ctx, 0, sizeof (ctx));
	ctx.arr.max = ZED_SNAP_MAX_DATASETS;
	(void) zfs_iter_root(g_lzh, ds_cb, &ctx);

	nvarr_finish(&ctx.arr, res, "datasets");
	fnvlist_add_boolean_value(res, "truncated",
	    ctx.truncated ? B_TRUE : B_FALSE);
	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/* -------------------- snapshots -------------------- */

typedef struct {
	nvarr_t arr;
	BOOL truncated;
} snap_ctx_t;

/* Takes ownership of zhp. */
static int
snap_cb(zfs_handle_t *zhp, void *arg)
{
	snap_ctx_t *ctx = arg;
	nvlist_t *e = fnvlist_alloc();
	const char *name = zfs_get_name(zhp);
	const char *at = strchr(name, '@');

	fnvlist_add_string(e, "name", at != NULL ? at + 1 : name);
	add_u64_str(e, "creation", zfs_prop_get_int(zhp, ZFS_PROP_CREATION));
	add_u64_str(e, "used", zfs_prop_get_int(zhp, ZFS_PROP_USED));
	add_u64_str(e, "refer", zfs_prop_get_int(zhp, ZFS_PROP_REFERENCED));
	add_u64_str(e, "holds", zfs_prop_get_int(zhp, ZFS_PROP_USERREFS));
	if (!nvarr_add(&ctx->arr, e))
		ctx->truncated = TRUE;
	zfs_close(zhp);
	return (0);
}

char *
zed_list_snapshots_json(const char *dataset_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	snap_ctx_t ctx;
	zfs_handle_t *zhp;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));
	if (dataset_utf8 == NULL || dataset_utf8[0] == '\0' ||
	    strchr(dataset_utf8, '@') != NULL ||
	    !zfs_name_valid(dataset_utf8, ZFS_TYPE_FILESYSTEM |
	    ZFS_TYPE_VOLUME))
		return (fail_json(res, "invalid dataset name", out_len));
	fnvlist_add_string(res, "dataset", dataset_utf8);

	zhp = zfs_open(g_lzh, dataset_utf8,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
	if (zhp == NULL)
		return (fail_json(res, lzh_err("dataset not found"), out_len));

	memset(&ctx, 0, sizeof (ctx));
	ctx.arr.max = ZED_SNAP_MAX_SNAPSHOTS;
	// Oldest first.
	(void) zfs_iter_snapshots_sorted(zhp, snap_cb, &ctx, 0, 0);
	zfs_close(zhp);

	nvarr_finish(&ctx.arr, res, "snapshots");
	fnvlist_add_boolean_value(res, "truncated",
	    ctx.truncated ? B_TRUE : B_FALSE);
	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/* -------------------- create / destroy -------------------- */

char *
zed_snapshot_create_json(uint32_t flags, const char *dataset_utf8,
    const char *snap_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	char full[ZFS_MAX_DATASET_NAME_LEN];
	int rc;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));
	if (dataset_utf8 == NULL || dataset_utf8[0] == '\0' ||
	    snap_utf8 == NULL || snap_utf8[0] == '\0' ||
	    strchr(dataset_utf8, '@') != NULL ||
	    strchr(snap_utf8, '@') != NULL)
		return (fail_json(res, "invalid name", out_len));
	fnvlist_add_string(res, "dataset", dataset_utf8);

	_snprintf_s(full, sizeof (full), _TRUNCATE, "%s@%s", dataset_utf8,
	    snap_utf8);
	if (!zfs_name_valid(full, ZFS_TYPE_SNAPSHOT))
		return (fail_json(res, "invalid snapshot name", out_len));

	rc = zfs_snapshot(g_lzh, full,
	    (flags & ZSNAP_RECURSIVE) ? B_TRUE : B_FALSE, NULL);
	if (rc != 0)
		return (fail_json(res, lzh_err("snapshot failed"), out_len));

	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

char *
zed_snapshot_destroy_json(uint32_t flags, const char *dataset_utf8,
    const char *names, uint32_t count, size_t names_len, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	nvlist_t *snaps = NULL;
	const char *p = names;
	size_t remain = names_len;
	uint32_t i;
	int rc;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));
	if (dataset_utf8 == NULL || dataset_utf8[0] == '\0' ||
	    strchr(dataset_utf8, '@') != NULL || count == 0)
		return (fail_json(res, "invalid arguments", out_len));
	fnvlist_add_string(res, "dataset", dataset_utf8);

	snaps = fnvlist_alloc();
	for (i = 0; i < count; i++) {
		char full[ZFS_MAX_DATASET_NAME_LEN];
		size_t n = (remain > 0) ? strnlen(p, remain) : 0;

		if (remain == 0 || n >= remain || n == 0 ||
		    strchr(p, '@') != NULL) {
			fnvlist_free(snaps);
			return (fail_json(res, "invalid snapshot list",
			    out_len));
		}
		_snprintf_s(full, sizeof (full), _TRUNCATE, "%s@%s",
		    dataset_utf8, p);
		if (!zfs_name_valid(full, ZFS_TYPE_SNAPSHOT)) {
			fnvlist_free(snaps);
			return (fail_json(res, "invalid snapshot name",
			    out_len));
		}
		fnvlist_add_boolean(snaps, full);
		p += n + 1;
		remain -= n + 1;
	}

	// All or nothing, in one transaction.
	rc = zfs_destroy_snaps_nvl(g_lzh, snaps,
	    (flags & ZSNAPD_DEFER) ? B_TRUE : B_FALSE);
	fnvlist_free(snaps);
	if (rc != 0)
		return (fail_json(res, lzh_err("destroy failed"), out_len));

	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/* -------------------- rollback -------------------- */

typedef struct {
	uint64_t txg;		/* creation txg of the target snapshot */
	uint64_t nnewer;	/* snapshots newer than the target */
	uint64_t nclones;	/* clones of those snapshots */
	char first_clone[ZFS_MAX_DATASET_NAME_LEN];
} roll_ctx_t;

/* Takes ownership of zhp. */
static int
roll_cb(zfs_handle_t *zhp, void *arg)
{
	roll_ctx_t *ctx = arg;

	if (zfs_prop_get_int(zhp, ZFS_PROP_CREATETXG) > ctx->txg) {
		nvlist_t *clones = zfs_get_clones_nvl(zhp);
		nvpair_t *nvp = NULL;

		ctx->nnewer++;
		// Owned by the handle, not ours to free.
		while (clones != NULL &&
		    (nvp = nvlist_next_nvpair(clones, nvp)) != NULL) {
			if (ctx->nclones++ == 0)
				_snprintf_s(ctx->first_clone,
				    sizeof (ctx->first_clone), _TRUNCATE,
				    "%s", nvpair_name(nvp));
		}
	}
	zfs_close(zhp);
	return (0);
}

char *
zed_snapshot_rollback_json(uint32_t flags, const char *dataset_utf8,
    const char *snap_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	char full[ZFS_MAX_DATASET_NAME_LEN];
	char msg[ZFS_MAX_DATASET_NAME_LEN + 128];
	zfs_handle_t *zhp, *shp;
	roll_ctx_t ctx;
	int rc;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));
	if (dataset_utf8 == NULL || dataset_utf8[0] == '\0' ||
	    snap_utf8 == NULL || snap_utf8[0] == '\0' ||
	    strchr(dataset_utf8, '@') != NULL ||
	    strchr(snap_utf8, '@') != NULL)
		return (fail_json(res, "invalid name", out_len));
	fnvlist_add_string(res, "dataset", dataset_utf8);

	_snprintf_s(full, sizeof (full), _TRUNCATE, "%s@%s", dataset_utf8,
	    snap_utf8);
	if (!zfs_name_valid(full, ZFS_TYPE_SNAPSHOT))
		return (fail_json(res, "invalid snapshot name", out_len));

	zhp = zfs_open(g_lzh, dataset_utf8,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
	if (zhp == NULL)
		return (fail_json(res, lzh_err("dataset not found"), out_len));
	shp = zfs_open(g_lzh, full, ZFS_TYPE_SNAPSHOT);
	if (shp == NULL) {
		zfs_close(zhp);
		return (fail_json(res, lzh_err("snapshot not found"),
		    out_len));
	}

	memset(&ctx, 0, sizeof (ctx));
	ctx.txg = zfs_prop_get_int(shp, ZFS_PROP_CREATETXG);
	(void) zfs_iter_snapshots(zhp, B_FALSE, roll_cb, &ctx, ctx.txg, 0);

	if (ctx.nnewer > 0 && !(flags & ZROLL_DESTROY_NEWER)) {
		_snprintf_s(msg, sizeof (msg), _TRUNCATE,
		    "%llu newer snapshot%s would be destroyed",
		    (unsigned long long)ctx.nnewer,
		    ctx.nnewer == 1 ? "" : "s");
		fnvlist_add_boolean_value(res, "newer_block", B_TRUE);
		zfs_close(shp);
		zfs_close(zhp);
		return (fail_json(res, msg, out_len));
	}
	if (ctx.nclones > 0 && !(flags & ZROLL_DESTROY_CLONES)) {
		_snprintf_s(msg, sizeof (msg), _TRUNCATE,
		    "%s%s depends on a newer snapshot",
		    ctx.first_clone, ctx.nclones > 1 ? " and others" : "");
		fnvlist_add_boolean_value(res, "clones_block", B_TRUE);
		zfs_close(shp);
		zfs_close(zhp);
		return (fail_json(res, msg, out_len));
	}

	// Destroys the newer snapshots (and their clones), then rolls back.
	rc = zfs_rollback(zhp, shp,
	    (flags & ZROLL_DESTROY_CLONES) ? B_TRUE : B_FALSE);
	zfs_close(shp);
	zfs_close(zhp);
	if (rc != 0)
		return (fail_json(res, lzh_err("rollback failed"), out_len));

	add_u64_str(res, "destroyed", ctx.nnewer);
	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

/* -------------------- create / destroy datasets -------------------- */

#define	ZED_DS_MAX_PROPS	64
#define	ZED_DS_MAX_TREE		4096

/* "key=value" lines into a string nvlist; FALSE if a line is malformed. */
static BOOL
parse_props(const char *text, nvlist_t *props, char *msg, size_t msgsz)
{
	char *copy, *line, *next;
	int n = 0;
	BOOL ok = TRUE;

	if (text == NULL || text[0] == '\0')
		return (TRUE);
	copy = _strdup(text);
	if (copy == NULL)
		return (FALSE);
	for (line = copy; line != NULL && ok; line = next) {
		char *eq, *end;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		// Trim
		while (*line == ' ' || *line == '\t' || *line == '\r')
			line++;
		end = line + strlen(line);
		while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
		    end[-1] == '\r'))
			*--end = '\0';
		if (*line == '\0' || *line == '#')
			continue;
		eq = strchr(line, '=');
		if (eq == NULL || eq == line || n >= ZED_DS_MAX_PROPS) {
			_snprintf_s(msg, msgsz, _TRUNCATE,
			    "bad property line '%s'", line);
			ok = FALSE;
			break;
		}
		*eq++ = '\0';
		fnvlist_add_string(props, line, eq);
		n++;
	}
	free(copy);
	return (ok);
}

char *
zed_dataset_create_json(const char *name_utf8, const char *props_utf8,
    const char *passphrase_utf8, size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();
	char msg[256];
	zfs_handle_t *zhp;
	wchar_t keyfile[MAX_PATH] = { 0 };
	int rc;
	BOOL mounted = FALSE;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL) {
		fnvlist_free(props);
		return (fail_json(res, "libzfs is not available", out_len));
	}
	if (name_utf8 == NULL || name_utf8[0] == '\0' ||
	    strchr(name_utf8, '@') != NULL ||
	    strchr(name_utf8, '/') == NULL ||
	    !zfs_name_valid(name_utf8, ZFS_TYPE_FILESYSTEM)) {
		fnvlist_free(props);
		return (fail_json(res, "invalid dataset name", out_len));
	}
	fnvlist_add_string(res, "dataset", name_utf8);
	msg[0] = '\0';
	if (!parse_props(props_utf8, props, msg, sizeof (msg))) {
		fnvlist_free(props);
		return (fail_json(res, msg[0] ? msg : "invalid properties",
		    out_len));
	}

	if (passphrase_utf8 != NULL && passphrase_utf8[0] != '\0') {
		char keyfile_utf8[MAX_PATH * 3];
		char keyloc[MAX_PATH * 3 + 8];

		if (!zed_write_temp_keyfile(passphrase_utf8, keyfile,
		    _countof(keyfile))) {
			fnvlist_free(props);
			return (fail_json(res, "failed to stage encryption "
			    "key", out_len));
		}
		WideCharToMultiByte(CP_UTF8, 0, keyfile, -1, keyfile_utf8,
		    sizeof (keyfile_utf8), NULL, NULL);
		// Same quirk as for a pool: zfs_prop_valid_keylocation() wants
		// "file:///", get_key_material_file() strips only "file://",
		// and Windows takes the leftover slash before the drive.
		_snprintf_s(keyloc, sizeof (keyloc), _TRUNCATE, "file:///%s",
		    keyfile_utf8);
		// A cipher picked in the dialog stays, otherwise the default.
		if (!nvlist_exists(props, "encryption"))
			fnvlist_add_string(props, "encryption", "on");
		fnvlist_add_string(props, "keyformat", "passphrase");
		fnvlist_add_string(props, "keylocation", keyloc);
	}

	// The parent has to be there; this never makes missing ancestors.
	rc = zfs_create(g_lzh, name_utf8, ZFS_TYPE_FILESYSTEM,
	    nvlist_empty(props) ? NULL : props);
	fnvlist_free(props);
	if (keyfile[0] != L'\0')
		DeleteFileW(keyfile);
	if (rc != 0)
		return (fail_json(res, lzh_err("create failed"), out_len));

	// Like "zfs create": mount it unless canmount says otherwise.
	zhp = zfs_open(g_lzh, name_utf8, ZFS_TYPE_FILESYSTEM);
	if (zhp != NULL) {
		// The keyfile is gone, so unlock it like any other from now.
		if (passphrase_utf8 != NULL && passphrase_utf8[0] != '\0')
			(void) zfs_prop_set(zhp, "keylocation", "prompt");
		if (zfs_prop_get_int(zhp, ZFS_PROP_CANMOUNT) ==
		    ZFS_CANMOUNT_ON && zfs_mount(zhp, NULL, 0) == 0)
			mounted = TRUE;
		zfs_close(zhp);
	}
	fnvlist_add_boolean_value(res, "mounted",
	    mounted ? B_TRUE : B_FALSE);
	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	return (finish_json(res, out_len));
}

typedef struct {
	char (*names)[ZFS_MAX_DATASET_NAME_LEN];	/* parents first */
	int n;
	int cap;
	nvlist_t *snaps;	/* every snapshot below, as full names */
	uint64_t nsnaps;
	uint64_t nclones;
	char first_clone[ZFS_MAX_DATASET_NAME_LEN];
	BOOL overflow;
} tree_ctx_t;

static int tree_cb(zfs_handle_t *zhp, void *arg);

static int
tree_snap_cb(zfs_handle_t *zhp, void *arg)
{
	tree_ctx_t *ctx = arg;
	nvlist_t *clones = zfs_get_clones_nvl(zhp);
	nvpair_t *nvp = NULL;

	fnvlist_add_boolean(ctx->snaps, zfs_get_name(zhp));
	ctx->nsnaps++;
	// Owned by the handle, not ours to free.
	while (clones != NULL &&
	    (nvp = nvlist_next_nvpair(clones, nvp)) != NULL) {
		if (ctx->nclones++ == 0)
			_snprintf_s(ctx->first_clone,
			    sizeof (ctx->first_clone), _TRUNCATE, "%s",
			    nvpair_name(nvp));
	}
	zfs_close(zhp);
	return (0);
}

/* Takes ownership of zhp: record it and everything below it. */
static int
tree_cb(zfs_handle_t *zhp, void *arg)
{
	tree_ctx_t *ctx = arg;

	if (ctx->n >= ZED_DS_MAX_TREE) {
		ctx->overflow = TRUE;
		zfs_close(zhp);
		return (0);
	}
	if (ctx->n == ctx->cap) {
		int ncap = ctx->cap ? ctx->cap * 2 : 64;
		char (*nn)[ZFS_MAX_DATASET_NAME_LEN];

		if (ctx->names == NULL)
			nn = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			    ncap * ZFS_MAX_DATASET_NAME_LEN);
		else
			nn = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			    ctx->names, ncap * ZFS_MAX_DATASET_NAME_LEN);
		if (nn == NULL) {
			ctx->overflow = TRUE;
			zfs_close(zhp);
			return (0);
		}
		ctx->names = nn;
		ctx->cap = ncap;
	}
	_snprintf_s(ctx->names[ctx->n], ZFS_MAX_DATASET_NAME_LEN, _TRUNCATE,
	    "%s", zfs_get_name(zhp));
	ctx->n++;
	(void) zfs_iter_snapshots(zhp, B_FALSE, tree_snap_cb, ctx, 0, 0);
	if (zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM)
		(void) zfs_iter_filesystems(zhp, tree_cb, ctx);
	zfs_close(zhp);
	return (0);
}

/*
 * Unmount every mounted filesystem in the tree, deepest first. If one will
 * not, the ones already unmounted are mounted again and msg says which.
 */
static BOOL
unmount_tree(const tree_ctx_t *ctx, char *msg, size_t msgsz)
{
	int i, j;

	for (i = ctx->n - 1; i >= 0; i--) {
		// Only filesystems open as one, volumes are skipped.
		zfs_handle_t *dh = zfs_open(g_lzh, ctx->names[i],
		    ZFS_TYPE_FILESYSTEM);
		char *where = NULL;
		BOOL mounted;

		if (dh == NULL)
			continue;
		mounted = zfs_is_mounted(dh, &where);
		free(where);
		if (mounted && zfs_unmount(dh, NULL, 0) != 0) {
			_snprintf_s(msg, msgsz, _TRUNCATE, "%s: %s",
			    ctx->names[i], lzh_err("could not be unmounted"));
			zfs_close(dh);
			for (j = i + 1; j < ctx->n; j++) {
				zfs_handle_t *rh = zfs_open(g_lzh,
				    ctx->names[j], ZFS_TYPE_FILESYSTEM);

				if (rh == NULL)
					continue;
				if (zfs_prop_get_int(rh, ZFS_PROP_CANMOUNT) ==
				    ZFS_CANMOUNT_ON)
					(void) zfs_mount(rh, NULL, 0);
				zfs_close(rh);
			}
			return (FALSE);
		}
		zfs_close(dh);
	}
	return (TRUE);
}

char *
zed_dataset_destroy_json(uint32_t flags, const char *name_utf8,
    size_t *out_len)
{
	nvlist_t *res = fnvlist_alloc();
	char msg[ZFS_MAX_DATASET_NAME_LEN + 128];
	zfs_handle_t *zhp;
	tree_ctx_t ctx;
	int i, failed = 0;
	char *json;

	*out_len = 0;
	fnvlist_add_boolean_value(res, "ok", B_FALSE);
	if (g_lzh == NULL)
		return (fail_json(res, "libzfs is not available", out_len));
	if (name_utf8 == NULL || name_utf8[0] == '\0' ||
	    strchr(name_utf8, '@') != NULL ||
	    !zfs_name_valid(name_utf8, ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME))
		return (fail_json(res, "invalid dataset name", out_len));
	fnvlist_add_string(res, "dataset", name_utf8);
	if (strchr(name_utf8, '/') == NULL)
		return (fail_json(res, "that is a pool, destroy it from the "
		    "tray menu", out_len));

	zhp = zfs_open(g_lzh, name_utf8, ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
	if (zhp == NULL)
		return (fail_json(res, lzh_err("dataset not found"), out_len));

	memset(&ctx, 0, sizeof (ctx));
	ctx.snaps = fnvlist_alloc();
	(void) tree_cb(zhp, &ctx);	// takes zhp

	if (ctx.overflow) {
		json = fail_json(res, "too many datasets below it", out_len);
		goto out;
	}
	if (ctx.nclones > 0) {
		_snprintf_s(msg, sizeof (msg), _TRUNCATE,
		    "%s is a clone of a snapshot of it, destroy or promote it "
		    "first", ctx.first_clone);
		json = fail_json(res, msg, out_len);
		goto out;
	}
	if ((ctx.n > 1 || ctx.nsnaps > 0) && !(flags & ZDSD_RECURSIVE)) {
		_snprintf_s(msg, sizeof (msg), _TRUNCATE,
		    "it has %d child dataset%s and %llu snapshot%s",
		    ctx.n - 1, ctx.n == 2 ? "" : "s",
		    (unsigned long long)ctx.nsnaps,
		    ctx.nsnaps == 1 ? "" : "s");
		fnvlist_add_boolean_value(res, "needs_recursive", B_TRUE);
		add_u64_str(res, "children", (uint64_t)(ctx.n - 1));
		add_u64_str(res, "snapshots", ctx.nsnaps);
		json = fail_json(res, msg, out_len);
		goto out;
	}

	// zfs_destroy() does not unmount here, so unmount everything first.
	if (!unmount_tree(&ctx, msg, sizeof (msg))) {
		json = fail_json(res, msg, out_len);
		goto out;
	}

	// All the snapshots in one go, then the datasets, deepest first.
	if (ctx.nsnaps > 0 &&
	    zfs_destroy_snaps_nvl(g_lzh, ctx.snaps, B_FALSE) != 0) {
		json = fail_json(res, lzh_err("could not destroy snapshots"),
		    out_len);
		goto out;
	}
	for (i = ctx.n - 1; i >= 0; i--) {
		zfs_handle_t *dh = zfs_open(g_lzh, ctx.names[i],
		    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);

		if (dh == NULL || zfs_destroy(dh, B_FALSE) != 0) {
			failed = 1;
			if (dh != NULL)
				zfs_close(dh);
			break;
		}
		zfs_close(dh);
	}
	if (failed) {
		_snprintf_s(msg, sizeof (msg), _TRUNCATE, "%s: %s",
		    ctx.names[i], lzh_err("destroy failed"));
		json = fail_json(res, msg, out_len);
		goto out;
	}

	fnvlist_remove(res, "ok");
	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	json = finish_json(res, out_len);
out:
	fnvlist_free(ctx.snaps);
	if (ctx.names != NULL)
		HeapFree(GetProcessHeap(), 0, ctx.names);
	return (json);
}

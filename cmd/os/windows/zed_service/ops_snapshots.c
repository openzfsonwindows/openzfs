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
		(void) zfs_iter_children(zhp, ds_cb, ctx);
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

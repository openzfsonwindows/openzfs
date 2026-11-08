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

#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <libzfs.h>
#include <libzutil.h>
#include <sys/nvpair.h>

#include "ops_import.h"
#include "ops_crypto.h"
#include "ops_status.h"
#include "memfile.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

extern libzfs_handle_t *g_lzh;

static void
add_guid_string(nvlist_t *dst, uint64_t g)
{
	char gs[32];
	_snprintf_s(gs, sizeof (gs), _TRUNCATE, "%llu", (unsigned long long)g);
	fnvlist_add_string(dst, "guid", gs);
}

static nvlist_t *
build_import_props(uint32_t flags, const char *altroot_utf8)
{
	nvlist_t *props = NULL;
	VERIFY0(nvlist_alloc(&props, NV_UNIQUE_NAME, 0));
	if (flags & ZIMP_READONLY)
		VERIFY0(nvlist_add_string(props, "readonly", "on"));
	if (flags & ZIMP_NOMOUNT)
		VERIFY0(nvlist_add_string(props, "mountpoint", "none"));
	if (altroot_utf8 && altroot_utf8[0])
		VERIFY0(nvlist_add_string(props, "altroot", altroot_utf8));
	return (props);
}

/*
 * Search for importable pools. With "paths" (back to back NUL-terminated
 * folders or files, as for zpool import -d) only those are searched,
 * otherwise the disks are. The paths are tidied up the way that is known to
 * work: forward slashes, no trailing one.
 */
static nvlist_t *
search_pools(boolean_t can_be_active, uint64_t guid, const char *paths,
    size_t paths_len)
{
	importargs_t ia = { 0 };
	char *copy = NULL;
	char **dirs = NULL;
	int ndirs = 0;

	if (paths != NULL && paths_len != 0) {
		copy = malloc(paths_len + 1);
		if (copy == NULL)
			return (NULL);
		memcpy(copy, paths, paths_len);
		copy[paths_len] = 0;

		for (size_t off = 0; off < paths_len; ) {
			size_t len = strlen(copy + off);
			if (len != 0)
				ndirs++;
			off += len + 1;
		}
		if (ndirs != 0) {
			dirs = calloc(ndirs, sizeof (char *));
			if (dirs == NULL) {
				free(copy);
				return (NULL);
			}
		}
		int n = 0;
		for (size_t off = 0; off < paths_len && n < ndirs; ) {
			char *d = copy + off;
			size_t len = strlen(d);
			off += len + 1;
			if (len == 0)
				continue;
			for (size_t i = 0; i < len; i++)
				if (d[i] == '\\')
					d[i] = '/';
			while (len > 1 && d[len - 1] == '/' &&
			    !(len == 3 && d[1] == ':'))
				d[--len] = 0;
			dirs[n++] = d;
		}
	}

	ia.path = dirs;
	ia.paths = ndirs;
	// No Scan, use blkid, unless we were told where to look.
	ia.scan = B_FALSE;
	ia.can_be_active = can_be_active;
	ia.guid = guid;

	libpc_handle_t lpch = {
	    .lpc_lib_handle = g_lzh,
	    .lpc_ops = &libzfs_config_ops,
	    .lpc_printerr = B_FALSE
	};

	nvlist_t *pools = zpool_search_import(&lpch, &ia);

	free(dirs);
	free(copy);
	return (pools);
}

typedef struct {
	const nvlist_t **ent;
	uint64_t *guid;
	size_t n;
	size_t cap;
} cand_list_t;

static void
add_candidates(cand_list_t *cl, nvlist_t *pools)
{
	if (pools == NULL)
		return;

	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;

		if (nvpair_value_nvlist(nvp, &cfg) != 0 || cfg == NULL)
			continue;

		char *pname = NULL;
		uint64_t pguid = 0, hostid = 0, pstate = 0;
		nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &pname);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &pguid);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_HOSTID, &hostid);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_STATE, &pstate);

		/* The same pool can turn up both on a disk and by path */
		boolean_t dup = B_FALSE;
		for (size_t i = 0; i < cl->n; i++)
			if (cl->guid[i] == pguid)
				dup = B_TRUE;
		if (dup)
			continue;

		if (cl->n == cl->cap) {
			size_t ncap = cl->cap ? cl->cap * 2 : 16;
			const nvlist_t **ne = realloc(cl->ent,
			    ncap * sizeof (*ne));
			if (ne == NULL)
				return;
			cl->ent = ne;
			uint64_t *ng = realloc(cl->guid,
			    ncap * sizeof (*ng));
			if (ng == NULL)
				return;
			cl->guid = ng;
			cl->cap = ncap;
		}

		nvlist_t *ent = fnvlist_alloc();
		if (pname)
			fnvlist_add_string(ent, "name", pname);
		add_guid_string(ent, pguid);
		if (hostid) {
			char hs[32];
			_snprintf_s(hs, sizeof (hs), _TRUNCATE, "%llu",
			    (unsigned long long) hostid);
			fnvlist_add_string(ent, "hostid", hs);
		}
		{
			char st[32];
			_snprintf_s(st, sizeof (st), _TRUNCATE, "%llu",
			    (unsigned long long) pstate);
			fnvlist_add_string(ent, "state", st);
		}
		cl->ent[cl->n] = ent;
		cl->guid[cl->n] = pguid;
		cl->n++;
	}
}

/* -------------------- OP_IMPORT_SCAN -------------------- */

char *
zed_import_scan_json(const char *paths, size_t paths_len, size_t *out_len)
{
	*out_len = 0;
	nvlist_t *root = fnvlist_alloc();
	cand_list_t cl = { 0 };

	dprintf("%s: \n", __func__);

	// show even if active elsewhere (like -f visibility)
	nvlist_t *pools = search_pools(B_TRUE, 0, NULL, 0);
	dprintf("%s: zpool_search_import: %p\n", __func__, pools);
	add_candidates(&cl, pools);

	nvlist_t *bypath = NULL;
	if (paths != NULL && paths_len != 0) {
		bypath = search_pools(B_TRUE, 0, paths, paths_len);
		dprintf("%s: by path: %p\n", __func__, bypath);
		add_candidates(&cl, bypath);
	}

	dprintf("import_scan: candidates=%zu\n", cl.n);
	fnvlist_add_nvlist_array(root, "candidates",
	    cl.n ? cl.ent : NULL, (uint_t)cl.n);

	for (size_t i = 0; i < cl.n; i++)
		fnvlist_free((nvlist_t *)cl.ent[i]);
	free(cl.ent);
	free(cl.guid);
	if (pools)
		nvlist_free(pools);
	if (bypath)
		nvlist_free(bypath);

	memfile_t mf;
	FILE *fp = memfile_open(&mf);
	if (!fp) {
		fnvlist_free(root);
		return (NULL);
	}
	nvlist_print_json(fp, root);
	fclose(fp);
	fnvlist_free(root);
	return (memfile_take(&mf, out_len));
}

/* -------------------- OP_IMPORT_ONE -------------------- */

static int
zed_import_one(libzfs_handle_t *g_lzh, nvlist_t *target_cfg,
    const char *pool_name, const char *new_name, nvlist_t *props,
    uint32_t imp_flags, uint32_t flags)
{
	// First import
	int rc = zpool_import_props(g_lzh, target_cfg,
	    new_name,
	    props, imp_flags);

	if (rc != 0)
		return (rc);

	char *name;

	if (new_name)
		name = new_name;
	else
		name = pool_name;

	return (rc);
}

char *
zed_import_one_json(uint32_t flags, uint64_t guid,
    const char *new_name_utf8, const char *altroot_utf8,
    const char *paths, size_t paths_len, size_t *out_len)
{
	*out_len = 0;
	nvlist_t *res = fnvlist_alloc();
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	boolean_t can_be_active = (flags & ZIMP_FORCE) ? B_TRUE : B_FALSE;
	nvlist_t *pools = search_pools(can_be_active, guid, NULL, 0);

	/* Not on the disks, so it was found by path */
	if (!pools && paths != NULL && paths_len != 0)
		pools = search_pools(can_be_active, guid, paths, paths_len);

	if (!pools) {
		fnvlist_add_string(res, "err", "no candidates");
		goto serialize;
	}

	nvlist_t *target_cfg = NULL;
	char *found_name = NULL;
	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;
		if (nvpair_value_nvlist(nvp, &cfg) != 0 || !cfg)
			continue;

		uint64_t pguid = 0;
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &pguid);
		if (pguid == guid) {
			target_cfg = cfg;
			nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME,
			    &found_name);
			break;
		}
	}

	if (!target_cfg) {
		nvlist_free(pools);
		fnvlist_add_string(res, "err", "not found");
		goto serialize;
	}

	nvlist_t *props = build_import_props(flags, altroot_utf8);

	int imp_flags = 0;

	int rc = zed_import_one(g_lzh, target_cfg, found_name,
	    (new_name_utf8 && *new_name_utf8) ? new_name_utf8 : NULL,
	    props, imp_flags, flags);

	if (rc == 0) {
		fnvlist_add_boolean_value(res, "ok", B_TRUE);
		if (new_name_utf8 && *new_name_utf8)
			fnvlist_add_string(res, "name", new_name_utf8);
		else if (found_name)
			fnvlist_add_string(res, "name", found_name);
		dprintf("zed_import_one_json: name %s: imported\n",
		    (new_name_utf8 && *new_name_utf8) ? new_name_utf8 :
		    found_name);
	} else {
		const char *e = libzfs_error_description(g_lzh);
		fnvlist_add_string(res, "err", e ? e : "import failed");
		if (found_name)
			fnvlist_add_string(res, "name", found_name);
	}

	nvlist_free(pools);
	nvlist_free(props);

serialize:
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

/* -------------------- OP_IMPORT_ALL -------------------- */

char *
zed_import_all_json(uint32_t flags, const char *altroot_utf8,
    size_t *out_len)
{
	*out_len = 0;
	nvlist_t *res = fnvlist_alloc();
	fnvlist_add_string_array(res, "imported", NULL, 0);
	fnvlist_add_nvlist_array(res, "errors", NULL, 0);

	importargs_t ia = { 0 };
	ia.scan = B_FALSE;
	ia.can_be_active = (flags & ZIMP_FORCE) ? B_TRUE : B_FALSE;

	dprintf("zed_import_all_json: scanning for pools\n");
	libpc_handle_t lpch = {
	    .lpc_lib_handle = g_lzh,
	    .lpc_ops = &libzfs_config_ops,
	    .lpc_printerr = B_FALSE
	};

	nvlist_t *pools = NULL;
	pools = zpool_search_import(&lpch, &ia);

	if (!pools)
		goto serialize;

	dprintf("zed_import_all_json: looping\n");
	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;
		if (nvpair_value_nvlist(nvp, &cfg) != 0 || !cfg)
			continue;

		char *pname = NULL;
		nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &pname);
		dprintf("zed_import_all_json: name %s: flags %x\n",
		    pname, flags);

		nvlist_t *props = build_import_props(flags, altroot_utf8);
		int imp_flags = 0;

		int rc = zed_import_one(g_lzh, cfg, pname,
		    NULL, props, imp_flags, flags);

		if (rc == 0) {
			if (pname) {
				const char *one[1] = { pname };
				fnvlist_add_string_array(res, "imported",
				    one, 1);
			}
		} else {
			nvlist_t *e = fnvlist_alloc();
			if (pname) fnvlist_add_string(e, "name", pname);
			const char *d = libzfs_error_description(g_lzh);
			fnvlist_add_string(e, "err", d ? d : "import failed");
			const nvlist_t *onee[1] = { e };
			fnvlist_add_nvlist_array(res, "errors", onee, 1);
			fnvlist_free(e);
		}
		nvlist_free(props);
	}
	nvlist_free(pools);

serialize:
	dprintf("zed_import_all_json: serializing\n");

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

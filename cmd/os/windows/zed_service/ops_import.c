#define	_CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <libzfs.h>
#include <libzutil.h>
#include <sys/nvpair.h>

#include "ops_import.h"
#include "ops_status.h"
#include "memfile.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

static libzfs_handle_t *g_lzh = NULL;

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

/* -------------------- OP_IMPORT_SCAN -------------------- */

char *
zed_import_scan_json(size_t *out_len)
{
	*out_len = 0;
	nvlist_t *root = fnvlist_alloc();
	fnvlist_add_nvlist_array(root, "candidates", NULL, 0);

#ifdef ENABLE_FAKE_POOLS
	{
		nvlist_t *c = fnvlist_alloc();
		fnvlist_add_string(c, "name", "FAKE_IMPORT");
		add_guid_string(c, 0xF00DFADEFACEULL);
		fnvlist_add_string(c, "state", "2");
		const nvlist_t *one[1] = { c };
		fnvlist_add_nvlist_array(root, "candidates", one, 1);
		fnvlist_free(c);
	}
#endif

	dprintf("%s: \n", __func__);
	g_lzh = libzfs_init();

	if (!g_lzh)
		goto serialize;

	importargs_t ia = { 0 };
	// No Scan, use blkid.
	ia.scan = B_FALSE;
	// show even if active elsewhere (like -f visibility)
	ia.can_be_active = B_TRUE;

	libpc_handle_t lpch = {
	    .lpc_lib_handle = g_lzh,
	    .lpc_ops = &libzfs_config_ops,
	    .lpc_printerr = B_FALSE
	};

	nvlist_t *pools = NULL;
	pools = zpool_search_import(&lpch, &ia);

	dprintf("%s: zpool_search_import: %p\n", __func__, pools);
	if (!pools)
		goto serialize;


	size_t count = 0;
	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		count++;
	}
	dprintf("import_scan: candidates=%zu\n", count);

	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;

		dprintf("%s: checking we have cfg\n", __func__);

		if (nvpair_value_nvlist(nvp, &cfg) != 0 ||
		    cfg == NULL)
			continue;

		char *pname = NULL;
		uint64_t pguid = 0, hostid = 0, pstate = 0;
		nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &pname);
		dprintf("%s: name %s\n", __func__, pname);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &pguid);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_HOSTID, &hostid);
		nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_STATE, &pstate);

		nvlist_t *ent = fnvlist_alloc();
		if (pname) fnvlist_add_string(ent, "name", pname);
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
		const nvlist_t *one[1] = { ent };
		fnvlist_add_nvlist_array(root, "candidates", one, 1);
		fnvlist_free(ent);
	}

	nvlist_free(pools);

serialize:
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

char *
zed_import_one_json(uint32_t flags, uint64_t guid,
    const char *new_name_utf8, const char *altroot_utf8,
    size_t *out_len)
{
	*out_len = 0;
	nvlist_t *res = fnvlist_alloc();
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

#ifdef ENABLE_FAKE_POOLS
	if (guid == 0xF00DFADEFACEULL) {
		fnvlist_add_boolean_value(res, "ok", B_TRUE);
		fnvlist_add_string(res, "name",
		    (new_name_utf8 && *new_name_utf8) ? new_name_utf8 :
		    "FAKE_IMPORT");
		goto serialize;
	}
#endif
	g_lzh = libzfs_init();
	if (!g_lzh) {
		fnvlist_add_string(res, "err", "libzfs_init failed");
		goto serialize;
	}

	importargs_t ia = { 0 };
	ia.scan = B_FALSE;
	ia.can_be_active = (flags & ZIMP_FORCE) ? B_TRUE : B_FALSE;
	ia.guid = guid;

	libpc_handle_t lpch = {
	    .lpc_lib_handle = g_lzh,
	    .lpc_ops = &libzfs_config_ops,
	    .lpc_printerr = B_FALSE
	};

	nvlist_t *pools = NULL;
	pools = zpool_search_import(&lpch, &ia);

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
	int rc = zpool_import_props(g_lzh, target_cfg,
	    (new_name_utf8 && *new_name_utf8) ? new_name_utf8 : NULL,
	    props, imp_flags);

	if (rc == 0) {
		fnvlist_add_boolean_value(res, "ok", B_TRUE);
		if (new_name_utf8 && *new_name_utf8)
			fnvlist_add_string(res, "name", new_name_utf8);
		else if (found_name)
			fnvlist_add_string(res, "name", found_name);
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

#ifdef ENABLE_FAKE_POOLSXXX
	{
		const char *one[1] = { "FAKE_IMPORT" };
		fnvlist_add_string_array(res, "imported", one, 1);
		goto serialize;
	}
#endif

	g_lzh = libzfs_init();
	if (!g_lzh) goto serialize;

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

	if (!pools) goto serialize;

	dprintf("zed_import_all_json: looping\n");
	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL);
	    nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;
		if (nvpair_value_nvlist(nvp, &cfg) != 0 || !cfg)
			continue;

		char *pname = NULL;
		nvlist_lookup_string(cfg, ZPOOL_CONFIG_POOL_NAME, &pname);
		dprintf("zed_import_all_json: name %s\n", pname);

		nvlist_t *props = build_import_props(flags, altroot_utf8);
		int imp_flags = 0;

		int rc = zpool_import_props(g_lzh, cfg, NULL, props, imp_flags);
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

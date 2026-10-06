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
#include <sddl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <winioctl.h>

#include <libzfs.h>
#include <libzutil.h>
#include <sys/nvpair.h>
#include "zfeature_common.h"

#include "ops_common.h"
#include "ops_pool.h"
#include "memfile.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

extern libzfs_handle_t *g_lzh;

/* -------------------- disk enumeration -------------------- */

// Highest \\.\PhysicalDriveN index probed. SetupDiGetClassDevs() with
// GUID_DEVINTERFACE_DISK was tried first, but on this driver's storage
// stack that GUID class also turns up volume-manager and the openzfs_bus
// virtual device's own interfaces (not just real PhysicalDriveN PDOs),
// with colliding/bogus IOCTL_STORAGE_GET_DEVICE_NUMBER results - so a
// direct bounded probe is both simpler and more reliable here.
#define	ZED_MAX_PHYSICAL_DRIVE 63

// One entry per whole disk, immediately followed by one entry per usable
// partition on that disk (part_number > 0). Partition entries carry the
// "HarddiskNPartitionM" name that zpool_create() accepts as-is.
typedef struct {
    char path[64]; // "PhysicalDriveN" or "HarddiskNPartitionM"
    int  disk_number;
    int  part_number; // 0 == whole disk
    uint64_t offset; // partition start in bytes (part_number > 0 only)
    uint64_t size;
    char model[128]; // partition entries: GPT partition name, if any
    BOOL is_boot;
    BOOL has_pool;
} disk_info_t;

// Partition types never offered as pool members: they have no business
// holding a pool, and listing them only invites mistakes.
static const GUID zed_guid_msr = {
	0xE3C9E316, 0x0B5C, 0x4DB8,
	{ 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE }
};
static const GUID zed_guid_esp = {
	0xC12A7328, 0xF81F, 0x11D2,
	{ 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};

static void
query_disk_model(HANDLE h, char *model, size_t modelsz)
{
	BYTE buf[1024];
	STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty,
	    PropertyStandardQuery };
	DWORD ret = 0;

	model[0] = '\0';
	if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof (q),
	    buf, sizeof (buf), &ret, NULL))
		return;

	STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;
	if (desc->VendorIdOffset == 0 && desc->ProductIdOffset == 0)
		return;

	char tmp[160] = { 0 };
	size_t pos = 0;
	if (desc->VendorIdOffset && desc->VendorIdOffset < sizeof (buf)) {
		const char *v = (const char *)buf + desc->VendorIdOffset;
		size_t vl = strnlen(v, sizeof (tmp));
		if (vl && pos + vl < sizeof (tmp)) {
			memcpy(tmp + pos, v, vl);
			pos += vl;
			tmp[pos++] = ' ';
		}
	}
	if (desc->ProductIdOffset && desc->ProductIdOffset < sizeof (buf)) {
		const char *p = (const char *)buf + desc->ProductIdOffset;
		size_t pl = strnlen(p, sizeof (tmp) - pos);
		if (pl && pos + pl < sizeof (tmp)) {
			memcpy(tmp + pos, p, pl);
			pos += pl;
		}
	}
	tmp[pos] = '\0';

	// Trim trailing whitespace left over from fixed-width SCSI fields.
	while (pos > 0 && isspace((unsigned char)tmp[pos - 1]))
		tmp[--pos] = '\0';

	_snprintf_s(model, modelsz, _TRUNCATE, "%s", tmp);
}

// Makes room for at least one more entry. Returns FALSE (arr untouched) on
// allocation failure.
static BOOL
reserve_entry(disk_info_t **arr, int *cap, int count)
{
	if (count < *cap)
		return (TRUE);
	disk_info_t *na = (disk_info_t *)HeapReAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, *arr, (*cap * 2) * sizeof (disk_info_t));
	if (!na)
		return (FALSE);
	*arr = na;
	*cap *= 2;
	return (TRUE);
}

// Reads the disk's partition table and appends one entry per partition that
// could sensibly hold a pool. Failure to read the layout (e.g. a RAW disk)
// just means no partition entries.
static void
append_partitions(HANDLE h, int disk_number, disk_info_t **arr, int *cap,
    int *count)
{
	DWORD bufsz = sizeof (DRIVE_LAYOUT_INFORMATION_EX) +
	    128 * sizeof (PARTITION_INFORMATION_EX);
	DRIVE_LAYOUT_INFORMATION_EX *dl = NULL;
	DWORD ret = 0;
	BOOL ok = FALSE;

	// Grow until it fits; 128 GPT entries is the common case but MBR/EBR
	// chains and larger GPT tables can exceed that.
	while (bufsz <= 1024 * 1024) {
		dl = (DRIVE_LAYOUT_INFORMATION_EX *)HeapAlloc(
		    GetProcessHeap(), 0, bufsz);
		if (!dl)
			return;
		if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
		    dl, bufsz, &ret, NULL)) {
			ok = TRUE;
			break;
		}
		HeapFree(GetProcessHeap(), 0, dl);
		dl = NULL;
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			return;
		bufsz *= 2;
	}
	if (!ok)
		return;

	for (DWORD i = 0; i < dl->PartitionCount; i++) {
		const PARTITION_INFORMATION_EX *p = &dl->PartitionEntry[i];

		if (p->PartitionNumber == 0 || p->PartitionLength.QuadPart <= 0)
			continue;

		char name[128] = "";
		if (p->PartitionStyle == PARTITION_STYLE_GPT) {
			if (IsEqualGUID(&p->Gpt.PartitionType, &zed_guid_msr) ||
			    IsEqualGUID(&p->Gpt.PartitionType, &zed_guid_esp))
				continue;
			WideCharToMultiByte(CP_UTF8, 0, p->Gpt.Name, -1, name,
			    sizeof (name), NULL, NULL);
		} else if (p->PartitionStyle == PARTITION_STYLE_MBR) {
			// Extended partition containers hold other partitions,
			// not data.
			if (p->Mbr.PartitionType == PARTITION_EXTENDED ||
			    p->Mbr.PartitionType == 0x0F ||
			    p->Mbr.PartitionType == 0x85 ||
			    p->Mbr.PartitionType == PARTITION_ENTRY_UNUSED)
				continue;
		} else {
			continue;
		}

		if (!reserve_entry(arr, cap, *count))
			break;

		disk_info_t *d = &(*arr)[*count];
		ZeroMemory(d, sizeof (*d));
		d->disk_number = disk_number;
		d->part_number = (int)p->PartitionNumber;
		d->offset = (uint64_t)p->StartingOffset.QuadPart;
		d->size = (uint64_t)p->PartitionLength.QuadPart;
		_snprintf_s(d->path, sizeof (d->path), _TRUNCATE,
		    "Harddisk%dPartition%d", disk_number, d->part_number);
		_snprintf_s(d->model, sizeof (d->model), _TRUNCATE, "%s", name);
		(*count)++;
	}

	HeapFree(GetProcessHeap(), 0, dl);
}

static int
enumerate_physical_disks(disk_info_t **out, int *out_count)
{
	*out = NULL;
	*out_count = 0;

	int cap = 16, count = 0;
	disk_info_t *arr = (disk_info_t *)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, cap * sizeof (disk_info_t));
	if (!arr)
		return (-1);

	for (int n = 0; n <= ZED_MAX_PHYSICAL_DRIVE; n++) {
		wchar_t path[32];
		_snwprintf_s(path, _countof(path), _TRUNCATE,
		    L"\\\\.\\PhysicalDrive%d", n);

		HANDLE h = CreateFileW(path, GENERIC_READ,
		    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		    0, NULL);
		if (h == INVALID_HANDLE_VALUE)
			continue;

		STORAGE_DEVICE_NUMBER sdn = { 0 };
		DWORD ret = 0;
		if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL,
		    0, &sdn, sizeof (sdn), &ret, NULL) ||
		    sdn.DeviceType != FILE_DEVICE_DISK) {
			CloseHandle(h);
			continue;
		}

		if (!reserve_entry(&arr, &cap, count)) {
			CloseHandle(h);
			break;
		}

		disk_info_t *d = &arr[count];
		ZeroMemory(d, sizeof (*d));
		d->disk_number = n;
		// Bare form, no "\\.\" prefix: zpool_create() rejects the
		// "\\.\PhysicalDriveN" form with EINVAL on this driver
		// (confirmed empirically - zpool.exe hits the same error
		// against "\\.\PhysicalDriveN" but succeeds against the
		// bare form). CreateFileW() above still needs the "\\.\"
		// form to open the device at all; only the reported/vdev
		// path drops it.
		_snprintf_s(d->path, sizeof (d->path), _TRUNCATE,
		    "PhysicalDrive%d", n);

		DISK_GEOMETRY_EX geo;
		if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL,
		    0, &geo, sizeof (geo), &ret, NULL))
			d->size = (uint64_t)geo.DiskSize.QuadPart;

		query_disk_model(h, d->model, sizeof (d->model));

		count++;
		// Partitions follow their disk. Note arr may move here, so d is
		// not used after this call.
		append_partitions(h, n, &arr, &cap, &count);

		CloseHandle(h);
	}

	*out = arr;
	*out_count = count;
	return (0);
}

static BOOL
get_boot_disk_number(int *out_disknum, uint64_t *out_offset)
{
	wchar_t sysdir[MAX_PATH];
	if (!GetSystemDirectoryW(sysdir, MAX_PATH) || sysdir[1] != L':')
		return (FALSE);

	wchar_t volpath[8];
	_snwprintf_s(volpath, _countof(volpath), _TRUNCATE, L"\\\\.\\%c:",
	    sysdir[0]);

	HANDLE h = CreateFileW(volpath, 0,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return (FALSE);

	BYTE buf[sizeof (VOLUME_DISK_EXTENTS) +
	    4 * sizeof (DISK_EXTENT)];
	DWORD ret = 0;
	BOOL ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
	    NULL, 0, buf, sizeof (buf), &ret, NULL);
	CloseHandle(h);
	if (!ok)
		return (FALSE);

	VOLUME_DISK_EXTENTS *vde = (VOLUME_DISK_EXTENTS *)buf;
	if (vde->NumberOfDiskExtents < 1)
		return (FALSE);

	*out_disknum = (int)vde->Extents[0].DiskNumber;
	*out_offset = (uint64_t)vde->Extents[0].StartingOffset.QuadPart;
	return (TRUE);
}

// Finds "needle" (lower case) in path, case-insensitively, and returns a
// pointer to the character after it, or NULL.
static const char *
find_after(const char *path, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t plen = strlen(path);

	for (size_t i = 0; i + nlen <= plen; i++) {
		if (_strnicmp(path + i, needle, nlen) == 0)
			return (path + i + nlen);
	}
	return (NULL);
}

// Works out which disk, and which partition of it, a vdev path refers to.
// Recognised forms:
// - "\\.\PhysicalDriveN" or "PhysicalDriveN": whole disk
// - "#offset#size#PHYSICALDRIVEn": partition by byte offset, as
//   zfs_append_partition() produces once a disk has been labeled (see
//   lib/libzutil/os/windows/zutil_device_path_os.c)
// - "\\?\HarddiskNPartitionM": partition by number
// Returns the disk number, or -1 if the path is none of these. *part is the
// partition number (0 if unknown/whole disk) and *offset the byte offset
// (0 if unknown).
static int
parse_vdev_path(const char *path, int *part, uint64_t *offset)
{
	*part = 0;
	*offset = 0;
	if (!path)
		return (-1);

	const char *p = find_after(path, "harddisk");
	if (p && isdigit((unsigned char)*p)) {
		int dn = atoi(p);
		while (isdigit((unsigned char)*p))
			p++;
		if (_strnicmp(p, "partition", 9) == 0 &&
		    isdigit((unsigned char)p[9]))
			*part = atoi(p + 9);
		return (dn);
	}

	p = find_after(path, "physicaldrive");
	if (p && isdigit((unsigned char)*p)) {
		if (path[0] == '#')
			*offset = _strtoui64(path + 1, NULL, 10);
		return (atoi(p));
	}
	return (-1);
}

static void
mark_disk_path(nvlist_t *vdev, disk_info_t *disks, int ndisks)
{
	const char *path = NULL;
	if (nvlist_lookup_string(vdev, ZPOOL_CONFIG_PATH, &path) == 0) {
		int part = 0;
		uint64_t off = 0;
		int dn = parse_vdev_path(path, &part, &off);
		if (dn >= 0) {
			for (int i = 0; i < ndisks; i++) {
				if (disks[i].disk_number != dn)
					continue;
				if (disks[i].part_number == 0) {
					// A pool anywhere on the disk, in a
					// partition or not: using the whole
					// disk would destroy it.
					disks[i].has_pool = TRUE;
				} else if ((part && disks[i].part_number ==
				    part) || (off && disks[i].offset == off)) {
					disks[i].has_pool = TRUE;
				}
			}
		}
	}

	nvlist_t **children = NULL;
	uint_t nchildren = 0;
	if (nvlist_lookup_nvlist_array(vdev, ZPOOL_CONFIG_CHILDREN,
	    &children, &nchildren) == 0) {
		for (uint_t i = 0; i < nchildren; i++)
			mark_disk_path(children[i], disks, ndisks);
	}
}

static void
mark_disks_with_pools(disk_info_t *disks, int ndisks)
{
	importargs_t ia = { 0 };
	ia.scan = B_FALSE;
	ia.can_be_active = B_TRUE;

	libpc_handle_t lpch = {
	    .lpc_lib_handle = g_lzh,
	    .lpc_ops = &libzfs_config_ops,
	    .lpc_printerr = B_FALSE
	};

	nvlist_t *pools = zpool_search_import(&lpch, &ia);
	if (!pools)
		return;

	for (nvpair_t *nvp = nvlist_next_nvpair(pools, NULL); nvp != NULL;
	    nvp = nvlist_next_nvpair(pools, nvp)) {
		nvlist_t *cfg = NULL;
		if (nvpair_value_nvlist(nvp, &cfg) != 0 || !cfg)
			continue;

		nvlist_t *tree = NULL;
		if (nvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE,
		    &tree) == 0)
			mark_disk_path(tree, disks, ndisks);
	}

	nvlist_free(pools);
}

char *
zed_list_disks_json(size_t *out_len)
{
	*out_len = 0;
	nvlist_t *root = fnvlist_alloc();
	fnvlist_add_nvlist_array(root, "disks", NULL, 0);

	disk_info_t *disks = NULL;
	int ndisks = 0;
	if (enumerate_physical_disks(&disks, &ndisks) != 0 || !disks)
		goto serialize;

	// The boot disk is flagged as a whole; only the partition actually
	// holding the system volume is flagged among its partitions, so a
	// data partition on the boot disk stays selectable.
	int boot_disk = -1;
	uint64_t boot_off = 0;
	if (get_boot_disk_number(&boot_disk, &boot_off)) {
		for (int i = 0; i < ndisks; i++) {
			if (disks[i].disk_number != boot_disk)
				continue;
			if (disks[i].part_number == 0 ||
			    disks[i].offset == boot_off)
				disks[i].is_boot = TRUE;
		}
	}

	mark_disks_with_pools(disks, ndisks);

	// Build every entry first and add the array to "disks" exactly once:
	// nvlist_add_nvlist_array() on an NV_UNIQUE_NAME list *replaces* the
	// existing value for a key rather than appending, so calling it once
	// per disk (as ops_import.c's candidate/pool loops do) would silently
	// keep only the last entry.
	nvlist_t **ents = (nvlist_t **)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, ndisks * sizeof (nvlist_t *));
	if (!ents) {
		HeapFree(GetProcessHeap(), 0, disks);
		goto serialize;
	}
	for (int i = 0; i < ndisks; i++) {
		nvlist_t *ent = fnvlist_alloc();
		fnvlist_add_string(ent, "path", disks[i].path);
		char sz[32];
		_snprintf_s(sz, sizeof (sz), _TRUNCATE, "%llu",
		    (unsigned long long)disks[i].size);
		fnvlist_add_string(ent, "size", sz);
		fnvlist_add_string(ent, "model", disks[i].model);
		// "part" is 0 for a whole disk. Partition entries directly
		// follow their disk, which is how the UI nests them.
		_snprintf_s(sz, sizeof (sz), _TRUNCATE, "%d",
		    disks[i].disk_number);
		fnvlist_add_string(ent, "disk", sz);
		_snprintf_s(sz, sizeof (sz), _TRUNCATE, "%d",
		    disks[i].part_number);
		fnvlist_add_string(ent, "part", sz);
		fnvlist_add_boolean_value(ent, "is_boot",
		    disks[i].is_boot ? B_TRUE : B_FALSE);
		fnvlist_add_boolean_value(ent, "has_pool",
		    disks[i].has_pool ? B_TRUE : B_FALSE);
		ents[i] = ent;
	}
	fnvlist_add_nvlist_array(root, "disks", (const nvlist_t **)ents,
	    ndisks);
	for (int i = 0; i < ndisks; i++)
		fnvlist_free(ents[i]);
	HeapFree(GetProcessHeap(), 0, ents);

	HeapFree(GetProcessHeap(), 0, disks);

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

/* -------------------- create pool -------------------- */

// Pool-only properties (zpool_create()'s "props" / -o), matched
// case-insensitively. Everything else in the free-text blob is treated as
// a dataset property (-O) and left to zfs_valid_proplist()/zpool_create()
// to accept or reject.
static const char *pool_only_props[] = {
	"ashift", "autoexpand", "autoreplace", "cachefile", "delegation",
	"failmode", "listsnaps", "autotrim", "compatibility", NULL
};

static int
is_pool_only_prop(const char *key)
{
	if (_strnicmp(key, "feature@", 8) == 0)
		return (1);
	for (int i = 0; pool_only_props[i]; i++) {
		if (_stricmp(key, pool_only_props[i]) == 0)
			return (1);
	}
	return (0);
}

// zpool_create() does not enable any pool features on its own - the CLI's
// zpool_do_create() (cmd/zpool/zpool_main.c) is what enables features by
// adding "feature@<name>"="enabled" into the pool props nvlist before
// calling zpool_create(), gated by the "compatibility" property
// (zpool_load_compat(), lib/libzfs/libzfs_pool.c - unset/""/"off" requests
// every feature, "legacy" requests none, a compatibility.d filename list
// requests the intersection of those files). Without mirroring that here,
// a pool created through the tray ends up with zero features enabled, and
// setting any feature-gated root-fs property (e.g. compression=lz4, which
// needs SPA_FEATURE_LZ4_COMPRESS) fails with ENOTSUP from
// zfs_set_prop_nvlist()/zfs_check_settable() right after zpool_create()
// itself reports success - the kernel then unwinds the pool via
// spa_destroy() (module/zfs/zfs_ioctl.c zfs_ioc_pool_create()).
static void
enable_compat_features(nvlist_t *props)
{
	const char *compat = NULL;
	(void) nvlist_lookup_string(props, "compatibility", &compat);

	boolean_t requested[SPA_FEATURES];
	(void) zpool_load_compat(compat, requested, NULL, 0);

	for (spa_feature_t i = 0; i < SPA_FEATURES; i++) {
		zfeature_info_t *feat = &spa_feature_table[i];
		char propname[MAXPATHLEN];
		const char *existing;

		if (!feat->fi_zfs_mod_supported || !requested[i])
			continue;

		_snprintf_s(propname, sizeof (propname), _TRUNCATE,
		    "feature@%s", feat->fi_uname);

		// A deliberate "feature@name=disabled" line in the free-text
		// blob removes it instead of being overridden here; any other
		// explicit value (e.g. "enabled") is left untouched.
		if (nvlist_lookup_string(props, propname, &existing) == 0) {
			if (strcmp(existing, ZFS_FEATURE_DISABLED) == 0)
				(void) nvlist_remove_all(props, propname);
			continue;
		}

		fnvlist_add_string(props, propname, ZFS_FEATURE_ENABLED);
	}
}

// Parses "key=value\n"-delimited lines (also tolerates \r\n) into either
// props (pool, -o) or fsprops (dataset, -O) based on is_pool_only_prop().
// Malformed lines (no '=') are silently skipped; zpool_create() itself is
// responsible for rejecting bad property *values*.
static void
parse_props_blob(const char *blob, nvlist_t *props, nvlist_t *fsprops)
{
	if (!blob || !blob[0])
		return;

	char *copy = _strdup(blob);
	if (!copy)
		return;

	char *saveptr = NULL;
	for (char *line = strtok_s(copy, "\r\n", &saveptr); line;
	    line = strtok_s(NULL, "\r\n", &saveptr)) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line)
			continue;

		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = line;
		char *val = eq + 1;

		// trim trailing whitespace off key
		size_t klen = strlen(key);
		while (klen > 0 &&
		    (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
			key[--klen] = '\0';
		if (!klen)
			continue;

		fnvlist_add_string(is_pool_only_prop(key) ? props : fsprops,
		    key, val);
	}

	free(copy);
}

// Writes passphrase_utf8 + '\n' to a fresh, admin-only-ACL temp file and
// returns its path, for use as a "file://" keylocation. zpool_create()
// reads it synchronously via zfs_crypto_create() -> get_key_material(); the
// caller deletes the file immediately afterward. This avoids needing an
// interactive console (keylocation=prompt has no meaning in a service).
static BOOL
write_temp_keyfile(const char *passphrase_utf8, wchar_t *out_path,
    DWORD out_path_cch)
{
	wchar_t tmpdir[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, tmpdir))
		return (FALSE);

	wchar_t tmpfile[MAX_PATH];
	if (!GetTempFileNameW(tmpdir, L"zpk", 0, tmpfile))
		return (FALSE);

	SECURITY_ATTRIBUTES sa = { sizeof (sa), NULL, FALSE };
	PSECURITY_DESCRIPTOR sd = NULL;
	static const wchar_t *sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
	(void) ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,
	    SDDL_REVISION_1, &sd, NULL);
	sa.lpSecurityDescriptor = sd;

	// GetTempFileNameW() already created the (empty, default-ACL) file;
	// recreate it under our own tight ACL before writing anything to it.
	DeleteFileW(tmpfile);
	HANDLE h = CreateFileW(tmpfile, GENERIC_WRITE, 0, &sa, CREATE_NEW,
	    FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (sd)
		LocalFree(sd);
	if (h == INVALID_HANDLE_VALUE)
		return (FALSE);

	DWORD wr = 0;
	BOOL ok = WriteFile(h, passphrase_utf8, (DWORD)strlen(passphrase_utf8),
	    &wr, NULL);
	if (ok)
		ok = WriteFile(h, "\n", 1, &wr, NULL);
	CloseHandle(h);

	if (!ok) {
		DeleteFileW(tmpfile);
		return (FALSE);
	}

	wcsncpy_s(out_path, out_path_cch, tmpfile, _TRUNCATE);
	return (TRUE);
}

// Labels a whole disk with an EFI/GPT ZFS-tagged partition and returns the
// resulting "#offset#size#\\?\PhysicalDriveN" encoded path zpool_create()
// actually needs for ZPOOL_CONFIG_PATH. Mirrors what the CLI's make_disks()
// (cmd/zpool/zpool_vdev.c) does for every whole-disk leaf before calling
// zpool_create() - a raw "\\.\PhysicalDriveN"/"PhysicalDriveN" path alone
// is rejected by this driver's ZFS_IOC_POOL_CREATE with EINVAL; only the
// encoded path (produced *after* labeling) is accepted. name_utf8 is the
// bare device name (e.g. "PhysicalDrive2", no DISK_ROOT prefix).
static BOOL
label_whole_disk(const char *name_utf8, char *encoded_out, size_t encoded_cch)
{
	char **lines = NULL;
	int lines_cnt = 0;

	// zpool_prepare_disk() (lib/libzfs/libzfs_util.c) reads
	// ZPOOL_CONFIG_PATH out of vdev_nv to populate the optional
	// zfs_prepare_disk script's VDEV_PATH/VDEV_UPATH env vars - passing
	// NULL here (as opposed to the CLI's make_disks(), which always hands
	// in the real leaf nvlist) leaves that path NULL for the whole
	// prepare-script hook.
	char path_buf[MAXPATHLEN];
	_snprintf_s(path_buf, sizeof (path_buf), _TRUNCATE, "%s%s", DISK_ROOT,
	    name_utf8);
	nvlist_t *vdev_nv = fnvlist_alloc();
	fnvlist_add_string(vdev_nv, ZPOOL_CONFIG_PATH, path_buf);

	int rc = zpool_prepare_and_label_disk(g_lzh, NULL, name_utf8, vdev_nv,
	    "create", &lines, &lines_cnt);
	fnvlist_free(vdev_nv);

	if (rc != 0) {
		if (lines)
			libzfs_free_str_array(lines, lines_cnt);
		return (FALSE);
	}
	if (lines)
		libzfs_free_str_array(lines, lines_cnt);

	_snprintf_s(encoded_out, encoded_cch, _TRUNCATE, "%s%s", DISK_ROOT,
	    name_utf8);

	(void) zpool_label_disk_wait(encoded_out, DISK_LABEL_WAIT);

	// Rewrites encoded_out in place to "#offset#size#<path>" once it can
	// read back the EFI label just written.
	(void) zfs_append_partition(encoded_out, (int)encoded_cch);

	return (encoded_out[0] == '#');
}

char *
zed_create_pool_json(uint32_t flags, uint32_t topology, uint32_t ndisks,
    const char * const *disk_paths_utf8, const char *poolname_utf8,
    const char *props_blob_utf8, const char *passphrase_utf8,
    size_t *out_len)
{
	(void) flags;
	*out_len = 0;
	nvlist_t *res = fnvlist_alloc();
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	if (!g_lzh || !poolname_utf8 || !poolname_utf8[0] || ndisks == 0 ||
	    !disk_paths_utf8) {
		fnvlist_add_string(res, "err", "invalid arguments");
		goto serialize;
	}

	// Relabeling a whole disk destroys its partitions, so a disk and one of
	// its own partitions can't both be members. Check before touching
	// anything.
	for (uint32_t i = 0; i < ndisks; i++) {
		int pi = 0;
		uint64_t oi = 0;
		int di = parse_vdev_path(disk_paths_utf8[i], &pi, &oi);
		for (uint32_t j = 0; j < ndisks; j++) {
			int pj = 0;
			uint64_t oj = 0;
			int dj = parse_vdev_path(disk_paths_utf8[j], &pj, &oj);
			if (di >= 0 && di == dj && pi == 0 && pj > 0) {
				fnvlist_add_string(res, "err",
				    "a disk and one of its own partitions "
				    "cannot both be selected");
				goto serialize;
			}
		}
	}

	nvlist_t **leaves = (nvlist_t **)HeapAlloc(GetProcessHeap(),
	    HEAP_ZERO_MEMORY, ndisks * sizeof (nvlist_t *));
	if (!leaves) {
		fnvlist_add_string(res, "err", "out of memory");
		goto serialize;
	}
	for (uint32_t i = 0; i < ndisks; i++) {
		char encoded[MAXPATHLEN];
		int pdisk = 0, ppart = 0, pend = 0;

		// An existing partition ("HarddiskNPartitionM") is used as-is,
		// exactly like "zpool create pool \\?\HarddiskNPartitionM":
		// no relabeling, and not a whole-disk vdev. The strict match
		// keeps anything else from being passed through unlabeled.
		if (sscanf_s(disk_paths_utf8[i], "Harddisk%dPartition%d%n",
		    &pdisk, &ppart, &pend) == 2 && pend > 0 &&
		    disk_paths_utf8[i][pend] == '\0' && pdisk >= 0 &&
		    ppart > 0) {
			_snprintf_s(encoded, sizeof (encoded), _TRUNCATE,
			    "\\\\?\\Harddisk%dPartition%d", pdisk, ppart);
			leaves[i] = fnvlist_alloc();
			fnvlist_add_string(leaves[i], ZPOOL_CONFIG_PATH,
			    encoded);
			fnvlist_add_string(leaves[i], ZPOOL_CONFIG_TYPE,
			    VDEV_TYPE_DISK);
			fnvlist_add_uint64(leaves[i], ZPOOL_CONFIG_WHOLE_DISK,
			    0);
			continue;
		}

		if (!label_whole_disk(disk_paths_utf8[i], encoded,
		    sizeof (encoded))) {
			const char *desc = libzfs_error_description(g_lzh);
			fnvlist_add_string(res, "err", desc ? desc :
			    "failed to label disk");
			for (uint32_t j = 0; j < i; j++)
				fnvlist_free(leaves[j]);
			HeapFree(GetProcessHeap(), 0, leaves);
			goto serialize;
		}

		leaves[i] = fnvlist_alloc();
		fnvlist_add_string(leaves[i], ZPOOL_CONFIG_PATH, encoded);
		fnvlist_add_string(leaves[i], ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_DISK);
		fnvlist_add_uint64(leaves[i], ZPOOL_CONFIG_WHOLE_DISK, 1);
	}

	nvlist_t *nvroot = fnvlist_alloc();
	fnvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);

	if (topology == 0) {
		// stripe: every disk is its own top-level vdev. Top-level
		// vdevs need ZPOOL_CONFIG_IS_LOG set (even to 0) - the CLI's
		// construct_spec() always adds it to a lone device that
		// becomes a top-level vdev by itself (cmd/zpool/zpool_vdev.c
		// ~line 1949); omitting it here is what caused zpool_create()
		// to reject an otherwise-correct config with EINVAL.
		for (uint32_t i = 0; i < ndisks; i++)
			fnvlist_add_uint64(leaves[i], ZPOOL_CONFIG_IS_LOG, 0);
		fnvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)leaves, ndisks);
	} else {
		const char *gtype = (topology == 1) ? VDEV_TYPE_MIRROR :
		    VDEV_TYPE_RAIDZ;
		nvlist_t *grp = fnvlist_alloc();
		fnvlist_add_string(grp, ZPOOL_CONFIG_TYPE, gtype);
		fnvlist_add_uint64(grp, ZPOOL_CONFIG_IS_LOG, 0);
		if (topology >= 2 && topology <= 4) {
			fnvlist_add_uint64(grp, ZPOOL_CONFIG_NPARITY,
			    topology - 1);
		}
		fnvlist_add_nvlist_array(grp, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)leaves, ndisks);
		const nvlist_t *grparr[1] = { grp };
		fnvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
		    grparr, 1);
		fnvlist_free(grp);
	}
	for (uint32_t i = 0; i < ndisks; i++)
		fnvlist_free(leaves[i]);
	HeapFree(GetProcessHeap(), 0, leaves);

	nvlist_t *props = fnvlist_alloc(); // pool props (-o)
	nvlist_t *fsprops = fnvlist_alloc(); // dataset (root fs) props (-O)

	// Recommended baked-in default; a deliberate "atime=..." line in the
	// free-text blob (parsed next) overrides it.
	fnvlist_add_string(fsprops, "atime", "off");

	parse_props_blob(props_blob_utf8, props, fsprops);
	enable_compat_features(props);

	wchar_t keyfile[MAX_PATH] = { 0 };
	if (passphrase_utf8 && passphrase_utf8[0]) {
		if (!write_temp_keyfile(passphrase_utf8, keyfile,
		    _countof(keyfile))) {
			fnvlist_add_string(res, "err",
			    "failed to stage encryption key");
			nvlist_free(nvroot);
			nvlist_free(props);
			nvlist_free(fsprops);
			goto serialize;
		}

		char keyfile_utf8[MAX_PATH * 3];
		WideCharToMultiByte(CP_UTF8, 0, keyfile, -1, keyfile_utf8,
		    sizeof (keyfile_utf8), NULL, NULL);

		// zfs_prop_valid_keylocation() (module/zcommon/zfs_prop.c)
		// requires the literal "file:///" prefix (three slashes), but
		// get_key_material_file() (lib/libzfs/libzfs_crypto.c) only
		// ever strips exactly 7 chars ("file://") before fopen()'ing
		// the rest - so the third slash ends up as part of the path
		// handed to fopen(), e.g. "/C:\Users\...". Windows accepts a
		// leading slash ahead of a drive letter in file paths.
		char keyloc[MAX_PATH * 3 + 8];
		_snprintf_s(keyloc, sizeof (keyloc), _TRUNCATE, "file:///%s",
		    keyfile_utf8);

		fnvlist_add_string(fsprops, "encryption", "on");
		fnvlist_add_string(fsprops, "keyformat", "passphrase");
		fnvlist_add_string(fsprops, "keylocation", keyloc);
	}

	// zpool_create() distinguishes "no props" (NULL) from "empty props"
	// (non-NULL, zero entries): the latter still runs
	// zpool_valid_proplist()/zfs_valid_proplist() and reaches the
	// ZFS_IOC_POOL_CREATE ioctl with a present-but-empty nvlist, which
	// this driver rejects with EINVAL. The CLI never hits this because
	// zpool_main.c only allocates props/fsprops when -o/-O was actually
	// given. fsprops is never empty here (atime=off is always set), but
	// props (-o) usually is when the user gave no pool-level overrides.
	nvlist_t *props_arg = nvlist_empty(props) ? NULL : props;
	nvlist_t *fsprops_arg = nvlist_empty(fsprops) ? NULL : fsprops;

	int rc = zpool_create(g_lzh, poolname_utf8, nvroot, props_arg,
	    fsprops_arg);

	if (keyfile[0])
		DeleteFileW(keyfile);

	nvlist_free(nvroot);
	nvlist_free(props);
	nvlist_free(fsprops);

	if (rc != 0) {
		const char *desc = libzfs_error_description(g_lzh);
		fnvlist_add_string(res, "err", desc ? desc : "create failed");
		goto serialize;
	}

	zfs_handle_t *zhp = zfs_open(g_lzh, poolname_utf8,
	    ZFS_TYPE_FILESYSTEM);
	if (zhp) {
		if (passphrase_utf8 && passphrase_utf8[0]) {
			// Leave the pool usable via the tray's normal
			// unlock flow (PromptPassphrase) rather than
			// referencing a keyfile that no longer exists.
			(void) zfs_prop_set(zhp, "keylocation", "prompt");
		}
		(void) zfs_mount(zhp, NULL, 0);
		zfs_close(zhp);
	}

	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	fnvlist_add_string(res, "name", poolname_utf8);

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

/* -------------------- destroy pool -------------------- */

typedef struct { uint64_t want; zpool_handle_t *out; } find_guid_ctx_t;

static int
find_guid_cb(zpool_handle_t *zhp, void *data)
{
	find_guid_ctx_t *ctx = (find_guid_ctx_t *)data;
	nvlist_t *cfg = zpool_get_config(zhp, NULL);
	uint64_t g = 0;
	if (cfg)
		(void) nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID, &g);
	if (g == ctx->want) {
		ctx->out = zhp;
		return (1);
	}
	zpool_close(zhp);
	return (0);
}

char *
zed_destroy_pool_json(uint32_t flags, uint64_t guid,
    const char *pool_name_utf8, size_t *out_len)
{
	(void) flags;
	*out_len = 0;
	nvlist_t *res = fnvlist_alloc();
	fnvlist_add_boolean_value(res, "ok", B_FALSE);

	zpool_handle_t *zhp = NULL;
	if (guid) {
		find_guid_ctx_t ctx = { guid, NULL };
		(void) zpool_iter(g_lzh, find_guid_cb, &ctx);
		zhp = ctx.out;
	}
	if (!zhp && pool_name_utf8 && pool_name_utf8[0])
		zhp = zpool_open(g_lzh, pool_name_utf8);

	if (!zhp) {
		fnvlist_add_string(res, "err", "pool not found");
		goto serialize;
	}

	char name[ZFS_MAX_DATASET_NAME_LEN];
	_snprintf_s(name, sizeof (name), _TRUNCATE, "%s",
	    zpool_get_name(zhp));

	if (zpool_disable_datasets(zhp, B_TRUE) != 0) {
		const char *desc = libzfs_error_description(g_lzh);
		fnvlist_add_string(res, "err",
		    desc ? desc : "could not unmount datasets");
		fnvlist_add_string(res, "name", name);
		zpool_close(zhp);
		goto serialize;
	}

	int rc = zpool_destroy(zhp, "zfs_tray");
	if (rc != 0) {
		const char *desc = libzfs_error_description(g_lzh);
		fnvlist_add_string(res, "err", desc ? desc : "destroy failed");
		fnvlist_add_string(res, "name", name);
		zpool_close(zhp);
		goto serialize;
	}
	zpool_close(zhp);

	fnvlist_add_boolean_value(res, "ok", B_TRUE);
	fnvlist_add_string(res, "name", name);

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

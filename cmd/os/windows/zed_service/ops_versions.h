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

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// { "ok":true, "dataset", "mount", "obj", "is_dir", "size", "mtime",
//   "mtime_ns", "versions":[ {"snap","creation","size","mtime","mtime_ns",
//   "path","same_as_live","changed"}, .. ] } oldest snapshot first,
// or { "ok":false, "err":"<msg>" }.
// "client" is the connected pipe handle, impersonated while the path is
// opened. Returns HeapAlloc'ed string; caller must HeapFree() it.
char *zed_file_versions_json(void *client, const char *path_utf8,
    size_t *out_len);

// { "ok":true, "dataset", "truncated", "snapshots", "items":[ {"name",
//   "is_dir", "versions":[ {"snap","oldest_snap","creation","size","mtime",
//   "path"}, .. newest first ]}, .. ] } for names in the directory that exist
// only in snapshots, or { "ok":false, "err":"<msg>" }.
char *zed_deleted_items_json(void *client, const char *path_utf8,
    size_t *out_len);

#ifdef __cplusplus
}
#endif

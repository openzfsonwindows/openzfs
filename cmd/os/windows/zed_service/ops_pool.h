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

// JSON (UTF-8) builders. Return HeapAlloc'ed string and set *out_len.
// Caller must HeapFree() the returned buffer.

// { "disks": [ {"path","size","model","is_boot":bool,"has_pool":bool}, .. ] }
char *zed_list_disks_json(size_t *out_len);

// { "ok":true, "name":"tank" } or { "ok":false, "err":"<msg>" }
char *zed_create_pool_json(uint32_t flags, uint32_t topology,
    uint32_t ndisks, const char * const *disk_paths_utf8,
    const char *poolname_utf8, const char *props_blob_utf8,
    const char *passphrase_utf8, size_t *out_len);

// { "ok":true, "name":"tank" } or { "ok":false, "err":"<msg>" }
char *zed_destroy_pool_json(uint32_t flags, uint64_t guid,
    const char *pool_name_utf8, size_t *out_len);

#ifdef __cplusplus
}
#endif

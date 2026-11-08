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

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OP_SNAPSHOT_CREATE flags */
#define	ZSNAP_RECURSIVE	0x01	/* also snapshot every child dataset */

/* OP_SNAPSHOT_DESTROY flags */
#define	ZSNAPD_DEFER	0x01	/* mark for deferred destroy if in use */

/* OP_DATASET_SETPROP flags */
#define	ZSETPROP_INHERIT	0x01	/* zfs inherit, the value is ignored */

/* OP_DATASET_DESTROY flags */
#define	ZDSD_RECURSIVE	0x01	/* also child datasets and all snapshots */

/*
 * OP_SNAPSHOT_ROLLBACK flags. Rolling back to anything but the newest
 * snapshot destroys the newer ones (and any clones of them), which libzfs
 * does without asking, so the service refuses unless the caller says it is
 * what the user wants.
 */
#define	ZROLL_DESTROY_NEWER	0x01	/* newer snapshots may be destroyed */
#define	ZROLL_DESTROY_CLONES	0x02	/* so may clones of those snapshots */

/*
 * All return a HeapAlloc'ed UTF-8 JSON string, caller HeapFree()s, or NULL
 * on out of memory. On failure the shape is { "ok":false, "err":"<msg>" }.
 *
 * zed_list_datasets_json(): every filesystem, parents before children:
 *   { "ok":true, "datasets":[ {"name","pool"(bool),"mounted"(bool),
 *     "mountpoint","used","avail","refer","origin","encrypted"(bool),
 *     "keyloaded"(bool)}, .. ] }
 *   sizes are decimal strings (u64).
 *
 * zed_list_snapshots_json(): snapshots of one dataset, oldest first:
 *   { "ok":true, "dataset", "snapshots":[ {"name","creation","used",
 *     "refer","holds","clones"}, .. ] }  "name" is the part after '@'.
 *
 * zed_snapshot_create_json() / zed_snapshot_destroy_json():
 *   { "ok":true, "dataset" } or { "ok":false, "dataset", "err" }
 */
char *zed_list_datasets_json(size_t *out_len);
char *zed_list_snapshots_json(const char *dataset_utf8, size_t *out_len);
char *zed_snapshot_create_json(uint32_t flags, const char *dataset_utf8,
    const char *snap_utf8, size_t *out_len);
/* "names" is count back to back NUL-terminated snapshot names ('@' part) */
char *zed_snapshot_destroy_json(uint32_t flags, const char *dataset_utf8,
    const char *names, uint32_t count, size_t names_len, size_t *out_len);
/*
 * Roll the dataset back to the snapshot (the name without the '@').
 *   { "ok":true, "dataset", "destroyed":"<number of snapshots>" }
 *   { "ok":false, "dataset", "err", "newer_block"|"clones_block":true }
 * where newer_block / clones_block say which flag would be needed.
 */
char *zed_snapshot_rollback_json(uint32_t flags, const char *dataset_utf8,
    const char *snap_utf8, size_t *out_len);

/*
 * Clone dataset@snapshot as the new dataset "target" (a full name, anywhere
 * in the same pool), and mount it.
 *   { "ok":true, "dataset":"<target>", "mounted":bool } or
 *   { "ok":false, "err" }
 */
char *zed_snapshot_clone_json(const char *dataset_utf8, const char *snap_utf8,
    const char *target_utf8, size_t *out_len);

/*
 * Promote a clone: it takes over the snapshots it was cloned from, and the
 * dataset it was cloned from becomes the clone of it.
 *   { "ok":true, "dataset" } or { "ok":false, "dataset", "err" }
 */
char *zed_dataset_promote_json(const char *name_utf8, size_t *out_len);

/*
 * Create a filesystem. "props" is optional text, one "key=value" per line.
 * A non-empty "passphrase" makes it a new encryption root with that
 * passphrase (it is left with keylocation=prompt, to be unlocked like any
 * other).
 *   { "ok":true, "dataset", "mounted":bool } or { "ok":false, "err" }
 */
char *zed_dataset_create_json(const char *name_utf8, const char *props_utf8,
    const char *passphrase_utf8, size_t *out_len);

/*
 * Destroy a dataset. A dataset with child datasets or snapshots is refused
 * unless ZDSD_RECURSIVE is given, with
 *   { "ok":false, "err", "needs_recursive":true, "children":"N",
 *     "snapshots":"M" }
 * so the caller can say what would go. A pool is never destroyed here.
 */
char *zed_dataset_destroy_json(uint32_t flags, const char *name_utf8,
    size_t *out_len);

/*
 * The properties of a dataset, as "zfs get all" would show them:
 *   { "ok":true, "dataset", "props":[ {"name","value","source","from",
 *     "readonly","inheritable","hint"}, .. ] }
 * "source" is local, inherited (from the dataset named in "from"), received,
 * temporary, default or none. "readonly" is also set for the ones that can
 * only be chosen when the dataset is created. "hint" is ZFS's own list of
 * the values it takes, "on | off", or a pattern, and may be empty.
 */
char *zed_dataset_props_json(const char *dataset_utf8, size_t *out_len);

/*
 * Set a property, or with ZSETPROP_INHERIT make it inherit again.
 *   { "ok":true, "dataset" } or { "ok":false, "dataset", "err" }
 */
char *zed_dataset_setprop_json(uint32_t flags, const char *dataset_utf8,
    const char *prop_utf8, const char *value_utf8, size_t *out_len);

#ifdef __cplusplus
}
#endif

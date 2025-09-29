#pragma once
#include <stddef.h>

#include "pipe_rpc.h"

// Returns HeapAlloc'd JSON for { "pools": [ ... ] }.
// Caller HeapFree()s the returned buffer. On error returns NULL.
char *zed_status_json_build(size_t *out_len);
char *zed_list_json_build(size_t *out_len);
char *zed_status_json_build_by_guid(uint64_t guid,
    zfs_status_verbosity_t verb, size_t *out_len);

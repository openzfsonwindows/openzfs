#pragma once
#include <stddef.h>
#include <stdint.h>

// JSON (UTF-8) builders. Return HeapAlloc'ed string and set *out_len.
// Caller must HeapFree() the returned buffer.

// { "candidates":[ {name,guid,state?,hostid?}, ... ] }
char *zed_import_scan_json(size_t *out_len);

// { "ok":true, "name":"...", "renamed":"..."} or { "ok":false, "err":"..."}
char *zed_import_one_json(uint32_t flags, uint64_t guid,
    const char *new_name_utf8, const char *altroot_utf8,
    size_t *out_len);

// { "imported":[...], "errors":[{"name":"...","err":"..."}] }
char *zed_import_all_json(uint32_t flags, const char *altroot_utf8,
    size_t *out_len);

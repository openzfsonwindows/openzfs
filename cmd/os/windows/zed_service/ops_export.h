#pragma once
#include <stddef.h>
#include <stdint.h>

char *zed_export_one_json(uint32_t flags, uint64_t guid, size_t *out_len);
char *zed_export_all_json(uint32_t flags, size_t *out_len);

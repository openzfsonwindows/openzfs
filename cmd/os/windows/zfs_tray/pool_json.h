#pragma once
#include <windows.h>
#include <stdint.h>

int ParsePoolNames(const char *json, int json_len,
    wchar_t names[][64], int maxnames);

typedef struct {
    wchar_t name[64];
    uint64_t guid;
    wchar_t health[32];
    wchar_t capacity_pct[16];
    wchar_t alloc[64];
    wchar_t freeb[64];
} PoolSummary;

BOOL GetPoolSummaryFromStatusJSON(const char *json, int json_len,
    PoolSummary *out);

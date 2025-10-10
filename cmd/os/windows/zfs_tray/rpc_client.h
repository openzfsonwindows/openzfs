#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    HANDLE h;		// pipe handle or INVALID_HANDLE_VALUE
    wchar_t name[128];	// pipe name
    DWORD  timeout_ms;	// per-call timeout
} zrpc_t;

static inline uint64_t
parse_u64_from_utf8(const char *s, int len)
{
	char tmp[40];
	int n = (len < 39 ? len : 39);
	memcpy(tmp, s, n);
	tmp[n] = 0;
	return (_strtoui64(tmp, NULL, 10));
}

BOOL  zrpc_init(zrpc_t *c, const wchar_t *pipename, DWORD timeout_ms);
void  zrpc_close(zrpc_t *c);

// Returns TRUE on success. Allocates *out on success; caller HeapFree’s it.
// On ERROR_BROKEN_PIPE / NOT_CONNECTED it will attempt one reconnect
// automatically.
BOOL  zrpc_call(zrpc_t *c, uint32_t op, const void *in, uint32_t in_len,
    uint32_t *status, uint8_t **out, uint32_t *out_len);

#include "rpc_client.h"

// Include after libzfs for dprintf
#include "pipe_rpc.h"

static BOOL
zrpc_connect(zrpc_t *c)
{
	if (c->h != INVALID_HANDLE_VALUE)
		return (TRUE);
	for (;;) {
		HANDLE h = CreateFileW(c->name, GENERIC_READ|GENERIC_WRITE,
		    0, NULL, OPEN_EXISTING,
		    SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION |
		    SECURITY_EFFECTIVE_ONLY | FILE_FLAG_OVERLAPPED, NULL);

		if (h != INVALID_HANDLE_VALUE) {
			c->h = h;
			dprintf("%s: connected\n", __func__);
			return (TRUE);
		}
		DWORD e = GetLastError();
		if (e == ERROR_PIPE_BUSY) {
			if (!WaitNamedPipeW(c->name, c->timeout_ms))
				return (FALSE);
			continue;
		}
		return (FALSE);
	}
}

BOOL
zrpc_init(zrpc_t *c, const wchar_t *pipename, DWORD timeout_ms)
{
	ZeroMemory(c, sizeof (*c));
	c->h = INVALID_HANDLE_VALUE;
	lstrcpynW(c->name, pipename, _countof(c->name));
	c->timeout_ms = timeout_ms ? timeout_ms : 5000;
	return (zrpc_connect(c));
}

void
zrpc_close(zrpc_t *c)
{
	if (c->h != INVALID_HANDLE_VALUE) {
		CloseHandle(c->h);
		c->h = INVALID_HANDLE_VALUE;
	}
}

static BOOL
read_all(HANDLE h, void *buf, uint32_t len, DWORD timeout)
{
	uint8_t *p = (uint8_t *)buf;
	DWORD got = 0, tot = 0;
	while (tot < len) {
		if (!ReadFile(h, p+tot, len-tot, &got, NULL))
			return (FALSE);
		tot += got;
	}
	return (TRUE);
}

static BOOL
write_all(HANDLE h, const void *buf, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	DWORD sent = 0, tot = 0;
	while (tot < len) {
		if (!WriteFile(h, p+tot, len-tot, &sent, NULL))
			return (FALSE);
		tot += sent;
	}
	return (TRUE);
}

typedef struct { uint32_t op; uint32_t len; } rq_hdr_t;
typedef struct { uint32_t status; uint32_t len; } rs_hdr_t;

static BOOL
do_call_once(zrpc_t *c, uint32_t op, const void *in, uint32_t in_len,
    uint32_t *status, uint8_t **out, uint32_t *out_len)
{
	rq_hdr_t rq = { op, in_len };

	dprintf("%s: sending op %d\n", __func__, op);


	if (!write_all(c->h, &rq, sizeof (rq)))
		return (FALSE);
	if (in_len && !write_all(c->h, in, in_len))
		return (FALSE);

	rs_hdr_t rs;
	if (!read_all(c->h, &rs, sizeof (rs), c->timeout_ms))
		return (FALSE);

	uint8_t *buf = NULL;
	if (rs.len) {
		buf = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, rs.len);
		if (!buf)
			return (FALSE);
		if (!read_all(c->h, buf, rs.len, c->timeout_ms)) {
			HeapFree(GetProcessHeap(), 0, buf);
			return (FALSE);
		}
	}
	if (status) *status = rs.status;
	if (out) *out = buf;
	else if (buf)
		HeapFree(GetProcessHeap(), 0, buf);
	if (out_len) *out_len = rs.len;
	return (TRUE);
}

BOOL
zrpc_call(zrpc_t *c, uint32_t op, const void *in, uint32_t in_len,
    uint32_t *status, uint8_t **out, uint32_t *out_len)
{
	dprintf("%s: call op %d \n", __func__, op);

	if (!zrpc_connect(c))
		return (FALSE);

	if (do_call_once(c, op, in, in_len, status, out, out_len))
		return (TRUE);

	DWORD e = GetLastError();
	if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) {
		zrpc_close(c);
		if (!zrpc_connect(c))
			return (FALSE);
		return (do_call_once(c, op, in, in_len, status, out, out_len));
	}
	return (FALSE);
}

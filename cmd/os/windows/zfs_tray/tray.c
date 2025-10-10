#define	UNICODE
#define	_UNICODE
#define	_WIN32_WINNT 0x0600   // Vista+ for Task Dialog APIs

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdint.h>
#include "pipe_rpc.h"
#include "pool_json.h"

#define	JSMN_STATIC
#include "jsmn.h" // single-header JSON tokenizer
#include "rpc_client.h" // zrpc_t + zrpc_init/zrpc_call from earlier

#include "import_window.h"

#pragma comment(lib, "comctl32.lib")

// Ensure v6 common-controls manifest is present
#pragma comment(linker, \
	"/manifestdependency:\"type='win32' " \
	"name='Microsoft.Windows.Common-Controls' " \
	"version='6.0.0.0' processorArchitecture='*' " \
	"publicKeyToken='6595b64144ccf1df' language='*'\"")

#define	WM_TRAYICON	(WM_USER + 1)
#define	ID_TRAY_ICON	1001
#define	IDM_STATUS	2001
#define	IDM_EXIT	2003

#define	IDM_POOLS_BASE	3000 // pool items will be 3000..3099
#define	IDM_POOLS_MAX	100
#define	IDM_POOLS_HEADER 2100 // “Pools” parent (not clickable)

#define	IDM_IMPORT_ALL	2201
#define	IDM_IMPORT_WIN  2202

#define	IDM_EXPORT_ALL 2301

#define	IDM_EXPORT_BASE 3100

static HINSTANCE g_hInst;
static HWND g_hWnd;
static NOTIFYICONDATAW g_nid;
static zrpc_t g_rpc;
static wchar_t g_pool_names[IDM_POOLS_MAX][64];
static uint64_t g_pool_guids[IDM_POOLS_MAX];
static int g_pool_count = 0;

// RPC helpers
static BOOL RpcGetStatus(wchar_t *out, size_t cchOut);
static HANDLE RpcSubscribe();
static DWORD WINAPI EventThread(LPVOID);
static void ShowBalloon(LPCWSTR title, LPCWSTR msg);

#include "resource.h"

int
ParsePoolsNameGuid(const char *json, int json_len,
    wchar_t names[][64], uint64_t guids[], int maxnames)
{
	jsmn_parser p;
	jsmn_init(&p);
	jsmntok_t tok[512];

	int r = jsmn_parse(&p, json, json_len, tok, (int)_countof(tok));
	if (r < 0)
		return (0);

	// find pools array
	int pools_arr = -1;
	for (int i = 1; i < r - 1; ++i) {
		if (tok[i].type == JSMN_STRING &&
		    tok[i + 1].type == JSMN_ARRAY) {
			int klen = tok[i].end - tok[i].start;
			if (klen == 5 &&
			    memcmp(json + tok[i].start, "pools", 5) == 0) {
				pools_arr = i + 1;
				break;
			}
		}
	}
	if (pools_arr < 0)
		return (0);

	int idx = pools_arr + 1, elems = tok[pools_arr].size, count = 0;

	for (int e = 0; e < elems && count < maxnames && idx < r; ++e) {
		if (tok[idx].type != JSMN_OBJECT) { // skip non-objects
			int end = tok[idx].end; idx++;
			while (idx < r && tok[idx].start < end) idx++;
			continue;
		}
		int obj = idx++, end = tok[obj].end;
		int name_v = -1, guid_v = -1;

		while (idx + 1 < r && tok[idx].start < end) {
			jsmntok_t *k = &tok[idx], *v = &tok[idx + 1];
			if (k->type == JSMN_STRING) {
				int klen = k->end - k->start;
				const char *ks = json + k->start;
				if (klen == 4 && memcmp(ks, "name", 4) == 0)
					name_v = idx + 1;
				else if (klen == 4 &&
				    memcmp(ks, "guid", 4) == 0)
					guid_v = idx + 1;
			}
			int vend = v->end;
			idx += 2;
			while (idx < r && tok[idx].start < vend) idx++;
		}

		if (name_v > 0 && (tok[name_v].type == JSMN_STRING)) {
			int nlen = tok[name_v].end - tok[name_v].start;
			int w = MultiByteToWideChar(CP_UTF8, 0,
			    json + tok[name_v].start, nlen, names[count], 63);
			names[count][w < 0 ? 0 : w] = 0;
			if (guid_v > 0 && (tok[guid_v].type == JSMN_STRING ||
			    tok[guid_v].type == JSMN_PRIMITIVE)) {
				int glen = tok[guid_v].end - tok[guid_v].start;
				guids[count] = parse_u64_from_utf8(
				    json + tok[guid_v].start,
				    glen);
			} else {
				guids[count] = 0ULL;
			}
			count++;
		}
	}
	return (count);
}

static int
ParsePoolNamesUTF16(const char *json, int json_len, wchar_t names[][64],
    int maxnames)
{
	jsmn_parser p;
	jsmn_init(&p);
	jsmntok_t tok[512];
	int r = jsmn_parse(&p, json, json_len, tok, (int)_countof(tok));
	if (r < 0)
		return (0);

	// find "pools" : array
	int pools_arr = -1;
	for (int i = 1; i < r - 1; ++i) {
		if (tok[i].type == JSMN_STRING) {
			int klen = tok[i].end - tok[i].start;
			const char *k = json + tok[i].start;
			if (klen == 5 && memcmp(k, "pools", 5) == 0 &&
			    tok[i + 1].type == JSMN_ARRAY) {
				pools_arr = i + 1;
				break;
			}
		}
	}
	if (pools_arr < 0)
		return (0);

	int count = 0;
	int elems = tok[pools_arr].size;
	int idx = pools_arr + 1;

	for (int e = 0; e < elems && count < maxnames && idx < r; ++e) {
		if (tok[idx].type == JSMN_STRING) {
			int len = tok[idx].end - tok[idx].start;
			MultiByteToWideChar(CP_UTF8, 0, json + tok[idx].start,
			    len, names[count], 63);
			names[count][63] = 0;
			count++; idx++;
		} else if (tok[idx].type == JSMN_OBJECT) {
			int obj_end = tok[idx].end;
			idx++;
			// scan object for "name"
			while (idx + 1 < r && tok[idx].start < obj_end) {
				jsmntok_t *k = &tok[idx], *v = &tok[idx + 1];
				if (k->type == JSMN_STRING &&
				    v->type == JSMN_STRING) {
					int klen = k->end - k->start;
					const char *ks = json + k->start;
					if (klen == 4 &&
					    memcmp(ks, "name", 4) == 0) {
						int len = v->end - v->start;
						MultiByteToWideChar(CP_UTF8, 0,
						    json + v->start, len,
						    names[count], 63);
						names[count][63] = 0;
						count++;
						break;
					}
				}
				// advance over key/value (skip subtree)
				int vend = v->end; idx += 2;
				while (idx < r && tok[idx].start < vend) idx++;
			}
			// skip to after object
			while (idx < r && tok[idx].start < obj_end) idx++;
		} else {
			// skip unknown node
			int end = tok[idx].end; idx++;
			while (idx < r && tok[idx].start < end) idx++;
		}
	}
	return (count);
}

static void
RefreshPoolsFromService(void)
{
	g_pool_count = 0;
	uint32_t st = 0;
	uint8_t *out = NULL;
	uint32_t outlen = 0;
	if (zrpc_call(&g_rpc, OP_LIST_POOLS, NULL, 0, &st, &out, &outlen) &&
	    st == 0 && out) {
		g_pool_count = ParsePoolsNameGuid((const char *)out,
		    (int)outlen, g_pool_names, g_pool_guids, IDM_POOLS_MAX);
	}
	if (out)
		HeapFree(GetProcessHeap(), 0, out);
}

static void
ShowPoolSummary(HWND hWnd, const PoolSummary *ps)
{
	wchar_t mainInstr[128];
	_snwprintf_s(mainInstr, _countof(mainInstr), _TRUNCATE, L"%s — %s",
	    ps->name, (ps->health[0] ? ps->health : L"?"));

	wchar_t content[512];
	_snwprintf_s(content, _countof(content), _TRUNCATE,
	    L"Capacity: %s%%\r\nAllocated: %s\r\nFree: %s\r\nGUID: %llu",
	    (ps->capacity_pct[0] ? ps->capacity_pct : L"?"),
	    (ps->alloc[0] ? ps->alloc : L"?"),
	    (ps->freeb[0] ? ps->freeb : L"?"),
	    (unsigned long long)ps->guid);

	HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");
	if (hComCtl) {
		typedef HRESULT(WINAPI *PFN_TaskDialogIndirect)(
		    const TASKDIALOGCONFIG *, int *, int *, BOOL *);

		PFN_TaskDialogIndirect pTaskDialogIndirect =
		    (PFN_TaskDialogIndirect)GetProcAddress(hComCtl,
		    "TaskDialogIndirect");
		if (pTaskDialogIndirect) {
			TASKDIALOGCONFIG cfg = { sizeof (cfg) };
			cfg.hwndParent = hWnd;
			cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
			    TDF_POSITION_RELATIVE_TO_WINDOW;
			cfg.pszWindowTitle = L"OpenZFS";
			cfg.pszMainInstruction = mainInstr;
			cfg.pszContent = content;
			cfg.dwCommonButtons = TDCBF_OK_BUTTON;
			pTaskDialogIndirect(&cfg, NULL, NULL, NULL);
			FreeLibrary(hComCtl);
			return;
		}
		FreeLibrary(hComCtl);
	}
	// Fallback
	wchar_t msg[640];
	_snwprintf_s(msg, _countof(msg), _TRUNCATE, L"%s\n\n%s", mainInstr,
	    content);
	MessageBoxW(hWnd, msg, L"OpenZFS", MB_OK | MB_ICONINFORMATION);
}

static HICON
LoadAppIconForTray(HINSTANCE hInst)
{
	int cx = GetSystemMetrics(SM_CXSMICON);
	int cy = GetSystemMetrics(SM_CYSMICON);
	return ((HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP),
	    IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}

static LRESULT CALLBACK
WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{

	switch (msg) {

	case WM_CREATE: {
		zrpc_init(&g_rpc, L"\\\\.\\pipe\\openzfs_zed", 5000);

		ZeroMemory(&g_nid, sizeof (g_nid));
		g_nid.cbSize = sizeof (g_nid);
		g_nid.hWnd = hWnd;
		g_nid.uID = ID_TRAY_ICON;
		g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
		g_nid.uCallbackMessage = WM_TRAYICON;
		g_nid.hIcon = LoadAppIconForTray((HINSTANCE)GetWindowLongPtrW(
		    hWnd, GWLP_HINSTANCE));
		g_nid.uVersion = NOTIFYICON_VERSION_4;
		Shell_NotifyIconW(NIM_ADD, &g_nid);
		Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

		lstrcpynW(g_nid.szTip, L"OpenZFS", ARRAYSIZE(g_nid.szTip));
		Shell_NotifyIconW(NIM_ADD, &g_nid);
		// Start event thread (fake stream)
		HANDLE h = CreateThread(NULL, 0, EventThread, NULL, 0, NULL);
		if (h) CloseHandle(h);

		// Load v6 common controls (makes styles render correctly)
		INITCOMMONCONTROLSEX icc = { sizeof (icc),
		    ICC_STANDARD_CLASSES };
		InitCommonControlsEx(&icc);

		return (0);
	}
	case WM_TRAYICON: {
		UINT code = LOWORD(lParam), nid = HIWORD(lParam);
		if (nid != ID_TRAY_ICON)
			return (0);

		// Legacy sends both WM_CONTEXTMENU and WM_RBUTTONUP;
		// modern sends only WM_CONTEXTMENU
		// if (code == WM_CONTEXTMENU || code == WM_RBUTTONUP) {
		if (code == WM_CONTEXTMENU) {
			POINT pt;
			GetCursorPos(&pt);

			// Always refresh pools just-in-time so the
			// menu is current
			RefreshPoolsFromService();

			HMENU m = CreatePopupMenu();

			// Build Pools submenu
			HMENU pools = CreatePopupMenu();
			if (g_pool_count == 0) {
				AppendMenuW(pools, MF_GRAYED, 0, L"(no pools)");
			} else {
				for (int i = 0; i < g_pool_count; ++i) {
					AppendMenuW(pools, MF_STRING,
					    IDM_POOLS_BASE + i,
					    g_pool_names[i]);
				}
			}
			AppendMenuW(m, MF_POPUP, (UINT_PTR)pools, L"Pools");

			AppendMenuW(m, MF_SEPARATOR, 0, NULL);

			AppendMenuW(m, MF_STRING, IDM_IMPORT_ALL,
			    L"Import (All)…");
			AppendMenuW(m, MF_STRING, IDM_IMPORT_WIN, L"Import…");

			AppendMenuW(m, MF_STRING, IDM_EXPORT_ALL,
			    L"Export (All)…");
			HMENU ex = CreatePopupMenu();
			AppendMenuW(m, MF_POPUP, (UINT_PTR)ex, L"Export ▶");
			// Fill Export > with current imported pools (names),
			// just like Pools >
			for (int i = 0; i < g_pool_count; ++i)
				AppendMenuW(ex, MF_STRING, IDM_EXPORT_BASE + i,
				    g_pool_names[i]);

			AppendMenuW(m, MF_SEPARATOR, 0, NULL);
			AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

			SetForegroundWindow(hWnd);
			TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x,
			    pt.y, 0, hWnd, NULL);
			DestroyMenu(m);
			PostMessageW(hWnd, WM_NULL, 0, 0);
			return (0);
		}

		if (code == WM_LBUTTONUP) {
			// Optional: left-click action
			SendMessageW(hWnd, WM_COMMAND, IDM_STATUS, 0);
			return (0);
		}
		return (0);
	}

	case WM_COMMAND: {
		UINT id = LOWORD(wParam);
		if (id >= IDM_POOLS_BASE &&
		    id < IDM_POOLS_BASE + IDM_POOLS_MAX) {
			int idx = (int)(id - IDM_POOLS_BASE);
			if (idx < g_pool_count) {
				op_get_status_by_guid_req_t rq = { 0 };
				rq.verbosity = ZFSV_SUMMARY;
				rq.guid = g_pool_guids[idx];

				uint8_t *out = NULL;
				uint32_t st = 0, outlen = 0;

				if (zrpc_call(&g_rpc, OP_GET_STATUS, &rq,
				    sizeof (rq), &st, &out, &outlen) &&
				    st == 0 && out) {
					PoolSummary ps;
					if (GetPoolSummaryFromStatusJSON(
					    (const char *)out, (int)outlen,
					    &ps))
						ShowPoolSummary(hWnd, &ps);
					else
						MessageBoxW(hWnd,
						    L"Could not parse status.",
						    L"OpenZFS",
						    MB_OK | MB_ICONWARNING);
					HeapFree(GetProcessHeap(), 0, out);
				} else {
					MessageBoxW(hWnd,
					    L"Service unavailable.",
					    L"OpenZFS", MB_OK | MB_ICONWARNING);
				}
			}
			return (0);
		}

		switch (id) {
		case IDM_STATUS:
			// call OP_GET_STATUS, parse, show dialog/balloon…
			return (0);
		case IDM_IMPORT_ALL:
		{
			if (MessageBoxW(hWnd, L"Import all detectable pools?",
			    L"OpenZFS", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
				break;
			op_import_all_req_t rq = { .flags = 0 };
			uint8_t *out = NULL;
			uint32_t st = 0, outlen = 0;
			if (zrpc_call(&g_rpc, OP_IMPORT_ALL, &rq, sizeof (rq),
			    &st, &out, &outlen) && st == 0 && out) {
				// MVP: show JSON text
				wchar_t w[2048] = { 0 };
				MultiByteToWideChar(CP_UTF8, 0, (char *)out,
				    (int)outlen, w, 2047);
				MessageBoxW(hWnd, w, L"Import result",
				    MB_OK | MB_ICONINFORMATION);
				HeapFree(GetProcessHeap(), 0, out);
			} else {
				MessageBoxW(hWnd,
				    L"Import failed or service unavailable.",
				    L"OpenZFS", MB_OK | MB_ICONERROR);
			}
			break;
		}

		case IDM_IMPORT_WIN:
		{
			CreateImportWindow(hWnd, &g_rpc);
			break;
		}

		case IDM_EXPORT_ALL:
		{
			if (MessageBoxW(hWnd, L"Export ALL imported pools?\n"
			    "Datasets will be unmounted.",
			    L"OpenZFS", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
				break;
			op_export_all_req_t rq = { .flags = 0 };
			uint8_t *out = NULL;
			uint32_t st = 0, outlen = 0;
			if (zrpc_call(&g_rpc, OP_EXPORT_ALL, &rq, sizeof (rq),
			    &st, &out, &outlen) && st == 0 && out) {
				wchar_t w[2048] = { 0 };
				MultiByteToWideChar(CP_UTF8, 0, (char *)out,
				    (int)outlen, w, 2047);
				MessageBoxW(hWnd, w, L"Export result",
				    MB_OK | MB_ICONINFORMATION);
				HeapFree(GetProcessHeap(), 0, out);
				RefreshPoolsFromService();
			} else {
				MessageBoxW(hWnd,
				    L"Export failed or service unavailable.",
				    L"OpenZFS", MB_OK | MB_ICONERROR);
			}
			return (0);
		}

		default:
		{
			// Export ▶ items
			if (id >= IDM_EXPORT_BASE &&
			    id < IDM_EXPORT_BASE + IDM_POOLS_MAX) {
				int idx = id - IDM_EXPORT_BASE;
				if (idx < g_pool_count) {
					wchar_t q[256];
					_snwprintf_s(q, _countof(q), _TRUNCATE,
					    L"Export pool “%s”?\n"
					    "Datasets will be unmounted.",
					    g_pool_names[idx]);
					if (MessageBoxW(hWnd, q, L"OpenZFS",
					    MB_OKCANCEL | MB_ICONWARNING) !=
					    IDOK)
						return (0);

					op_export_one_req_t rq = { .flags = 0,
					    .guid = g_pool_guids[idx]
					};
					uint8_t *out = NULL;
					uint32_t st = 0, outlen = 0;
					if (zrpc_call(&g_rpc, OP_EXPORT_ONE,
					    &rq, sizeof (rq), &st, &out,
					    &outlen) && st == 0 && out) {
						// show result, refresh pools
						wchar_t w[1024] = { 0 };
						MultiByteToWideChar(CP_UTF8, 0,
						    (char *)out, (int)outlen,
						    w, 1023);
						MessageBoxW(hWnd, w, L"Export",
						    MB_OK | MB_ICONINFORMATION);
						HeapFree(GetProcessHeap(), 0,
						    out);
						RefreshPoolsFromService();
					} else {
			MessageBoxW(hWnd,
			    L"Export failed or service unavailable.",
			    L"OpenZFS", MB_OK | MB_ICONERROR);
					}
				}
				return (0);
			}
		}


		case IDM_EXIT:
			PostQuitMessage(0);
			return (0);
		}
		return (0);
	}

	case WM_DESTROY:
		DestroyIcon(g_nid.hIcon);
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		return (0);
	}
	return (DefWindowProcW(hWnd, msg, wParam, lParam));
}

int APIENTRY
wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
	g_hInst = hInst;
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = L"ZfsTrayWnd";
	RegisterClassW(&wc);
	g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"ZfsTray",
	    0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);

	MSG msg;

	while (GetMessageW(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return (0);
}

static BOOL
SendRequest(HANDLE h, uint32_t op, const void *in, uint32_t inLen,
    uint32_t *outStatus, BYTE **outBuf, uint32_t *outLen)
{
	req_hdr_t rh = { op, inLen };
	DWORD wr;
	if (!WriteFile(h, &rh, sizeof (rh), &wr, NULL))
		return (FALSE);
	if (inLen) {
		if (!WriteFile(h, in, inLen, &wr, NULL))
			return (FALSE);
	}
	rsp_hdr_t rsp;
	DWORD rd;

	if (!ReadFile(h, &rsp, sizeof (rsp), &rd, NULL))
		return (FALSE);

	*outStatus = rsp.status;
	*outLen = rsp.len;
	*outBuf = NULL;

	if (rsp.len) {
		BYTE *buf = (BYTE*)HeapAlloc(GetProcessHeap(),
		    0, rsp.len + 1);
		if (!buf)
			return (FALSE);
		if (!ReadFile(h, buf, rsp.len, &rd, NULL)) {
			HeapFree(GetProcessHeap(), 0, buf);
			return (FALSE);
		}
		buf[rsp.len] = 0; // NUL-terminate for convenience
		*outBuf = buf;
	}

	return (TRUE);
}

static BOOL
RpcGetStatus(wchar_t *out, size_t cchOut)
{
	BOOL ok = FALSE;
	HANDLE h = CreateFileW(L"\\\\.\\pipe\\openzfs_zed",
	    GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	if (h == INVALID_HANDLE_VALUE)
		return (FALSE);
	BYTE *buf = NULL;
	uint32_t len = 0, st = 0;
	ok = SendRequest(h, OP_GET_STATUS, NULL, 0, &st, &buf, &len);
	if (ok && st == 0 && buf) {
		// naïve ANSI->Wide assuming JSON ASCII
		MultiByteToWideChar(CP_ACP, 0, (LPCSTR)buf, -1, out,
		    (int)cchOut);
	}
	if (buf)
		HeapFree(GetProcessHeap(), 0, buf);
	CloseHandle(h);
	return (ok && st == 0);
}

static HANDLE
RpcSubscribe()
{
	HANDLE h = CreateFileW(L"\\\\.\\pipe\\openzfs_zed",
	    GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return (NULL);
	uint32_t st = 0, len = 0;
	BYTE *buf = NULL;

	if (!SendRequest(h, OP_SUBSCRIBE_EVENTS, NULL, 0, &st, &buf, &len)) {
		CloseHandle(h);
		return (NULL);
	}

	if (buf) HeapFree(GetProcessHeap(), 0, buf); // stream header had len=0
	return (h); // read stream lines from h
}

static DWORD WINAPI
EventThread(LPVOID)
{
	HANDLE h = RpcSubscribe();
	if (!h)
		return (0);
	char line[256];
	DWORD rd;
	size_t pos = 0;
	for (;;) {
		if (!ReadFile(h, line+pos, 1, &rd, NULL))
			break;
		if (rd == 0)
			break;
		if (line[pos] == '\n' || pos == sizeof (line)-2) {
			line[pos] = 0;
			wchar_t w[256];
			MultiByteToWideChar(CP_ACP, 0, line, -1, w, 256);
			ShowBalloon(L"ZFS Event", w);
			pos = 0;
		} else {
			pos++;
		}
	}
	CloseHandle(h);
	return (0);
}

static void
ShowBalloon(LPCWSTR title, LPCWSTR msg)
{
	g_nid.uFlags = NIF_INFO;
	lstrcpynW(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle));
	lstrcpynW(g_nid.szInfo, msg, ARRAYSIZE(g_nid.szInfo));
	g_nid.dwInfoFlags = NIIF_INFO;
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
